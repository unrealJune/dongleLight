# dongleLight

A physical "an agent needs you" light that crosses the machine boundary.
GitHub Copilot CLI and Claude Code sessions on cloud devboxes signal through RDP to a
USB dongle on your laptop, which drives Bluetooth LED strips and a little
screen creature named Lumi.

```
devbox(es)                  RDP                    your laptop                 Bluetooth
agent hook ─► WinUI app ─► RDP playback ─► USB speaker ─► ESP32-S3 dongle ─► LED strips
                      ◄─ RDP recording ◄─ USB microphone ◄─ shared state
```

## Native control plane

`desktop\DongleLight` is the Windows 11 WinUI 3 / Mica application. It uses
**duplex audio only**, with four leased devbox channels and dongle-authoritative
shared colors, rules, lamp assignments, agent activity, and acknowledgements.
Closing the window keeps its notification-area application running; sign-in
startup and hook installation are opt-in.

Build/publish with `desktop\publish.ps1`, then follow
[the control-plane setup guide](desktop/README.md). The x64 app and updated
audio firmware build successfully; host modem/model regressions and hook
configuration tests pass. Physical USB duplex audio and a QHM controller's BLE
discovery, persistence, and connection have been checked; simultaneous RDP
microphone fan-out remains unverified. The legacy `ks.py` utility below is
still available; it is not the native application's transport selector.

**Supported strips:** Triones / HappyLighting controllers (names starting
`Triones`, `LEDBLE`, `LEDBlue`, `QHM`, `BRGlight`, `Dream`), the same family
`trionesControl` drives, and KS / KeepSmile lamps (`KS03~` etc.). The family is
picked per lamp from its advertised name. (The `ks` names in the code are
historical: the project started on KS lamps.)

![Lumi](docs/lumi_portrait.gif)

## Two firmwares, pick one

| | `firmware/card` (legacy smart card) | `firmware/audio` (duplex USB audio) |
| --- | --- | --- |
| Dongle shows up as | CCID smart card reader | speaker + microphone "KS Light Bridge" |
| Direction | **two-way**: `ks.py` gets "delivered ✓" or a real error | native app: commands out, shared snapshots back; legacy `ks.py` remains playback-only |
| Several devboxes | serialised by Windows' smart card service | 4 tone channels; simultaneous sends survive (see below) |
| Needs | smart card redirection allowed for your RDP sessions | playback **and microphone** redirection for the native app |
| Downside | an unusual USB device class on a corp machine | laptop audio routing and microphone fan-out; ~2–5 s per uplink command, slower shared-state confirmation |

`ks.py` auto-detects: it uses the card when it finds a bridge reader, otherwise audio.

**One honest caveat on card mode:** it's a homemade CCID reader. It doesn't
impersonate anything: it has its own ATR, its own proprietary AID, and it answers
"not found" to every other SELECT, including PIV and GIDS. Windows sees an
unknown card and leaves it alone. Still, check it against your device policy
before plugging it into a managed machine.

## What's verified vs not

