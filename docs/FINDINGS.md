# Findings

## Engine and install (2026-10-19)

- Game: `E:\SteamLibrary\steamapps\common\Still Wakes the Deep`, project name `Habitat`, exe
  `Habitat\Binaries\Win64\StillWakesTheDeep.exe` (DirectX 12, Agility SDK in `D3D12\`). The exe
  has no version string and no PDB. Its build path is `hab_dlc_mainline`.
- Engine: UE 5.3 or 5.4. The exe has `r.Substrate` and `r.Nanite.Tessellation` but no
  `r.MegaLights` (5.5).
- Upscalers: DLSS (Streamline, frame generation), FSR 3 and XeSS.
- User settings: `%LOCALAPPDATA%\Habitat\Saved\Config\Windows\GameUserSettings.ini`, section
  `[/Script/Habitat.HabitatLocalSettings]`. There's no user Engine.ini yet.

## HDR

- It's the stock UE path, with no custom pipeline like Until Dawn's `r.UCP.*`. The output
  devices are the stock list (3/4 = ACES 1000/2000 nit PQ, 5/6 = scRGB), and the exe has
  `r.HDR.Display.MidLuminance`, `.MaxLuminance`, `.MinLuminanceLog10`,
  `r.HDR.Aces.SceneColorMultiplier`, `r.HDR.Aces.GamutCompression`, `r.HDR.UI.Luminance` and
  `r.HDR.UI.CompositeMode`.
- Menu: "HDR Content Brightness" = `HDRDisplayOutputNits` (peak, 3700 here) and "HDR UI
  Brightness" = `HDRUIDisplayOutputNits` (250). Paper white / mid grey isn't exposed.
- ReShade 6.8 is installed in the game folder. ReShade.ini still has `[renodx-preset*]` sections
  from an earlier RenoDX add-on, but no RenoDX add-on is installed now.

## SWTDProbe (0.0.1)

This build captures the back buffer at Present (F9 or the overlay button) and writes the
`SWTDProbe\` reports next to the exe. See the header of `src/probe.cpp`.

## Post-process passes (frame trace, 2026-10-19)

The shipped DXIL keeps its reflection names, so `dxc -dumpbin` shows UE's parameter names.
Hashes are CRC-32C of the bytecode (SWTDProbe 0.0.2).

- **Tonemap PS `0x5F7EA512`** (VS `0x3B70ED46`, draw_indexed 3 into the 3840x2160
  R10G10B10A2 target). Bindings: `ColorTexture` t1, `BloomTexture` t2, `BloomDirtMaskTexture`
  t4, `ColorGradingLUT` t5 (Texture3D), `EyeAdaptationBuffer` t0, `SceneColorApplyParamaters`
  t3; `_RootShaderParameters` cb0 (828 bytes: `LUTScale` @808, `LUTOffset` @812,
  `EditorNITLevel` @816, `BackbufferQuantizationDithering` @820, `bOutputInHDR` @824), `View`
  cb1.
  - Scene colour (with bloom, exposure and ColorScale0) is **PQ-encoded as `color * 0.01`**,
    so scene linear 1.0 = 100 nits on the LUT's input axis. The coordinate is then
    `PQ * LUTScale + LUTOffset`, and the LUT is sampled.
  - The LUT output × 1.05 is written as it is: in HDR the LUT already holds the final
    PQ-encoded value. Tone curve, grading, ACES and the ODT all live in the **LUT-building
    pass**, so that pass is what to replace. `bOutputInHDR` is UE's editor HDR-preview path
    (PQ → nits / EditorNITLevel → sRGB) and is off in game.
  - The pass also writes luma (Rec.601 weights) to a second output, plus dithering.
- **UI composite PS `0xBBC72169`** (draw_indexed 3): `UITexture` + `SceneTexture`, `$Globals`.
- **LUT-building pass: CS `0x129A2FB0`**, UE `CombineLUTs` `MainCS`, numthreads (8,8,8),
  dispatched 4×4×4 (32³), SM 6.6. Bindings: `_RootShaderParameters` b0 (680 bytes, stock
  ACES/grading/film constants, `OutputDevice` @652), `WorkingColorSpace` b1, `RWOutputTexture`
  u0. For HDR devices it PQ-decodes the coordinate and stores the final value × 1/1.05. It only
  runs when grading changes (`r.LUT.UpdateEveryFrame` = 0). The user Engine.ini
  (`%LOCALAPPDATA%\Habitat\Saved\Config\Windows\Engine.ini`) now has
  `[SystemSettings] r.LUT.UpdateEveryFrame=1`, which is how the trace caught it. See
  `docs/HDR_PLAN.md`.
- The 64³ RGBA16F 3D targets in the trace (several PS) are the translucency lighting volume,
  not the LUT.

## First HDR vs SDR capture (2026-10-19, interior corridor, one fluorescent tube)

The swap chain is R10G10B10A2 in both modes; only the colour space changes (sRGB ↔ HDR10 PQ).
HDR is on the game's peak of 3700 nits. SDR is given in nits for a 100-nit white, decoded with
gamma 2.2.

| | HDR | SDR (2.2, 100-nit white) |
|---|---|---|
| min / 0.1% / 1% | 0.0091 / 0.013 / 0.023 | 0.0023 / 0.0047 / 0.013 |
| median / log-average | 0.83 / 0.78 | 1.09 / 0.83 |
| 99% / 99.9% / max | 149 / 384 / 999 | 59 / 83 / 97 |

- **Mid grey is low.** The HDR midtones match SDR on a display with roughly a 75-80-nit white.
  That fits the stock `r.HDR.Display.MidLuminance` = 15 (about 80-nit paper white). The menu
  can't change it.
- **Blacks are raised.** Scaled to the same mid grey, the HDR deepest 0.1% is about 3-4x
  brighter than SDR on a gamma 2.2 display. HDR never goes below 0.009 nits, and its histogram
  has nothing under 10^-2, where SDR reaches 10^-2.75. The SDR grade has a black floor in sRGB
  terms that gamma 2.2 crushes, but the PQ path reproduces it. This is the usual
  sRGB-vs-2.2 mismatch.
- **Highlights are hue-shifted.** Measured on a 1/6 sample of pixels, highlights above 10 nits
  have saturation 0.64 in HDR vs 0.49 in SDR, and are warmer (R/Y 1.41 vs 1.25, B/Y 0.50 vs
  0.64). The fluorescent tube goes yellow-orange in HDR where SDR's shoulder takes it towards
  white. This is ACES 1.x RRT behaviour. Midtones are slightly more saturated, and shadows are
  less saturated (they're greyed by the raised floor).
- **Peak isn't proven capped.** The brightest channel codes form a smooth tail to 1367 nits
  with no pile-up, so this scene simply has nothing brighter. Whether the curve is really
  built for 3700 still needs a brighter scene or a slider A/B test.
- **The settings file changed with HDR.** With HDR off, GameUserSettings.ini had
  `HDRDisplayOutputNits=400`, `HDRUIDisplayOutputNits=50`. Turning HDR off may reset the
  sliders. Check them after turning HDR back on.
