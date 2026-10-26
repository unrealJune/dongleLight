---
name: dongleLight
description: Native Windows Settings grammar for a shared audio light bus.
rounded:
  card: "8px"
spacing:
  row: "8px"
  section: "12px"
  card-inset: "20px"
  page: "24px"
components:
  settings-card:
    rounded: "{rounded.card}"
    padding: "{spacing.card-inset}"
---

# Design System: dongleLight

## Overview

**Creative North Star: "A native instrument for the shared audio light bus"**

The shipped interface follows Windows 11 Settings grammar: a quiet navigation pane, Mica window material, system typography, theme-aware cards, and real WinUI controls. Its character is clean, restrained, and operational. Shared lights, Event rules, Audio channels, Integrations, and Settings occupy one consistent shell rather than separate visual worlds.

The implementation is code-first. The user-pinned native direction supersedes degraded seed `96720d49`; there are no roll boards, mock compositions, generated artwork, or shipping raster assets. Native color-picker flyouts are the characteristic interaction, not an illustrated identity.

**Key Characteristics:**
- Mica behind theme-aware native controls.
- System text styles instead of a custom display face or numeric type ramp.
- Consistent padded cards and a vertically scrolling settings layout.
- Shared-state evidence kept distinct from local diagnostics.
- Explicit connection, ownership, pending, and stale states.

This is a source-derived record, not finish approval. Primary evidence is `desktop/DongleLight/MainWindow.xaml`, `MainWindow.xaml.cs`, `App.xaml`, and `App.xaml.cs`; product and deployment context comes from `PRODUCT.md`, `.impeccable/config.json`, and `desktop/README.md`. The handoff reports Computer Use launches of both debug and published RELEASE builds, with accessibility-tree confirmation of the empty state and Audio channels. The collapsed pane footer no longer wraps because it is hidden when the pane closes. Missing published PRI/XBF resources were addressed with an explicit post-Publish copy, and startup of the actual published executable was subsequently verified.

No screenshots were saved, no external comp exists, and earlier material inspection exists only inline. The finish reviewer returned **recapture**, requiring on-disk native screenshots. No finish approval, screenshot-based visual comparison, or material/contrast certification is claimed here. Reported validation includes the full regression suite, 17 hook-configuration checks, and a firmware build; physical hardware, RDP, live installed hooks, and reboot/startup validation remain unperformed. These are handoff evidence statements, not checks rerun during documentation.

Impeccable's context loader does not recognize the current `PRODUCT.md` platform wording and misclassifies the target as web. This record follows the implemented WinUI artifact; product/config metadata was left unchanged within the documentation-only scope.

## Colors

The palette is Windows-theme-driven chrome around actual lamp and event colors, not a fixed brand palette.

### Primary

Native emphasis comes from `AccentButtonStyle` and the title-bar icon's `AccentTextFillColorPrimaryBrush`. Accent appearance follows the resolved Windows/WinUI theme; there is no project-owned primary hex value.

### Neutral

Cards use `CardBackgroundFillColorDefaultBrush`. Page subtitles and the pane's bridge description use `TextFillColorSecondaryBrush`; ordinary text inherits its native style foreground. The title-bar grid is transparent over the window material. Settings supports Windows default, Light, and Dark through `Root.RequestedTheme`.

Lamp swatches and event-rule swatches represent RGB data received from the dongle. They are not semantic UI accent tokens. The manual picker's initial teal and initial brightness are input defaults, not house colors.

Color primitives are deliberately absent from frontmatter: XAML theme-resource identifiers are not CSS colors, and no resolved light/dark palette was captured. The sidecar preserves native resource bindings rather than inventing hex approximations or tonal ramps.

**The State Is Data Rule.** Lamp and event colors describe the shared scene or editable event values; connection, confirmation, and ownership remain explicit text, not color-only claims.

## Typography

**Display and body:** the system WinUI/Segoe family, resolved through `XamlControlsResources`. The source direction names Segoe UI Variable; the app does not pin a font file or override the resource styles' numeric metrics.

