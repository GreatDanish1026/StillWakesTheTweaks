# Engine.ini presets

The add-on's overlay picks among these ("Lighting quality") and writes the chosen one to
`%LOCALAPPDATA%\Habitat\Saved\Config\Windows\Engine.ini`, read-only, since the game deletes the
file on exit otherwise. A change applies at the next start. The build embeds this folder, so
the files here are the only copy. The first line (`; StillWakesTweaks preset: <id>`) is how the
add-on recognises its own file. A user's own Engine.ini is only replaced on request, and is
backed up as `Engine.ini.before-StillWakesTweaks`. On first run, with no Engine.ini, the add-on
writes `epicplus` with the recommended overlay extras (4 px fog, 16x filtering, 4 GB texture
pool). To install by hand, copy a file there and make it read-only.

| File | What it does | Cost (RTX 5080, 4K, DLSS, galley) |
|---|---|---|
| `Engine.base.ini` | Only what the HDR fix needs: `r.LUT.UpdateEveryFrame=1` (settings changes show at once) and `r.LUT.Size=64` (near-black precision) | none (~110 fps) |
| `Engine.epicplus.ini` | Base, plus true UE 5.4 Epic GI, shadows and fog, and full-resolution Lumen reflections. **Recommended.** | ~16% (92 fps) |
| `Engine.cine.ini` | Base, plus UE 5.4's Cine tier for GI, reflections, shadows and fog | ~60% (44 fps) |

`[SystemSettings]` outranks the game's menu, so the menu's lighting, reflection and shadow
quality no longer lower these values.

Hardware ray tracing isn't possible: the game ships without ray-tracing shaders, and enabling
it crashes at start-up (`docs/FINDINGS.md`).
