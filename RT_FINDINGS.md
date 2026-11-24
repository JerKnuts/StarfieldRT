# Starfield hidden ray tracing — reverse-engineering notes

Exe: `Starfield.exe` (Steam, 102,476,200 bytes, dated 2026-06-15). All addresses are absolute VAs (image base 0x140000000).

## Summary
- Starfield ships a dormant **ray-traced GI probe** path (DXR 1.1 inline RayQuery). It covers diffuse indirect lighting only. There are no RT reflections or shadows.
- Shaders are present in `Data/Starfield - Shaders.ba2` → `shadersfx/shaderspc_dx12_105.fxp` (uncompressed GNRL BA2 v2):
  - `RayTraceProbeCompute` technique id 0xAE, 10 variants, all SFI0 flags 0x02100000 (RT tier 1.1 + descriptor-heap indexing)
  - `RayTraceColorCompute` technique id 0x88, 6 variants, same flags
  - These are the only 16 shaders out of about 5,630 that use RayQuery.

## Settings
| Setting | Object | Default | Notes |
|---|---|---|---|
| `bEnableRaytracing:Display` | 0x145fa25c8 (value +8) | 0 | Copied to renderer byte 0x145ed712d. Turning it on sets 0x145ed712e, which **nothing reads**. Turning it off sets 0x145ed712f, which is polled by 0x142b30ca0 → teardown 0x14298cf10. |
| `iIndirectOverride:Display` | 0x145925840 (value 0x145925848) | 4 | **Forces the probe GI mode** when ≤ 3. Mode 2 = ray-traced. |
| `RaytracingVisualization` console cmd | — | — | Execute handler 0x1402b75d0 is a stub (`mov al,1; ret`) |

## Probe mode selection — 0x142a60900(probeData, probeId)
- Natural result is 0 or 1 (from a scene flag byte), or 3 if bit 18 of probe flags is clear. **It never returns 2 on its own.**
- `if (iIndirectOverride <= 3) mode = iIndirectOverride`
- Caller 0x14291fa90 (around 0x1429226b3) stores the mode at `probe+0x1d8` and sets `[x+0x182] = (mode != 2)` (looks like a "use fallback cubemap" flag).

## RT pass build gates — ProbeRenderPass vtable 0x144db8a48, slot 8 = 0x1429ff3f0
The RT passes are added only if all of the following hold (each failure jumps to 0x1429ffc5b):
1. `0x142a61020(node)` returns true
2. probe count != 0
3. `probe+0x1d8 == 2` (ray-traced mode)
4. `0x1429fe970(id)`: the probe's textures/resources are valid
5. `0x1429fe6a0(id)`: a table lookup, **and** the qwords at 0x145fc2dd0 and 0x145fc2dc8 are non-null (suspected RT scene / TLAS objects). 0x14298cd20 frees 0x145fc2dd0, and no rip-relative writer that sets it was found yet.
6. `0x1429fe720()`: required shaders/resources are loaded (cached in byte 0x1461fad20)

Then it constructs `RayTraceProbeNonRandomRaysComputeRenderPass` (vtable 0x144db8c58) and `RayTraceProbeComputeRenderPass` (vtable 0x144db8ce8).

## DXR capability
At 0x1429d0674 the game calls CheckFeatureSupport(OPTIONS5). If the RT tier is not 0 and not 1.0, byte 0x145e76599 is set and capability bit 0 is set at device+0x1a8.

## Tools
Scratchpad scripts (sfre.py: capstone + pefile + numpy xref scanner) are not kept between sessions. Rebuild them as needed.

## Live test (2026-11-17, game running, read-only memory peek)
- With `bEnableRaytracing=1` and `iIndirectOverride=2` in the StarfieldCustom.ini of the okok profile:
  - iIndirectOverride = 2 (loaded). bEnableRaytracing = **0**. It is an **INIPrefSettingCollection** setting (vtable 0x144b030b8), so it belongs in StarfieldPrefs.ini, not Custom.
  - ptrA 0x145fc2dd0 = 0 and ptrB 0x145fc2dc8 = 0 → gate 5 fails. readyCache 0x1461fad20 = 0. HW RT flag = 1.
- Static check: **no code in the exe ever writes a non-null value to ptrA or ptrB** (rip-relative scan of the block 0x145fc2d40–0x145fc2e00). ptrA is only released and zeroed (0x14298cd20 → 0x142a15270, a GPU resource release). The RT scene creation is stripped like the enable consumer, so ini settings alone cannot turn on the RT probe passes.
- Visual A/B screenshots: no lighting difference (the pixel diff showed only camera and DLSS jitter).
- Reverted the profile ini.

## BLAS side (static, 2026-11-17)
- RaytracingSceneManager code sits around 0x14298c000–0x14298f000. RTTI: `BLASDB` keyed by `Model` and `GeometryGroup`, `BLASGeometryData@ApiAbstractionLayer`, `RaytracingInfo` (a per-Model StorageTable column), `TLASFastConvergeEvent` (a Scene StorageTable column).
- **BLAS registration is ungated**: the mesh loader 0x14248a130 calls 0x14298d660 (at 0x14248a5d5, skipped only by a local flag r13b), which queues a BSService `TCmd` (vtable 0x144db2770). That leads to dispatcher 0x14298e4d0 → 0x14298de70, which inserts a GeometryGroup entry into the DB singleton `*(0x1461facd0)` (entry table at DB+0x110). The GPU build lambdas are `TypeErasedFuncImpl<…BLASGeometryData…>` (vtable 0x144db26d0).
- The DB teardown is 0x14298cd20 (it also releases ptrA).
- Still to confirm live: whether the DB fills up and whether real GPU BLASes get built. The script `peek2.py` (scratchpad) reads `*(0x1461facd0)`.

## SFSE plugin scope
**Already in the game:** RT shaders (16), the RT probe render passes, the probe mode override (`iIndirectOverride=2`), DXR 1.1 detection, and probably per-mesh BLAS registration.
**Missing:** TLAS build, setting ptrA (0x145fc2dd0) and ptrB (0x145fc2dc8), and the readiness flag (0x1461fad20).

Phases:
0. **Diagnostics (small).** Confirm live that the BLAS DB is populated and holds GPU BLAS addresses. If it doesn't, the scope grows a lot: we would have to build BLASes from Starfield mesh streams ourselves.
1. **Reverse the RT pass execute** (RayTraceProbeComputeRenderPass vtable 0x144db8ce8, slots 3–4 = 0x1429ff050/0x1429ff090 plus the generic compute execute). Find out exactly what ptrA and ptrB are (TLAS resource? instance buffer? descriptor holder?) and how the TLAS SRV reaches the shader. The shaders use `ResourceDescriptorHeap` indexing (SFI0 bit 0x2000000), so expect a heap index in a constant buffer.
2. **TLAS builder in the plugin.** Each frame, before the ProbeRenderPass build (vtable 0x144db8a48 slot 8): walk the scene instances, look up BLAS GPU VAs from BLASDB, fill `D3D12_RAYTRACING_INSTANCE_DESC`s, build or update the TLAS on the engine's D3D12 queue, then publish ptrA/ptrB and set readiness. Write `iIndirectOverride=2` to the setting at 0x145925848.
3. **Hardening:** streaming in and out, cell changes, device-lost, performance (TLAS updates instead of rebuilds), and an ini toggle.

Risks: the BLAS DB might hold only CPU-side data; the exact resource-binding contract is unknown; the plugin must be rebased for each game update (use Address Library IDs).

## Live BLAS check + master switch (2026-11-17)
- The BLAS DB singletons live in exe .data: DB<GeometryGroup> 0x148564580 (also pointed to by 0x1461facd0), DB<Model> 0x148564380, plus Cache objects at +0x40.
- A heap scan (5.75 GiB of private RW memory) found **0** BLAS TEntryType objects, 0 BLASGeometryData lambdas, and 0 BLAS TCmds, so no BLASes exist in a stock session.
- **Master switch: `bUseSerializedBLAS:Display`** (INISettingCollection, i.e. StarfieldCustom.ini, default 0, obj 0x1459238f8). It is read once at init by 0x14248a8c0 (called from 0x14189a71d). When it is 0 the function returns. When it is 1 it queues a task (0x14248a960) that registers the stream handler 0x14248a5f0 → for each mesh it tries a `.blas` file (loader 0x14248a130: magic "BLAS" 0x53414c42, version 2) and falls back to a runtime build via 0x14298d660.
- No `.blas` files ship in any BA2 or as loose files, so with the switch on, every BLAS would be built at runtime.
- Other BLAS settings (`bCompactBLAS`, `bCapBLASUpdatesPerFrame`, `iMaxBLASUpdateSize`) have no readers. They are orphaned.
- Test queued: `bUseSerializedBLAS=1` in the Custom ini of the okok profile (backup: StarfieldCustom.ini.bak-before-blas). Verify with heapscan.py (count the TEntryType objects at vt 0x144db26b8).

