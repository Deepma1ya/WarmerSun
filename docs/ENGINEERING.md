# Engineering notes

How WarmerSun is put together, why the pieces are where they are, and what is
planned next. This document describes WarmerSun's own design; it does not
reproduce any third-party code or assets.

## Module layout

```
dxgi.dll        DXGI proxy, swap-chain hook, renderer, embedded HLSL
dinput8.dll     DirectInput 8 proxy, early bootstrap, panic switch
WarmerSun.ini   settings, four complete preset sections
```

Both DLLs are statically linked and import only `KERNEL32`/`USER32` plus the
Universal CRT. There is no network code, no licence check and no telemetry.

## Hooking

`dxgi.dll` exports `CreateDXGIFactory`, `CreateDXGIFactory1`,
`CreateDXGIFactory2`, `DXGIDeclareAdapterRemovalSupport` and
`DXGIGetDebugInterface1`. Each forwards to the real `%SystemRoot%\System32\dxgi.dll`
loaded by full path, so the proxy never resolves to itself.

Two layers bring the swap chain into reach:

1. **Factory vtable.** After a real factory is created, vtable slots for
   `CreateSwapChain` (10) and `CreateSwapChainForHwnd` / `CoreWindow` /
   `Composition` (15–17) are patched. Only slots whose target lives inside the
   real dxgi module are touched.
2. **IAT fallback.** `D3D11CreateDeviceAndSwapChain` in the host executable is
   repointed at a detour. This covers the case where d3d11 creates the factory
   internally before any exported entry point is called.

The returned swap chain then gets its own vtable slots patched: `Present` (8),
`ResizeBuffers` (11), `Present1` (20) and `ResizeBuffers1` (37). `Present1`
clears the game's dirty rectangles after grading, because the whole back buffer
changed.

## Hooking the engine

The game resolves D3D11 dynamically, so the proxy hooks `GetProcAddress` in the
host executable and redirects `D3D11CreateDevice` to a detour. That detour
installs inline hooks (MinHook) on `CreatePixelShader` and the draw methods of
the immediate context. The runtime replaces vtable entries during startup, so
inline function hooks are used instead of vtable patching.

## Frame pipeline

```
back buffer ──CopyResource──► scene texture
                                  │
                    64×64 log-luminance
                                  │
                          16×16 → 1×1 average
                                  │
                    ┌──── ping-pong exposure (1×1 RG32F) ────┐
                    │  EV = lerp(prevEV, targetEV, 1-e^-kt)  │
                    └────────────────────────────────────────┘
                                  │
             fullscreen pass: CAS → exposure → white balance →
             pre-contrast → tonal zones → vibrance/saturation →
             contrast → warm highlights → soft shoulder → sRGB
```

- The back buffer is render-target only, so it cannot be sampled. It is copied
  with `CopyResource` into a same-format texture that has `BIND_SHADER_RESOURCE`.
- Luminance is reduced in two passes (64×64, then 1×1) and stored as log2, so
  exposure changes are perceptually linear.
- Adaptation state is a 1×1 `RG32F` ping-pong: `.x` is EV, `.y` is average
  linear luminance. The final pass derives a night factor from `.y`
  (`1 - smoothstep(0.015, 0.12, avgLum)`), so no CPU readback is needed.
- All grading happens in linear light; the final encode is skipped when the
  swap chain uses an `_SRGB` format, because the hardware encodes on write.
- Dither (1/255, hash-based) is added just before output to keep 8-bit gradients
  clean.

## Colour model

| Stage | Implementation |
|---|---|
| White balance | RGB gains from `Temperature` (−100..100) and `Tint`, normalised to preserve luminance |
| Pre-contrast | power curve around 0.18 |
| Highlights / shadows | luminance-masked multiplicative lift or recovery |
| Whites / blacks | quartic end-point offsets |
| Vibrance | saturation boost weighted by inverse chroma |
| Saturation | lerp from luminance |
| Contrast | power curve around 0.18 |
| Sun warmth | warm tint applied with `smoothstep(0.55, 1.0, luma)` |
| Shoulder | only compresses values above 1.0, so it cannot crush the midtones |

## Sharpening

Contrast-adaptive sharpening (CAS) runs on five linear taps before the grade:

```
amp    = sqrt(saturate(min(mn, 1-mx) / mx))
peak   = -1 / lerp(8, 5, sharpness) * lerp(0.70, 1.30, edges)
result = (c + (n+s+e+w) * amp * peak) / (1 + 4 * amp * peak)
```

`Sharpness` controls the peak strength, `SharpenEdges` biases how much the
edge mask is allowed to contribute. Defaults are 0.20 and 0.40.

## State safety

Before drawing, the renderer saves render targets, depth-stencil view, blend
and depth states, rasterizer state, viewports, scissor rects, input layout,
topology, the five shader stages, PS samplers, PS/VX constant buffers and all
PS shader resources. Everything is restored afterwards, so the game's next
frame starts from the state it left.

Failure is always a pass-through: a missing `d3dcompiler_47.dll`, a shader
compile error or an unsupported back-buffer format disables the grade and logs
the reason instead of presenting a broken frame.

## Configuration

`WarmerSun.ini` holds four complete preset sections. The loader starts from the
built-in defaults for the selected preset, then applies any keys present in the
file, so a partial section is valid. `F12` reloads the file while playing.

Hotkeys are polled with `GetAsyncKeyState` inside `Present`; no extra thread or
keyboard hook is installed.

## Roadmap

1. **HDR shader pass.** Replace the engine tonemap shader with the same pipeline
   before tonemapping. That recovers highlight headroom the LDR path cannot see.
2. **Material lighting.** Extend the hook to the road and vegetation shader
   families for wet-road normals and light-response improvements.
3. **Per-preset curves.** Optional LUT export/import so presets can be authored
   in an image editor and applied 1:1.
4. **Overlay.** A small in-game panel for live tuning, replacing the ini-only
   workflow.