The hierarchy is a short settings ramp, not a marketing display scale:

| Role | Implemented resource | Usage |
| --- | --- | --- |
| Page title | `TitleTextBlockStyle` | Current page heading |
| Section title | `SubtitleTextBlockStyle` | Repeated settings sections |
| Strong body | `BodyStrongTextBlockStyle` | Lamp names, event names, empty-state lead, devbox identity |
| Body | `BodyTextBlockStyle` | Descriptions, instructions, shared-state rows |
| Caption | `CaptionTextBlockStyle` | Freshness, readback, diagnostics, hook paths, title-bar label |

The `Text` helper wraps text; the page subtitle also wraps. Native form headers, descriptions, and button labels retain their control typography. No independently measured font sizes, weights, line heights, or scale ratio are asserted.

**The System Type Rule.** Use the shipped WinUI text styles for hierarchy; do not replace the native ramp with a custom display treatment.

## Layout

The window initially requests `(1160 × 820)` through `AppWindow.Resize`; the presenter requests minimum dimensions `(760 × 600)`. These are window sizing values, not browser breakpoints. The title-bar row is `(48)` XAML units high, with a drag region registered through `SetTitleBar` and space reserved beside the app label for native window controls.

Navigation uses `PaneDisplayMode="Auto"` and an open pane length of `(224)` XAML units. Back navigation is hidden; the native Settings item remains visible. The machine/bridge footer is visible only while the pane is open. Compact and expanded behavior belongs to NavigationView; the app does not define custom breakpoint thresholds.

Page content lives in a ScrollViewer with horizontal scrolling disabled and padding `(36 left, 20 top, 36 right, 32 bottom)`. Its stretched stack has a maximum width of `(1000)` XAML units. The frontmatter's portable `px` spacing/radius entries denote these source XAML effective-pixel values, not physical display pixels.

The recurring rhythm is page spacing, section spacing, and tighter row spacing from the frontmatter. Cards share one inset. Horizontal action groups generally use section spacing. The build also has localized denser diagnostics and specialized form spacing; these are not additional global scale steps.

The implemented forms and action rows are primarily stacked and horizontal StackPanels. Automatic control reflow beyond native NavigationView behavior has not been established; do not infer a mobile layout or universal narrow-window fit.

## Elevation & Depth

`Window.SystemBackdrop` supplies `MicaBackdrop`. Theme-filled cards separate settings groups from that material without custom card shadows, gradients, or application-defined elevation effects. Flyouts and control interaction states retain the native WinUI treatment.

No project-owned shadow or motion tokens exist. Mica is a requested system material, not a fixed background color or a promise of identical rendering in every Windows session.

**The Native Depth Rule.** Keep window material, flyout elevation, focus, and control states under WinUI ownership; do not add an independent web-style shadow system.

## Shapes

The repeated container is a gently rounded, padded card using the frontmatter's card radius and inset. Its `Border` sets a fill but no custom stroke or shadow. Buttons, fields, toggles, pickers, and navigation retain native templates rather than inheriting the card radius.

Circular lamp-scene swatches communicate physical lights; small rounded rectangular rule swatches communicate editable colors. These are data-bearing shapes, not general-purpose icon or badge geometry.

## Components

### Buttons

Plain WinUI buttons carry ordinary actions. `AccentButtonStyle` distinguishes actions such as Acknowledge all, Save and connect, Save shared rules, and Install / update hooks. Default, hover, pressed, focus, and disabled appearance comes from WinUI; the project does not define custom state colors or button padding.

### Cards / Containers

The shared `Surface` helper wraps content in a Border with the theme card fill, the frontmatter card radius, and the common inset. It is reused for lamp rows, manual controls, connection settings, event rows, empty state, and startup settings. It does not add a custom outline or elevation.

### Inputs / Fields

TextBox, ComboBox, NumberBox, Slider, and ToggleSwitch are real native controls with visible headers or descriptions where implemented. Device selectors stretch horizontally. Native validation bounds include brightness `(0–100)` and the devbox-name length `(8 characters)`; save-time errors appear through the shared InfoBar. Number fields, choices, and pickers for another channel are disabled.

