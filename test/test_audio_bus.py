"""Duplex bus contract tests; KSBUS_HARNESS points to the compiled C harness."""
import array
import binascii
import json
import math
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
import ks_audio as bus


class BusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.harness = os.environ["KSBUS_HARNESS"]
        cls.payload = subprocess.check_output([cls.harness])
        cls.pcm = subprocess.check_output([cls.harness, "--pcm"])

    def test_snapshot_c_python_contract(self):
        snapshot = bus.parse_snapshot(self.payload)
        self.assertEqual(snapshot["generation"], 7)
        self.assertEqual(snapshot["applied"], 6)
        self.assertEqual(snapshot["uptime"], 12345)
        self.assertEqual(snapshot["channels"][0]["name"], "box-a")
        self.assertEqual(snapshot["sources"][0]["state"], "needs-you")
        self.assertEqual(snapshot["sources"][0]["lamp"], 1)
        self.assertEqual(snapshot["lamps"][0]["color"], "#091B2C")
        self.assertEqual(len(snapshot["rules"]), 40)
        self.assertTrue(snapshot["lamps"][0]["verified"])

    def test_malformed_snapshot_rejected(self):
        for n in (0, 8, 24, len(self.payload) - 1):
            with self.assertRaises(ValueError):
                bus.parse_snapshot(self.payload[:n])
        with self.assertRaises(ValueError):
            bus.parse_snapshot(self.payload + b"\0")

    def test_pairing_snapshot_and_old_firmware_capability(self):
        self.assertIsNone(bus.parse_snapshot(self.payload)["pairing"])
        payload = subprocess.check_output([self.harness, "--pairing"])
        self.assertEqual(bus.parse_snapshot(payload)["pairing"],
                         dict(state="paired", lamp=1, since=12000))
        for damaged in (payload[:-1], payload[:-6] + bytes([99]) + payload[-5:],
                        payload[:-5] + bytes([4]) + payload[-4:]):
            with self.assertRaises(ValueError):
                bus.parse_snapshot(damaged)

    def test_pairing_does_not_reuse_a_previous_success_or_retry_automatically(self):
        worker = object.__new__(bus.Worker)
        worker.latest = bus.parse_snapshot(subprocess.check_output([self.harness, "--pairing"]))
        worker.last_received = 0
        worker.lock = bus.threading.Lock()
        worker.changed = bus.threading.Event()
        worker.ready = lambda: worker.latest
        sent = []
        worker.send = lambda op, args: sent.append((op, args))
        with patch.object(bus.time, "monotonic", side_effect=[0, 0, 0, 41]):
            with self.assertRaisesRegex(TimeoutError, "Pairing was not confirmed"):
                worker.execute(dict(action="claim"))
        self.assertEqual(sent, [(bus.OP_CLAIM, [])])

    def test_signal_fits_existing_audio_frame(self):
        args = bus.styled_args("needs-you", "12345678", "#123456", 80, 4)
        self.assertEqual(len(bus.ks.build_frame(bus.OP_STYLED, args)), 16)
        self.assertEqual(args[:6], [bus.ks.source_id("12345678"), 0x44, 18, 52, 86, 80])
        for brightness, lamp in ((101, 0), (50, 5), (-1, 0)):
            with self.assertRaises(ValueError):
                bus.styled_args("done", "box", "#123456", brightness, lamp)

    def test_c_reed_solomon_parity_and_burst_recovery(self):
        frame = subprocess.check_output([self.harness, "--frame"])
        raw = self.payload + binascii.crc32(self.payload).to_bytes(4, "little")
        self.assertEqual(frame[20:], bytes(bus._RS.encode(raw)))
        damaged = bytearray(frame[20:])
        for i in range(16):
            damaged[20 + i] ^= 0xA5
        self.assertEqual(bytes(bus._RS.decode(damaged)[0]), raw)

    def test_low_confidence_uart_byte_is_recovered_not_truncated(self):
        samples = array.array("h", self.pcm)
        start = (20 + 40) * bus.BITS_PER_BYTE * bus.BIT_SAMPLES
        samples[start:start + bus.BITS_PER_BYTE * bus.BIT_SAMPLES] = array.array("h", [0] * 800)
        self.assertEqual(bus.decode_burst(samples), self.payload)

    def test_two_damaged_sync_bits_still_recover_valid_frame(self):
        samples = array.array("h", self.pcm)
        for bit, frequency in ((16 * 10 + 1, 1000), (17 * 10 + 2, 1800)):
            start = bit * bus.BIT_SAMPLES
            samples[start:start + bus.BIT_SAMPLES] = array.array("h", (
                round(6554 * math.sin(2 * math.pi * frequency * i / 48000))
                for i in range(bus.BIT_SAMPLES)))
        self.assertEqual(bus.decode_burst(samples), self.payload)

    def test_return_audio_roundtrip_irregular_chunks(self):
        received = []
        rx = bus.Receiver(received.append)
        rng = random.Random(12)
        offset = 0
        while offset < len(self.pcm):
            count = rng.randint(80, 3500) * 2
            rx.feed(self.pcm[offset:offset + count])
            offset += count
        self.assertEqual(len(received), 1)
        self.assertEqual(received[0]["uptime"], 12345)

    def test_return_audio_frame_starts_inside_capture_buffer(self):
        for leading in (1600, 2400, 4400):
            with self.subTest(leading_samples=leading):
                received = []
                rx = bus.Receiver(received.append)
                pcm = bytes(leading * 2) + self.pcm
                for offset in range(0, len(pcm), 9600):
                    rx.feed(pcm[offset:offset + 9600])
                self.assertEqual(len(received), 1)
                self.assertEqual(received[0]["uptime"], 12345)

    def test_gain_noise_and_phase(self):
        samples = array.array("h", self.pcm)
        rng = random.Random(5)
        degraded = array.array("h", [0] * 23 + [round(x * 0.08) + rng.randint(-8, 8) for x in samples])
        self.assertEqual(bus.decode_burst(degraded), self.payload)

    def test_clock_drift(self):
        samples = array.array("h", self.pcm)
        for factor in (0.993, 1.007):
            drifted = array.array("h", (samples[min(len(samples) - 1, int(i / factor))]
                                      for i in range(int(len(samples) * factor))))
            self.assertEqual(bus.decode_burst(drifted), self.payload)

    def test_bad_crc_never_publishes(self):
        samples = array.array("h", self.pcm)
        # Destroy the middle of the frame without damaging pilot or sync.
        for i in range(40000, 90000):
            samples[i] = 0
        self.assertIsNone(bus.decode_burst(samples))

    @unittest.skipUnless(shutil.which("ffmpeg"), "ffmpeg not installed")
    def test_return_stream_through_audio_codecs(self):
        for codec, rate, container in (("libopus", "32k", "opus"), ("libopus", "16k", "opus"),
                                       ("aac", "48k", "adts")):
            with self.subTest(codec=codec, rate=rate):
                encoded = subprocess.check_output(["ffmpeg", "-v", "error", "-f", "s16le", "-ar", "48000",
                    "-ac", "1", "-i", "pipe:0", "-c:a", codec, "-b:a", rate, "-f", container, "pipe:1"], input=self.pcm)
                decoded = subprocess.check_output(["ffmpeg", "-v", "error", "-i", "pipe:0", "-f", "s16le",
                    "-ar", "48000", "-ac", "1", "pipe:1"], input=encoded)
                self.assertEqual(bus.decode_burst(array.array("h", decoded)), self.payload)

    def test_all_devboxes_observe_same_configuration_and_acknowledgement(self):
        before = bus.parse_snapshot(self.payload)
        ack = subprocess.check_output([self.harness, "--ack"])
        viewers = [bus.parse_snapshot(ack), bus.parse_snapshot(ack)]
        self.assertFalse(before["sources"][0]["acked"])
        for view in viewers:
            self.assertTrue(view["sources"][0]["acked"])
            self.assertEqual(view["lamps"][0]["color"], "#000000")
            self.assertEqual(view["rules"], before["rules"])
            self.assertEqual(view["channels"], before["channels"])

    def test_events_send_dongle_rule_and_collision_free_source(self):
        worker = object.__new__(bus.Worker)
        worker.name, worker.channel = "box-a", 0
        snapshot = bus.parse_snapshot(self.payload)
        worker.ready = lambda: snapshot
        sent = []
        worker.send = lambda op, args: sent.append((op, args))
        for provider in bus.PROVIDERS:
            worker.execute(dict(action="signal", provider=provider, state="error"))
        self.assertEqual([args[0] for _, args in sent], [0xE0, 0xE1])
        self.assertEqual(sent[0][1][2:6], [255, 64, 64, 75])
        self.assertTrue(all(op == bus.OP_STYLED for op, _ in sent))

    def test_readiness_refuses_another_owner(self):
        worker = object.__new__(bus.Worker)
        worker.name = "box-b"
        worker.channel = 0
        worker.latest = bus.parse_snapshot(self.payload)
        worker.last_received = bus.time.monotonic()
        worker.lock = bus.threading.Lock()
        worker.changed = bus.threading.Event()
        worker.hello = lambda: None
        with self.assertRaisesRegex(ValueError, "belongs to box-a"):
            worker.ready(timeout=0.1)

    def test_readiness_skips_redundant_hello_when_channel_is_reserved_and_fresh(self):
        worker = object.__new__(bus.Worker)
        worker.name = "box-a"
        worker.channel = 0
        worker.latest = bus.parse_snapshot(self.payload)
        worker.latest["channels"][0]["owner"] = bus.ks.source_id("box-a")
        worker.last_received = bus.time.monotonic()
        worker.last_hello = bus.time.monotonic()
        worker.lock = bus.threading.Lock()
        worker.changed = bus.threading.Event()
        hellos = []
        worker.hello = lambda: hellos.append(1)
        self.assertEqual(worker.ready(timeout=0.1)["generation"], 7)
        self.assertEqual(hellos, [])

    def test_no_return_audio_never_sends_commands(self):
        worker = object.__new__(bus.Worker)
        worker.name, worker.channel, worker.latest = "box", 0, None
        worker.last_received = 0
        worker.last_hello = bus.time.monotonic()
        worker.lock = bus.threading.Lock()
        worker.changed = bus.threading.Event()
        worker.hello = lambda: None
        with self.assertRaises(TimeoutError):
            worker.ready(timeout=0.01)


if __name__ == "__main__":
    unittest.main()
