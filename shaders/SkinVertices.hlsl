// 0.2.32: skinned characters (NPCs, the player) for the ray-traced scene. One dispatch per skinning slot writes the
// instance's current object-space vertex positions (float3, 12 B) for a per-slot BLAS. Decoded from the game's
// StandardMaterial vertex shader (analysis/shaders/StandardMaterial_935_VS.txt):
//   position  = int16 xyz (6 B/vertex) at the LOD position handle (LOD entry +8: bits 0-13 bindless slot,
//               (h >> 6) & 0xFFFF00 byte offset, bit 30 = already skinned and scale x2) x mesh scale (mesh +48)
//   skinning  = sum over j < min(mesh +40, 8) of weight_j x bone_j (3x4 rows) applied to the position, where the
//               weight stream (mesh +28 handle) holds one dword per (vertex, j): low 16 bits bone index (clamped to
//               boneCount - 1), high 16 bits weight / 65535; bone_j = transforms[(instance +4 & 0xFFFFFF) + index]
//   The instance transform (quaternion, translation, uniform scale) is applied by the TLAS instance, as for rigid meshes.

ByteAddressBuffer   Instances  : register(t0);  // global table +2, 96 B
ByteAddressBuffer   Lods       : register(t1);  // +4, 20 B, 4 per instance
ByteAddressBuffer   Meshes     : register(t2);  // +5, 52 B
ByteAddressBuffer   Transforms : register(t3);  // +9, 48 B (3 x float4 rows)
ByteAddressBuffer   IndexPool  : register(t4);  // global table +7 (16-bit indices), 0.2.35
RWByteAddressBuffer Out        : register(u0);  // float3 per vertex
RWByteAddressBuffer OutIndices : register(u1);  // 0.2.35: uint32 per index, clamped to VertexCount - 1
// 0.5.50: per slot (32 B): float3 pose signature, frames frozen while others moved, instance id. The player has two
// bodies (first- and third-person); the game animates only the one in use, the other stays in its bind pose (T-pose) and
// cast a second, T-pose shadow in third person. 0.5.51: a body near the camera is collapsed once it has stayed frozen
// through kFrozenFrames frames in which some other character moved. When the whole game stops (pause, photo mode)
// nothing moves, so nothing new is collapsed; a body already collapsed comes back when its own pose changes.
// At 4096: the last frame in which any character's pose changed.
RWByteAddressBuffer SlotState  : register(u2);

cbuffer Params : register(b0)
{
    uint Instance;
    uint Mesh;          // LOD 0 mesh of the instance
    uint VertexCount;
    uint TransformCount; // entries readable in Transforms
    uint IndexCount;     // 0.2.35
    uint FirstIndex;     // 0.2.35: first 16-bit index in IndexPool
    uint Slot;           // 0.5.50
    uint Frame;          // 0.5.51: frame number (low 32 bits, never 0)
    uint Dump;           // 0.5.53: 1 = copy this instance's records to SlotState 8192 + Slot * 176 (probe)
};
cbuffer PerScene : register(b1) { float4 PerSceneRows[1]; };  // row 0 = camera world position

static const uint  kFrozenFrames = 60;
static const float kFrozenRadius = 6.0f;

static const uint kBindlessBase = 32768;

uint LoadU16(ByteAddressBuffer b, uint byteAddr)
{
    const uint w = b.Load(byteAddr & ~3u);
    return (byteAddr & 2u) ? (w >> 16) : (w & 0xFFFFu);
}