## Result with bUseSerializedBLAS=1 (2026-11-17, live)
- The setting loaded (value 1). The handler registration ran: the global 0x145fc2dc0 now holds 0x14248a960.
- Still **0** BLAS entries on the heap. Reason: `.blas` resources are only requested by **`BSOfflineRTExtraData`** (vtable 0x144d76998, ctor 0x1424c0a80; slot 42 = 0x1424c1080 builds the `<…>.blas` path). That is NIF extra data attached to a mesh.
- **No shipped mesh carries BSOfflineRTExtraData**: 0 of 9,000 sampled NIFs across Meshes01/02/Patch. The scanner sanity check found BSGeometry in 299 of 300.
- So the chain is: mesh has BSOfflineRTExtraData → request `.blas` → loader 0x14248a130 (file) or fallback 0x14298d660 (runtime build) → BLAS DB entry. The chain is intact, but nothing enters it.
- CreationKit.exe has richer RT strings ("RT BLAS Buffer", "pBLASScratchBuffer", "SerializedBLASData", "numBLASMeshes", "initialized BLAS instance"), so it may be a useful reference for the BLAS/TLAS internals.

## Updated plugin plan
- **Entry point for BLASes:** either (a) a plugin that calls the runtime-build enqueue 0x14298d660(geomId*, array*) for each streamed geometry, or (b) one that attaches a BSOfflineRTExtraData to meshes at NIF load, so the engine's own path runs (no .blas file → runtime fallback). (b) reuses more engine code. (a) needs fewer hooks.
- Next RE: work out the arguments of 0x14298d660 / 0x14248a5f0 (what the geometry ID is and where it comes from) and confirm the runtime fallback really builds GPU BLASes (the TypeErasedFuncImpl<BLASGeometryData> lambdas → ApiAbstractionLayer).
- Then TLAS + ptrA/ptrB as before.

## BLAS build pipeline: what actually exists (2026-11-17)
- Enqueue `0x14298d660(const BSResource::ID& id /*3 dwords*/, BSTArray<StrongKey<Storage::Geometry>>&& geoms)` queues a TCmd (vtable 0x144db2770). The dispatcher 0x14298e4d0 → 0x14298de70 does the DB insert keyed by ID → a slot is taken from the bitmap allocator → a 128-byte BLAS state record in `*(0x145fab5d8)` (capacity 0x145fab5d0) → a BLASGeometryData lambda runs:
  - **runtime lambda** (vtable 0x144db2700 → 0x14298e270): `0x14298cb00(record, geoms)` picks a LOD for each geometry (mesh+0x65 LOD bitmask) and fills record+0x60 with per-geometry refs, then **appends the BLAS index to a pending list** (count 0x145fa0320, cap 0x145fa0324, array 0x145fa0328).
  - **serialized lambda** (vtable 0x144db26d0 → 0x14298e0e0): allocates a GPU buffer of the blob size (0x142a8eb20), uploads the .blas blob (0x142a04ae0), sets record+0x5c=1, and also appends to the pending list.
- **No code drains the pending list except the disable/flush path 0x14298cf10.** There is no per-frame consumer that issues the GPU BuildRaytracingAccelerationStructure (or a CopyRTAS deserialize of the serialized blobs). The GPU BLAS build is stripped, just like the TLAS build.

## Revised plugin scope
The plugin must do the real DXR work itself:
1. Get the engine's ID3D12Device5 and a command list / queue slot every frame (hook the renderer frame or the ProbeRenderPass build).
2. For each streamed mesh: feed the engine's enqueue (0x14298d660) so the engine does bookkeeping and LOD selection, **or** walk the geometry directly. Then read the vertex/index buffers (Starfield positions are probably 16-bit SNORM with a scale → R16G16B16A16_SNORM is DXR-legal, with the scale folded into the instance transform) and build GPU BLASes.
3. Build the TLAS over the visible instances each frame (update instead of rebuild).
4. Reverse the RT pass binding contract (what ptrA/ptrB hold, and which descriptor-heap index the shader reads the TLAS SRV from), then publish them and set readiness 0x1461fad20.
5. Force `iIndirectOverride=2`.
This is a multi-week project. The engine still supplies the RT probe shaders and passes, the probe placement and LOD selection, and the denoise/integration into GI.
- Profile ini reverted (bUseSerializedBLAS removed; the plugin can set it in memory if path (b) is chosen).

## Frame map (Phase 0, 2026-11-17)
- 479 `*RenderPass@CreationRendererPrivate` classes. Relevant ones: DeferredGBuffer, DepthPrepass, **ShadowMask** (screen-space sun shadow mask, vtable 0x144ee48f0), ScreenSpaceShadows (0x144db4ea0), TiledDeferredContactShadows, **Lighting** (0x144ee1168), TiledDeferredLighting* / TiledBinning_idTech7* (id Tech 7-style tiled lighting, where probe reflections are applied), DeferredComposite (0x144edf480), GatherGeometryInstances / GatherTransforms (instance data for the TLAS), ReflectionProbe* / BlendReflectionProbe.
- **No SSR pass exists.** Reflections are cubemap probes only, so RT reflections replace the probe specular term.
- Render pass vtable: slot 0 dtor, slots 1–4 shared base (0x142a504d0, 0x142a50cb0, 0x1429ff050, 0x1429ff090), **slot 7 = per-pass Execute(this, ctx, resources)**. ctx+0x140 = recorder index, ctx+0x108[index] = per-thread recorder, recorder+0x28 is passed to the engine command helpers.
- **RayTraceProbeComputeRenderPass slot 7 = 0x1402b56d0 = `ret 0`.** Bethesda's RT probe pass records nothing in retail, so reviving their GI (Option A) is likely dead. Plan on our own GI pass.
- D3D12 device and graphics queue: CommonLibSF `RE::CreationRendererPrivate::Renderer::GetSingleton()->GetDevice()/GetGraphicsQueue()`.

