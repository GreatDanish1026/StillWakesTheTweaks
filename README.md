# StillWakesTweaks

A ReShade add-on for **Still Wakes the Deep** (Unreal Engine 5.4, DirectX 12). It replaces
the game's HDR output with one built from its SDR image, and puts graphics settings beyond
the game's menu in the ReShade overlay. Graphics changes apply at once, with no restart. See
[docs/FINDINGS.md](docs/FINDINGS.md) for the measurements behind each choice.

**Version 1.0.0.**

## What it fixes

The game's stock ACES HDR output, compared with its own SDR image:

- **Mid grey is too dark.** The midtones match SDR on a display with roughly a 75-80-nit white.
  The fix puts SDR white at a chosen paper white (203 nits by default). In a test corridor,
  mid grey went from 0.68 to 1.80 nits, matching SDR's 1.82.
- **Blacks are raised.** HDR reproduces the SDR grade's sRGB black floor, which looks lifted
  next to SDR on a gamma 2.2 display. The fix shows shadows as they look in SDR on gamma 2.2.
- **Highlights shift hue.** ACES pushes fluorescent tubes and lamps toward yellow-orange. The
  fix extends highlights by luminance and keeps SDR's hue, with an adjustable amount of the
  original colour.

It still uses the full peak. In bright-scene tests a welding arc reached 3,382 nits on a
3,700-nit display, and the gas flare 2,499 nits, with no clipping and with their colour kept.

## Install