float3 SkinVertex(uint v);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint v = tid.x;
    // 0.2.35: copy this slot's indices out of the shared pool, clamped to the vertex count. An index past the
    // vertices is undefined behaviour for a BLAS build and hung the GPU (the pool range can change under a
    // character whose mesh streams while it holds a slot).
    if (v < IndexCount)
        OutIndices.Store(v * 4, min(LoadU16(IndexPool, (FirstIndex + v) * 2), VertexCount - 1));
    // 0.5.50: frozen-pose check (state as of last frame; thread 0 updates it, the other threads may see either value).
    const uint so = Slot * 32;
    const uint4 st = SlotState.Load4(so + 12);  // frames unchanged, instance id
    const float3 t = asfloat(Instances.Load3(Instance * 96 + 32));
    // 0.5.120: never a body the game skinned this frame (LOD +8 bit 30): the game only skins what it draws, and the
    // player's unused body never has the bit (RT_FINDINGS). Mannequins are drawn but never move, so while NPCs walked
    // around them they were collapsed after 60 frames: no mannequin in reflections or shadows (soak test, Neon).
    const bool gameSkinned = (Lods.Load(Instance * 80 + 8) & 0x40000000u) != 0;
    const bool frozen = !gameSkinned && st.y == Instance && st.x >= kFrozenFrames && length(t - PerSceneRows[0].xyz) < kFrozenRadius;
    if (Dump != 0 && v < 44)  // 0.5.53: instance record (24 dwords) then LOD record (20 dwords)
    {
        const uint src = v < 24 ? Instances.Load(Instance * 96 + v * 4) : Lods.Load(Instance * 80 + (v - 24) * 4);
        SlotState.Store(8192 + Slot * 176 + v * 4, src);
    }
    if (v == 0)
    {
        float3 sig = 0;
        [unroll] for (uint k = 0; k < 4; ++k)
            sig += SkinVertex(min(VertexCount - 1, VertexCount * k / 4)) * float(k + 1);
        const float3 prev = asfloat(SlotState.Load3(so));
        const bool same = st.y == Instance && all(sig == prev);
        uint count = 0;
        if (same)
        {
            // frozen: count this frame only if some other character moved in this or the previous frame
            const uint lastMove = SlotState.Load(4096);
            count = st.x + ((lastMove + 1u >= Frame && lastMove != 0u) ? 1u : 0u);
            count = min(count, 100000u);
        }
        else
        {
            uint dummy;
            SlotState.InterlockedMax(4096, Frame, dummy);
        }
        SlotState.Store3(so, asuint(sig));
        SlotState.Store2(so + 12, uint2(count, Instance));
    }
    if (v >= VertexCount)
        return;
    Out.Store3(v * 12, asuint(frozen ? 0.0f.xxx : SkinVertex(v)));
}

float3 SkinVertex(uint v)
{
    const uint mb = Mesh * 52;
    const uint posHandle = Lods.Load(Instance * 80 + 8);
    const bool preSkinned = (posHandle & 0x40000000u) != 0;
    float scale = asfloat(Meshes.Load(mb + 48)) * (preSkinned ? 2.0f : 1.0f);
    if (!isfinite(scale) || abs(scale) > 1.0e3f)
        scale = 0.0f;  // 0.2.35: garbage mesh record -> collapse to a point instead of NaN
    ByteAddressBuffer vb = ResourceDescriptorHeap[kBindlessBase + (posHandle & 0x3FFFu)];
    const uint a = ((posHandle >> 6) & 0xFFFF00u) + v * 6;
    const float3 p = float3(int(LoadU16(vb, a) << 16) >> 16, int(LoadU16(vb, a + 2) << 16) >> 16, int(LoadU16(vb, a + 4) << 16) >> 16) * scale;

    float3 o = p;
    const uint bpv = min(Meshes.Load(mb + 40), 8u);
    const uint weights = Meshes.Load(mb + 28);
    const uint ib = Instance * 96;
    const uint packed = Instances.Load(ib + 4);
    const uint first = packed & 0xFFFFFFu, bones = packed >> 24;
    if (!preSkinned && bpv != 0 && bones != 0 && (weights & 0x3FFFu) >= 8)
    {
        ByteAddressBuffer wb = ResourceDescriptorHeap[kBindlessBase + (weights & 0x3FFFu)];
        const uint wbase = (weights >> 6) & 0xFFFF00u;
        float4 r0 = 0, r1 = 0, r2 = 0;
        float wsum = 0;
        for (uint j = 0; j < bpv; ++j)
        {
            const uint d = wb.Load(wbase + (bpv * v + j) * 4);
            const float w = float(d >> 16) / 65535.0f;
            const uint bone = first + min(d & 0xFFFFu, bones - 1);
            if (bone >= TransformCount)
                continue;
            const uint tb = bone * 48;
            r0 += w * asfloat(Transforms.Load4(tb));
            r1 += w * asfloat(Transforms.Load4(tb + 16));
            r2 += w * asfloat(Transforms.Load4(tb + 32));
            wsum += w;
        }
        if (wsum > 0.0f)
            o = float3(dot(r0.xyz, p) + r0.w, dot(r1.xyz, p) + r1.w, dot(r2.xyz, p) + r2.w);
    }
    // Garbage guard (a degenerate triangle is harmless; NaN/huge positions are not). 0.3.2: was 10 km. Right after a load
    // the bone data can be garbage for a few frames; vertices skinned tens of metres away made triangles spanning the
    // scene, every ray then met thousands of them, and the GPU hung. No actor (big creatures included) is 50 m across.
    if (!all(isfinite(o)) || dot(o, o) > 50.0f * 50.0f)
        o = 0.0f;
    return o;
}
