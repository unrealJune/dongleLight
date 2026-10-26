# dongleLight control plane

Native Windows 11 x64 application using WinUI 3, Mica, system theme resources,
native color pickers, and a persistent notification-area process. No web view,
cloud service, smart-card fallback, or network broker is involved.

## Build and install

From the repository root in PowerShell, with the .NET 10 SDK installed:

```powershell
.\desktop\publish.ps1
```

Copy the **entire** `desktop\.artifacts\app` folder to a permanent local
directory on each devbox. The published folder includes .NET and Windows App
SDK runtimes, but Python is a separate prerequisite:

```powershell
python -m pip install -r .\host\requirements.txt
.\DongleLight.exe
```

Run those commands from the copied application folder. Python 3.10+ is required.
If using a virtual environment, select its `Scripts\python.exe` under
**Settings > Python executable**, then reconnect. `reedsolo` is required for
return-channel recovery; device enumeration itself needs only Python's standard
library. Only x64 publishing is supported by the supplied script.

If diagnostics report `ModuleNotFoundError: No module named 'reedsolo'`, install
the requirements using the exact Python executable selected in Settings.
Installing into a different virtual environment does not repair the running
worker. An absolute executable path avoids interpreter-selection ambiguity.

Keep the application at that permanent path: installed hooks and the optional
sign-in startup entry reference its executable. Publish to a separate folder
or exit the running app before overwriting an installation.

## Firmware and audio routing

Flash the updated `firmware\audio` project using ESP-IDF v5.4.2 and target
`esp32s3`. The current host requires duplex snapshot protocol v3; older duplex
firmware remains readable but does not provide compact acknowledgements.
An older speaker-only image cannot supply the required return stream.
The device exposes a 48 kHz, 16-bit speaker and mono microphone. The microphone
contains encoded shared state, not ambient sound.

```text
each devbox app -- RDP playback --> laptop -- USB speaker --> dongle --> BLE lamps
each devbox app <-- RDP recording -- laptop <-- USB microphone <-- shared snapshot
```

On the laptop, route each RDP client's playback to **KS Light Bridge** and
select the dongle's microphone for remote recording. In each RDP connection's
remote audio settings select **Play on this computer** and **Record from this
computer**. Disable enhancements, noise suppression, echo cancellation, and
automatic gain control where available: this is a data link, not speech.
Do not loop the return microphone into the dongle speaker.

Every participating RDP session must receive the same microphone stream.
Whether the laptop/client permits simultaneous microphone fan-out must be
tested in the actual environment. If it cannot, an approved shared capture or
audio-routing arrangement is required; the app does not implement an audio
router or bypass device policies.

In **Audio channels** on each devbox:

1. Set a unique printable-ASCII name, at most eight characters.
2. Pick a different channel, **0 through 3**, for every devbox.
3. Select the redirected playback and recording endpoints, then connect.
4. Wait for **Duplex audio live** and verify that the shared ownership list
   shows all participating devboxes.

Saved endpoints are resolved by name on every connection, not blindly reused
by index. Missing or ambiguous named endpoints require reselection. The Windows
default-device option deliberately follows Windows' current default.

The dongle reserves channels for 60 seconds; active workers renew their leases.
An occupied channel is refused rather than silently stolen. This coordinates
collisions; it is not authentication or protection against malicious audio.
Four channels support two agent providers each, not an unlimited number of
independent agent sessions per devbox.

## Shared behavior

**Shared lights** shows the dongle's current scenes, BLE connection/readback,
agent status, and acknowledgement state. Manual colors are the base layer:
alerts temporarily override them. Acknowledging restores the base when no
higher-priority active signal remains. The physical BOOT button still works.

### Add a lamp

1. Power on the lamp's controller near the dongle and close its phone app.
2. Wait for **Duplex audio live**, then select **Shared lights > Add lamp**.
3. Wait for the shared pairing result. The dongle searches for four seconds;
   confirmation can take up to 40 more seconds over the audio return stream.

The strongest nearby unpaired supported lamp is selected, with a maximum of
four lamps. Existing lamps and their mappings are preserved. Search progress,
success, and no-lamp-found results are shared with every listening devbox.
**Paired** means the dongle saved the lamp; the lamp row separately reports
BLE connection and color readback. A timeout is not proof that pairing failed:
check the shared lamp list and return audio before trying again.

In-app pairing requires the updated firmware's bus snapshot version 2 or newer.
The app still supports older duplex firmware but disables Add lamp and
explains that an update is needed. Update the app/host on **every devbox**
when installing the new firmware: older hosts cannot decode version 2.
Holding BOOT for three seconds remains an alternative. Do not hold it for
ten seconds: that clears all lamp memory and mappings.

If a QHM controller is visible in Windows but Add lamp reports no lamp found,
use the current firmware: it accepts the zero-padded Bluetooth advertisements
used by these controllers. Pair with the dongle through this app, not through
Windows' Bluetooth pairing menu.

### Rules and shared state

**Event rules** configures RGB, brightness, and target lamp independently for
Copilot CLI and Claude Code on each channel. The owning devbox can edit its
channel; other devboxes can inspect it. Idle clears that agent instead of
applying a color. Preview sends a **real physical event**; a working preview
must be cleared, while urgent previews can also be acknowledged.

