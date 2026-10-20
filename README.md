# KS audio bridge

Control a KS Bluetooth LED strip from a locked-down cloud devbox, using the one
channel RDP never blocks: **audio**.

```
devbox                 RDP audio            your laptop               Bluetooth
ks_tx.py rgb ff8800 ──► (opus/whatever) ──► USB "speaker" = ESP32-S3 ──► KS strip
   DTMF tones                                decodes tones -> KS packets
```

The dongle enumerates as a USB speaker named **KS Light Bridge**. The devbox
"plays" DTMF tones, and the dongle decodes them, then writes the matching KS
packet to the lamp over BLE.

## What's verified vs not

| Part | Status |
| --- | --- |
| Decoder + framing (portable C) | **Tested on host**: clean, Opus 32k/16k/12k-voip, AAC 64k, MP3 48k, 0.7% clock error, at 0/-20/-45 dB; chime mid-frame; 60 s of junk audio with 300 random DTMF bursts → zero false commands. Margins: works to Opus 6 kbps, -55 dB, 6 dB SNR. |
| KS packet encoders | **Tested** against upstream `golden_packets.json` |
| Transmitter (`ks_tx.py`) | **Tested** (round-trips through the C decoder) |
| Firmware (USB audio + BLE) | **Builds clean** (zero warnings in our code) on ESP-IDF v5.4.2 with usb_device_uac 1.3.2, in both normal and debug mode; ~540 KB image. **Not run on hardware yet.** |
| T-Dongle-S3 LED pins (39/40) | Guess from memory. Check your board's pinout and change in menuconfig. |
| Profiles other than `KS03~` | Inherited from upstream, unverified there too |

## Layout

```
host/ks_tx.py                     devbox transmitter (stdlib only)
firmware/                         ESP-IDF project for ESP32-S3
  components/ksaudio/             portable core: DTMF rx, framing, KS packets
  main/                           USB audio, NimBLE central, status LED
test/run_tests.py                 host test suite (gcc, ffmpeg, numpy)
```

## Bring-up, in order

**0. Host tests** (any Linux/WSL box): `python3 test/run_tests.py` → `ALL PASSED`.

**1. Bluetooth half alone (debug mode).** Close the KS app on your phone, since
the lamp takes one connection only.

```
cd firmware
idf.py set-target esp32s3
idf.py menuconfig        # KS audio bridge -> enable "Debug: skip USB audio"
idf.py build flash monitor
```

You should see `found 'KS03~....' -> profile KS03~`, `ready: ...`, then the
strip cycling red/green/blue/orange every 4 s. Note the name prefix it prints.
If you have several lamps, set the name prefix or BLE address in menuconfig.

**2. Real mode.** Turn debug off, build, and flash. Once TinyUSB owns the port,
the USB console is gone, so to reflash later **hold BOOT while plugging in**.

**3. Laptop side.** Plug in the dongle. Windows sees a speaker called *KS Light
Bridge*. Rather than making it the system default (you'd lose your own audio),
route only the RDP client to it: Settings → System → Sound → Volume mixer →
set the Remote Desktop / Windows App process's output to *KS Light Bridge*. I
believe per-app routing applies to the RDP client, but haven't tested it; if
not, make it the default output while you test. In the connection's audio
settings, make sure remote audio plays **on this computer**. Turn off audio
enhancements / spatial sound for the device.

**4. Devbox.**

```
python ks_tx.py rgb ff8800
python ks_tx.py rgb 00ffcc --brightness 40
python ks_tx.py on / off
python ks_tx.py effect purple-breathing --speed 40     # KS03~ only
python ks_tx.py white 200                               # KS03~ only
python ks_tx.py rgb ff0000 --wav red.wav               # write instead of play
```

Each command takes about 2 s of audio (a 250 ms lead-in, the frame twice, and a tail).
Turn off devbox system sounds so dings don't land mid-frame. A corrupted copy
is rejected by CRC, and the second copy usually gets through.

## Status LED

| Color | Meaning |
| --- | --- |
| dim white | booting |
| blue | scanning for a lamp |
| green | connected, ready |
| white flash | valid command heard |
| magenta flash | frame-shaped audio failed CRC |
| red flash | no lamp / write failed / op unsupported by this lamp |

## Wire format

One DTMF tone pair = one hex nibble (`0-9`, `A-D`, `*`=E, `#`=F), 50 ms tone and
30 ms gap. A frame is `[0x5<<4 | op] [args] [crc8]`, high nibble first, and ends
after ≥150 ms with no tone. CRC-8 uses poly 0x07, init 0.

| op | args |
| --- | --- |
| 1 power | on (0/1) |
| 2 rgb | r, g, b, brightness 0–100 |
| 3 effect | id 0x82–0x8A, speed 0–100, brightness 1–100 |
| 4 white | level 0–255 |

The firmware maps ops to KS packets per the lamp's advertised prefix. It sends
power-on first for color/effect/white, waits 300 ms after connecting, and spaces
writes 100 ms apart (all as upstream does). On a failed write it reconnects and
replays once. The standard-format lamps (`KS03-`, `KS01-`...) have no brightness
byte, so RGB is scaled instead.

Protocol details come from
[H4ch1Net/ks-led-controller](https://github.com/H4ch1Net/ks-led-controller) (MIT).
