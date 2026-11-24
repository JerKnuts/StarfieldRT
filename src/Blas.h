#pragma once
// Bottom-level acceleration structures built directly from Starfield's own GPU geometry.
// See RT_FINDINGS.md "Record layouts" for the data this reads.

#include <Windows.h>
#include <d3d12.h>

#include <cstdint>

namespace rt
{
    struct BlasSettings
    {
        bool enabled = true;
        int  buildsPerFrame = 128;   // BLAS builds recorded per frame at most
        int  budgetMB = 2048;       // total BLAS memory cap
        int  scanInterval = 10;     // frames between mesh-record scans (0.5.11: 60 -> 10, less pop-in)
        bool poolOnly = false;      // 0.1.27: default off (1 = only meshes whose positions live in the persistent geometry pool)
        bool tlasEnabled = true;
        int  tlasMaxInstances = 262144;  // TLAS instance slots (instance records beyond this are ignored)
        int  tlasMaxExtent = 1000;       // leave out instances larger than this (world units half-extent): sky/planet shells
        bool alphaTest = true;
        bool glassSeparate = true;       // 0.2.44; 0.2.50: on, only reflection rays skip the tagged see-through materials: glass (material flags +108 bit 0 clear) gets instance mask 0x02, solid 0x01
        float skinRadius = 20.0f;        // 0.2.32: skinned characters within this distance join the RT scene (0 = none)
        int   skinSlots = 32;            // 0.2.32: at most this many skinned instances (max 128; a character is several)
        int  tlasCullDistance = 1500;    // 0.2.3: instances farther than this from the camera are left out (0 = off)           // 0.1.23: alpha-test materials with an opacity texture (leaves, grass, fences)
    };

    // GPU address of the most recently built TLAS (0 until the first build).
    D3D12_GPU_VIRTUAL_ADDRESS GetTlasAddress();

    void SetBlasSettings(const BlasSettings& s);
    void RequestSkinDump();  // 0.5.52: log the game's records of every skinned character copy (Numpad . probe)

    // 0.1.23: live alpha-test on/off (hotkey). Returns the new state.
    bool ToggleAlphaTest();
    bool AlphaTestOn();
    bool ToggleGlassSeparate();  // 0.2.44

    // 0.2.12: true while the scene is still filling in after a load (many BLAS builds queued or records settling).
    bool SceneLoading();
    // 0.3.7: true while loading and for ~3 s after the scene reports ready (extended by TLAS reallocations in that
    // window). Every save-load GPU hang so far happened in the first second after "ready", in a reflection/glass/GI pass.
    bool SceneSettling();

    // Once per frame, on an engine command list that is about to run compute work.
    // `globalTable` is the scene root table (compute root parameter 1, space 2).
    // Returns true if it recorded any commands (the caller must then restore compute bindings).
    // 0.2.3: perScene = PerSceneConstants CBV (camera position for distance culling; 0 = no culling).
    bool BlasFrameUpdate(ID3D12Device* device, ID3D12GraphicsCommandList* list, D3D12_GPU_DESCRIPTOR_HANDLE globalTable,
        D3D12_GPU_VIRTUAL_ADDRESS perScene,
        std::uint64_t frame);
}
