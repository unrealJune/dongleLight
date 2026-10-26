"""End-to-end PCM tests, with only OS/device I/O replaced by a virtual cable."""
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import threading
import unittest
from unittest.mock import patch

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
import ks_audio as bus


def codec_roundtrip(pcm, codec, rate, container):
    encoded = subprocess.check_output([
        "ffmpeg", "-v", "error", "-f", "s16le", "-ar", "48000", "-ac", "1",
        "-i", "pipe:0", "-c:a", codec, "-b:a", rate, "-f", container, "pipe:1"
    ], input=pcm)
    return subprocess.check_output([
        "ffmpeg", "-v", "error", "-i", "pipe:0", "-f", "s16le",
        "-ar", "48000", "-ac", "1", "pipe:1"
    ], input=encoded)


class VirtualAudio:
    def __init__(self, cable, callback, on_error):
        self.cable, self.callback, self.on_error = cable, callback, on_error
        self.snapshots = 0
        cable.listeners.append(self)

    def play(self, pcm):
        self.cable.exchange(pcm)

    def close(self):
        self.cable.listeners.remove(self)


class VirtualDongle:
    def __init__(self, codec=None, pairing=None):
        self.process = subprocess.Popen([os.environ["KSBUS_HARNESS"], "--duplex" + ("-" + pairing if pairing else "")],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.codec = codec
        self.listeners = []
        self.workers = []
        self.changed = threading.Condition()
        self.drop_return = False
        self.advance_pairing = pairing in ("pair", "missing")

    def worker(self, name, channel):
        messages = []
        endpoint = None

        def emit(message):
            with self.changed:
                messages.append(message)
                if message["kind"] == "snapshot":
                    endpoint.snapshots += 1
                self.changed.notify_all()

        def audio_factory(output, input_device, callback, on_error):
            nonlocal endpoint
            endpoint = VirtualAudio(self, callback, on_error)
            return endpoint

        worker = bus.Worker(dict(name=name, channel=channel), emit, audio_factory)
        worker.messages = messages
        self.workers.append(worker)
        return worker

    def read(self, length):
        result = bytearray()
        while len(result) < length:
            part = self.process.stdout.read(length - len(result))
            if not part:
                raise AssertionError(f"Virtual firmware stopped with code {self.process.poll()}")
            result.extend(part)
        return bytes(result)

    def exchange(self, pcm=b"", advance_ms=0):
        if self.codec and pcm:
            pcm = codec_roundtrip(pcm, *self.codec)
        mono = np.frombuffer(pcm, dtype="<i2")
        stereo = np.repeat(mono, 2).tobytes()
        self.process.stdin.write(struct.pack("<II", len(stereo), advance_ms))
        self.process.stdin.write(stereo)
        self.process.stdin.flush()
        length, = struct.unpack("<I", self.read(4))
        returned = self.read(length)
        original = returned
        if self.drop_return:
            return
        if self.codec:
            returned = codec_roundtrip(returned, *self.codec)
        listeners = list(self.listeners)
        before = [listener.snapshots for listener in listeners]
        rng = random.Random(19)
        offset = 0
        while offset < len(returned):
            count = rng.randint(40, 1700) * 2
            for listener in listeners:
                listener.callback(returned[offset:offset + count])
            offset += count
        with self.changed:
            if not self.changed.wait_for(
                lambda: all(listener.snapshots > old for listener, old in zip(listeners, before)), 10
            ):
                artifacts = os.environ.get("KSBUS_ARTIFACTS")
                if artifacts:
                    destination = Path(artifacts)
                    destination.mkdir(parents=True, exist_ok=True)
                    (destination / "return-original.s16le").write_bytes(original)
                    (destination / "return-impaired.s16le").write_bytes(returned)
                raise AssertionError("Return PCM was not decoded by every simulated devbox")
        if (self.advance_pairing and self.workers and
                (self.workers[0].latest.get("pairing") or {}).get("state") == "searching"):
            self.exchange(advance_ms=4500)

    def close(self):
        for worker in self.workers:
            worker.close()
        self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise AssertionError("Virtual firmware did not stop")
        finally:
            self.process.stdout.close()
        if self.process.returncode:
            raise AssertionError(f"Virtual firmware exited with {self.process.returncode}")


class DuplexIntegrationTests(unittest.TestCase):
    def setUp(self):
        render = bus.ks.render
        self.seeded = patch.object(bus.ks, "render", side_effect=lambda *args, **kwargs:
                                   render(*args, **kwargs, rng=random.Random(1)))
        self.seeded.start()
        self.addCleanup(self.seeded.stop)

    def cable(self, codec=None, pairing=None):
        cable = VirtualDongle(codec, pairing)
        self.addCleanup(cable.close)
        return cable

    def assert_shared(self, workers):
        for worker in workers[1:]:
            self.assertEqual(workers[0].latest, worker.latest)
        for worker in workers:
            self.assertFalse([m for m in worker.messages if m["kind"] == "error"])
        # The virtual cable cannot establish BLE delivery or lamp readback.
        self.assertEqual(workers[0].latest["applied"], 0)
        self.assertTrue(all(not lamp["verified"] for lamp in workers[0].latest["lamps"]))

    def exercise_roundtrip(self, codec=None, channels=2):
        cable = self.cable(codec)
        workers = [cable.worker(f"box-{ch}", ch) for ch in range(channels)]
        for worker in workers:
            self.assertTrue(worker.execute(dict(action="connect"))["ok"])
        owner, observer = workers[:2]
        self.assertEqual([ch["name"] for ch in owner.latest["channels"][:channels]],
                         [f"box-{ch}" for ch in range(channels)])
        owner.execute(dict(action="rgb", color="#224466", brightness=50, lamp=1))
        self.assertEqual(observer.latest["lamps"][0]["color"], "#112233")
        rule = dict(provider="copilot", state="needs-you", color="#123456", brightness=80, lamp=1)
        saved = owner.execute(dict(action="rules", rules=[rule]))
        self.assertEqual(saved["message"], "Rules confirmed by dongle")
        for worker in workers:
            self.assertTrue(any(all(r.get(k) == v for k, v in rule.items()) and r["channel"] == 0
                                for r in worker.latest["rules"]))
        owner.execute(dict(action="signal", provider="copilot", state="needs-you"))
        self.assertEqual(observer.latest["lamps"][0]["color"], "#0E2944")
        self.assertEqual(observer.latest["sources"][0]["state"], "needs-you")
        self.assertEqual(observer.latest["sources"][0]["lamp"], 1)
        self.assertFalse(observer.latest["sources"][0]["acked"])
        observer.execute(dict(action="ack", lamp=1))
        self.assertTrue(owner.latest["sources"][0]["acked"])
        self.assertEqual(owner.latest["lamps"][0]["color"], "#112233")
        owner.execute(dict(action="signal", provider="copilot", state="idle"))
        self.assertEqual(observer.latest["sources"], [])
        self.assert_shared(workers)

    def test_clean_four_devboxes_command_state_return_and_remote_ack(self):
        self.exercise_roundtrip(channels=4)

    def test_pairing_adds_lamps_and_broadcasts_result_without_forgetting_existing(self):
        cable = self.cable(pairing="pair")
        owner = cable.worker("box-a", 0)
        observer = cable.worker("box-b", 1)
        owner.execute(dict(action="connect"))
        observer.execute(dict(action="connect"))
        self.assertEqual(owner.latest["lamps"], [])
        result = owner.execute(dict(action="claim"))
        self.assertTrue(result["ok"])
        self.assertIn("Lamp 1 paired", result["message"])
        first = observer.latest["lamps"][0]["name"]
        owner.execute(dict(action="claim"))
        self.assertEqual(len(observer.latest["lamps"]), 2)
        self.assertEqual(observer.latest["lamps"][0]["name"], first)
        self.assertEqual(observer.latest["pairing"]["state"], "paired")
        self.assertEqual(observer.latest["pairing"]["lamp"], 2)
        self.assert_shared([owner, observer])

    def test_pairing_reports_missing_busy_full_and_old_firmware(self):
        for mode, error in (("missing", "No unpaired lamp found"), ("busy", "already running"),
                            ("full", "All four"), (None, "Update the dongle firmware")):
            with self.subTest(mode=mode):
                cable = self.cable(pairing=mode)
                worker = cable.worker("box-a", 0)
                worker.execute(dict(action="connect"))
                lamps = list(worker.latest["lamps"])
                with self.assertRaisesRegex(ValueError, error):
                    worker.execute(dict(action="claim"))
                self.assertEqual(worker.latest["lamps"], lamps)

    @unittest.skipUnless(shutil.which("ffmpeg"), "ffmpeg not installed")
    def test_lossy_codec_in_both_directions_full_roundtrip(self):
        for codec in (("libopus", "16k", "opus"), ("libopus", "32k", "opus"),
                      ("aac", "48k", "adts")):
            with self.subTest(codec=codec):
                self.exercise_roundtrip(codec)

    def test_longest_repeated_command_is_applied_once(self):
        cable = self.cable()
        worker = cable.worker("box-a", 0)
        worker.execute(dict(action="connect"))
        generation = worker.latest["generation"]
        args = bus.styled_args("needs-you", "12345678", "#123456", 80, 1)
        worker.send(bus.OP_STYLED, args)
        self.assertEqual(worker.latest["generation"], generation + 1,
                         "The second audio copy must not re-apply a maximum-size command")
        worker.send(bus.OP_STYLED, args)
        self.assertEqual(worker.latest["generation"], generation + 2,
                         "Consuming the repeat copy must not lock out an immediate re-send")

    def test_four_simultaneous_uplinks_reach_all_four_receivers(self):
        cable = self.cable()
        workers = [cable.worker(f"box-{ch}", ch) for ch in range(4)]
        for worker in workers:
            worker.execute(dict(action="connect"))
        pcms = []
        for ch in range(4):
            args = bus.styled_args("error", f"box-{ch}", "#123456", 100, ch + 1)
            args[0] = 0xE0 + ch * 2
            samples = bus.ks.render(bus.ks.build_frame(bus.OP_STYLED, args), channel=ch)
            pcms.append(np.asarray(samples))
        mixed = np.zeros(max(len(pcm) for pcm in pcms))
        for pcm in pcms:
            mixed[:len(pcm)] += pcm / 4
        cable.exchange(np.clip(mixed * 32767, -32767, 32767).round().astype("<i2").tobytes())
        self.assertEqual({source["id"] for source in workers[0].latest["sources"]},
                         {0xE0, 0xE2, 0xE4, 0xE6})
        self.assertEqual([lamp["color"] for lamp in workers[0].latest["lamps"]], ["#123456"] * 4)
        self.assert_shared(workers)
        workers[3].execute(dict(action="ack", lamp=2))
        self.assertEqual({source["lamp"] for source in workers[0].latest["sources"] if source["acked"]}, {2})
        self.assertEqual([lamp["color"] for lamp in workers[0].latest["lamps"]],
                         ["#123456", "#000000", "#123456", "#123456"])
        self.assert_shared(workers)

    def test_conflicting_owner_is_refused_then_lease_can_expire(self):
        cable = self.cable()
        owner = cable.worker("box-a", 0)
        owner.execute(dict(action="connect"))
        intruder = cable.worker("box-b", 0)
        with self.assertRaisesRegex(ValueError, "belongs to box-a"):
            intruder.execute(dict(action="connect"))
        self.assertEqual(owner.latest["channels"][0]["name"], "box-a")
        cable.exchange(advance_ms=60001)
        self.assertFalse(owner.latest["channels"][0]["used"])
        intruder.execute(dict(action="connect"))
        self.assertEqual(owner.latest["channels"][0]["name"], "box-b")
        self.assert_shared([owner, intruder])

    def test_bad_uplink_crc_cannot_change_state(self):
        cable = self.cable()
        worker = cable.worker("box-a", 0)
        worker.execute(dict(action="connect"))
        generation = worker.latest["generation"]
        frame = bytearray(bus.ks.build_frame(bus.ks.OP_RGB, [255, 0, 0, 100]))
        frame[-1] ^= 0x80
        samples = bus.ks.render(frame)
        cable.exchange((np.asarray(samples) * 32767).round().astype("<i2").tobytes())
        self.assertEqual(worker.latest["generation"], generation)
        self.assertEqual([lamp["color"] for lamp in worker.latest["lamps"]], ["#000000"] * 4)

    def test_lost_return_keeps_cached_state_then_recovers_over_audio(self):
        cable = self.cable()
        worker = cable.worker("box-a", 0)
        worker.execute(dict(action="connect"))
        generation = worker.latest["generation"]
        cable.drop_return = True
        worker.send(bus.ks.OP_RGB, [32, 64, 96, 100])
        self.assertEqual(worker.latest["generation"], generation)
        self.assertEqual(worker.latest["lamps"][0]["color"], "#000000")
        worker.last_received -= 16
        with self.assertRaises(TimeoutError):
            worker.ready(timeout=0.01)
        cable.drop_return = False
        cable.exchange()
        self.assertGreater(worker.latest["generation"], generation)
        self.assertEqual([lamp["color"] for lamp in worker.latest["lamps"]], ["#204060"] * 4)


if __name__ == "__main__":
    unittest.main(verbosity=2)