| Part | Status |
| --- | --- |
| Audio decoder + framing | **Host-tested**: Opus 32/16/12k-voip, AAC, MP3, 0.7% clock error, -45 dB, chime mid-frame, 60 s of junk → 0 false commands. Single channel survives Opus 6 kbps and 6 dB SNR. |
| Duplex return stream | C/Python framing and Reed-Solomon parity agree; snapshot, burst-error recovery, noise, drift, Opus 16/32 kbps and AAC regressions pass. Actual RDP recording codec/routing is unverified. |
| Simulated duplex integration | Real host workers and production C codecs/model complete command-to-state-to-return-audio round trips, including remote acknowledgement and shared rules. Run `python test/run_tests.py --duplex-only`; device I/O, RTOS scheduling and BLE remain simulated. |
| Simultaneous channels (`test/multichannel.py`) | 1–2 at once: 100% through Opus ≥16k and AAC. 4 at once: 98–100% at Opus ≥24k, 71% at 16k. Opus 12k "voip" mode can't handle overlap. 0 false frames in 2000 decodes. |
| Signal/ack/expiry logic, lamp mapping | **Host-tested** (`test/test_logic.c`): auto-assign, pinning, overflow, per-lamp acks and colors |
| Card applet + CCID messages | **Host-tested** with the exact APDUs `ks.py` sends, incl. a Windows-style PIV probe |
| Triones packets | Byte-identical to the real `trionesControl` over 202 commands (`test/xcheck_trionescontrol.py`); effect codes and speed encoding from flux_led / Home Assistant led-ble |
| Triones characteristic | Discovered per lamp: first writable of `FFD5`, `FFD9`, `FFE9`, `FFE5` (led-ble's order; batches differ). A physical QHM controller connected using `FFD9` and accepted diagnostic writes. |
| QHM discovery | Zero-padded advertisements are accepted. The captured packet and malformed/truncated cases have regressions; a physical controller was saved and reconnected after restoring normal audio firmware. |
| Readback | Reply parsing, verify/mismatch, drift and adoption rules **host-tested** (`test/test_logic.c`). Reply layout from flux_led; power + RGB fields proven on your strips by `light.py` and hardware-confirmed on the dongle with a physical QHM controller (`FFD4` notify). |
| KS packets | Match upstream `golden_packets.json` |
| Screen | Rendered on the host from the same C (`test/ui_preview.py` → `docs/`) |
| Both firmwares | **Build clean** on ESP-IDF v5.4.2 (zero warnings in our code). Audio firmware has run on physical ESP32-S3 hardware; card firmware hardware bring-up remains unverified. |
| Multi-lamp BLE | Up to 4 simultaneous connections (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=4`); single-lamp claiming, NVS persistence, and reconnect verified on a QHM controller. Multiple simultaneous lamps remain unverified. |
| Board pins | LCD 3/5/4/2/1, backlight 38, LED 40/39 come from LILYGO's own examples. The BOOT button is assumed to be GPIO0. |
| RDP behaviour | Unverified: whether multiple sessions share one redirected reader cleanly, which audio codec/bitrate RDP uses, and per-app audio routing. |

## Bring-up, in order

**0. Host tests.** Install `host/requirements.txt` and `test/requirements.txt`;
put FFmpeg on PATH. Linux/WSL uses gcc; Windows uses the test environment's
`ziglang` compiler. Run `python test/run_tests.py` → `ALL PASSED`.

**1. Bluetooth + screen alone.** Close the HappyLighting (or KS) app on your
phone: each strip accepts one connection. Flash the audio firmware in debug mode:

```
cd firmware/audio
idf.py set-target esp32s3
idf.py menuconfig    # KS light bridge -> enable "Debug: skip USB..."
idf.py build flash monitor
```

Expect `found 'KS03~....' -> profile KS03~`, the strip cycling colors, and
every 4th step a fake "demo!" agent asking for you. Lumi goes amber and
bounces. Press **BOOT** to ack: she gets a heart.

**1b. Lamps.** A fresh dongle claims the nearest KS lamp by itself (strongest
signal over a 4 s window) and remembers it. For a second strip, hold that strip
near the dongle and **hold BOOT ~3 s** until the screen says *let go to add a
lamp*, then release. Lumi asks "hold a lamp close by…", then cheers *lamp 2!*.
Up to 4 lamps. **Holding 10 s** forgets every lamp and devbox mapping. After
that the dongle only ever reconnects to its claimed lamps, so housemates' and
neighbours' strips are safe.

**2. Pick a transport** and flash `firmware/card` or `firmware/audio` normally.
After that the USB console is gone (TinyUSB owns the port), so to reflash,
**hold BOOT while plugging in**.

**3. Laptop.**
- *Card:* Windows loads its built-in CCID driver. It may toast that it
  couldn't install a driver for the *card*, which is fine. In your RDP connection
  settings, enable smart card redirection.
- *Audio:* the dongle is a speaker and microphone. Route the RDP client's output to it in
  Settings → System → Sound → Volume mixer, rather than making it the system
  default. Enable microphone recording redirection too, using the dongle's
  microphone on the laptop. Untested whether fan-out works across simultaneous
  RDP sessions. Turn off enhancements and microphone noise suppression.

**4. Each devbox.** For the native app, follow [desktop setup](desktop/README.md).
For the legacy CLI instead, copy `host/ks.py` (stdlib only), then:

```
python ks.py status                 # card: shows the lamp and every agent
python ks.py install-hooks          # wire Claude Code to the light (idempotent)
python ks.py signal needs-you       # try it
python ks.py ack
```

Set `KS_SOURCE=name` to choose how a devbox appears on the screen (default:
hostname, 8 chars). For audio, set `KS_CHANNEL=0..3` so each devbox gets
its own channel. The default is a hash of the name, which can collide.

## Lamps × devboxes

Each strip is its own Bluetooth device, so they're addressed separately. Pixels
*within* a strip aren't: KS controllers only do whole-strip color and effects.

- **One strip per devbox, automatically.** The first devbox to signal gets lamp 1,
  the next gets lamp 2, and so on. The mapping is sticky across reboots. If it
  guessed backwards, set `KS_LAMP=2` on a devbox to pin it. A devbox beyond the
  number of lamps shows on every lamp, so nothing is missed.
- **Your colors** go to every lamp unless you say otherwise:
  `ks.py rgb 00c8b4 --lamp 2`, `ks.py off --lamp 1`. Each lamp remembers its
  own last color for `on`.
- **Acks:** tapping BOOT acks everything. `ks.py ack --lamp 2` acks just what's on
  lamp 2, including any unmapped devbox showing there.
- `ks.py status` (card mode) lists each lamp, whether it's connected, and which
  devbox is on which lamp.

| BOOT button | does |
| --- | --- |
| tap | acknowledge all alerts |
| hold 3 s | claim another lamp (strongest nearby) |
| hold 10 s | forget all lamps and devbox mappings |

## Agents → light

`install-hooks` adds async hooks to `~/.claude/settings.json`:

| Claude Code event | signal | strip | screen |
| --- | --- | --- | --- |
| `Notification` (`permission_prompt`, `idle_prompt`) | needs-you | pulsing amber (yellow breathing on KS03~) | full-screen amber `!` flash, then pulsing amber Lumi |
| `UserPromptSubmit` | working | unchanged | full-screen blue activity flash, blue bottom block |
| `Stop` | done | green, clears after 2 min | full-screen green `✓` flash, green Lumi + bottom block |
| `StopFailure` | error | red until acked | full-screen red `✕` flash, then pulsing red frowning Lumi |
| `SessionEnd` | idle | block removed | brief cyan clear flash |

The most urgent unacknowledged signal wins. It overlays whatever color you set
by hand, and the strip returns to your color when everything is acked or clear.
**BOOT button** or `ks.py ack` acknowledges all. A Claude asking *again* re-alerts;
re-sending the same error doesn't. Up to 8 devboxes are tracked; quiet ones
are evicted first.

## Lumi

**Orientation:** the default is portrait, 80 × 160, with the dongle standing and the
USB plug pointing away from you, so the top of the screen is the plug end. If
Lumi comes up upside down on your unit, turn on *Rotate the screen 180 degrees*
(`CONFIG_KSA_LCD_FLIP`). Landscape is still available (`CONFIG_KSA_LCD_LANDSCAPE`).

**Full-bleed screen:** because the 0.96″ panel is only 10.9 × 21.8 mm and often
viewed off-axis, there is no tiny text. Every update (signal, color change, ack,
or claim) flashes the entire screen in that event's color with a giant symbol
(`!`, `✓`, `✕`, `♥`, `✦`) and lamp number badge (`1`–`4`). Otherwise, Lumi fills
the entire screen edge-to-edge in whatever color the strip is showing, with
chunky lamp bars along the top edge and active-agent color blocks along the bottom.

**Emulator:** `tools/lumi-sim/build.sh` compiles the real model and screen
renderer (`firmware/components/kscore`) to WebAssembly with zig
(`pip install ziglang`) and writes `tools/lumi-sim/lumi-sim.html`, a playground
with both devboxes, a BOOT button you can tap or hold, viewing-angle simulation,
and the strips.

## Readback (Triones strips)

Triones controllers answer `EF 01 77` with their actual state:
`66 model power pattern mode speed R G B W ver`. Your `light.py` confirmed
the power and RGB fields on real hardware, using notifications on `FFD4`. The
dongle subscribes to them on connect (from the candidates `FFD0`/`FFD4`/`FFE0`/`FFE4`,
whichever has a CCCD) and uses them three ways:

- **Verify every write.** About 0.4 s after a write it asks the strip what it's showing.
  A static color must match the exact bytes sent. A mismatch gets one resend, and
  then the result is reported honestly. In card mode, `ks.py` prints
  `delivered: confirmed by the lamp`, `delivered: sent (lamp can't confirm)`, or
  exits 1 with `lamp shows something else`.
- **Notice hand-made changes.** Every 5 s, while idle, it polls each strip. If the IR
  remote or the phone app changed it, the dongle **adopts** that as the strip's
  new color instead of fighting you, and Lumi bounces. If an alert was showing on
  that strip, **touching the light counts as an ack**. Power changes and remote
  effects are adopted at once. A new static color is adopted only once two polls
  agree, so a running effect isn't mistaken for a stream of colors.
- **`ks.py status`** shows `connected, confirmed` per lamp when the last check matched.

Effects can't be verified as strictly, because RGB changes while they run. A matching
pattern byte counts as confirmed; anything else counts as "sent". KS lamps have no
usable readback (per upstream), so everything works as before for them.

## Triones notes

- Brightness: Triones has no brightness byte, so the RGB is scaled instead.
  Built-in effects always run at full brightness.
- Effects map onto Triones built-in modes: `*-breathing` → that color's
  "gradual change" (`0x26`–`0x2C`), and `seven-color-fade`/`rgb-fade` → `0x25`.
  Speed 0–100 maps to delay 31–1, where lower delay is faster.
- `white` is drawn as warm RGB, because RGB-only strips have no white channel and
  the Triones white command would just turn them off.
- The needs-you pulse uses yellow gradual change (`BB 29 0A 44`).
- `Dream…` controllers use a different effect set (per led-ble). They get color
  and power only, and effects fall back to a static color.

## Wire formats

**Command body**: `[0x5<<4 | op] [args]`, shared by both transports.

| op | args |
| --- | --- |
| 1 power | on (0/1), [lamp mask] |
| 2 rgb | r, g, b, brightness 0–100, [lamp mask] |
| 3 effect | id 0x82–0x8A, speed 0–100, brightness 1–100, [lamp mask] |
| 4 white | level 0–255, [lamp mask] |
| 5 signal | source id (crc8 of name), lamp<<4 \| state (lamp 0 = auto), name (0–8 ASCII) |
| 6 ack | [lamp mask] |

Lamp mask: bit *i* = lamp *i*+1; absent or 0 = every lamp.

**Audio**: body + CRC-8 (poly 0x07), one tone pair per nibble (50 ms tone,
30 ms gap), and a frame ends after ≥150 ms of no tones. Tones sit on an 85 Hz
grid from 600 Hz. Channel *c* owns slots *c, c+4, …, c+28*, so its four row
and four column tones are 340 Hz apart and interleave with the other
channels. Each command is sent twice with a random 300–900 ms gap.

**Card**: AID `F0 4B 53 4C 49 47 48 54` ("·KSLIGHT"). `80 10 00 00 Lc body`
→ 2-byte generation + `9000`. `80 20 00 00 00` → status record (layout in
`ksc_card.h`). `ks.py` wraps SELECT + command in one PC/SC transaction so
other devboxes' sessions can't interleave, then polls status until the
reconcile task has applied that generation.

**Lamp**: packets from [H4ch1Net/ks-led-controller](https://github.com/H4ch1Net/ks-led-controller)
(MIT). Upstream's timing is kept: power-on before color, a 300 ms settle after
connect, and 100 ms between writes. The firmware reconciles a model rather than
replaying commands, so a dropped link heals itself by re-applying the current state.

## Layout

```
host/ks.py                        devbox CLI + hook installer (stdlib only)
firmware/components/kscore/       portable C: audio rx, frames, KS packets,
                                  signal logic, CCID + applet, screen renderer
firmware/components/ksbridge/     ESP glue: BLE central, reconcile task, LCD/LED/button
firmware/components/st7735/       LILYGO's ST7735 driver (MIT, vendored)
firmware/audio/  firmware/card/   the two apps
test/                             run_tests.py, multichannel.py, ui_preview.py, C harnesses
```
