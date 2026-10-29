# WarmerSun

A DRM-free lighting grade for **Euro Truck Simulator 2** and **American Truck Simulator**.

WarmerSun is a two-DLL D3D11 hook: it proxies `dxgi.dll` and `dinput8.dll` from
the game's `bin\win_x64` folder, captures each presented frame, and applies a
warm, filmic grade with adaptive exposure, highlight recovery and CAS
sharpening. No account, no license key, no network access, no telemetry.

```
  frame ──► copy ──► log-luminance ──► adaptive exposure ──► grade ──► present
                        64×64 → 1×1        1×1 ping-pong      CAS + tonemap
```

## Install

1. Close the game.
2. Copy into `...\Euro Truck Simulator 2\bin\win_x64\` (or ATS):
   - `dxgi.dll`
   - `dinput8.dll`
   - `WarmerSun.ini`
3. Start the game.

Hotkeys:

| Key | Action |
|-----|--------|
| `F10` | toggle the grade on/off (A/B) |
| `F11` | cycle preset A → B → C → D |
| `F12` | reload `WarmerSun.ini` |

Remove the three files to uninstall. Create an empty `WarmerSun.off` next to the
DLLs to load the proxies in pass-through mode.

## Presets

| Preset | Character |
|--------|-----------|
| A | Natural — subtle correction |
| B | Cinematic — deeper contrast, stronger roll-off |
| C | Night — lifts dark scenes without washing out headlights |
| D | **WarmerSun** (default) — golden highlights, neutral shadows |

Every control from the reference mod's panel is present in the ini:
`Temperature`, `Tint`, `PreExposure`, `Exposure`, `PreContrast`, `Contrast`,
`Saturation`, `Vibrance`, `Highlights`, `Shadows`, `Whites`, `Blacks`,
`NightExposure`, plus `SunWarmth`, dynamic exposure and sharpening.

## Build

Requires MinGW-w64 (x86_64) and a Windows SDK-ish environment; no MSVC needed.

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

Outputs `dist\bin\win_x64\{dxgi.dll,dinput8.dll,WarmerSun.ini,INSTALL.txt}`.
The DLLs are statically linked and import only `KERNEL32`/`USER32` plus the
Universal CRT that ships with Windows 10+.

## Verification

`tests\` contains a real end-to-end harness: it creates a D3D11 swap chain in a
folder containing the proxy, renders a known pattern, presents 180 frames and
writes both the pre-present and graded back buffers.

```powershell
powershell -ExecutionPolicy Bypass -File tests\build-tests.ps1
python tests\check_frames.py tests\out\harness_before.dds tests\out\WarmerSun_frame_00000.dds
```

Measured on the reference pattern (preset D):

| metric | before | after |
|--------|--------|-------|
| warmth (R/B) | 1.277 | **1.413** |
| contrast (σ luma) | 0.287 | **0.315** |
| mean luma | 0.477 | 0.492 |
| highlight clipping | 4.48% | **4.08%** |

## Scope and fidelity

This is a clean-room reimplementation of the *behaviour* of a commercial ETS2
lighting mod, built from static analysis of its shipped DLLs (see
[ANALYSIS.md](ANALYSIS.md)). It is **not** a byte-for-byte clone and contains no
code or assets from it.

- The color pipeline, control set, presets, hook architecture and two-DLL
  bootstrap match the reference design.
- The reference mod replaces the game's HDR `def.tonemap` / `def.adapt` shaders
  and downloads a separate shader pack at runtime. Those shaders are not in the
  shipped DLLs, so WarmerSun currently grades the **presented LDR frame**
  instead of the HDR buffer. That is why exposure, highlight roll-off and
  sharpening are approximations rather than identical pixel output.
- A shader-level mode (hooking `def.tonemap` / `def.adapt` and the road/rain
  material path) is the next phase and needs the game installed to capture the
  real shader bytecode and constant-buffer layouts.

## License

No license has been chosen yet. Until one is added, all rights are reserved by
the repository owner.
