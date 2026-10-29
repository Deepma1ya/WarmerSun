# Changelog

All notable changes to WarmerSun. The format follows Keep a Changelog and the
project uses semantic versioning once a licence is chosen.

## v0.2.0

- Added the End-key in-game panel (Dear ImGui) with presets, colour, tone,
  adaptation and detail controls, plus save-to-ini.
- The panel uses a virtual cursor driven by raw mouse input, so the game's
  cursor lock cannot block or offset it.
- Moved the grade to the scene/UI boundary: D3D11 device/context hooks
  (MinHook) detect the first UI shader draw and grade before it. Opaque UI
  pixels are now bit-identical with the grade on or off.
- Vendored Dear ImGui 1.91.8 (MIT) and MinHook 1.3.3 (BSD-2-Clause).
- Added UI-shader and tonemap-shader hash tables generated from the game's
  effective shader set for pass identification.
- Harness re-verified (PASS).

## v0.1.3

Presentation release; the renderer is unchanged from v0.1.1/v0.1.2.

- WarmerSun is documented as a standalone mod with its own architecture and
  roadmap in `docs/ENGINEERING.md`.
- README no longer frames the project against any other product; it describes
  the download, presets, pipeline and roadmap directly.
- Regenerated the preset grid with a neutral caption.

## v0.1.2

Repository and packaging release; the DLLs are unchanged from v0.1.1.

- Published a stable download asset named `WarmerSun.zip` so
  `/releases/latest/download/WarmerSun.zip` always resolves.
- Added a hero banner, preset grid and before/after image generated from the
  real harness output.
- Rewrote the README around install, hotkeys, presets, configuration and
  troubleshooting.
- Added a GitHub Actions workflow that builds both DLLs and packages the zip on
  every push, and attaches it to tag releases.
- Added `scripts/package.ps1`, this changelog, and repository topics.

## v0.1.1

- Default sharpening set to 0.20 with the new `SharpenEdges` control at 0.40.
- `SharpenEdges` exposed in all four presets and mapped to the CAS edge gain.
- Added the first engineering notes for the renderer pipeline.
- Rebuilt and re-verified with the D3D11 harness (PASS).

## v0.1.0

First public release.

- `dxgi.dll`: DXGI proxy, factory/swap-chain vtable hooks, IAT fallback for
  `D3D11CreateDeviceAndSwapChain`, log-luminance chain, adaptive exposure,
  white balance, contrast, saturation, vibrance, tonal zones, night lift, warm
  highlights and CAS sharpening.
- `dinput8.dll`: full DirectInput 8 proxy, early bootstrap, `WarmerSun.off`
  panic switch.
- Four presets, live ini reload and hotkeys.
- End-to-end harness with before/after frame comparison.
- No DRM, no account, no network, no telemetry.

