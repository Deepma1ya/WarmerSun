# Analysis: what the reference mod is, and what WarmerSun reproduces

This document records the reverse-engineering work behind WarmerSun. It is
descriptive only: no code, shaders or assets from the reference product are
included in this repository, and no licence mechanism was bypassed.

## Targets

| File | SHA-256 | Size | Format |
|------|---------|------|--------|
| `dxgi.dll` | `d819c16805a9a0ca17f34ef46cc445c5847f32308d10e69bf08f3c1b4779f4dc` | 18,304,568 | PE32+ x86-64 DLL |
| `dinput8.dll` | `cf13e776228fb62930482c769138237b79e45c803936312c9172259d7dbed5bd` | 730,448 | PE32+ x86-64 DLL |

Method: static PE inspection (imports, exports, resources, sections, entropy),
string carving, and extraction of embedded HLSL text. Both files were treated as
untrusted data; neither was executed during analysis.

## dxgi.dll — the renderer and mod core

**Exports**

- `CreateDXGIFactory`, `CreateDXGIFactory1`, `CreateDXGIFactory2` — DXGI proxy
  entry points, the standard way a D3D11 mod takes ownership of presentation.
- `NVSDK_NGX_D3D11_*` / `NVSDK_NGX_Parameter_*` — a DLSS API surface, i.e. the
  module also acts as an `nvngx` provider for DLSS/DLAA.

**Imports of note**

- `D3DCOMPILER_47.dll` (2 functions) — shaders are compiled at runtime.
- `gdiplus`, `COMCTL32`, `OLEACC`, `UxTheme`, `IMM32`, `wxWidgets` strings — a
  desktop UI stack, consistent with an ImGui/wx settings panel.
- `WINHTTP` usage strings — the mod downloads assets and performs licence
  checks over HTTPS.

**Resources / manifest**

The only resource is a manifest requesting `requireAdministrator`. There is no
version-info resource, so the product identity is not in the PE version block.

**Sections**

`.text` ≈ 4.1 MB, `.rdata` ≈ 6.2 MB, `.data` ≈ 2.2 MB raw with a 0.59 printable
ratio, plus a ≈ 5 MB `.vlizer` section. The printable `.data` region is where
the embedded shader source lives; `.vlizer` is a protected/obfuscated section
(no readable strings, high entropy) and was not analysed further.

**String evidence**

- Product/update: `snowymoon.io`, `cdn.snowymoondl.net/lighting_v2/...`
- Licence flow: `Invalid license!`, `Incorrect or expired license key.`,
  `Computer changed!`, `Change Locked Computer`, `Assets not found!`,
  `Put "snowymoon_lighting" folder to "bin\win_x64"`.
- Shader file names: `/effect/deferred/def_tonemap/def.tonemap.col.rfx1`,
  `def.tonemap.drr.bdist.rfx1`, `/effect/deferred/def_adapt/def.adapt.rfx1`,
  `/effect/deferred/def_compose_lighting/def.compose.lighting...rfx1`,
  `def_fog`, `def_apply_fake_shadow`, many `eut2.*` material variants.
- Settings labels: `Temp`, `Pre-Contrast`, `Pre-Exposure`, `Saturation`,
  `Exposure`, `Vibrance`, `Contrast`, `Highlights`, `Shadows`, `Whites`,
  `Blacks`, `Night Exposure`, `Interior Intensity`, `SSAO Intensity`,
  `Albedo Saturation`, `Normals Intensity`, `Leaves/Grass Thickness`,
  `Sharpness`, `Sharpen Edges`, TAA/DLSS/motion-blur options.

**Embedded HLSL**

A ~23 KB contiguous HLSL block was extracted from `.data`. It is a rewritten
`eut2` material shader with:

- rain wetness driven by gloss and a rain-info constant, with two scrolling
  ripple normal layers blended in tangent space;
- luminance-derived normal maps for road surfaces (Sobel-style gradient plus a
  local AO term), with distance-based normal scaling;
- vegetation thickness parameters and per-material variants selected by
  `SHADERTYPE`;
- writes into multiple render targets (normal, gloss/wetness, TAA data).

This is direct evidence that the reference mod patches material shading, not
only post-processing.

**What is not in the DLLs**

The tonemap/adaptation shader pack is downloaded separately and is not present
on disk. `.vlizer` holds protected code. Exact preset constants could not be
recovered statically.

## Ghidra pass (Ghidra 12.1.4)

A full import of `dxgi.dll` yields **14,496 procedures and 20,291 strings**
across the normal sections plus `.vlizer` (5,089,792 bytes) and a small `tdb`
segment. The three proxy exports resolve to `0x180014820`, `0x180014860` and
`0x1800148a0`.

