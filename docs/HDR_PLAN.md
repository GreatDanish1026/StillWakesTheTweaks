# HDR replacement plan: the LUT builder

## What we're replacing

| | |
|---|---|
| Pass | UE `CombineLUTs` compute: `MainCS`, `[numthreads(8,8,8)]`, dispatched 4×4×4 → a 32³ LUT |
| Shader | CS `0x129A2FB0` (CRC-32C of the DXIL), SM 6.6, ~3,000 lines of DXIL |
| Bindings (space 0) | `_RootShaderParameters` b0 (680 bytes), `WorkingColorSpace` b1 (324 bytes), `RWOutputTexture` u0 (RWTexture3D) |
| Input axis | Thread id → LUT coordinate. For `OutputDevice > 2` the coordinate is PQ, decoded to linear with 1.0 = 100 nits. That matches the tonemap PS (`0x5F7EA512`), which PQ-encodes `sceneColor × 0.01`. SDR devices use UE's LogToLin. |
| Output | The final display value × (1/1.05). The tonemap PS multiplies by 1.05 and writes it straight to the back buffer, so in HDR the LUT holds **final PQ code values**. |
| Runs | Only when grading changes (`r.LUT.UpdateEveryFrame` = 0). Our Engine.ini sets it to 1 so slider changes show at once. The cost is one 32³ dispatch per frame. |

The constant buffer is stock UE 5.3/5.4: `ACESMinMaxData`/`ACESMidData`/`ACESCoefs*` (the
parametric ACES curve built from the game's peak and `r.HDR.Display.MidLuminance`),
`ACESSceneColorMultiplier`, `ACESGamutCompression`, the full `Color{Saturation,Contrast,Gamma,
Gain,Offset}{,Shadows,Midtones,Highlights}` grade, `BlueCorrection`, `ExpandGamut`,
`ToneCurveAmount`, `Film{Slope,Toe,Shoulder,BlackClip,WhiteClip}`, `MappingPolynomial`,
`InverseGamma`, `OutputDevice`, `OutputGamut`. No artist LUT textures are bound in this
permutation.

Replacing this one shader moves every pixel of the game's HDR output. The tonemap and UI
passes stay as they are.

## What the new shader does

Approach: **SDR look, HDR range.** The game was graded on SDR. The stock HDR path replaces
the SDR tone curve with ACES RRT/ODT, and that's where all three faults come from. The
new shader builds the SDR image exactly as the game does, then extends it.

1. **Decode the coordinate** exactly as now: PQ → linear, 1.0 = 100 nits.
2. **Grade** exactly as now: `ColorCorrectAll`, BlueCorrection, ExpandGamut, all in AP1. We
   then have `graded`, the untonemapped graded colour.
3. **SDR reference:** `sdr = FilmToneMap(graded)` with the game's Film* constants and
   ToneCurveAmount, in BT.709. This is the image the SDR captures showed.
4. **Black floor (fix 1):** convert `sdr` to the light a gamma 2.2 display emits,
   `pow(sRGB_encode(sdr), 2.2)`. This is what SDR players see and what the capture measured
   (0.0047 vs HDR's 0.013 nits at 0.1%). An overlay slider runs from 0 (sRGB curve, the
   current HDR behaviour) to 1 (full 2.2), default 1.
5. **Mid grey (fix 2):** `nits = sdr22 × PaperWhite`, with PaperWhite from a slider (default
   203, the BT.2408 reference). Stock is about 80, with mid grey near 15 nits. That's why the
   HDR median measured 0.83 against 1.09 for SDR.
6. **Highlights to the peak, keeping SDR hue (fix 3):**
   - Build an HDR luminance curve from `graded` that matches the SDR curve's slope through
     mid grey, then rolls off smoothly (an exponential or BT.2390-style knee) to
     `Peak / PaperWhite`. Peak comes from a slider whose default reads the game's menu value
     out of `ACESMinMaxData`.
   - Blend from SDR to that curve with a smooth weight above roughly 0.5 × SDR white, so the
     shadows and midtones stay exactly the SDR result.
   - Apply it **as a luminance ratio to the SDR colour**: `hdr = sdr22 × (Y_hdr / Y_sdr)`.
     Hue and chroma then come from SDR, so ACES's yellow-orange shift in highlights goes
     away. The capture had highlight R/Y 1.41 vs 1.25 and saturation 0.64 vs 0.49.
   - A "highlight saturation" slider (0-1) can bring some of the extra chroma back for those
     who like it, by blending the ratio result toward a per-channel extension.
   - Per-channel max is clamped to the peak so nothing clips to a hue-shifted colour.
7. **Output:** BT.709 → BT.2020, nits / 10000 → PQ, × (1/1.05), then store. SDR
   (`OutputDevice ≤ 2`) runs the original code path unchanged.

The 32³ LUT spans PQ 0..1. At 3700-nit peak, 32 samples along the PQ axis are adequate
(UE ships this), so no LUT size change is needed. `r.LUT.Size` exists if banding shows.

## How it's injected (ReShade add-on, D3D12)

1. **Swap the bytecode.** In `create_pipeline`, when the compute subobject's CRC-32C is
   `0x129A2FB0`, replace it with our compiled CSO (SM 6.6, signed by dxc's dxil.dll so D3D12
   accepts it). Bindings must stay b0/b1/u0 in space 0 with the same cbuffer layouts, so
   the game's own root signature still fits.
2. **Pass our settings.** The game's root signature has no room for new constants. In
   `create_pipeline_layout`, for the layout this PSO uses, add one root-constants parameter
   (`b13, space50`, 8 DWORDs: peak, paper white, black-floor strength, highlight saturation,
   enable). In `bind_pipeline` for the replaced PSO, `push_constants` them. This is
   RenoDX's proven pattern. If changing that layout causes trouble, there's a fallback:
   compile the values into the shader and recompile at runtime with a bundled
   dxcompiler.dll. That's slower to change, but needs no root signature edits.
3. **Keep the LUT live.** Ship `r.LUT.UpdateEveryFrame=1` in the README's Engine.ini lines.
   Without it, slider changes only show after a graphics setting change.
4. **Safety:** if the game updates and the hash isn't seen within 30 s of the first HDR
   present, show "LUT shader not found (game updated?)" in the overlay and leave the game
   alone.

## Where the shader source comes from

We need HLSL equivalent to UE 5.3/5.4 `PostProcessCombineLUTs.usf` `MainCS`. There are two
routes:
- **Preferred:** port from UE's own source (`PostProcessCombineLUTs.usf`, `TonemapCommon.ush`,
  `ACES.ush`, `GammaCorrectionCommon.ush`) at the matching version. That needs an Epic-linked
  GitHub account. Check your account, or install UE 5.4 from the launcher; the shaders ship
  with the binary engine.
