# Changelog

All notable changes to WarmerSun. The format follows Keep a Changelog and the
project uses semantic versioning once a licence is chosen.

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

- Default sharpening now matches the reference mod's recovered settings:
  `Sharpness` 0.20 (its 2/10) and the new `SharpenEdges` 0.40 (its 4/10).
- `SharpenEdges` exposed in all four presets and mapped to the CAS edge gain.
- `ANALYSIS.md` documents the Ghidra pass: 14,496 procedures in `dxgi.dll`, the
  shader-path classifier, the settings registry, and the neutral tonemap
  defaults.
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