Two functions are directly relevant to fidelity:

- `FUN_1800a3290` is a **shader classifier**. It compares the engine's shader
  path against known passes and assigns a numeric id, for example
  `/effect/deferred/def_tonemap/def.tonemap.col.rfx1` → 101,
  `def.tonemap` → 100, `.../def.tonemap.drr.bdist.rfx1` → 200, and
  `/effect/generic/ui/ui.rfx1` → 102. This confirms the mod keys its work on
  exact engine shader names rather than on a generic post-process.
- `FUN_18007a560` is the **settings registry**. It registers every panel
  parameter with a default, and for integer sliders a min/max. Reading the
  default cells out of `.rdata`/`.data` shows that the tonemap parameters ship
  **neutral**: `Exposure = 0`, `Pre_Exposure = 0`, `Pre_Contrast = 0`,
  `Contrast = 1`, `Saturation = 1`, `Vibrance = 0`, `Highlights = 0`,
  `NightExp = 0`, `Dynamic_Contrast = 1`.

Recovered non-neutral defaults:

| Setting | Default | Range |
|---|---|---|
| `sharpness` | 2 | 0..10 |
| `sharpenedges` | 4 | 0..10 |
| `motionblurintensity` | 5 | 0..10 |
| `colorpreset` | 0 | 0..1 |
| `Tonemap_Tonemap` | 0 | 0..6 |
| `Tonemap_TonemapA` | 7 | 0..8 |

The important consequence: the reference's *look* is not produced by these
parameters. It comes from the selected tonemap curve and colour preset, and
from the separately downloaded shader pack. No tonemap HLSL, ACES curve or
`linear_to_srgb` helper exists in the binary — a string search for those terms
returns nothing.

WarmerSun therefore adopts the two evidence-backed defaults (sharpening 2/10
and edges 4/10) and leaves the colour grade to its own presets, which is the
only honest mapping while the HDR shader replacement is not implemented.

`dinput8.dll` imports cleanly as a normal CRT build (3,147 procedures).
`DirectInput8Create` lazily resolves the system DLL, forwards the call, and
post-processes the returned interface; no protected payload is present there.

## dinput8.dll — the bootstrap stub

Exports the complete DirectInput 8 proxy surface (`DirectInput8Create`,
`DllRegisterServer`, `DllUnregisterServer`, `DllCanUnloadNow`,
`DllGetClassObject`, `GetdfDIJoystick`). Imports only `KERNEL32.dll` (110
functions), including thread enumeration, `Get/SetThreadContext`,
`SuspendThread`/`ResumeThread`, `VirtualAlloc`, `VirtualProtect` and
`FlushInstructionCache`. It contains essentially no readable strings.

Interpretation (inference, not proof): a compact loader that forwards DirectInput
and prepares the process for the renderer. WarmerSun keeps the same shape — a
forwarding `dinput8.dll` that bootstraps the `dxgi.dll` renderer — but implements
it with a plain `LoadLibrary` of the sibling proxy, no thread patching and no
protected payload.

## Reference design vs WarmerSun

| Reference | WarmerSun |
|-----------|-----------|
| `dxgi.dll` proxy + present hook | same (vtable patch of `Present`/`Present1`/`ResizeBuffers*`, factory hook, IAT fallback for `D3D11CreateDeviceAndSwapChain`) |
| `dinput8.dll` bootstrap | same (full DirectInput proxy + early load of `dxgi.dll`) |
| Settings panel and presets | `WarmerSun.ini`, presets A–D, hotkeys |
| Licence server, HWID lock, asset download | none — no network code, no account, no DRM |
| HDR `def.tonemap` / `def.adapt` replacement | present-level grade (v1); HDR hook is the next phase |
| Road/rain/vegetation shader patch | not implemented in v1 |

## Fidelity limits

The reference runs inside the game's deferred pipeline, before tonemapping, and
replaces several shaders. WarmerSun v1 runs after the game has already
tonemapped and encoded the frame. It can match the *direction* and control set
(warmth, contrast, vibrance, highlight recovery, night lift, sharpening) and was
tuned against a synthetic reference pattern, but it cannot recover HDR headroom
that the game has already clipped.

Reaching closer parity requires the game installed, then:

1. capture the actual `def.tonemap` / `def.adapt` bytecode via a
   `CreatePixelShader` hook;
2. replace those shaders with WarmerSun's HDR pipeline;
3. hook the `eut2` material path for the road/rain normal work.

That is planned as phase 2 and is the reason the renderer is isolated behind a
small `Renderer` interface.