Rules and lamp mappings persist on the dongle. Activity, current colors and
acknowledgements are broadcast to every listener. Local theme, Python path,
audio endpoint selection, startup preference and diagnostics are not shared.
The worker waits for matching snapshot values before reporting rules saved.
Playback completion alone does not prove delivery or BLE application.

The duplex stream sends compact shared-state snapshots between full rule
snapshots. Ordinary commands are confirmed by a newer dongle generation and
retried once if no acknowledgement arrives; pairing is never retried.
Uplink commands still take seconds, and multi-rule updates take longer.
Initial channel admission waits up to 40 seconds. Stale state is marked in
the UI; commands fail closed without a fresh return stream.

## Agent hooks and background operation

After connecting, use **Integrations > Install / update hooks** for the
providers you use. Restart agent sessions afterward. The installer preserves
unrelated handlers/settings, replaces its own handlers idempotently, and
keeps the first existing configuration backup beside the original file.
Remove any old `ks.py` light hooks manually to avoid duplicate signals.

| Event meaning | Copilot CLI | Claude Code | Light state |
| --- | --- | --- | --- |
| Prompt submitted | `userPromptSubmitted` | `UserPromptSubmit` | working |
| Agent turn completed | `agentStop` | `Stop` | done |
| Agent error | `errorOccurred` | `StopFailure` | error |
| Permission needed | `permissionRequest` | `Notification` permission/idle matcher | needs-you |
| Additional input needed | `notification` elicitation matcher | permission/idle notifications where emitted | needs-you |
| Session ended | `sessionEnd` | `SessionEnd` | idle |

Copilot configuration is written to `%USERPROFILE%\.copilot\hooks\donglelight.json`,
or `%COPILOT_HOME%\hooks\donglelight.json` when set. Claude configuration is
merged into `%USERPROFILE%\.claude\settings.json`. Hook schemas were checked
against the official GitHub Copilot hooks reference and Claude Code hooks
reference; actual installed-agent execution is still a bring-up check.

Hooks relay only provider and event state through a current-user/session
named pipe. They do not read or transmit prompts, transcripts or tool input,
and failures never block the coding session. A hook starts the app in the
background if necessary. Audio setup must already have been completed.

Closing the window keeps the bridge alive in the notification area.
**Settings > Start at Windows sign-in** is opt-in and uses the current user's
Run entry. This is a desktop-session app, not a pre-login Windows service:
it starts after sign-in, and RDP disconnect policies can affect audio.
The worker retries failed/stale connections automatically. Use **Exit** in
Settings or the tray menu to stop it.

Settings and rolling diagnostics live in `%LOCALAPPDATA%\DongleLight`.
Uninstall by removing this application's hook entries, disabling sign-in
startup, exiting the app, and removing its installation folder. Keep or remove
its local configuration separately as desired.

## Validation and remaining bring-up

```powershell
.\.venv\Scripts\python.exe .\test\run_tests.py
.\.venv\Scripts\python.exe .\test\run_tests.py --duplex-only
Set-Location .\desktop
dotnet run --project .\DongleLight.Tests\DongleLight.Tests.csproj
```

The host suite covers the existing uplink codecs/model/card behavior and the
new C/Python snapshot contract, Reed-Solomon parity and recovery, malformed
frames, gain/noise, clock drift, Opus 16/32 kbps, AAC, channel refusal,
shared rules and acknowledgements. Native hook configuration has 17 passing
checks. The WinUI x64 release and ESP32-S3 audio firmware build successfully.

The duplex-only suite builds a persistent virtual dongle and connects real
Python workers through an in-memory audio cable. It exercises:

```text
Worker.execute -> frame builder -> uplink PCM -> C tone decoder -> frame/CRC parser
 -> C state model -> snapshot -> RS/CRC32 -> return PCM -> Python streaming Receiver
 -> each devbox's shared state -> remote acknowledgement -> restored base color
```

It covers four listeners, simultaneous four-channel sends, custom shared rules,
selective acknowledgements, channel conflicts/lease expiry, malformed uplink
CRC, missing return audio, recovery, and complete round trips with Opus 16/32
kbps or AAC 48 kbps in **both** directions. It also checks that a maximum-size
repeated command applies once. Low-confidence return bytes are passed to
Reed-Solomon recovery rather than truncating the frame.

Pairing tests cover adding first and subsequent lamps, preserving existing
lamps, shared results, no lamp found, busy/full states, old firmware, and
rejecting an earlier success as confirmation of a new request. Pairing is
sent once, never automatically retried.

Only device/OS I/O is replaced. The harness mirrors the firmware's admission
and duplicate-filter wrapper; the decoders, frame parser, model, snapshot
encoder and return modulator are compiled from production C. Transactions run
faster than real time; virtual time advances with uplink samples or an explicit
test step. It does not simulate continuous USB scheduling, RTOS contention,
NVS storage, BLE transmission, or the actual RDP client/codec configuration.
Virtual lamps deliberately never report successful hardware readback.

Before relying on this operationally, verify the physical dongle, BLE lamps,
real installed hooks, simultaneous RDP playback/microphone fan-out, reconnect
after RDP loss, and startup after sign-in/reboot. No firmware was flashed,
live hooks installed, or startup enabled by the automated software tests.
