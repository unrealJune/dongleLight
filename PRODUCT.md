# dongleLight

<!-- impeccable:product-schema 1 -->

## Platform

Native Windows desktop.

## Stack

WinUI 3 / Windows App SDK, with the existing stdlib Python transport library.
Firmware may change to support the control plane.

## Users

Developers running GitHub Copilot CLI and Claude Code on Windows devboxes.

## Product Purpose

Make agent activity and requests for attention visible on physical lamps and
the dongle display across the RDP machine boundary.

## Operating Context

The same native application runs on each Windows devbox. Audio playback
redirection carries commands to the laptop-connected dongle. A new USB
microphone interface broadcasts shared state through RDP microphone
redirection. Microphone redirection is available in the user's environment.
No network listener or cloud service is required.

## Capabilities and Constraints

- Persistent notification-area application with optional sign-in startup.
- Per-agent and per-event colors, brightness, lamp assignment, preview, and
  acknowledge controls.
- Configurable hooks for GitHub Copilot CLI and Claude Code.
- Synchronize agent state, lamp colors, acknowledgement, lamp assignments,
  channel ownership, and color rules between connected devboxes.
- Explicit playback/capture device selection and four-channel diagnostics.
- Audio-only control plane; no smart-card dependency.
- Preserve firmware priority, acknowledgement, expiry, and manual light layers.
- Hardware and RDP validation must be reported separately from host tests.

## Brand Commitments

Modern native WinUI 3, maximum emphasis on clean Mica materials and native
Windows controls. Build code-first, not from generated image mockups.

## Evidence on Hand

`host\ks.py` implements the redirected transports. `firmware\components\kscore`
contains the shared model and renderer. The README records hardware limitations.