- **Alternative:** hand-decompile the 3,000-line DXIL. It's slower and riskier on the grading
  maths. RenoDX's MIT-licensed UE "lutbuilder" decompilations are a reference for structure.

To check it before changing anything, compile our shader with the HDR changes **off** (step 7
emitting the stock ACES result) and compare the LUT it writes with the game's own, through the
probe's back-buffer captures. It should be bit-close. Then turn the fixes on.

## Testing with the probe

Use the same corridor and the HDR/SDR capture pair. Targets:
- HDR median ≈ SDR(2.2) median × PaperWhite/100. At 203 that's about 2.2 nits, vs 0.83 now.
- HDR 0.1% black ≈ SDR(2.2) 0.1% × PaperWhite/100, about 0.0095. The floor goes from 0.009
  to near 0.002.
- Highlights > 10 nits: saturation and R/Y within ~5% of SDR (`chroma.py`).
- A bright scene (exterior night with flares/lightning) should reach the set peak, with
  `pct_top_code` 0 and no pile-up below the peak.

## Status (2026-10-19, SWTDProbe 0.0.2)

- `src/shaders/lutbuilder.hlsl` is built by `build_shaders.ps1` against the UE 5.4 install
  (`E:\unreal\UE_5.4`). The game's reflection confirmed 5.4 (`OutputMaxLuminance` @660). The
  cbuffer offsets of every field the game's shader uses match ours exactly. There are two
  variants, without and with white balance (`SKIP_TEMPERATURE`).
- Injection is in the add-on:
  - A b13/space50 8-DWORD root-constants parameter goes on every root signature with room.
  - The LUT CS is recognised by its reflected names (not its hash) and its bytecode swapped
    in `create_pipeline`.
  - Constants are pushed in the `dispatch` event when the bound compute PSO is ours.
  - The overlay's "HDR fix" section has the sliders, saved to ReShade.ini. F8 toggles the
    fix in game.
- `ACESMinMaxData.w` is the ACES curve's max luminance in nits (the game's peak). It's the
  default peak.