### Navigation

Shared lights is initially selected. Event rules, Audio channels, Integrations, and the built-in Settings entry retain native navigation affordances. Native `SymbolIcon`/`FontIcon` usage is platform chrome, not a custom pictogram font or a decorative glyph-based icon system. The footer's visibility follows `IsPaneOpen`, not text wrapping workarounds.

### Native color flyouts

Manual and event color buttons open native Flyout-hosted ColorPickers with alpha disabled. Event buttons pair a swatch with a hex label and an automation name identifying the event. Picker changes update the editable value; applying a manual color or saving shared rules remains an explicit action.

### Shared-state rows and status

Lamp rows combine a scene-color circle, lamp identity, textual connection/readback status, and an acknowledge action. Swatches have automation names exposing scene color. The no-snapshot empty state offers Set up audio; no active agents is a distinct state.

The persistent, nonclosable InfoBar uses native informational, warning, and error severities. Freshness text reports snapshot age; when a snapshot is older than `(15 seconds)`, the stale warning says the display is cached. `App.xaml.cs` calls freshness updates from a `(15-second)` timer, so the threshold is not an exact real-time visual deadline.

Add lamp sits directly under Lamps with preparation copy, a native button,
and a progress ring only while a request or shared search is pending. Its
wrapped caption distinguishes firmware support, live audio, channel ownership,
capacity, and the shared result. Pairing does not imply BLE connection or
verified color. Existing lamps remain visible throughout the search.
For this extension, accessibility-tree inspection confirmed Add lamp disabled
with the older firmware and enabled after the updated physical dongle returned
live version-2 state. Actual BLE pairing was not exercised; no screenshot-based
finish approval is implied.

**The Confirmation Rule.** A queued command or playback completion is not delivery evidence. Preserve pending rule-save language until matching dongle values return, and distinguish scene color from lamp readback.

**The Shared Versus Local Rule.** Lamps, agents, channel ownership, and rules derive from dongle snapshots; the This devbox activity area is explicitly local diagnostics.

### Event editing and background settings

Other devboxes' channels are inspectable, not editable. Saving shared rules additionally checks the selected channel against this devbox and requires a snapshot; transport admission remains responsible for live ownership. The app must not silently take a busy channel.

Preview event sends a real event, not a visual simulation. The working preview requires Clear agent to restore the underlying scene; urgent events can also be acknowledged. Idle is presented as Clear agent with explanatory text, not as an editable color row. Manual light is a base layer; another active alert may still take priority after acknowledgement or clearing.

Hooks install only through an explicit action. Sign-in startup is an explicit ToggleSwitch preference. Closing the window hides it while the bridge continues in the notification area; Exit stops the application. The tray uses the Windows system application icon and native tray menu, not a custom brand asset.

## Do's and Don'ts

### Do:
- **Do** retain native WinUI resources, controls, Mica, and Windows theme selection.
- **Do** reuse the card inset, card radius, and page/section/row rhythm recorded in frontmatter.
- **Do** keep shared snapshot state separate from local diagnostics and make stale or unconfirmed state explicit.
- **Do** preserve ownership gating, opt-in hooks/startup, and the real-event meaning of preview.
- **Do** label idle as clearing the agent rather than selecting a light color.

### Don't:
- **Don't** treat command submission or playback completion as proof of delivery or lamp application.
- **Don't** turn lamp RGB values or the manual picker's initial value into a brand palette.
- **Don't** replace native control states with invented browser styling, custom shadows, or display typography.
- **Don't** add invented artwork provenance, a mock-comp lineage, or finish approval to this code-first record.
- **Don't** infer screenshot comparison, hardware/RDP validation, live-hook execution, or reboot validation from software tests.

**Not canonized:** the isolated preview-button font-size override is not a second button type scale; the generic system tray icon is not a brand mark. Neither has evidence as a reusable identity decision. Native icon controls are retained as Windows grammar, not promoted into a custom glyph-icon convention.
