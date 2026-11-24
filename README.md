# Starfield RT 0.5.60

An SFSE plugin that adds ray-traced **lighting (GI), shadows and reflections** to Starfield on DXR 1.1+ GPUs.

> **Status: Phase 1 (RT sun shadows) and Phase 2 (RT reflections) work; 0.4.0 ray-traces every lamp with shadows (Numpad 2 compares with the game's lamps). Now: Phase 3, ray-traced GI (0.3.0, being tested: one ray per 2x2 pixels, lit by the sun and the game's lamps, accumulated over time; Numpad 1 cycles on / debug / off; starts intense, `fGIStrength`).** Reflections: glossy RT reflections on by default, lit by the game's own lamps (Numpad 5), with RT reflections on thin glass (Numpad 4 cycles on / stage debug / off). Numpad 0 cycles reflections off / on / debug views. **All keys are on the numpad (NumLock on); the key map is at the top of StarfieldRT.ini.** Every hotkey shows what it switched as a HUD message (`bOnScreenMessages`).

## What works today

- **DLSS Ray Reconstruction (0.5.5, confirmed in game):** NVIDIA's AI denoiser replaces DLSS Super Resolution whenever DLSS is selected in the game's Display settings. Needs `bDLSSRayReconstruction=1` and a matching Streamline 2.14.16 set (MO2 mod `Streamline 2.14.16 (RT)`: `sl.*.dll` 2.14.16 with `sl.dlss_d.dll`, `nvngx_dlss*.dll` 310.9.16). The log shows `[RR] DLSS Ray Reconstruction is running` when it takes over.
- **Ray-traced sun shadows** that replace the game's own sun shadow cascades while RT is on (the game's ShadowMapInstance pass draws nothing, so there is no flicker; **Numpad 3** toggles RT against the game's shadows; **Ctrl+Numpad 3** cycles the debug views: shadow-ray hit distance, scene/depth alignment, then back).
- **A complete ray-traced scene:** BLASes for every rigid mesh, built from the engine's own GPU geometry, including meshes in separately streamed buffers such as many trees and props. A per-frame TLAS is built from the game's instance records. About 25,000 BLASes and 400 MB in New Atlantis.
- **Alpha-tested shadows:** leaves, grass and fences block light only where their opacity texture is solid, using the game's own material data and alpha test (standard and vegetation materials). **Numpad *** toggles it.
- **Soft shadows:** several rays per pixel toward the sun's disc plus a penumbra-aware filter, so shadows stay sharp at contact and soften with distance. **Numpad /** toggles it.
- **Stable under streaming and save loads:** meshes settle for 30 frames before their BLAS is built, and builds read sanitized copies of the geometry.
- **Debugging tools:** a probe key (Numpad .) (what rays hit, with records, materials and leaf-test details), DRED GPU crash reports in the log, and debug views (`iSunShadows=3/4/5`).
- Loads only on Starfield **1.16.244** (Steam).

**Known limits:** animated (skinned) characters aren't in the ray-traced scene yet; they keep the game's own character shadows. Performance hasn't been measured or tuned. Soft shadows are filtered spatially only (no temporal accumulation). Distant buildings switching level of detail can briefly cast an extra shadow.

## Settings

`StarfieldRT.ini` sits next to the DLL in `SFSE\Plugins`.

**In game (0.2.36):** Settings > Gameplay Options > **Starfield RT** has **RT Reflections** and **RT Shadows** (Off / Low / Medium / High / Ultra), applied live and saved with the game. They need `StarfieldRT.esm` enabled, and they override `iReflectionQuality` / `iShadowQuality` unless those are `-1` (custom).