## Plugin 0.1.0 (Phase 0 diagnostics)
- `src/main.cpp`: vtable-hooks ShadowMask Execute (slot 7, verified against the stock pointer), logs device, queue and DXR tier on the first frame, and scans recorder/ctx pointer fields (depth 2) for D3D12 command lists. QueryInterface is only called when the vtable lives in a D3D12/driver/Streamline module. No rendering changes.
- Build: PowerShell → `xmake f -p windows -a x64 -m release -y` then `xmake -y` (Git Bash makes xmake pick MinGW). Output is `build/windows/x64/release/StarfieldRT.dll`, staged to `C:\GTS\mods\Starfield RT (Dev)\SFSE\Plugins\`.

## Command list discovery result (plugin 0.1.1, live, 2026-11-17)
- The engine device and command lists go through **Streamline** (`sl.interposer.dll` vtables). Creating lists from the engine device gives the same proxy vtable, so hooks catch the game's calls.
- **ShadowMask Execute records directly into a D3D12 DIRECT command list** during the call (no deferred engine command stream). Each execute makes 3 calls: SetGraphicsRootSignature, SetPipelineState, DrawIndexedInstanced (method mask 0x42002). It is a graphics (full-screen/volume draw) pass, not compute.
- Many ShadowMask executes per frame (8 within ~50 ms) on different worker threads, each with its own command list. Probably one per shadowed view/light/tile; still to identify.
- Consequence: our own work can be recorded into the frame's command list from inside a hooked pass Execute, on that thread's list, before or after the original call.

## ShadowMask is not the sun shadow mask (plugin 0.1.3, live)
- ShadowMask Execute: 1 DrawIndexedInstanced of **36 indices × 1024 instances** (cubes) into a **64×64 viewport, depth-only** (DSV bound, no RTV). It is a coarse caster/occupancy mask (probably for shadow-map culling), not a screen-space shadow buffer.
- The render graph binds targets and viewports **before** calling a pass's Execute, so per-list state has to be tracked continuously.
- Next: the 0.1.4 survey of ScreenSpaceShadows, TiledDeferredContactShadows(+BlurX), TiledDeferredLightingProcessLights, Lighting, SkyLighting, SAO, DeferredGBuffer, DeferredComposite, SunDisk, ThinDeferredLighting, TiledBinning_idTech7SampleIndirectLighting, GatherGeometryInstances, GatherTransforms. Lighting and SAO share Execute 0x142a53bb0 (a generic runner), and TiledDeferredLightingPrepare's slot 7 is a stub.

## Pass survey results (plugin 0.1.4, live, 2026-11-17)
- **Internal render resolution is 1720×720** (DLSS Performance from 3440×1440). Lighting and shadow passes run at 1720×720, and DeferredComposite draws a full-screen triangle with 1 RT.
- Frame order per view: GatherTransforms → GatherGeometryInstances (CPU only, no GPU calls in Execute) → DeferredGBuffer (in Execute: 1 dispatch 64×1×1 plus indirect-arg buffer transitions; the geometry draws happen elsewhere, through indirect/meshlet passes) → TiledBinning_idTech7SampleIndirectLighting (dispatch 143×1×1) → ShadowMask (64×64 depth caster mask) → **ScreenSpaceShadows (2 dispatches, first 64×16×25)** → **TiledDeferredContactShadowsBlurX (dispatch 430×180 with **8×8 threads per group (PSO 11800 NumThreads=(8,8,1)) = 3440×1440, so the contact mask is OUTPUT resolution**, not the 1720×720 viewport. Until 0.1.18 the plugin assumed 4×4 and only covered the top-left quarter)** → TiledDeferredLightingProcessLights (CPU only) → SkyLighting (CPU only) → DeferredComposite → SunDisk (CPU only).
- TiledDeferredContactShadows (non-blur), Lighting and SAO **never ran** through their slot 7 during the capture. Lighting/SAO share the generic Execute 0x142a53bb0, so they are probably subgraph containers.
- ThinDeferredLighting: 256×256 RT, dispatch 32×32×6. This is cubemap-probe lighting (6 faces, 8×8 tiles).
- Passes do almost no resource barriers inside Execute (the render graph does transitions outside), so barriers can't tell us which buffers a compute pass writes. Compute outputs are bound through root descriptor tables, so identifying them needs descriptor tracking (CreateUAV/SRV → CPU handle → CopyDescriptors → GPU handle → SetComputeRootDescriptorTable) or a GPU frame capture (PIX / Nsight Graphics / RenderDoc).
- Candidate RT-shadow integration: the screen-space shadow term produced by ScreenSpaceShadows / TiledDeferredContactShadows (1720×720) that the tiled lighting consumes.

## Swapchain stack / PIX compatibility (2026-11-17)
- The swapchain path in the user's setup: ReShade `dxgi.dll` proxy (from the MO2 mod "Starfield DLSS5", with renodx-dlss.addon64) → Streamline sl.interposer (DLSS, DLSS-G, Reflex, PCL) → driver. The game folder's `nvngx_dlssnr.dll`/`.ini` is a DLSS-NR proxy from the same DLSS5 work.
- The game calls Present at 0x142a0ef43 (`call [rax+0x40]`). On errors other than DEVICE_REMOVED/DEVICE_RESET-type codes it deliberately crashes at 0x142a0ef86 (`mov [0], al`).
- With WinPixGpuCapturer loaded alongside ReShade, Present returns E_ABORT, so PIX captures need the ReShade-based mods disabled.
- Long-term: Starfield RT must coexist with the ReShade/RenoDX DLSS5 mod and Streamline (our hooks sit on the Streamline proxy vtables, which works).

## PIX capture session: temporary changes to undo afterward (2026-11-17)
1. `C:\GTS\overwrite-backup-pix\Root\dxgi.dll` and `renodx-dlss.addon64` → move back to `C:\GTS\overwrite\Root\` (ReShade 6.8 + RenoDX).
2. `C:\GTS\profiles\okok\StarfieldPrefs.ini.bak-before-pix` → restore over `StarfieldPrefs.ini` (frame generation 1, Reflex 2).
3. Re-enable the MO2 mod "Starfield DLSS5" if wanted.
4. Set `[Debug] bLoadPixGpuCapturer=0` in the dev mod's `StarfieldRT.ini`.

## Frame map from the PIX capture (2026-11-17)
Capture: `Documents\StarfieldRT Captures\Starfield_2026-11-17_23-41-01.wpix` (5.6 GB, F10 programmatic, ~2 frames, ReShade off, DLSS-Q upscaling on).
Exported with `pixtool open-capture … export-to-cpp analysis\cpp_export`. Tools (in `tools/`):
- `frame_map.py` builds a pass list (PSO, RT/DS, UAV transitions, table UAVs) → `analysis/frame_map.txt`
- `pso_names.py` names every PSO by matching DXBC container hashes in resources.bin against the game's `shaderspc_dx12_105.fxp` techniques. **399/399 named.** → `analysis/pso_names.tsv`
- `table_dump.py` resolves what a descriptor table points at.

**Lighting pipeline (per view, in order):**
1. G-buffer at internal res (1720×720 used region of 3440×1440 allocations): depth **#629** D32S8, **#631** RGBA8, **#632** R10G10B10A2 (normals), **#633** RGBA8, **#6901** RGBA8, **#630** R16.
2. `ContactShadows#0` (PSO 11820, CS): reads depth, normals and #630, writes the **R8 screen-space contact shadow mask #6857**. `ContactShadows#10` (PSO 11800) blurs it #6857→#6858→#6857 (Dispatch 430×180). No shadow maps involved.
3. `TiledDeferredLight` (PSOs 11822 #578 tile setup, then 11798 #508 / 11809 #510 through ExecuteIndirect): **reads #6857 at t4 space4** (root param 4, table offset 7) plus depth, G-buffer, AO **#6854** (R8 1720×720) and tile data #6833. It writes the direct lighting to **UAVs #6911 RGBA8 + #6912 R16G16**. The sun/local shadow maps are **not in its tables**, so they're accessed bindlessly (SM6.6 ResourceDescriptorHeap).
4. `Indirect#0` (PSO 11844, full-screen draw): writes the indirect lighting RTs **#6907 / #6906 R11G11B10** (probe GI diffuse and specular).
5. `Composite#0` (PSO 11846): combines everything into the HDR scene color **#6902 R11G11B10** (3440×1440).
- Shadow map candidates: **#11026 R16 2048×2048×5** (likely the 5 sun cascades) plus a pool of R16 1K/2K/4K maps (local lights).
- Root parameter 1 of the lighting root signatures = global scene buffers: **#132 4 GB**, #131 512 MB, #142–#146 256 MB, etc. This looks like a shared bindless geometry/instance pool, which is very relevant for building BLASes from the engine's own vertex/index data.
- No RT acceleration structures in the frame (the export's AccelStructure files are empty).

**RT integration points:**
- **Shadows:** after ContactShadows#10, write RT sun visibility into **#6857** (multiply with contact). To remove shadow-map shadows entirely, later neutralize the sun cascades (#11026) so they always read as lit.
- **GI:** replace or augment the `Indirect` diffuse output (#6907/#6906) with RT diffuse GI before `Composite`.
- **Reflections:** replace the probe specular term in `Indirect`'s output with RT reflections before `Composite`.

## Shadow-mask injection works (plugin 0.1.8, live, 2026-11-18)
- After `TiledDeferredContactShadowsBlurY` Execute (vtable RVA 0x4DB4F78, Execute 0x29AD5B0), dispatching our own CS into the blur's output UAV (compute root table 4 + 2 descriptors; descriptor size 32 through Streamline) shows up directly in the lit image. UAV barriers before and after, then restoring the engine's root signature, tables, root constants and PSO, works without side effects.
- R8_UNORM mask: writing 0 darkens (shadow) sunlit areas, so **1 = lit, 0 = shadowed**.
- This is the delivery path for RT sun shadows: write `min(contact, rtVisibility)` here.

## Scene geometry layout, for BLAS/TLAS (from the capture + shader disassembly, 2026-11-18)
Sources: `StandardMaterial` VS (PSO 935) and `GPUCulling_Meshlet#130` CS (PSO 933), extracted with `tools/extract_shader.py` and `dxc -dumpbin` → `analysis/shaders/*.txt`. Struct names survive in the DXIL metadata (`SDrawParams`, `SIndirectDrawIndexedArgumentsDrawID`).

**Drawing:** all scene geometry uses `ExecuteIndirect` with command signature 11738 (2 root constants + DrawIndexed, stride 32), root signature 924, **no vertex buffers**. The index buffer is a per-frame list built by GPUCulling_Meshlet#130 in transient heap 364 (resource #578 at +62455808, R32_UINT). Each index value = `(clusterSlot << 16) | meshLocalVertex`.

**Root table 1 (space 2), same base for geometry and lighting:**
| reg | resource | contents |
|---|---|---|
| t0 | #144 256 MB | instance records, stride 96 (dword +4: low 24 bits / high 8 bits used) |
| t2 | #146 256 MB | cluster/submesh records, stride 20. **+8: bits 0–13 = vertex buffer bindless slot (heap index 32768 + slot); `(v >> 6) & 0xFFFF00` = byte offset; bit 30 = scale ×2** |
| t3 | #11006 1.4 MB | mesh records, stride 52. **+48 = float position scale**, +40 flags, +28 used |
| t5 | **#132 4 GB** | **geometry pool: uint16 triangle index lists** (ByteAddressBuffer) |
| t7 | #11007 9 MB | **per-instance world transform, stride 48 = 3 float4 rows (3×4)** |
| t24 | #131 512 MB | meshlet records, stride 32. **+28 = first index (uint16 units) into #132** |
| t0 space5 | per-draw | `SDrawParams` stride 12: {u32 meshIndex, u32 packed (cluster/instance; >>2 = instance), f32} |

**Vertex positions:** 3 × int16 per vertex, **tightly packed, 6-byte stride**, in a raw buffer at descriptor heap slot `32768 + slot`. Those slots are thousands of separate buffers (#8…#9999, streaming pages), up to 16 MB each. Object-space position = `int16 × scale` (mesh record +48, ×2 if bit 30).
**Triangles:** 16-bit mesh-local indices, ≤64 triangles per meshlet, read 6 × uint16 per lane (2 triangles).

**DXR mapping (no conversion needed):**
- `D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC.VertexFormat = DXGI_FORMAT_R16G16B16A16_SNORM`, `VertexBuffer.StrideInBytes = 6` (the DXR spec allows a 6-byte stride with A16 ignored), `StartAddress = GPUVA(vertex buffer[slot]) + offset`.
- `IndexFormat = DXGI_FORMAT_R16_UINT`, `IndexBuffer = GPUVA(#132) + firstIndex × 2` (needs a submesh's meshlet index ranges to be contiguous; to verify).
- SNORM normalizes by 1/32767, so the geometry/instance transform scale = `scale × 32767` (×2 if bit 30).
- TLAS instance transform = the t7 3×4 row-major matrix (same layout as `D3D12_RAYTRACING_INSTANCE_DESC.Transform`).

**Still unknown (next RE step):** full layouts of the 20/32/52/96-byte records (vertex count, triangle/meshlet counts per submesh, instance → mesh link), and which records/transforms describe all loaded objects versus only visible ones. Runtime plan: hook device `CreateShaderResourceView` for heap slots ≥32768 to map slot → ID3D12Resource/GPUVA, and read the record buffers' GPU addresses from root table 1.

## Record layouts, decoded and validated on captured data (2026-11-18)
Tools: `tools/capture_data.py` (replays the export's resources.bin read order: chunks start at 0, each with a 24-byte Compression API header, XPRESS through cabinet.dll; handles staged upload + CopyBufferRegion and sparse views of huge resources), `tools/records.py`, `tools/mesh_dump.py`, `tools/mesh_validate.py`, `tools/scene_rebuild.py`.

**Bindless handle** (used everywhere): `slot = v & 0x3FFF` → raw SRV at heap index `32768 + slot`; `offset = (v >> 6) & 0xFFFF00` (256-B aligned, up to 16 MB); bit 30 = position scale ×2. Slots 14…268 are **16 MB windows of the 4 GB pool #132** (`slot 14 + k` = bytes `k × 16 MB`); other slots are separate buffers (#133, #134, #137, #139, streaming pages). **Correction (0.1.28 probe):** only slot 14 is confirmed to be the pool base. Reading an index 425 MB into the pool through slot 39 returned zeros, so slots past 14 are not consecutive pool windows. For index data, use the pool's own GPU address (table +7) directly.

**#132 is a reserved (tiled) 4 GB buffer.** Its 64 KB tiles are mapped from heaps as geometry streams in (UpdateTileMappings).

**Mesh record** (#11006, 52 B, ~27.7k): +0 **position stream handle**, +4/+16/+20 further attribute streams (normal/UV/tangent sized, packed after positions), +12/+24 other streams, **+28 bone weight stream** (skinned; dword = boneIndex16 | weight16/65535), **+32 index count**, **+36 first index** (uint16 units from #132 byte 0), +40 flags/bones-per-vertex, **+44 vertex count**, **+48 position scale** (float, e.g. 2⁻¹², 2⁻¹⁵). Typical in-pool layout: [positions 6 B/vertex][attribute streams][uint16 indices], each 256-B aligned. **Validated: 2000/2000 meshes have every index < vertex count.**

**Meshlet record** (#131, 32 B): +12 low 16 bits = index count, +28 first index; other fields = packed bounding sphere/cone floats. A mesh's meshlets are contiguous, but the mesh record already gives the whole range (+36/+32).

**Instance record** (#144, 96 B, ~240k slots): **+16 rotation quaternion xyzw**, **+32 translation xyz**, **+44 uniform scale**, **+4 = firstTransform (low 24) | boneCount (high 8)** (skinned only; indexes the #11007 3×4 palette); +48… previous-frame data. 236,042/236,081 non-zero records have unit quaternions.

**LOD entries** (#146, 20 B): 4 per instance at `instance × 4 + lod`: +4 **mesh index**, +8 position handle (same as mesh +0), +12 previous-frame handle, +16 first meshlet; +0 only on the first.

**World position (rigid):** `rotate(q, int16xyz × meshScale × instanceScale) + t`. Validated: rebuilding all instances within 80 m of instance 250 from the capture gives a coherent city/park layout (`analysis/meshes/scene_250.obj`, `scene_250_top.png`).

**TLAS/BLAS recipe:**
- BLAS per mesh: VertexBuffer = GPUVA(resource behind slot) + offset, stride 6, `R16G16B16A16_SNORM`, VertexCount = +44; IndexBuffer = GPUVA(#132) + first × 2, `R16_UINT`, IndexCount = +32.
- Instance transform = `R(q) × (instanceScale × meshScale × 32767)` (SNORM normalizes) + t. LOD 0 = `lods[i*4]`.
- Skinned meshes (+28 ≠ 0, bones > 0) need a skinning/refit pass later. Rigid first.

## Runtime scene resolution (plugin 0.1.9, live, 2026-11-18)
- IAT hook of `sl.interposer!D3D12CreateDevice` (RVA 0x3A40BB0) at plugin load works. The device is a Streamline proxy; hooking its vtable (CreateDescriptorHeap 14, CBV 17, SRV 18, UAV 19, CopyDescriptors 23, CopyDescriptorsSimple 24) catches every engine descriptor. 6 CBV/SRV/UAV heaps, 3 shader-visible, main heap 98,304 descriptors, increment 32. ~1.7 M descriptor copy calls in the first minute (watch performance).
- The global table (compute root param 1 at BlurY) resolved instances (stride 96, reserved), LOD entries (20, reserved), **mesh records (52, UPLOAD heap, CPU-readable)**, **pool (raw, reserved, gpuva 0x1100000000)**, **transforms (48, UPLOAD)**, meshlets (32, reserved). Bindless slot 14 == pool base.
- Mesh-record/transform buffers were 1,024 / 4,096 entries a few seconds after load, versus 27k / 188k in the capture. They're **replaced as they grow**, so re-resolve every frame (cheap) and rescan meshes periodically.

## BLAS builds work (plugin 0.1.10, live, 2026-11-18)
- Building directly from engine buffers (R16G16B16A16_SNORM stride 6 from bindless slots, R16_UINT from pool #132) on the BlurY command list, 16 per frame, works: 26,616 live BLASes, 423 MB, 0 failures, no device removal.
- The mesh table grows (1,024 → 29,696 slots) and its buffer is replaced as the world streams in. Rescanning on buffer change plus every 60 frames keeps up. Average BLAS ≈ 16 KB (PREFER_FAST_TRACE, no compaction yet).
- Next: TLAS from instance records (quaternion/translation/scale × meshScale × 32767, LOD 0 mesh), then a debug trace pass.

## 0.1.11 GPU fault and 0.1.12 hardening (2026-11-18)
- 0.1.11 died after a few minutes with no crash log. Windows logged **nvlddmkm event 153** (GPU error) 12 s after the last plugin report, mid BLAS fill (8.7k queued). Most likely a BLAS build read freed/streamed-out geometry from a stale queue entry, or the 2-byte over-read at the end of a vertex buffer (R16G16B16A16 at stride 6 reads 8 bytes for the last vertex).
- 0.1.12: re-validates each queued mesh against the live (mapped UPLOAD) record before building, bounds-checks VB (`offset + 6n + 2 <= view size`) and IB, has `bBlasPoolOnly` (default on: slots 14…268 only), and a 30-frame graveyard.
- Lesson: GPU faults kill the game silently. Check `Get-WinEvent System` for nvlddmkm 153/13/4101 after an unexplained exit.

## ContactShadows#0 bindings (PSO 11820, toward the RT sun shadow pass)
From `analysis/shaders/ContactShadows_11820.txt`:
- `b0 space3`: **`PerSceneConstants`**, 53,008 bytes (the big per-scene/view CB: camera matrices and light data should be here; rows 41 and 130 read).
- `b1 space4`: `Data`, 112 bytes (contact-shadow params; rows 0, 1, 2, 3, 5 read: likely light direction and screen-space ray params).
- `b0 space0`: push constants `stub_PushConstantWrapper_TiledContactShadows`, 24 bytes (6 root constants).
- SRVs: t0 space4 `Texture2D<float>` (depth), t1 `Texture2D<float4>` (normals/G-buffer), t2 `Texture2D<uint>`; UAV u0 `RWTexture2D<float>` (the mask).
- Next: decode the PerSceneConstants rows (inverse view-projection, camera position, sun direction) so the RT shadow pass can reconstruct world positions in the same space as the instance translations.

## Camera and sun constants (decoded from the capture, 2026-11-18)
`tools/cb_dump.py` on PerSceneConstants (#1693 + 48833024, 53 KB; CBV at **table 3 + 0**, b0 space3) → `analysis/PerSceneConstants.txt`:
- **row 0: camera world position** (−599.27, 2187.7, 5.242). Same space as the instance translations (world, Z up).
- rows 1–4 / 5–8: view-projection for camera-relative positions (`clip = p.x·r1 + p.y·r2 + p.z·r3 + r4`), jittered/unjittered pair. clip.w = p·forward and clip.z ≈ 0.05, so **reversed-Z infinite projection, near 0.05** (depth = 0.05 / viewDepth; sky = 0).
- rows 9–11 camera basis, **row 16 forward** (−0.832, −0.453, −0.320).
- **rows 13–16: inverse view-projection**: `h = ndc.x·r13 + ndc.y·r14 + depth·r15 + r16; posRel = h.xyz / h.w` (checked: the screen center gives forward × viewDepth). World = posRel + row 0.
- row 39 (near 0.05, far 6000), row 41 projection params; rows 129/130 output size 3440×1440 and its reciprocal; rows 136/137 internal size 1720×720 and its reciprocal. Rows 43+ repeat the block for another view.
Contact-shadow `Data` CB (b1 space4, ContactShadows#0 table 4 + 0, 112 B): **row 5 = direction TOWARD the light in CAMERA space** (x right, y up, z forward), w = 50. Confirmed in 0.1.20 from two F8 probes at one spot with the camera turned 90°: rotated by the camera basis (right = PerScene row 13 orthogonalised against forward = row 16, up = right × forward) both give the same world direction (0.899, 0.383, 0.214) within 0.003. Before 0.1.21 the plugin used −row5 directly as a world direction, which was wrong.

## Alpha-tested materials (0.1.23)

Sources: cutout shadow PSO **971** (StandardMaterial#22 VS + PS; the solid variant is 977), material prepare **963** (GPUCulling_Meshlet#112), args setup **959** (GPUCulling_Meshlet#120). PSO chunks in resources.bin hold every stage of a pipeline in one raw LZ77 Xpress (MS-XCA) stream; Windows' raw XPRESS decompressor rejects them, so they are decoded in Python (scratch `pso_blobs.py`). `tools/extract_shader.py` only finds the first stage.

- **Material index = LOD entry 0, dword 0, bits 13–27.** That dword packs (bucket << 13) | (… ) | (pass << 2) | sub-slot. The culling shader appends instances into the pre-compaction draw-args entry `bucket + MaterialTypeOffsets[pass][sub]`. Shader 959 writes that entry's root constant `dword1 = entryIndex − typeOffset` = the bucket, and the draw shaders read `CommonMaterialEntry[pushConstant.y]`. So the bucket *is* the material index. Draw-args command signature 11738: 2 root constants (b0 space0: drawParams base, material) + DrawIndexed, stride 32.
- **CommonMaterialEntry** (288 B) is draw-shader register t9 space2 = global table **+11** (every scene buffer sits at table offset = register + 2). Fields used: **+56** UV scale (half2: hi = u, lo = v), **+60** UV offset (half2: hi = u, lo = v), **+64** UV set (0 = mesh +4 stream, else mesh +8), **+68** non-zero = multiply by a vertex-colour channel, **+72** that channel (index into [byte2, byte1, byte0, byte3] of the RGBA8 colour stream at mesh **+12**), **+104** alpha threshold (float), **+108** flags (bit 5 = dither), **+208** low 2 bits = sampler, **+276** opacity texture heap index (0 = none, full index into the CBV/SRV/UAV heap), **+280** index into a 32-byte alpha-params buffer (t26 = table +28; entry +4 = mode, mode 3 = remapped alpha using +24/+28).
- **Mesh streams:** +4 UV0 (half2 per vertex, 4 B), +8 UV1, +12 vertex colour (RGBA8). Same bindless handle encoding as positions.
- **Shadow pixel shader test:** `alpha = opacity.r × vertexAlpha` (mode 3 remaps), discard if `alpha < threshold` (plus optional dither/dissolve). The plugin reproduces the main test and ignores dither, dissolve and mode 3.
- **Material families.** The bucket indexes a per-family material table, and the family is the LOD dword's **bits 0–4** (pass << 2 | sub-slot, 14 values). Probe data: buildings are type 0, trees and plants type 3, and some instances type 7. **Type 0 → CommonMaterialEntry** (table +11, 288 B). **Type 3 → vegetation entry** (draw register t11 → table **+13**, 368 B): +24 UV scale, +28 UV offset, +32 UV set, +36 vertex-alpha flag, +40 channel, **+72 threshold**, +184 sampler, **+272 opacity texture**, +276 alpha-params index. The mapping is inferred from the probe data and should be confirmed with the 0.1.23 probe (it logs type and material).
- Trees use the **Vegetation** material (PSO 980, Vegetation#48), whose VS also applies wind. The RT copy is static.
- **Alpha-params entry (table +28, 32 B, index = standard material +280 / vegetation material +276):** +4 mode, +24 p, +28 q. Mode 3 remap (PSO 971/980 PS): `lo = clamp((p−0.5)·2, 0, 0.99902)`, `w = max(min(2p, 1) − lo, 0.001)`, `fade = clamp(vertexAlpha − lo, 0, w) / w`, `t2 = saturate(tex − 1 + 2·fade)`, `c = lo + w/2`, `h = q·w/2`, `hi2 = clamp(c + h, 0.001, 1)`, `lo2 = clamp(c − h, 0, 0.99902)`, `alpha = clamp(t2 − lo2, 0, max(hi2 − lo2, 0.001)) / max(hi2 − lo2, 0.001)`. Otherwise `alpha = tex × vertexAlpha`. Then discard if `saturate(alpha) < threshold`.

## GPU hang on save load (0.1.33 DRED, fixed in 0.1.34)
- DRED: reason `DXGI_ERROR_DEVICE_HUNG`. The GPU stopped in the plugin's BlurY command list at breadcrumb op 31 (`BuildRaytracingAccelerationStructure`), 226 of 538 ops done. Every other list (DXGI, frame pacer, game) was waiting behind it.
- Cause: BLAS builds read the game's vertex and index data directly. Right after a save load (or heavy streaming), a mesh record can exist before its geometry is uploaded or its pool tiles are mapped. Out-of-range indices are undefined in DXR, and in practice the build can hang.
- Fix: a mesh record must stay unchanged for 30 frames before it's built. Each build reads a sanitized copy (shaders/CopyGeometry.hlsl copies the positions and clamps every index into `[0, VertexCount)`) from a 3 × 32 MB staging ring, so the AS builder never touches engine memory.

## Shadow-map render passes (1.16.244, vtables from RTTI, 0.2.5)
- RTTI method: type descriptor `.?AV<Name>RenderPass@CreationRendererPrivate@@` → complete object locator (signature 1, pSelf) → the vtable follows the qword that points to the locator; Execute is slot 7. Verified against the known ShadowMask and TiledDeferredContactShadowsBlurY vtables.
- ShadowMapInstance vtable 0x4EE1FA8 / Execute 0x2B58E50; GroupedDynamicShadowMap 0x4EE2090 / 0x2B63240; ClearShadowMaps 0x4ED7800 / 0x2B5E340; ClearShadowMapGroup 0x4ED77B8 / 0x2B5E990; TerrainShadows 0x4DB2E10 / 0x2996780; SetupDynamicShadow 0x4EE2168 / 0x2B5CE60; ShadowMap 0x4EE1F60 / 0x3663148; ShadowMapSubGraph 0x4EE1E10 / 0x2A53BB0 (generic runner).
- `fMaxShadowDistance:Shadows` changed at runtime does NOT switch off the sun cascades (0.2.4 lit test: with RT forced to fully lit, the game's tree, lamp-post and ground shadows all stayed). The 0.1.24 "it works" impression was wrong: the game's shadows were hidden inside RT's big extra shadow.
- **0.2.5 skip test: ShadowMapInstance draws the sun cascades.** Its draws go into a 4096×4096 DSV at the tester's quality (GroupedDynamicShadowMap draws 2048×2048 maps, likely local lights). Dropping ShadowMapInstance's draw calls (the pass still runs, so state and dispatches stay intact) removes the game's sun shadows completely. Since 0.2.6 the plugin does this whenever RT sun shadows are on.
- **SceneShadowData** (TiledDeferredLight cb1 space3, 1072 B; compute passes share the space3 table, so it is table 3 + 1 at BlurY): `[4 × FocusShadowData (112 B, rows 0–27)] [4 × CascadeData (144 B, rows 28–63)]`, then row 64 = { cascade count, ?, focus-shadow bitmask, ? }. The lighting shader takes `min(contact mask, …)` over `min(row64.x, 4)` sun cascades and over the focus shadows set in row64.z (character shadows). Setting row64.x = 0 in the frame's upload copy makes lighting skip the sun cascades without touching the shadow-map passes (0.2.11). **Dead end (0.2.11-0.2.20):** SceneShadowData comes from a ring buffer at a new address every frame (one 1280-B CBV per frame), and on many frames the data is written after the view is created and was not catchable before submission. Patching only worked on 22-70% of frames, so the game's shadows flickered. The working solution is dropping the ShadowMapInstance pass's draws (`bReplaceGameSunShadows=2`, default since 0.2.21): no flicker, and no Frame Generation hangs, since the 0.2.6-0.2.9 hangs actually came from the RT pass (fixed in 0.2.12).
- Touching the cascade pass itself (dropping its draws in 0.2.6–0.2.9, re-clearing its DSV in 0.2.10) coincided with GPU hangs in the DLSS Frame Generation list on save load. Leaving the pass alone didn't hang.

## Indirect and Composite: where reflections go (2026-11-18, from PSO 11844 / 11846 disassembly)

Shaders were pulled from the capture with the scratch `pso_blobs.py` and disassembled with `dxc -dumpbin`.

- **`Indirect#0` (PSO 11844, full-screen PS) has two render targets.** SV_Target0 is the indirect diffuse (probe/volume GI × exposure). **SV_Target1 is the probe specular**: `prefiltered cubemap radiance × (F0·A + B)`, with (A, B) from a 2D BRDF lookup texture (t13 space4) sampled at (N·V, roughness). This is the classic split-sum. The prefiltered radiance comes from the reflection-probe cube arrays (t7/t8 space4, TextureCubeArray).
- **G-buffer reads in Indirect (all t#, space4, loaded at the pixel):**
  - t6: depth (sky = 0, discarded). (Earlier notes said t11; that was wrong: t11 space4 is a structured buffer. Corrected from the 0.2.24 survey and the PS's first texture load.)
  - t1: **normal**, `.xy` (plus a small blue-noise dither) → `v = xy·4 − 2`, `f = dot(v,v)/4`, `n = normalize(v·sqrt(1−f), −(1 − dot(v,v)/2))` in **view space**, rotated to world by camera rows. The same decode is in Composite (t7 there).
  - t2: `.x` = specular/ambient occlusion (min'd with a sampled AO), **`.y` = roughness** (saturated), `.z` = likely metalness.
  - t4: **material type** (uint, low 4 bits). Types 1 and 8 take roughness from a material table (t14 + 12, indexed by t5.z × 255) instead of the G-buffer: probably skin and hair.
- **`Composite#0` (PSO 11846)** adds the two Indirect targets (sampled at output resolution) to the direct light. Direct diffuse and direct specular are packed per pixel as 14-bit log luminance + 9-bit + 9-bit chroma (t1 R16G16 + t2 RGBA8 = TiledDeferredLight's #6912 / #6911). Indirect diffuse is multiplied by a multi-bounce AO term using the albedo (t3). Output = HDR scene colour.
- **R1 survey (0.2.24, in-game, 3440×1440 output):** `GatherIndirect` makes one `DrawInstanced(3, 1)` with two RTVs, both **R11G11B10_FLOAT 3440×1440** (RT0 diffuse, RT1 specular), viewport 3440×1440, DSV bound. The graphics root signature has 4 tables: param 1 = global scene table (same as compute, +0/+1 CBVs, +34 PerScene 53 KB, +37 SceneShadowData 1280 B), param 2 = sampler table, param 3 = space3 table, **param 4 = the space4 table, with table offset = register number**:

  | t (space4) | format (view) | size | meaning |
  |---|---|---|---|
  | t0 | R8G8B8A8_UNORM_SRGB | full | albedo (likely) |
  | t1 | R10G10B10A2_UNORM | full | normal (.xy octahedral-ish, view space) |
  | t2 | R8G8B8A8_UNORM | full | .x AO, .y roughness, .z metalness? |
  | t3 | R16G16_FLOAT | full | not read by Indirect (motion vectors?) |
  | t4 | R16_UINT | full | material type (& 15) |
  | t5 | R8G8B8A8_UNORM | full | .z × 255 = material table index (types 1/8) |
  | t6 | R32_FLOAT_X8X24 | full | **depth** (D32S8 resource) |
  | t7 | R11G11B10 cube array, 256², 198 faces (33 cubes), 7 mips | | prefiltered probe radiance |
  | t8 | R16_FLOAT cube array, 256², 192 faces | | probe depth/distance? |
  | t10 | 3D 9×9×96 R16G16B16A16_FLOAT | | probe volume / SH data |
  | t13 | R11G11B10 2D 512×512 | | BRDF lookup (split-sum A, B) |
  | t9, t11, t12, t14, t16 | structured buffers | | probe/volume descriptors, material table |

- **RT reflection plan:** after Indirect#0, overwrite its SV_Target1 with `RT radiance × (F0·A + B)` (same split-sum, using the game's own BRDF lookup), blended back to the probe value for rough surfaces. Composite then adds it like the probe specular it replaces, so exposure, AO and tone mapping stay the game's own.

## R2 result: the mirror rays are right (2026-11-19, 0.2.25)

- The normal decode (Indirect's `xy*4-2` view-space decode rotated by PerScene rows 17..19) gives smooth, consistent world normals: floors facing +Z, walls by facing direction.
- Mirror rays from the depth-reconstructed position (rows 13..16), offset `0.05 + 0.002·dist` along the normal, give a correct reflection: on a curved advert pillar in New Atlantis the view showed the room behind the camera upright, with the right curvature, including a standing character.
- Whatever the plugin writes to Indirect SV_Target1 is added by Composite more or less as-is: the debug colours showed at full strength on matte walls too. So the final pass must apply the split-sum weight (`F0·A + B`) itself, as the game's shader does, and blend toward the probe value on rough surfaces.
- The roughness view barely changed the image (a faint grey lift), which suggests that much of the scene is fairly smooth or that Composite scales the specular down. Check this again when R3 lands.


## Albedo at a ray hit (2026-11-19, from the G-buffer PS, StandardMaterial PSO 12209)

- The G-buffer is drawn on its own list by StandardMaterial / LayeredMaterial / Vegetation / Eyes / Mouth / MeshInstancePatch PSOs (12204-12238) through ExecuteIndirect into #631 RGBA8 (albedo), #632 R10G10B10A2 (normal), #633 RGBA8 (AO/roughness/...), #6901 RGBA8. The many StandardMaterial PSOs with no PS, or with no outputs (935, 11751, 12343...), are depth and shadow variants.
- **CommonMaterialEntry (global table +11, 288 B), G-buffer fields:** +108 flags (bit 4 = use instance byte 1 as SV_Target2.w), +112 bit 0 = UV set (0 = mesh +4 stream, 1 = mesh +8), **+116 = texture-set index**, +136 bit 0 (branch), +160 float, **+168 UV scale (half2, hi = u, lo = v), +172 UV offset (half2)**. These are not the alpha test's +56/+60 transform. +208 low 2 bits = sampler (s5+n, space2).
- **Texture set** (t25 space2 = global table **+27**, 40 B): +0 **albedo**, +4, +8, +12, +16 further textures (normal / roughness and so on: full heap indices, 0 = none), +28 flags (bit 1 = apply mip bias), +32 float.
- **Albedo** = `set+0 texture .rgb`, then a lerp toward another value under some flags, then **× instance tint**: instance record (table +2, 96 B) +12 low 16 bits indexes t4 space2 (table **+6**, 32 B), whose dword 0 holds the tint as bytes (R = bits 16-23, G = 8-15, B = 0-7) / 255.
- 0.2.26 uses this for the R3 view (types other than 0 get a flat colour).
- **R3 result (0.2.26, New Atlantis):** the albedo path is right. Floor-tile patterns, arches and kiosks show in the Trident pillar, and sushi adverts mirror correctly in a glossy black ceiling panel. The reflected texture edges look slightly stair-stepped (the distance-based mip guess, or low-res streamed mips). Ray cones in R5 should fix it.

## Indirect#0 specular, exactly (2026-11-19, PSO 11844, used by R4 in 0.2.27)

`SV_Target1 = (F0'·A + B) × P × O × E`, where:
- `F0 = lerp(c, albedo(t0 space4), metal(t2.z))`, with c = 0.04 (0.028 for material type 3);
- `F0' = F0 + (max(1 − rough, F0) − F0)·(1 − N·V)^5`, using N·V clamped to [0, 1];
- `(A, B) = t13.Sample(N·V, clamp(rough, 0.045, 0.95)).xy`;
- P = prefiltered probe radiance (cube arrays t7/t8, blended over probes);
- O = specular occlusion: `min(cone-based term mixed by row 356.x, 1 + AO·k·row 356.y)`, built from the AO in t2.x and the sampled AO;
- E = exposure: `row311.y` if > 1e-10, else `row311.z / max(1.2·row311.x, eps)`, clamped to `[row312.y, row312.z]` (PerScene rows).

R4 recomputes W = F0'·A + B and E, recovers `P·O = G / (W·E)` from the game's own output, and writes `lerp(G, W·E·L_rt, k(rough))`.

## Load hangs and TLAS reallocation (2026-11-19)

- Both Frame Generation load hangs after 0.2.12 (0.2.22 and 0.2.29) came right after a burst of `TLAS buffers for N instances` lines, with N rising 1,024 per line, several times a second. Each step reallocated about 110 MB (result, scratch and descriptors), and the graveyard keeps each replaced set for 30 frames. In 0.2.29 RT work was paused for the whole load, so the rays weren't the cause there.
- 0.2.30 grows the TLAS buffers by 1.5× in 16,384-instance steps. If loads still hang, the next suspects are VRAM pressure in general (BLAS pool + staging + reflection buffers) and the TLAS build itself on mid-load garbage.

## Skinning (2026-11-19, from the StandardMaterial VS, PSO 935; used from 0.2.32)

- Position: int16 xyz at the **LOD entry's** position handle (+8; bits 0-13 slot, `(h >> 6) & 0xFFFF00` offset) × mesh scale (+48), ×2 if handle bit 30 is set. **Bit 30 also means "already skinned"**: the VS skips skinning for those (the game poses them itself, see UpdateSkinnedGeometryInstances).
- Otherwise: bones per vertex = min(mesh +40, 8). The weight stream (mesh +28 handle) holds one dword per (vertex, j) at `base + (bpv·v + j)·4`: low 16 bits = bone index (clamped to boneCount − 1), high 16 bits = weight / 65535. Bone matrix = transforms (t7 space2 = global table **+9**, 48 B, 3 × float4 rows) at `(instance +4 & 0xFFFFFF) + index`, where boneCount = instance +4 >> 24. Skinned position = Σ wⱼ · Mⱼ · (p, 1).
- Then the usual instance transform: × uniform scale (+44), rotated by the quaternion (+16), plus the translation (+32). The VS works camera-relative (minus cbuffer row 12).
- 0.2.32 builds float3 BLASes from that output, one per character instance near the camera, every frame (PREFER_FAST_BUILD). The TLAS instance uses `R·s` (no mesh scale or 32767 factor) with the slot's BLAS.


## Gameplay Options records (2026-11-19, used from 0.2.36)

Mods add entries to Settings > Gameplay Options with two record types (first shipped in `SFBGS007.esm`; 452 options in 86 groups across the installed mods):

- `GPOF` (BGSGameplayOption): `EDID`; `NNAM` name and `DNAM` description (plain strings in a non-localized plugin); `TNAM` type (1 = value list, 0 = toggle); `VNAM` default and `WNAM` (always equal to VNAM) as floats; `GPOD` = min, max, step (floats); then one entry per choice: `VOVS` (empty string), `VORV` value (float), `RESN` label, `VORN` (always 0).
- `GPOG` (BGSGameplayOptionGroup): `EDID`, `NNAM` group title, `BNAM` byte (0 in most), `GOGL` = list of GPOF form IDs. A group with no parent shows up on its own in the menu.
- No field links an option to a global or a game setting. Mods read them from Papyrus: `GameplayOption.GetValue()`, `RegisterForGameplayOptionChangedEvent()`, `Event OnGameplayOptionChanged(GameplayOption[] aChangedOptions)`, then `GameplayOption.NotifyGameplayOptionUpdateFinished()`. The game saves the chosen values.
- Starfield RT's bridge: quest `SFRT_OptionsQuest` (start game enabled, `DNAM` 0x0111 as the CK writes) copies the options into globals; the DLL finds them with `TESForm::LookupByEditorID` (address ID 47403, present for 1.16.244) and reads `TESGlobal::value` (+0x40).
- A quest's `VMAD` needs no fragment section when the quest has no stage fragments: version 6, object format 2, script count, then per script name, flags byte, property count and properties (`name`, type 1 = object, status 1, `uint16 0, int16 -1 alias, uint32 formID`).
- Record header: form version 0x0240. Group stamp 0x3186. TES4 flags 0x101 = master + light; light-plugin form IDs are 0x800–0xFFF.

## Transparent passes and glass (2026-11-19, from the frame map and RTTI; used by G1 in 0.2.38)

- Frame order after the opaque scene: Composite → atmosphere and sky → volumetrics → LayeredEffect#28 (depth only) → Refraction* (mask, dilation, downsample, copy, blur) → Water → particles → LayeredEffect#2 into **two half-res moment targets** (R32G32B32A32 + R32, 1720×720 at 3440×1440) → LayeredEffect#4 into a **full-res R16G16B16A16** transparency target → **CompositeMomentBasedOIT** (fullscreen triangle) into the **HDR scene target (R11G11B10)** → a few more LayeredEffect#4/#6 draws straight into the HDR target → lens flare, glare.
- Every LayeredEffect draw is an ExecuteIndirect.
- Render pass classes (RTTI) include ForwardBlended{MainFullRes, MainHalfRes, FullResAlways, Correction, CopyToDest, AlphaOutput}, UnsortedForwardBlended{PreMBOIT, PostMBOIT, PreClouds}, MomentBasedOIT{RenderMoments, RenderTransparency, RenderAdditiveTransparency}, CompositeMomentBasedOIT, DeferredGBufferTranslucent and RefractionMask.
- **Finding vtables:** the REL ID in CommonLibSF `IDs_VTABLE.h` → the address library (format 5: uint32 RVA table after a 96-byte header) → the vtable RVA; Execute = the pointer at vtable + 0x38 in the exe image. `tools/pass_vtables.py` does this; GatherIndirect gives 0x4DB3C48 / 0x299F180, matching the hand-found values.

## G1 survey result (2026-11-19, 0.2.38 log)

- **DeferredGBufferTranslucent** (ExecuteIndirect, 4 RTs at full res) writes into the **main G-buffer textures**: RT0 albedo (R8G8B8A8_SRGB) = GatherIndirect space4 t0, RT1 normal (R10G10B10A2) = t1, RT2 material = t2, RT3 = t5. Thick glass therefore goes through deferred lighting and the RT reflection pass.
- **MomentBasedOITRenderMoments**: 2 RTs at half res (the moment targets). **MomentBasedOITRenderTransparency**: 1 RT, full-res R16G16B16A16_FLOAT; one table-4 block for the whole pass (heap indices alternate between two blocks per frame).
- Transparency table 4: +7 = **reflection-probe cube array** (TextureCubeArray, R11G11B10, 256×256, 198 faces, 7 mips; the same resource as GatherIndirect's t7); +8 = R16_FLOAT cube array (192 faces, same as GatherIndirect t8); +9 = 512×512 R11G11B10 (same as GatherIndirect t13, the BRDF LUT); +14 = depth (R32_FLOAT_X8X24); +15 = scene color with 6 mips (refraction source).
- Every pass that uses probes gets its own table blocks, so swapping +7 in the transparency pass's block touches only that pass (G2).
- (0.2.39 log) Some glass materials (the thick doors) bind a single **TextureCube** at transparency table 4 +7 instead of the probe array: a static per-material environment map. G2 blanks both kinds (0.2.40).
- (0.2.40 test) With RT reflections off and the glass probes hidden, the blurry shapes on the thick doors stay: they are the **interior seen through the glass** (the game's refraction blur, using the scene-color mip chain at transparency table 4 +15), not a reflection. G2 is complete for both thin glass and the doors.

## Glass in the TLAS (2026-11-19, 0.2.43 F8 probe grid)

- Thin window panes (mesh 3553, 2 triangles, ~4 m, material 281) are in the TLAS but not in the depth buffer, and blocked sun rays 0.2–7.5 m from the visible surface.
- CommonMaterialEntry **+108 flags**: panes **0x12**, solid buildings **0x11**, a skinned NPC **0x13**. Working assumption: bit 0 = drawn as a solid (G-buffer) surface. From 0.2.44, standard materials with bit 0 clear get TLAS instance mask 0x02 (glass), others 0x01; shadow and reflection rays trace mask 0x01. Still to confirm: thick glass that also writes the G-buffer (DeferredGBufferTranslucent).

## Thick glass in the G-buffer (2026-11-19, 0.2.45 debug views at the doors)

- Coverage: the doors are fully replaced by RT. Roughness: fairly smooth but not mirror-smooth, with smudgy variation. Mirror hit: a crisp, correct reflection. So their blur is the RT glossy reflection of the game's roughness, not the game's reflection.
- The door glass has a non-zero G-buffer material type (t4 low 4 bits); the frame around it is type 0. 0.2.46 adds a type legend view (F2 mode 7) and `iSmoothGlassTypes` / `fGlassRoughnessScale` to make chosen types reflect sharply.
- (0.2.46 type view) **G-buffer material type 2 = glass** (the thick doors); characters are type 4; most solid surfaces are type 0. From 0.2.47 type 2 reflects with roughness × fGlassRoughnessScale (default 0).
- (0.2.48) The streaks on the doors were alpha-tested foliage (palm cards) hit as solid quads by reflection rays. From 0.2.49 reflection rays run the shadow pass's alpha test.

## OIT composite inputs (2026-11-19, 0.2.50 log; used by G3 in 0.2.51)

- CompositeMomentBasedOIT (fullscreen DrawInstanced 3) writes RT0 = the HDR scene target (R11G11B10, full res). Root table 3: +0 depth (R32_FLOAT_X8X24, full res), +1 R32G32B32A32_FLOAT half res (higher moments), **+2 R32_FLOAT half res = b0** (the zeroth moment, the same resource as MomentBasedOITRenderMoments RT1), +3 R16G16B16A16_FLOAT full res (transparency accumulation).

## Scene table location (2026-11-19, 0.2.53 log)

- The global scene table moves to a new slot in the 98304-descriptor shader-visible heap **every frame** (heap indices 64517, 68613, 72197, 74536 within seconds). A cached handle from an earlier frame points at reused slots.
- At some locations, TiledDeferredContactShadowsBlurY's compute root table 1 is not the scene table (a handle in the sampler-heap range, 0xc5678a…). From 0.2.54 the RT build falls back to the newest root-table-1 handle bound by any pass that validates as the scene table.

## Local lights (TiledDeferredLight, PSO 11809 disassembly, 2026-11-20)

The main view's tiled lighting CS (PSO 11798 / 11809, NumThreads 32×1×1) reads every local light from constant buffers in its compute root table 4 (space4, table offset = register + 3 for SRVs, CBVs first):
- **cb0 space4 (+0), 64 KB:** per-light shadow data (8 rows per light, same index as the light records). Used when light flag bit 20 is set.
- **cb1 space4 (+1), 40 KB:** 5 rows per light (light index × 5): projector/gobo and clip-box data (flags bits 21–24).
- **cb2 space4 (+2), 64 KB = 512 lights × 8 rows (128 B):** the light records.
- **t0 space4 (+3), one 48-byte struct:** index ranges into the light records. **[+0,+4) point lights, [+8,+12) box-bounded directional lights, [+16,+20) spot lights**, +32 a single extra light index (valid if +36 ≠ 0; probably the main directional), +40 = number of 32-bit words in the per-tile light bitmask (t1 space4, 128 B per tile).
- **Positions and directions are in camera space** (x right, y up, z forward = linear depth), the same as the pixel position the shader reconstructs.

**Light record (cb2 space4, rows of float4):**
- row 0: .x flags (bit 20 = has shadow data in cb0, bit 21 = projector in cb1, bit 22 = gobo texture with heap index in row0.w >> 8, bits 24/25 = clip box tests), .y = light-channel mask (ANDed with the G-buffer material word bits 4–7), .z = falloff k, .w bit 0 = no inverse-square term.
- row 1: .xyz = direction (spot axis / directional light direction), .w = near offset along it.
- row 2: .xyz = position.
- row 3: .xyz = **colour** (the light term is colour × attenuation × N·L; row 3.w unknown).
- row 4: .x / .y = outer / inner radius for box lights, .y = cone cosine start for spots.
- row 5: .w = two halves: high = falloff exponent p, low = cone/radial exponent.
- row 6: .w = cone cosine end for spots.

**Attenuation (as the game computes it):**
- falloff = saturate(1 − (k·d²)^p), squared, times 1/(d²+1) unless row0.w bit 0. d = distance (points, spots) or the distance along the axis (box lights).
- spot: a = saturate(2·(dot(P − pos, axis) − row1.w)), times (3 − 2a)·a², times cone = saturate(saturate((cosθ − row4.y) / max(row6.w − row4.y, 1e-4))^plo).
- box light: L = −axis; radial term t = saturate((|axis × (P − pos)| − row4.y) / (row4.x − row4.y)), smoothstepped, ^plo.

The plugin (0.2.58) catches table 4 in SetComputeRootDescriptorTable by its 40 KB CBV, requires a render-resolution UAV texture in the same table (so the cubemap-probe lighting views are ignored), and passes the light CBV and the range buffer to the reflection passes.

## Effects vs glass in the TLAS (2026-11-20, 0.2.62 F8 glass dumps in Neon)

- A smoke sheet in the Neon walkway (drawn in the OIT pass): **LOD dword 0 = family 0, material 0**. Standard material entry 0 is blank (all zeros except +160 = −1.0f and +168 = half2(1, 1)), so flags +108 = 0, and the 0.2.44 rule (bit 0 clear = see-through) tagged it as glass. From 0.2.64, LOD dword 0 → instance mask 0x04 (effects), which reflection and glass rays skip.
- A Neon shop window: LOD dword 0xd14d = **family 13**, material 6. Not tagged as glass (the rule only looks at family 0); the glass pass found it with its "anything in front of the opaque surface" fallback ray. Family 13's material table isn't decoded yet.

## Indirect diffuse output (Indirect#0 PSO 11844 PS, re-read 2026-11-20 for GI)

- SV_Target0 = diffuse colour × (1/π) × irradiance (probe/volume GI) × AO-ish terms × exposure (PerScene row 311/312 path), with **diffuse colour = albedo (t0) × (1 − metalness (t2.z))**. So RT GI can write `diffuse colour × mean incoming radiance × exposure` (cosine-weighted rays: irradiance / π = mean radiance).

## Emission (2026-11-20, looking for emissive materials)

- In the PIX capture, the main G-buffer draws are StandardMaterial#10 (PSO 12209, ~4800 draws; 12214; 12231) and LayeredMaterial#8.
  - They write 3 targets: #631 albedo, #632 normal, #633 material (AO, rough, metal, w).
  - PSO 12209's PS has no emission term.
  - #6901 (4th G-buffer target) is written only by terrain, eyes, mouth, layered and vegetation variants.
- No pass in the capture draws material emission into the HDR scene colour #6902. The capture location had no emissive surfaces, so the emissive permutation is not in it.
- To decode: capture (or survey in-game) a draw of a glowing sign in Neon.
- Until then (0.3.3), GI gets emission from last frame's final image (screen-space light feedback).

## Material emission (2026-11-20, 0.3.5 F8 dumps in Neon)

- **CommonMaterialEntry (global table +11, 288 B) emission block**, zero on non-emissive materials. Neon sign, material 734:
  - **+0** luminous emittance (float, 159.5);
  - **+4** exposure offset in EV (3.5);
  - **+8** max offset (FLT_MAX);
  - **+16** flags (0xd801; 0 = no emission);
  - **+20** 0x2e1 (unknown);
  - **+28** tint (RGB = bytes 2,1,0: 0x75, 0xff, 0x0c).
- Other differences seen on the sign: +76..+88 floats (3.69, 0.068, 0.03, 4.0) and +140 (a float on the sign, a colour 0xff272a2c on the wall); not decoded.
- **Texture set (global table +27, 40 B = 10 dwords):**
  - **+0** albedo (heap index);
  - **+4** normal;
  - +8 / +16 / +20 engine defaults (0x12, 0x13, 0x13);
  - +12 a material texture;
  - **+24 emissive texture**: the sign reuses its albedo texture (same index); the wall has 0x10 = the engine's "none" placeholder;
  - +28 count (3 vs 2);
  - +32 = 1.0f;
  - +36 = 0x14.
- A glowing screen/panel in Neon is **material family 7** (LOD dword 0x1c6307, material 227). Its G-buffer reads black and fully metallic. The family 7 table isn't identified yet.
- 0.3.6 shades emission at ray hits as tint × emissive texture × 2^(exposure offset) / exposure ("adaptive", default) or × luminous emittance (iEmissiveModel=1), times fEmissiveScale. The exact game formula is still to be confirmed against the on-screen colour (F8 logs both).

## Direct lighting conventions and switching the game's lamps off (0.4.0)

- TiledDeferredLight (PSO 11809):
  - Diffuse = albedo × (1/π): the constant 0x3FD45F30 multiplies the albedo channels.
  - Specular F0 = (albedo − 0.04) × metal + 0.04.
  - Light term = colour (row 3) × attenuation × saturate(N·L).
- The lamp index ranges (t0 space4, 48 B) are in a CPU-written upload buffer. Writing zeros into dwords 0..5 (point, box, spot ranges) after the game fills them, at the moment the main view's lighting table is bound, makes the tiled lighting skip all local lamps.
- 0.4.0 does this only while its own lamp pass ran in the last 3 frames, and keeps a CPU snapshot of the ranges for its own passes.
- Unknown: whether OIT transparency, volumetrics or other passes read the same range buffer (they'd lose lamp light).
- (0.4.2 log) The lamp records CBV and the ranges SRV are **not** in any CPU-writable buffer created through `CreateCommittedResource` (or 1/2/3) or `CreatePlacedResource` (or 1/2) on the hooked device. So they're GPU memory, or created out of sight. 0.4.3 copies them with a compute shader at bind time, and switches the game's lamps off by writing a new SRV into its table slot +0 (ranges) that points at an emptied copy.

## DLSS Ray Reconstruction feasibility (2026-11-20)

**What the game folder has** (`C:\Program Files (x86)\Steam\steamapps\common\Starfield`):
- Streamline 2.14.16: sl.interposer, sl.common, sl.dlss, sl.dlss_g, sl.reflex, sl.nis.
- An old sl.nrd 2.2.0.
- nvngx_dlss / nvngx_dlssg 310.9.16 (originals 310.9.1 kept as `.original`).
- The tester's DLSS-NR proxy (nvngx_dlssnr.dll + .ini) and its ReShade add-on dlssnr-companion.
- **Missing for RR:** the Streamline DLSS-RR plugin and the RR model (expected names sl.dlss_d.dll and nvngx_dlssd.dll; the RR guide doesn't name them).

**Streamline RR, per NVIDIA's ProgrammingGuideDLSS_RR.md (v2.14.16):**
- `sl::kFeatureDLSS_RR` (`sl_dlss_d.h`). When on, it completely overrides DLSS-SR (same quality mode and output size).
- Calls: `slDLSSDGetOptimalSettings`, `slDLSSDSetOptions(viewport, sl::DLSSDOptions)`, `slEvaluateFeature(kFeatureDLSS_RR, …)`.
- Options: `colorBuffersHDR` must be true; `normalRoughnessMode` packed or unpacked; `worldToCameraView` / `cameraViewToWorld` are row-major, without jitter.
- **Required tags, all at render resolution:**
  - `ScalingInputColor`: the noisy RT-lit HDR image;
  - `Albedo`: diffuse, linear;
  - `SpecularAlbedo`: linear, an EnvBRDF approximation;
  - `NormalRoughness` (packed), or `Normals` plus `Roughness`;
  - `MotionVectors` and `Depth` (or `LinearDepth`);
  - `SpecularMotionVectors` or `SpecularHitDistance`;
  - `ScalingOutputColor` at output resolution.
- **Optional tags:** transparency layer and opacity, colour before transparency, SSS and DOF guides.
- The guide doesn't mention Frame Generation and doesn't say whether to remove the game's own denoisers.

**What we have, or can make:**
- Input colour, depth and motion vectors: the game already tags these for DLSS-SR.
- Albedo, specular albedo and normal + roughness: a compute pass over the G-buffer (albedo × (1 − metal); EnvBRDF from F0, roughness and N·V; decoded normal + roughness).
- Specular hit distance: our reflection trace (store t).

**Integration needed:**
1. Hook sl.interposer's exports: slInit (add kFeatureDLSS_RR to featuresToLoad), slDLSSSetOptions (mirror into DLSSDOptions), slSetTag / slSetTagForFrame (learn the game's tags), slEvaluateFeature (run RR in place of DLSS).
2. A "raw" mode for our passes (skip temporal and spatial filtering) so RR gets the noise it is trained on.

**Risks:**
- How it combines with the DLSS-NR proxy and RenoDX.
- Frame Generation.
- Exact plugin and model versions must match Streamline 2.14.16.


## Skinned characters: first- vs third-person player body (0.5.52-0.5.54)

- The player is two sets of skinned instances at the same spot: the third-person body (with face, hair, eyes) and the first-person rig (suit body, gloves, pack, **no head**). Instance ids change per session.
- **LOD record dword 2** (position handle, `Lods[id*80+8]`): **bit 30 set = the game GPU-skinned this instance this frame** (low bits then point at a per-frame skinned buffer, e.g. `c1bfc003`). Only drawn instances are skinned: the unused body has it clear (`1b1580d0`, `00000088`, ...). Some NPC head parts (eyes, hair: VS-skinned) never have it set even on screen.
- **Root rotation:** characters' instance quaternions are yaw only `(0, 0, z, w)`. The first-person rig's is tilted, y about -0.17 (same in first and third person).
- Instance record dwords 12-19 (+48) hold the previous frame's quaternion/translation/scale. Records are in a DEFAULT heap (not CPU-readable); the probe copies them on the GPU.
