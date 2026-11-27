# Starfield RT: plugin plan

Target: Starfield 1.16.244 (Steam), SFSE plugin, xmake + CommonLibSF, C++23 (same toolchain as Ultrawide Creations Menu and Widefield). Hardware baseline is DXR 1.1 (inline RayQuery). Dev machine has an RTX 5080.

Goal: ray-traced **lighting (GI), shadows, and reflections**. Reverse-engineering notes are in [RT_FINDINGS.md](RT_FINDINGS.md).

## Beta blockers from the 2026-11-25 soak test (0.5.99): beta on hold

Ultra, DLSS Quality, mostly FG on. 18:20 Neon, ~18:35 Jemison / MAST. Tester's notes plus the log:

1. **Big slowdown (6:29 Neon walkways, then Jemison).**
   - Neon: the RT scene jumped from 65k to 275k objects at 18:28:26 (outside on the walkways, 4 km range). Frame time went from 17 to 61 ms.
   - Jemison: 110k objects. VRAM climbed to 14.4 of 14.8 GB (97%), and frames reached 90-102 ms by 18:51.
   - 0.5.100-0.5.102: size cut, VRAM guard; breakdown and duplicate check found no clutter or duplicates (see RT_FINDINGS). Short runs hold 25-31 ms; a long run must confirm VRAM is the cause. Original plan: cull small objects by their size on screen (most of the 275k are clutter far away); a hard cap on the object count; shrink the range when VRAM is over ~85% of budget; check that what we keep alive is released after leaving an area (the buffer list rose from 22.5k to 30k).