| Setting | Values | Purpose |
|---|---|---|
| `[RT] bEnableBLAS` | 0 / 1 (default 1) | Build acceleration structures from the game's meshes |
| `[RT] iBlasBuildsPerFrame` | default 64 | Most BLAS builds per frame |
| `[RT] iBlasBudgetMB` | default 2048 | Video memory cap for BLASes |
| `[RT] bBlasPoolOnly` | 0 / 1 (default 0) | 1 = only build meshes stored in the persistent geometry pool (leaves many trees out) |
| `[RT] bEnableTLAS` | 0 / 1 (default 1) | Build the scene TLAS every frame |
| `[RT] iTlasMaxInstances` | default 524288 | TLAS instance slots |
| `[RT] iSunShadows` | 0 off, 1 on, 2 debug replace, 3 debug A/B, 4 debug hit distance, 5 debug alignment | Ray-traced sun shadows |
| `[RT] iSunBiasCm`, `iSunBiasPerKm` | 10, 2 | Shadow ray start offset |
| `[RT] iSunToggleKey` | virtual-key code (99 = Numpad 3) | Live on/off toggle for RT sun shadows |
| `[Debug] iProbeKey` | virtual-key code (110 = Numpad .) | Log what rays through the screen centre hit |
| `[RT] iReflectionQuality` | -1..4 (3) | Reflections: 0 off, 1 low, 2 medium, 3 high, 4 ultra (-1 = custom settings). Low/medium trace 1 ray per 2x2 pixels |
| `[RT] iShadowQuality` | -1..4 (3) | RT sun shadows: 0 off, 1 low (1 ray) .. 3 high (4 rays, the old default) .. 4 ultra (8 rays, full res) |
| `[RT] iSkinnedRadius` | units (20; set by the reflection quality unless custom) | Characters within this distance join the RT scene: reflections and RT shadows (0 = none) |
| `[RT] iSkinnedMax` | 1..64 (32) | Skinned instances in the RT scene at most (a character is several) |
| `[RT] iReflections` | 0/1 (1) | RT reflections on at start (Numpad 0 toggles) |
| `[RT] bReplaceGameReflections` | 0/1 (1) | 1 = RT replaces the game's cubemap reflections at every roughness; 0 = only smooth surfaces, fading to the game's |
| `[RT] fReflectionFadeStart` / `fReflectionFadeEnd` | roughness (0.45 / 0.7) | Where RT reflections fade back to the game's |
| `[RT] fReflectionProbeAmbient` | number (2.5) | Brightness of reflected objects (Numpad + / Numpad - adjust it live) |
| `[RT] fReflectionMinWeight` | 0..1 (0.045) | Fainter reflections keep the game's and trace no ray (speed) |
| `[Debug] iReflectionDebugKey` | virtual-key code (96 = Numpad 0) | Cycle RT reflections: off, on, then the debug views (world normal, mirror hit, roughness, RT reflection colour) |
| `[Debug] fReflectionDebugScale` | number (1.0) | Brightness of the reflection debug views |
| `[RT] iTlasMaxInstanceSize` | default 1000 | Leave out giant instances (sky/planet shells) |
| `[RT] iTlasCullDistance` | default 1500 | Leave instances farther than this from the camera out of the TLAS (0 = keep all) |
| `[RT] bAlphaTest` | default 1 | Alpha-test leaves, grass and fences in RT shadows (0 = solid sheets) |
| `[RT] iAlphaToggleKey` | default 106 (Numpad *) | Live alpha-test on/off |
| `[RT] bReplaceGameSunShadows` | 0, 2, 3 (default 2) | While RT is on: 0 keep the game's sun shadows; 2 the game's sun cascade pass draws nothing (no flicker); 3 clear the cascade maps after their pass. 1 is no longer used and acts as 2 |
| `[RT] bSoftShadows` | default 1 | Soft shadows (cone-traced rays + penumbra filter) |
| `[RT] iSunShadowRays` | default 4 (1-16) | Shadow rays per pixel |
| `[RT] fSunAngularRadiusDeg` | default 0.15 | Sun size; bigger = softer |
| `[RT] iSoftShadowMaxBlur` | default 4 | Largest penumbra blur radius in pixels |
| `[RT] iSoftShadowScale` | default 2 | Soft shadows trace one sample per NÃ—N pixels (2 â‰ˆ 4Ã— cheaper; 1 = full resolution) |
| `[RT] iSoftToggleKey` | default 111 (Numpad /) | Live soft-shadow on/off |
| `[Debug] bDred` | default 1 | DRED crash diagnostics: on a GPU crash the log records the faulting address and allocations |
| `[Debug] bProfiler` | default 1 | Logs `PERF` lines: frame time plus the plugin's GPU time per stage and CPU time |
| `[Debug] iLitTestKey` | default 115 (F4) | Lit test: RT forced fully lit, game sun shadow maps off; anything still shadowed is from another game system |
| `[Debug] iSkipPassKey` | default 114 (F3) | Cycles which shadow-map pass has its draws dropped while RT is on (debugging) |
| `[RT] iSunRayLength` | default 2000 | Shadow ray length |
| `[Debug] iMaskTest` | 0 off, 1 shadow, 2 lit | Debug checker pattern in the contact shadow mask |
| `[Debug] iMaskTestCell` | pixels (default 64) | Checker square size |
| `[Debug] bLoadPixGpuCapturer` | 0 / 1 | Loads Microsoft PIX's GPU capturer at startup for frame captures. Leave at 0 for normal play. |
| `[Debug] iPixCaptureKey` | virtual-key code (121 = F10) | In-game key that saves a one-frame GPU capture |

## Frame captures (PIX)