- First in-game runs:
  - Name matching failed. UE's DXIL keeps reflection names as LLVM bitcode, not plain text.
    The LUT CS is now matched by CRC-32C (`0x129A2FB0`), and the swap works ("LUT shader
    0x129A2FB0 replaced").
  - With the fix on, HDR highlights take SDR's hue: saturation 0.50 / R/Y 1.23 / B/Y 0.63,
    against SDR's 0.49 / 1.25 / 0.64 and stock's 0.64 / 1.41 / 0.50. Median 2.5 nits
    (stock 0.83).
  - F8 did nothing: the game **deletes the user Engine.ini on exit**, so
    `r.LUT.UpdateEveryFrame` was gone and the LUT was built only once. The file is now
    recreated read-only. The add-on logs LUT builds every 10 s.
  - The read-only Engine.ini and the hash-based pipeline tracking work: the LUT rebuilds
    every rendered frame (55-78 a second) and F8 switches live. The magic-value check makes
    missed constants fall back to stock.
  - **Same-session A/B (2026-10-19 15:45, corridor, peak 3700, paper white 203).** SDR is
    gamma 2.2 × 2.03, i.e. at a 203-nit white:

    | | Fix on | Stock ACES | SDR target |
    |---|---|---|---|
    | median nits | 1.80 | 0.68 | 1.82 |
    | 0.1% / 1% black | 0.047 / 0.062 | 0.032 / 0.039 | 0.040 / 0.050 |
    | black ÷ median | 0.026 | 0.047 | 0.022 |
    | highlights sat / R/Y / B/Y | 0.48 / 1.20 / 0.66 | 0.63 / 1.40 / 0.51 | 0.49 / 1.26 / 0.64 |
    | max nits | 1248 | 981 | |

    Mid grey matches SDR. Shadow contrast is close to SDR's (black relative to median 0.026
    vs 0.022; stock 0.047). Highlights take SDR's hue. The remaining ~20% black lift may be
    trilinear LUT interpolation near black (try `r.LUT.Size=64`).
  - Every launch with the add-on shows ~20 s of black before the first frame; a stock
    launch reached the menu in ~9 s. Suspects: start-up cost of the probe's per-pipeline
    hashing/bytecode copies, or our root-signature change defeating the PSO cache. Still
    open.
  - **Cause of the slow start:** the probe. With the lean `StillWakesTweaks.addon64` (only
    the LUT swap, root-signature extension, bound-compute tracking and dispatch push), the
    black start is ~2 s, and the first present comes 3.9 s after load. The probe's per-pipeline
    hashing and bytecode copies of every shader, plus its per-bind and per-draw tracking, were
    the cost. The root-signature change isn't. Install one or the other (`build.ps1 -Install`
    or `-Install -Probe`); never both.
  - Correction (2026-10-20): the slow start isn't the probe's hashing or the root-signature
    change. A lean build with `ExtendRootSignatures=0` was still slow, and no add-on at all
    was fast. The prime suspect is the `init_pipeline`/`destroy_pipeline` events: with those
    registered, ReShade wraps the game's `ID3D12PipelineLibrary`. The add-on was redesigned
    to not use them:
    - Only root signatures shaped like the LUT builder's are extended. The LUT's root
      signature, from the log: `table{uav u0-15} | table{cbv b0} | table{cbv b1} |
      table{6 samplers, space 1000}`, one of ~221.
    - The compute root signature in use is tracked from `bind_descriptor_tables` /
      `push_descriptors`.
    - Constants are pushed on every dispatch that uses an extended one.
  - **Resolved (2026-10-20):** an event-group bisect through ReShade.ini `Events` gave:
    - 0 (present only): fast
    - 1 (root signatures): fast
    - 3 (+ pipeline creation): fast
    - 7 (+ descriptor binds): slow
    - 27 (root signatures + creation + `init_pipeline` + pipeline binds + dispatch): fast

    So the slow start came from the **descriptor-bind events** (`bind_descriptor_tables` /
    `push_descriptors`). The probe also listened for draws and render-target binds, which
    resolve views the same way. The `init_pipeline` theory was wrong.
  - The shipping design (`Events=27`, the default): only the one LUT-shaped root signature is
    extended. Our LUT PSO is recognised in `init_pipeline`, compute pipeline binds are tracked
    per command list, and the constants are pushed on dispatches of our PSO. Start-up is back
    to a few seconds, and F8 switches live.
  - ReShade reports UE's CBV and sampler tables as `push_descriptors_with_ranges_and_flags`
    (type 5), not as `descriptor_table`. Match on both.
  - Superseded: 0.1% black measured 0.037 nits. The target is about 0.0095, but the SDR
    reference was another session and view, so it needs a same-session HDR-on / HDR-off /
    SDR set.

## Steps

1. Get UE 5.4 shader source (or decide to decompile). **Needs you:** an Epic-linked GitHub
   account, or a UE 5.4 launcher install.
2. Write `src/shaders/lutbuilder.hlsl` (stock port), plus `build_shaders.ps1` (dxc -T cs_6_6).
3. Add the bytecode swap and root-constant injection to the add-on. Verify the stock port
   matches the game.
4. Enable fixes 1-3 with overlay sliders. Measure with the probe and tune the defaults.
5. Fold the probe into a proper add-on (StillWakesTweaks). The probe tools stay under
   Advanced, like UntilDawnTweaks.