2. **RT takes 2-3 s to appear after a load:** the scene is still being built. Hide it until ready (fade in), or build what's near the camera first. 0.5.103: the post-load waits (90 + 180 frames, 7.6 s at 35 fps) are now at most 1 s + 2 s; testing. Logged as "RT effects back after N s". **Good enough for the beta (tester, 0.5.106):** fade-in (1.5 s, sun and lamp shadows included), the game's sun shadows kept until RT has faded in, RR held during loads (0.5.105, also fixed the interior-entry FG hang). Still 5-11 s to RT after a load start; faster needs the cause of the 0.3.7 after-load hangs.
3. **High still too noisy.** **Fixed and confirmed (0.5.107):** the noise was reflections (Numpad 0 test); High now traces 1 reflection ray per pixel like Ultra. Cost on the Neon walkways: reflections 3-5 -> 7-9 ms, frame 28-32 ms (FG on, 31-35 fps rendered).
4. **NPC hair very shiny.** **Fixed and confirmed (0.5.108):** skin/hair (G-buffer types 1/8) get roughness 0.5, metal 0 in RT lamps, RR guides, GI and capture (their t2 channels aren't roughness/metal). Later: read the game's material table (t14) for the real values.
5. **Video screens should cast coloured light and show in reflections** (North-star gap 2: emissives as lights). **Parked for future work (2026-11-25, after 0.5.118).** Not a beta blocker any more.
   - Done (0.5.109-0.5.118): a per-object **glow memory** in reflections, GI and thin glass. On-screen hits learn the colour our shading can't explain; off-screen hits add it back. Works for family 12 screens and other emissive surfaces (seen in reflections on the player).
   - Still failing: **family 7 video screens** (Neon's AURORA ad, mesh 9049). They are material flipbooks: every video frame is a separate instance record with its own material (0.5.115/0.5.116 probes). 0.5.116 tag fix, 0.5.117 position key and 0.5.118 learning from covered pixels didn't make the window reflect them; the last test had no probe, so the learned glow value is unknown.
   - Next time: probe the window (glass ring logs the hit, lookup result and glow lum), and probe the screen with a learning counter. The real fix is decoding the family 7 material table (not +11/+13; candidates in RT_FINDINGS "Other material families") and sampling the flipbook texture at the hit.
6. **NPC shadows "freaking out" (6:25 Neon):** shadows from characters jump around. **Deferred to the next long run (tester, 2026-11-25):** only seen in the long soak test, so it is checked together with #1 (VRAM over time). Possibly related: the 256-candidate cut-off fixed in 0.5.119 (skinned slots changed at random in crowds).
7. **Mannequin has no reflection.** **Fixed and confirmed (0.5.120):** the 0.5.51 frozen-body rule collapsed it (it never moves while NPCs do); bodies the game skinned this frame (LOD +8 bit 30) are now never collapsed. 0.5.119 also raised the skinned candidate list 256 -> 2048 (it was full in Neon).
8. **(Likely fixed, 0.5.121: confirm on the Neon walkways in the long run)** **Random long shadows on the Neon walkways:** probably sun shadow rays hitting the smoke or glass layers (thin glass/smoke related).
   - 0.5.121, confirmed on a Jemison walkway: the shadow came from invisible 16 m glowing panels (an emissive non-solid card, mask 0x10 now traced by nothing; a family 7 hologram, mask 0x08 now reflections/GI only).
9. **Distant building shadows in Jemison** are wrong.
10. **Random shadows on the floor in MAST Defense.** **2026-11-26 (0.5.128) probe:** at night, a hard dark desk-shaped patch on the floor. The "sun" ray (28 deg elevation at night: the night directional light) was blocked by the desk (instance 44048, solid, 7.2 m, 1.2 m away); the rest of the floor was also blocked, softly, by a 4-triangle 50 m family 6 plane (instance 2501) 7-11 m away, and an invisible glass-flagged object (instance 87592, flags 0x12, 4 m, 1.1 m above the floor) sat in front of the visible surface. Gone after a reload. Next time: F8 to compare with the game's own shadow, then probe.
11. **Tram very bright at night in Jemison.**
12. **Photo mode blurry and laggy.**
## Beta release checklist (decided 2026-11-24): "Starfield RT - Beta 0.6" on Nexus

Why now: another modder is posting RT screenshots on Reddit; the first working release becomes the reference. Release a clearly labelled beta, not the current dev build. Version 0.6.0 = the beta (bump B per AGENTS.md).

1. **Finish the open fixes:** DONE 2026-11-24 (0.5.82: poster circle and thin-glass disc confirmed fixed in game).
2. **Frame Generation:** DONE 2026-11-24 (0.5.86, per-viewport DLSS options; run D with menus confirmed no hang). Was: run the A/B/C test (see "Open issue ... Frame Generation" below). Either fix the menu hang, or detect FG at runtime and show an on-screen warning (and say so on the Nexus page). Most users with 40/50-series cards run FG.
3. **Soak test:** one hour with everything on (F8 on, Ultra): Neon, New Atlantis, outdoors, save/load several times, fast travel, menus, photo mode. No crash, no hang, no flicker, VRAM line steady.
4. **Presets and defaults:** 0.5.92: Gameplay Options reduced to one Ray Tracing On/Off option (quality from the ini, default Ultra); bStartAllOff=0; Medium reworked (GI 4x4, no reflection sun shadows, 2 lamp shadows). Split the menu back into per-effect settings after the beta.
   - Ultra/High/Medium/Low in Gameplay Options as the main control; F8 = all effects on/off.
   - Ship `bStartAllOff=0` (effects on at start); keep the debug keys documented in a separate "advanced" section, not the main guide.
   - Fold the tuned values into the defaults: reflection strength/gloss, thin-glass strength and blur, lamp softness.
5. **Performance numbers:** GPU ms per effect (the PERF lines) and FPS per preset on the RTX 5080 in Neon and outdoors, for the Nexus page. Neon Ultra so far (0.5.89, 3440x1440 DLSS Quality, **FG on**; noise approved by the tester): rendered frame 24-27 ms (37-41 real fps, about 75-80 shown with FG; the frame time includes the FG pass); reflections 4.7-6.2, GI 3.7-4.5, lamps 1.5, TLAS 1.6, glass 0.5-0.7, RR guides 0.6. Same spot with **FG off** (0.5.90): 19-24 ms (41-53 fps); reflections 3.1-6.0, GI 2.4-4.3, lamps 1.3-1.6. RT effects off (scene still built): 9.4-10.6 ms (95-106 fps). So Ultra RT costs about 10-13 ms per frame, and FG adds about 2-3 ms per rendered frame while doubling what is shown. High (FG off): 15.8-18.7 ms (53-63 fps); reflections 1.3-2.1, GI 1.8-2.7, lamps 1.0-1.5. High (FG on): 17.7-21.4 ms rendered (47-56 fps, about 95-110 shown). Medium (FG off): 15.2-18.5 ms (54-66 fps); reflections 1.2-2.6, GI 0.9-2.3, lamps 1.1-1.7. Medium saves almost nothing over High (both trace reflections at 2x2); consider making Medium cheaper (reflections 4x4 or GI off-screen only) or dropping it. Still to do: outdoors. (0.5.90: PERF lines now say FG on/off.) Recommend DLSS Quality, not DLAA (DLAA cost 66 ms on Ultra).
6. **Packaging:**
   - Contents: StarfieldRT.dll, StarfieldRT.ini, StarfieldRT.esm, and a readme.
   - Streamline 2.14.16 requirement. Check what NVIDIA's licences allow us to bundle (Streamline is MIT; the DLSS / DLSS-RR `nvngx_*` DLLs have their own terms). Otherwise link to the official download and give exact file versions.
   - Install guide for MO2 (and Vortex), including the Root Builder pitfall (mixed Streamline versions crash at start-up).
   - Requirements: Starfield 1.16.244 (Steam), SFSE, an RTX GPU (DXR 1.1; Ray Reconstruction is NVIDIA-only).
7. **Nexus page:**
   - "Experimental beta" label; feature list; before/after screenshots (Neon, Ryujin lobby, outdoors).
   - Known issues (FG, performance), how to report: where the log is (`Documents\My Games\Starfield\SFSE\Logs\Starfield RT.log`), F8 to compare.
   - GPL-3.0 and a link to the GitHub repo (source).
8. **Release:** tag v0.6.0 on GitHub (only when the user says to push), upload to Nexus.

## North star (set 2026-11-20): the look of Cyberpunk 2077 with path tracing

The end goal is Starfield looking like Cyberpunk 2077 with RT Overdrive. Neon is the reference scene. The gap, roughly in order of visual impact:

1. **Emissive surfaces light the world.** Neon signs, screens and light strips cast coloured light and bounce it onto floors, walls and people.
   - GI and reflection rays that hit an emissive surface must return its emission.
   - Needs the material's emissive colour and texture (RE, not decoded yet).
   - This is the single biggest step toward the Cyberpunk look.
2. **Multi-bounce GI.** Feed last frame's GI result back in as the light at ray hits (a screen-space or world-space radiance cache), instead of the game's-ambient stand-in. This gives soft, deep indirect light in interiors.
3. **Every light shadowed by rays (ReSTIR-style direct lighting).**
   - Replace the game's shadow-mapped local lights with ray-traced shadows from all lights.
   - Use importance-sampled light choice so hundreds of lamps stay affordable.
   - Contact-hardening soft shadows from area and tube lights.
4. **Denoising at Cyberpunk quality.**
   - Today: per-effect temporal + spatial filters.
   - Target: a proper denoiser (ReBLUR/ReLAX-style), or DLSS Ray Reconstruction through the game's own Streamline/DLSS. Streamline is already in the frame, so RR is worth a feasibility study.
5. **Reflections everywhere, including rough and wet surfaces.**
   - Mostly done (Phase 2).
   - Remaining: emissive in reflections (item 1), multi-bounce lighting at reflection hits (item 2), and thick glass/doors without the old cubemap.
6. **Performance.** Half-res tracing everywhere, shader execution reordering on RTX 40/50, light culling per tile, and a quality ladder in Gameplay Options.

### Status and order (updated 2026-11-24, 0.5.87)

**In place:** RT sun shadows (soft); glossy RT reflections everywhere, thin glass included, smoke shown in reflections; RT lamps with soft shadows (8 strongest per pixel); one-bounce GI with material emission and last-frame screen feedback; DLSS Ray Reconstruction (works with Frame Generation since 0.5.86).

**Still missing, by visual impact:**
1. **Every light shadowed (ReSTIR DI).** Only the 8 strongest lamps per pixel get a shadow ray; the rest light through geometry. Neon has hundreds. Use per-pixel reservoir light sampling (spatial and temporal reuse) so all lamps are shadowed at a fixed cost. **Biggest gap.**
2. **Emissive surfaces as direct light sources.** Neon signs and screens light their surroundings only through GI rays that happen to hit them (noisy, no clean shadows). Put emissive triangles in the light list sampled by 1. Decode the family-7 glowing panel material.
3. **Multi-bounce GI with a world-space radiance cache** (SHaRC-style hash grid). Rooms fill with bounced light; stable when sources are off screen. Replaces the game-ambient stand-in at GI hits.
4. **Sky light / sky visibility.** GI rays that escape should take sky radiance with proper occlusion, not the probe colour.
5. **One combined path tracer feeding Ray Reconstruction raw**, instead of separate passes with their own filters (the 0.5.7 raw mode flickered; revisit once 1 to 3 exist).
6. **Characters:** facial morphs missing in the RT copy; hair approximate; about 25 characters at once (128 skinned pieces).
7. **Transparents and fog lit by RT lights:** smoke, holograms, particles, volumetric fog (important for Neon's haze).
8. **Performance:** Ultra with everything on ran at 22 fps (0.5.86; reflections 11.5 ms). Shader execution reordering (RTX 40/50), lower-resolution tracing with RR upscaling, per-pass budgets per preset. The PERF timings for every pass exist since 0.5.87.

**Order:** finish the beta (checklist above), including a performance pass (item 8). Then 1, 2, 3, 5. Items 1 to 3 are each a multi-session project.

Status (0.5.5): **DLSS Ray Reconstruction runs** in place of DLSS-SR (item 4). It is fed our guide images: diffuse and specular albedo, normal + roughness, and specular hit distance. Next: a "raw" mode that hands RR our undenoised reflections/GI/lamps instead of our own temporal filters.

Status against this list (0.4.9, tester: "squares gone, wavy noise gone", Ultra):
- RT lamps work, switched on through a GPU copy of the lamp list and a descriptor swap.
- GI is calm at Ultra: 1 cell per pixel, 2 rays, plane-based history test.
- Reflections use last frame's image for on-screen hits.
- Open:
  - soft shadows from lamp size;
  - stochastic light choice (ReSTIR);
  - family-7 emissive screens off screen;
  - DLSS Ray Reconstruction study;
  - performance at Ultra;
  - the parked thin-glass flicker under smoke.

Earlier status (0.4.0):
- Item 3 has a first version: every lamp is ray traced, with shadows for the 4 strongest per pixel, and the game's lamp lighting is switched off.
- Next for item 3:
  - stochastic light choice (ReSTIR) instead of top-4, so every lamp gets shadowed;
  - soft shadows from lamp size;
  - check transparent and fog lighting.

Earlier status (0.3.3):
- RT sun shadows and RT reflections (lit by sun + the game's lamps) work.
- GI works, with emissive and multi-bounce light from anything visible last frame (screen-space light feedback: GI hits reuse last frame's lit image).
- Still needed:
  - emissive from off-screen surfaces (material emission: the capture's StandardMaterial#10 G-buffer PS, PSO 12209, has 3 outputs and no emission; the emissive permutation wasn't in the capture);
  - the same feedback for reflection hits;
  - ReSTIR;
  - a real denoiser (0.3.4 added temporal accumulation with neighbourhood clamping to reflections and GI; next: an a-trous spatial filter with variance guidance, or DLSS Ray Reconstruction).

## Open issue (2026-11-24, 0.5.81): GPU hang with Frame Generation on when opening a menu

- DRED: only `nv.sl.dlss_g.cmdCtx.pacer` and an internal DXGI list unfinished, none of the plugin's lists (the same signature as every FG hang since 0.2.x).
- Trigger seen: opening a menu with FG on. 6 s before, `slDLSSSetOptions viewport 2: mode 0` (the game switching DLSS off for the menu viewport).
- Suspects to check: Ray Reconstruction still set up on the main viewport while FG paces the menu frames; our tags (RR guides, colour before transparency) pointing at resources the menu path doesn't produce; the 3 s scene-settling pause not covering menu transitions.
- Until then: test with FG off.
- **Decided 2026-11-24:** Frame Generation stability is a workstream of this RT package (not a separate mod), scheduled after the current RT testing round. Reasons: we replaced the game's Streamline (and with it FG's `sl.dlss_g`) with 2.14.16 for Ray Reconstruction, and our hooks sit in the same Streamline path FG reads.
- **First step, the bisect** (FG on, open/close menus ~20 times, walk a few minutes):
  - A: plugin disabled in MO2, Streamline 2.14.16 kept. A hang here = Streamline/game, not us. **Result 2026-11-24: no hang** (menus opened repeatedly, played a while). Points at the plugin.
  - B: plugin on, every effect off (F8). A hang here = our hooks or Streamline setup. **Result 2026-11-24: no hang.** Our hooks alone are fine; the cause is the RT work or Ray Reconstruction.
  - C: plugin on, RT on, Ray Reconstruction paused (Ctrl+Numpad 0). Rules RR in or out. **Result 2026-11-24: no hang.** So: RR + FG.
  - **Fixed and confirmed 2026-11-24 (0.5.86):** run D (FG + RR + everything on, menus ~20 times) had no hang, and RR came back after every menu.
  - **Cause found (0.5.85):** our hook kept one global copy of the game's DLSS options. Opening a menu sets viewport 2 to DLSS off with output 0xFFFFFFFF x 0xFFFFFFFF, and that copy then configured RR on the main viewport. Fixed by keeping the options per viewport and running RR only where they're valid. To confirm: run D = everything on, RR on, FG on, menus.
- **2026-11-25 (0.5.104): same signature entering an interior** with FG on, 5 s after the load switched FG off and on; RT effects were paused, RR was still running. 0.5.105 holds RR off during loads and for 2 s after any FG switch. Testing.
- **2026-11-25 (0.5.122): same signature after opening a menu** (22:49:51 viewport 0 set to DLSS off, 22:49:52 viewport 2 valid + FG on, RR resumed on viewport 2; hang 19 s later with no further option change, so probably still in the menu). 0.5.86's run D toggled menus quickly; staying in a menu with FG + RR is untested. Next: stay in a menu 30-60 s with FG on, with and without RR (Ctrl+Numpad 0).
- **2026-11-26 (0.5.122):** a 55 s menu with FG + RR: no hang. Then a hang 14 s after RR resumed on viewport 2 (running to the Jemison tram), but with a different signature: no FG pacer list, one unnamed list stopped on a dispatch (541 of 838 ops). 0.5.123 labels every plugin dispatch in DRED so the next report says whether it was ours, and which pass.
- **2026-11-26 (0.5.123):** hung 5 s after opening a menu; labels show none of our passes running (FG pacer + DXGI only). Both 0.5.122/0.5.123 hangs followed a TLAS buffer growth 131072 -> 196608 by 4-5 s. 0.5.124 allocates 262144 up front; testing.

## Known issue, parked 2026-11-20 (0.3.10): thin-glass reflections flicker under smoke in the Neon walkway

- Numpad 4 (thin-glass reflections off) removes it, so the glass pass causes it; GI and the main reflections don't.
- F8 glass dumps on the smoke: "covered by OIT, camera ray hit nothing". At the crosshair the pass draws nothing on the smoke itself.
- Tried and not enough:
  - 0.3.8: the fallback rejects solid family-0 materials plus a 15 cm + 3% margin along the ray;
  - 0.3.9: GI capture skips OIT-covered pixels (a real fix, but for GI);
  - 0.3.10: fallback hits must be 20 cm off the plane behind.
- Next ideas:
  - turn the untagged-window fallback off (ini switch) to see if it is the fallback at all;
  - look for a temporal cause: the pass adds on top of the HDR image every frame with no history, so the OIT coverage flipping gives on/off;
  - fade the glass reflection by OIT coverage (b0) instead of a hard threshold.

## The key point
All three effects sit on the same foundation: **our own BLAS/TLAS of the live scene**. Once that exists, each effect is its own compute pass that traces against it. Bethesda's leftover code covers only part of GI (the probe passes). Shadows and reflections are entirely ours.

## Phases (each one is testable on its own)

### 0. Foundation (largest risk; everything depends on it)
**Done (0.1.9–0.1.34):** runtime BLAS/TLAS from engine geometry, stable under streaming and save loads.
- Hook the renderer to get the engine's `ID3D12Device5`, the graphics queue, and a command list at a known point in the frame.
- Pull geometry from the engine's own GPU buffers (mapped 2026-11-18, see RT_FINDINGS "Scene geometry layout"): positions are int16×3 at a 6-byte stride in bindless raw buffers (heap slot 32768+), indices are uint16 in the 4 GB pool #132, transforms are 3×4 in #11007. These map directly to DXR R16G16B16A16_SNORM (stride 6) + R16_UINT, so no conversion is needed. Record layouts (counts, instance → mesh, quaternion/translation/scale transforms) decoded and validated on captured data on 2026-11-18. The scene was rebuilt from the records.
- Build BLASes (async, budgeted per frame) and a per-frame TLAS (refit/update), with instance transforms from the renderer's scene tables. Handle streaming in and out and cell changes.
- **Debug view:** a compute pass that traces primary rays from the camera and writes hit distance / instance ID to a debug texture shown on screen. This proves the TLAS matches what is rendered.

### 1. RT sun shadows (best visual payoff for the effort)
**Done in 0.2.0 (2026-11-18):** RT sun shadows replace the game's sun cascades, with alpha-tested foliage and soft (cone-traced, penumbra-filtered) shadows. Left for later: temporal accumulation, skinned meshes, local-light shadows.
- Find the deferred sun-lighting pass and the screen-space shadow mask (or cascade lookup) it reads.
- Compute pass: from G-buffer depth and normal, trace a ray to the sun (cone-jittered for soft penumbra), write a shadow mask, denoise (temporal plus a light spatial filter, or NRD SIGMA), and substitute it for the cascaded result.
- Only needs alpha-tested foliage handled (any-hit equivalent → opacity check in the inline RayQuery loop).
- Later: RT shadows for a few local lights.

### 2. RT reflections (next, started 2026-11-18)
Chosen ahead of GI because it is the most visible upgrade: Starfield has **no SSR**, only cubemap probes.
Where it goes (RT_FINDINGS "Indirect and Composite"): `Indirect#0` writes the probe specular to its second render target as `probe radiance × (F0·A + B)`. We overwrite that with `RT radiance × (F0·A + B)` before `Composite#0`, so the game's exposure, AO and tone mapping still apply.
- **R1. Hook point and inputs.** Find the render pass whose Execute draws Indirect#0 and hook after it. Identify its second render target and the G-buffer views in its tables (t1 normal, t2 roughness/AO/metal, t4 material type, t6 depth), plus the BRDF lookup (t13) and probe cube arrays (t7/t8). **Done (0.2.24):** one full-screen draw into two R11G11B10 targets; root param 4 is the space4 table with offset = register. See RT_FINDINGS "R1 survey".
- **R2. Debug mirror view.** *(0.2.25: **passed** in New Atlantis. Normals decode smoothly with world Z up, and the mirror view on a curved pillar shows a coherent, upright, correctly curved reflection of the room behind the camera, characters included. Runs every frame, no crash.)* Trace one mirror ray per pixel from the G-buffer normal and write hit distance or instance colour into the specular target. This proves the rays, the normal decode and the hook.
- **R3. Hit shading.** *(0.2.26: **passed**. Real textures show up correctly mirrored: floor tiles, arches and kiosks in the Trident pillar, sushi adverts mirrored in a glossy ceiling panel. No crash. Known: reflected texture edges look a little blocky (mip choice or streamed low-res mips), the lighting is a placeholder, and the reflection pass is not in the PERF line yet.)* At the hit: albedo from the material's base colour texture (same material tables as the alpha test; find the albedo texture slot), a face normal from the triangle, then `albedo × (sun × RT shadow ray + ambient)`, plus emissive if it is easy. Rays that miss sample the game's probe/sky cubemap.
- **R4. Composite.** *(0.2.27: working. Outdoors a smooth floor mirrors buildings; indoors it was too dark (probe light already holds albedo), and it cost 2.6 ms indoors / 7.8 ms outdoors. 0.2.28: brighter default, PageUp/PageDown live tuning, no rays on too-faint reflections; being tested. Cost still needs R5: half-res tracing + upsample.)* Replace the probe specular with RT radiance through the game's own split-sum. Fade back to the probe value as roughness rises.
- **R5. Glossy, denoise, quality levels.** *(0.2.29: glossy GGX rays + edge-aware roughness filter. 0.2.31: RT replaces the game reflections at every roughness. 0.2.32: ray-cone texture LOD, characters (skinned meshes) in the RT scene; being tested. Still to do: temporal accumulation, half-res tracing for speed, ray cones for texture mips.)* Jittered glossy rays, a temporal and spatial filter, half-res tracing, and performance tuning.

### 3. RT global illumination
- Option A (likely dead): Bethesda's RayTraceProbeCompute Execute is an empty `ret 0` in retail.
- Option B (the plan now): our own screen-probe or per-pixel diffuse GI pass. It replaces or adds to Indirect#0's first render target (indirect diffuse) before `Composite`. Hit shading is shared with reflections (R3).

### Settings: each effect on/off with quality levels (goal for 1.0.0)
**Required for 1.0 (user decision 2026-11-25):** each type of RT has its own settings in Gameplay Options: reflections, thin-glass reflections, GI, RT lamps (lamp shadows), sun shadows and Ray Reconstruction. Each gets on/off plus a quality level, applied live and saved with the game, with the master **Ray Tracing On/Off** kept on top (same as F8).

History: 0.2.36 had RT Reflections and RT Shadows (Off to Ultra), with GI and lamps following the reflection level. **0.5.92 cut the menu to one Ray Tracing On/Off option for the beta** (quality from the ini only).

To build it:
- give each effect its own preset table in `ApplyQuality` (today GI, lamps, characters and scene range all follow the reflection level);
- one GPOF and one global per effect in `tools/make_options_esm.py`, all synced by `SFRT_OptionsQuest`;
- the plugin polls each global;
- use new form ids again so saved values never mismatch.

The old per-level targets below are a rough guide; the real numbers come from the PERF tables in the beta checklist.
| Effect | Off | Low | Medium | High | Ultra |
|---|---|---|---|---|---|
| Shadows | game's shadows | hard, half-res | soft, half-res (today's default) | soft, full-res | + temporal accumulation |
| Reflections | game's probes | mirror-like surfaces only (roughness < 0.3), half-res | roughness < 0.5, RT shadow at hits | full-res, glossy up to 0.7, denoised | + local lights at hits |
| Lighting (GI) | game's probes | 1 ray per 4 pixels | 1 ray per 2 pixels | 1 ray per pixel | + multi-bounce |

## Cross-cutting
- **Denoising:** NVIDIA NRD (ship our own copy, MIT-licensed. Don't depend on the stray NRD.dll in the game folder).
- **Shaders:** HLSL SM 6.5, compiled with DXC at build time, embedded in the DLL.
- **Settings:** an ini per feature, plus a quality scale and a master off switch. Fail safe: if anything is missing, run vanilla.
- **Version pinning:** RVAs for 1.16.244 behind a runtime check (like Widefield). Move to Address Library IDs where CommonLibSF has them.
- **Performance budget (5080, 3440x1440 with DLSS):** shadows about 1.5 ms, GI about 2 ms, reflections about 3 ms, plus TLAS update under 0.5 ms.

## Glass (started 2026-11-19, 0.2.38)

Glass is drawn after the opaque scene as LayeredEffect draws with moment-based OIT, plus its own probe reflection. Thick glass is also in the G-buffer (it gets RT and then the game's reflection on top); thin glass gets only the game's.

- **G1 survey (0.2.38):** which passes draw glass, which targets they write, and which bindings hold the reflection cubes and BRDF LUT.
- **G2 remove the game's glass reflection (0.2.39–0.2.40, done):** slot +7 of the transparency pass's table 4 (probe array, or a material's own cube) is swapped for a black cube; Home toggles it. The blur left on thick glass is the interior seen through it (refraction), not a reflection.
- **Sharp view through glass (0.2.42):** table 4 +15 (the scene-color mip chain the glass refracts) is rewritten as a mip-0-only view; Numpad 9 toggles it.
- **G3a glass separated in the TLAS (0.2.44):** glass (material flags +108 bit 0 clear) gets instance mask 0x02; shadow and reflection rays skip it, so sunlight passes through windows.
- **G3 thin-glass RT reflections (0.2.51, first version):** after CompositeMomentBasedOIT, pixels with OIT coverage (b0) get a camera ray against mask 0x02 to find the glass, then a mirror ray added with glass Fresnel. Numpad 4 toggles.
- **G3 RT reflections on glass:** glass isn't in the G-buffer, so find it per pixel (a camera ray against the TLAS that reports glass materials, or glass depth/normal captured from the glass draws), trace the reflection, and add it after CompositeMomentBasedOIT with the glass Fresnel.

## Status 2026-11-20 (0.3.0): GI started

- Reflections since 0.2.54:
  - thin glass reflects (0.2.56);
  - reflections are lit by the game's own local lights (0.2.58, light list decoded from TiledDeferredLight);
  - effects on material 0 (smoke, haze) are excluded (0.2.64);
  - characters are in the scene at Neon (0.2.66);
  - the first-person head is hidden from rays (0.2.67).
- Phase 3 GI, first version (0.3.0):
  - one cosine ray per 2×2 pixels, shaded like reflection hits;
  - temporal accumulation reprojected with last frame's camera (PerScene rows 0, 5–8 copied each frame);
  - edge-aware spatial filter;
  - written over GatherIndirect RT0 (the game's indirect diffuse = diffuse colour / π × irradiance × exposure, PSO 11844).
  - Starts intense (strength 2) on purpose.
- Next for GI:
  - tune strength and noise from tester feedback;
  - check the reprojection rows (5–8 assumed unjittered);
  - a real second bounce instead of the game's-ambient stand-in;
  - performance.

## Status 2026-11-19 evening (0.2.54, paused)

- Works: RT shadows and reflections (quality presets, Gameplay Options menu), glass: the game's probe reflection hidden (Numpad 8), sharp view through glass (Numpad 9), mirror-sharp thick glass (type 2), thick-glass see-through layer hidden (Numpad 6), reflections skip see-through materials (Numpad 7), alpha-tested foliage in reflections.
- Fixed in 0.2.54: at some locations the RT build pass doesn't bind the scene table; the build now uses the newest valid one bound by any pass (the scene table moves every frame).
- **Open, next:** thin-glass RT reflections (G3, Numpad 4) run (`applied` in the log) but show nothing. Suspects, in order: the b0 moment test (wrong texture / threshold / half-res mapping), the mask-0x02 camera ray finding no glass (glass flagged 0x11 here? TLAS count shows ~10k tagged), the depth SRV index, or the copy-back being overwritten by later passes. First step: a debug output (paint every pixel that passes each test) to see which stage fails.
- Also open: faint diagonal light streaks on the door glass; a game-side crash while running (no plugin frames); Frame Generation hangs.

## Immediate next steps
1. Reverse the frame structure: where the render graph executes, where the G-buffer, depth, sun shadow mask and reflection composite live (render-pass names exist as strings and RTTI).
2. Get the device/queue/command list hook working in a minimal plugin (logs the device, and its DXR tier, on first frame).
3. Geometry extraction plus the BLAS/TLAS foundation, then the debug ray view.

## Status 2026-11-19 (0.2.34)

- Quality levels exist for reflections and shadows (`iReflectionQuality`, `iShadowQuality`: off/low/medium/high/ultra). Lighting (GI) gets its own level when Phase 3 starts.
- Open: RT reflections on glass and other transparent surfaces (drawn in a later forward pass, untouched so far); Frame Generation hangs (testing with FG off; VRAM is ruled out); TLAS cost (~1.2 ms rebuilt every frame) and reflection temporal accumulation.