1. Install [PIX on Windows](https://devblogs.microsoft.com/pix/download/).
2. Disable ReShade-based mods (any `dxgi.dll` proxy, including leftovers in MO2's Overwrite). PIX can't capture through them.
3. Set `bLoadPixGpuCapturer=1`, then launch Starfield normally through MO2.
4. In-game, press **F10**. The capture is saved to `Documents\StarfieldRT Captures\`. You can also **Attach** PIX to `Starfield.exe` and use its Take Capture button.
5. Set `bLoadPixGpuCapturer` back to 0 afterward.

## Roadmap

Full plan in [PLAN.md](PLAN.md). Reverse-engineering notes are in [RT_FINDINGS.md](RT_FINDINGS.md).

| Phase | Goal | Status |
|---|---|---|
| 0 | Foundation: D3D12 hook Ã¢Å“â€, frame map Ã¢Å“â€, lighting injection Ã¢Å“â€, geometry layout Ã¢Å“â€, BLAS/TLAS, debug ray view | In progress |
| 1 | RT sun shadows (replace the ShadowMask pass) | Planned |
| 2 | RT global illumination | Planned |
| 3 | RT reflections (replace the cubemap probe specular) | Planned |

Starfield ships unused ray tracing code (GI probe shaders, BLAS bookkeeping), but the parts that would build the acceleration structures and run the passes were stripped. No ini setting turns ray tracing on, so this plugin builds them itself.

## Requirements

- Starfield 1.16.244 (Steam)
- [SFSE](https://sfse.silverlock.org/)
- A GPU with DXR Tier 1.1 or newer (NVIDIA RTX 30-series+, AMD RX 6000+)

## Install (Mod Organizer 2)

1. Put `StarfieldRT.dll` and `StarfieldRT.ini` in a mod folder at `SFSE\Plugins\`. Development builds live in `C:\GTS\mods\Starfield RT (Dev)`.
2. For the in-game options, put `StarfieldRT.esm` at the mod folder's root and `SFRT_OptionsQuest.pex` in its `Scripts\` folder (both are in `dist\`).
3. Enable the mod in MO2, tick `StarfieldRT.esm` in the plugin list, and launch through SFSE.

## Logs

- Plugin log: `Documents\My Games\Starfield\SFSE\Logs\Starfield RT.log`
- Crash logs: `Documents\My Games\Starfield\SFSE\Crashlogs\`

## Analysis tools

In `tools/`, run with `py -3 -I` on a PIX C++ export (`pixtool open-capture <capture.wpix> export-to-cpp <dir>`):

- `frame_map.py <export> [out.txt] [pso_names.tsv]`: lists the frame's passes with their render targets and outputs.
- `pso_names.py <export> <shaderspc_dx12_105.fxp> [out.tsv]`: names every pipeline with Starfield's own shader technique names.
- `table_dump.py <export> <heap> <start> <count>`: shows which resources a descriptor table points at.
- `capture_data.py`: library that reads resource contents out of an export (resources.bin chunks, XPRESS).
- `extract_shader.py <export> <fxp> <pso> <out.dxbc>`: pulls a pipeline's shader out of the game's archive, for `dxc -dumpbin`.
- `records.py`, `mesh_dump.py`, `mesh_validate.py`, `scene_rebuild.py`: decode and validate Starfield's mesh/instance records, and rebuild the captured scene as OBJ.

## Building

Requires Visual Studio 2022 Build Tools (MSVC) and [xmake](https://xmake.io). CommonLibSF is vendored in `external/commonlibsf`.

From PowerShell (Git Bash makes xmake pick MinGW):

```powershell
xmake f -p windows -a x64 -m release -y
xmake -y
```

Output: `build\windows\x64\release\StarfieldRT.dll`

Shaders live in `shaders/` and are compiled with the Windows SDK's dxc into headers in `src/generated/`. `powershell -File toolsuild_shaders.ps1` compiles all of them (0.5.12); one by hand:

```powershell
& "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe" -T cs_6_0 -E main -O3 -Qstrip_debug -Qstrip_reflect -Vn g_MaskTestPatternCS -Fh src\generated\MaskTestPatternCS.h shaders\MaskTestPattern.hlsl
```

The Gameplay Options plugin and its script are built from source, too:

```powershell
py -3 tools\make_options_esm.py
& "C:\Program Files (x86)\Steam\steamapps\common\Starfield\Tools\Papyrus Compiler\PapyrusCompiler.exe" SFRT_OptionsQuest -i="papyrus;<base sources>" -o="dist\Scripts" -f="<base sources>\Starfield_Papyrus_Flags.flg" -op
```

`<base sources>` is `Scripts\Source` extracted from the Creation Kit's `Tools\ContentResources.zip`.

## Versioning

`1.0.0` is launch. `0.B.0` marks big milestones, and every other update bumps the last number. See [AGENTS.md](AGENTS.md) and [CHANGELOG.md](CHANGELOG.md).

## License

Starfield RT is free software under the **GNU General Public License v3.0** (see [LICENSE](LICENSE)). It has to be: it compiles in [CommonLibSF](https://github.com/Starfield-Reverse-Engineering/CommonLibSF), which is GPL-3.0.

Third-party code in this repository:

- `external/commonlibsf`: CommonLibSF, GPL-3.0 ([COPYING](external/commonlibsf/COPYING)).
- `external/streamline/include`: NVIDIA Streamline SDK headers, MIT ([license.txt](external/streamline/license.txt)).

Not included: the NVIDIA Streamline/DLSS runtime DLLs (get them from NVIDIA's Streamline SDK) and any game files. Starfield is a trademark of Bethesda Softworks; this project isn't affiliated with Bethesda or NVIDIA.
