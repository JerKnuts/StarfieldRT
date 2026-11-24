# Starfield RT: plugin plan

Target: Starfield 1.16.244 (Steam), SFSE plugin, xmake + CommonLibSF, C++23 (same toolchain as Ultrawide Creations Menu and Widefield). Hardware baseline is DXR 1.1 (inline RayQuery). Dev machine has an RTX 5080.

Goal: ray-traced **lighting (GI), shadows, and reflections**. Reverse-engineering notes are in [RT_FINDINGS.md](RT_FINDINGS.md).

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
Each effect gets its own on/off switch and a quality level, in the INI first and later in an in-game menu. **Done for reflections and shadows in 0.2.36:** Settings > Gameplay Options > Starfield RT (`StarfieldRT.esm`), applied live. GI gets its own option when Phase 3 starts. A master switch turns everything off and runs vanilla.
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