1. Install [ReShade](https://reshade.me) **6.8 or later with add-on support** for
   `Habitat\Binaries\Win64\StillWakesTheDeep.exe`, choosing DirectX 12.
2. Copy `StillWakesTweaks.addon64` into that same folder.
3. Start the game and turn on **HDR** in its display settings. Set **HDR Content Brightness**
   to your display's peak brightness.
4. Restart the game once. On first run the add-on sets up the recommended settings:
   Epic-plus lighting, 4 px fog, 16x filtering and a 4 GB texture pool. They apply at once,
   and are written to the user `Engine.ini` (below), which the engine only reads at start-up.
   An existing `Engine.ini` is left alone.

Open the ReShade overlay (Home): the settings are in its **StillWakesTweaks** tab.

Don't install `SWTDProbe.addon64` alongside it. That's the research build: it replaces the
same shader and slows the game's start-up.

## The overlay

A header shows the version and whether the HDR fix and live settings are working. A `*` marks
any option changed from the game's value, and every option has a `(?)` with details.

**HDR**: **F8** toggles the fix in game.

| Setting | Default | What it does |
|---|---|---|
| Paper white | 203 nits | Brightness of SDR white, and so of everything that isn't a highlight |
| Peak | the game's HDR brightness | The brightest highlight |
| Black floor | 1 | 1 = gamma 2.2 shadows (deep blacks), 0 = the stock sRGB shadows |
| Highlight colour | 0.75 | How much colour bright lights keep: 0 rolls them toward white as in SDR |
| Highlight start | 0.5 | SDR level where highlights start to extend toward the peak |

**Graphics**:

- **Lighting quality**: Game menu / **Epic-plus** (recommended) / Cinematic. The game's Epic
  setting halves Lumen reflections and keeps several GI settings below UE's own Epic.
  Epic-plus gives true UE 5.4 Epic GI, shadows and fog, plus full-resolution reflections:
  visibly more bounce light in shadows, for about 16% of frame rate. Cinematic is UE's top
  tier, at about 60%. Details are in [config/README.md](config/README.md).
- **Lighting response**: how many frames Lumen's GI averages. The game uses 30, UE 10. Fewer
  frames react faster to swinging lamps and opening doors, with a little more noise.
- **Anisotropic filtering**: Game (8x) / 16x.
- **Texture pool**: Game (2 GB on Epic) / 4 GB / 6 GB.
- **Volumetric fog detail**: 8 px cells (game) / 6 px / 4 px (Cinematic). Crisper light shafts
  and halos; in testing, 4 px cost no frame rate you could feel.
- **Bloom**: Game (half resolution) / Full resolution / Off.
- **Lens flares** and **Vignette**: Game / Off.

**Performance**: a stutter monitor. It shows frame time, counts hitches and whether shader
compiles caused them. Each hitch is also logged.

**Diagnostics**: the state of the shader swap and of the connection to the game's settings.

## How it works

- **HDR.** The colour-grading LUT builder (UE's `CombineLUTs`, identified by a CRC-32C of its
  bytecode) is swapped for [src/shaders/lutbuilder.hlsl](src/shaders/lutbuilder.hlsl), a port
  of UE 5.4's own with the new HDR path. Its settings are passed as 8 root constants, which the
  add-on appends only to root signatures shaped like the LUT builder's.
- **The user Engine.ini** (`%LOCALAPPDATA%\Habitat\Saved\Config\Windows\Engine.ini`) holds
  `r.LUT.UpdateEveryFrame=1`, so HDR slider changes show at once, and `r.LUT.Size=64`, for
  near-black precision. It also holds the chosen preset and extras. The game deletes this file
  on exit, so the add-on keeps it read-only. A file of your own is only replaced when you pick
  a preset, and is backed up first as `Engine.ini.before-StillWakesTweaks`.
- **Live settings.** The add-on finds UE's console manager in the game's code (there's no
  PDB), using UE 5.4's vtable layout, and sets console variables from ReShade's thread. Some
  values are cached by the engine and only refreshed by its change callbacks, so it then asks
  the game thread to run them. Everything is fault-guarded: if the layout doesn't match, the
  settings apply after a restart instead.
- It deliberately doesn't listen for ReShade's descriptor events. With those, start-up showed
  20 s or more of black screen.

## Not possible or not worth it

- **Hardware ray tracing / hardware Lumen.** The game was cooked without ray-tracing shaders,
  so enabling it crashes at start-up.
- Tried and removed because they made no visible difference in this game:
  - sharper virtual shadow maps
  - distant detail (Nanite, LOD and view distance)
  - glossy reflections on rougher surfaces
  - traced glass and water reflections
  - tonemapper sharpening
- **Full-resolution skin.** It's already at full resolution on Epic effects quality.
- **Stutter.** The game ships no PSO cache, so a shader seen for the first time compiles
  mid-game (single compiles of up to 0.9 s in testing). The driver keeps them, so they don't
  repeat. The other long freezes measured came from ReShade saving 16-bit screenshots.

## Building

Requirements:

- Visual Studio 2022 or later with the C++ x64 tools. MSVC is required, because ReShade's API
  structs are returned differently by MinGW.
- The Windows SDK, for `dxc.exe`.
- **Unreal Engine 5.4** installed locally. The LUT shader includes UE's `ACES.ush`,
  `GammaCorrectionCommon.ush` and `TonemapCommon.ush`. They're copied from the engine at build
  time and not kept in this repo, since they're Epic's code. Once a build has copied them to
  `bin\ue_shaders`, later builds use that copy if the engine is no longer installed.

```powershell
.\build.ps1                     # builds bin\StillWakesTweaks.addon64 and bin\SWTDProbe.addon64
.\build.ps1 -Install            # also installs StillWakesTweaks into the game folder
.\build.ps1 -Install -Probe     # installs the research probe instead
.\build_shaders.ps1 -UE D:\UE_5.4   # if the engine isn't at E:\unreal\UE_5.4
```

`build.ps1` takes `-GameBin <folder>` if the game isn't installed at the default Steam path,
and refuses to install while the game is running.

## Repository layout

| Path | Contents |
|---|---|
| `src/hdr.cpp` | The add-on: shader swap, overlay, presets, live settings, stutter monitor |
| `src/cvars.hpp` | Finding and setting UE 5.4 console variables without symbols |
| `src/shaders/lutbuilder.hlsl` | The replacement LUT builder |
| `src/probe.cpp` | SWTDProbe, the research add-on (F9 frame captures, F10 frame trace) |
| `config/` | The Engine.ini lighting presets, embedded at build time |
| `docs/` | Findings, measurements and the HDR plan |
| `third_party/` | ReShade 6.8 add-on headers and Dear ImGui headers, with their licences |

## Logs

`StillWakesTweaks.log` is written next to the game's exe. It records the shader swap, every
settings change, each hitch and a summary of frame times every minute.
