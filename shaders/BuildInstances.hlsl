// Turns Starfield's instance records into D3D12_RAYTRACING_INSTANCE_DESCs for the TLAS.
// Layouts (RT_FINDINGS.md "Record layouts"):
//   instance record (96 B): +4 firstTransform | bones << 24, +16 quaternion xyzw, +32 translation xyz, +44 uniform scale
//   LOD entry (20 B, 4 per instance): +0 draw bucket (bits 13..27 = material index), +4 mesh index
//     and bits 0..4 = material type (0 = standard -> CommonMaterialEntry, 3 = vegetation)
//   CommonMaterialEntry (288 B): +276 opacity texture heap index (0 = none); vegetation entry (368 B): +272
//   mesh record (52 B): +0 position handle (bit 30 = scale x2), +48 position scale
// BLAS vertices are R16G16B16A16_SNORM (int16 / 32767), so the instance transform re-applies 32767.

ByteAddressBuffer   Instances : register(t0);
ByteAddressBuffer   Lods      : register(t1);
ByteAddressBuffer   Meshes    : register(t2);
ByteAddressBuffer   BlasAddr  : register(t3);  // uint64 BLAS GPU address per mesh index, 0 = none
ByteAddressBuffer   Materials : register(t4);  // 0.1.23: CommonMaterialEntry[MaterialCount]
ByteAddressBuffer   VegMaterials : register(t5);  // 0.1.23: vegetation material entries [VegMaterialCount]
RWByteAddressBuffer Out       : register(u0);  // D3D12_RAYTRACING_INSTANCE_DESC[Limit]
RWByteAddressBuffer Counter   : register(u1);  // 0.2.2: active instance count (compaction)
// 0.2.32: skinned characters. SkinSlots: [slot * 4] = instance record index (0xFFFFFFFF = free),
// [256 + slot * 8] = uint64 BLAS address (that slot's skinned BLAS, built this frame). SkinCands: skinned instances
// near the camera, for the CPU to assign slots: [0] = count, entries at 16 + i * 16 = (instance, mesh, distance, 0).
ByteAddressBuffer   SkinSlots : register(t6);
RWByteAddressBuffer SkinCands : register(u2);
static const uint kSkinCandidates = 256;

cbuffer Params : register(b0)
{
    uint InstanceCount;  // descriptors to write (= TLAS NumDescs)
    uint InstanceRecords;  // instance records readable in Instances
    uint LodRecords;     // LOD entries readable in Lods
    uint MeshCount;      // mesh records readable in Meshes (and entries in BlasAddr)
    float MaxExtent;     // instances whose max half-extent (world units) exceeds this are left out (sky/planet shells)
    uint MaterialCount;  // 0.1.23: material entries readable in Materials (0 = treat everything as opaque)
    uint AlphaTest;      // 0.1.23: bit 0 = mark instances with an opacity texture non-opaque (alpha tested in the shadow pass)
                         // 0.2.44: bit 1 = glass separation (glass gets InstanceMask 0x02, everything else 0x01)
    uint VegMaterialCount;
    uint Pass;           // 0.2.2: 0 = reset the counter, 1 = compact active instances, 2 = null the unused tail
    uint Limit;          // 0.2.2: TLAS NumDescs (descriptors [0, Limit) are read by the build)
    float CullDistance;  // 0.2.3: leave out instances whose nearest point is farther than this from the camera (0 = off)
    float SkinRadius;    // 0.2.32: skinned instances within this distance become candidates (0 = no skinned instances)
    uint  SkinSlotCount; // 0.2.32: slots in SkinSlots
};

cbuffer PerScene : register(b1) { float4 PerSceneRows[1]; };  // 0.2.3: row 0 = camera world position

static const uint kTriangleCullDisable = 0x1;  // D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE
static const uint kForceNonOpaque = 0x8;       // D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE

void WriteNull(uint slot)
{
    const uint o = slot * 64;
    Out.Store4(o + 0, uint4(0, 0, 0, 0));
    Out.Store4(o + 16, uint4(0, 0, 0, 0));
    Out.Store4(o + 32, uint4(0, 0, 0, 0));
    Out.Store4(o + 48, uint4(0, 0, 0, 0));  // mask 0, null BLAS = inactive
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint id = tid.x;
    if (Pass == 0)
    {
        if (id == 0)
        {
            Counter.Store3(0, uint3(0u, 0u, 0u));  // active count, glass count (0.2.44), player instances (0.2.67)
            SkinCands.Store(0, 0u);
        }
        return;
    }
    if (Pass == 2)
    {
        // Null every descriptor past the compacted ones.
        if (id < Limit && id >= Counter.Load(0))
            WriteNull(id);
        return;
    }
    if (id >= InstanceCount)
        return;

    bool valid = id < InstanceRecords && id * 4 < LodRecords;
    float4 q = 0;
    float3 t = 0;
    float s = 0;
    uint2 blas = 0;
    float k = 0;
    bool glass = false;   // 0.2.44
    bool effect = false;  // 0.2.64
    uint flags = kTriangleCullDisable;
    uint hitGroup = 0;  // 0.2.67: 1 = the player's own (first-person) body: rays ignore it within kHeadRadius of the camera
    if (valid)
    {
        const uint ib = id * 96;
        q = asfloat(Instances.Load4(ib + 16));
        t = asfloat(Instances.Load3(ib + 32));
        s = asfloat(Instances.Load(ib + 44));
        const uint bones = Instances.Load(ib + 4) >> 24;
        const uint mesh = Lods.Load(id * 4 * 20 + 4);
        const float qq = dot(q, q);
        // 0.2.12: also reject absurd positions (half-written records during loads): |t| over 1e6 units.
        valid = mesh < MeshCount && abs(qq - 1.0f) < 0.01f && s > 0.0f && s < 1000.0f && all(isfinite(t)) &&
                all(abs(t) < 1.0e6f);
        if (valid && bones != 0)
        {
            // 0.2.32: skinned instance. Near the camera it is a candidate for a skinning slot; if it has one, it uses
            // that slot's BLAS (positions already in object units: the transform carries only the instance scale).
            valid = false;
            if (SkinRadius > 0.0f)
            {
                const float dist = length(t - PerSceneRows[0].xyz);
                if (dist < SkinRadius)
                {
                    uint c;
                    SkinCands.InterlockedAdd(0, 1u, c);
                    if (c < kSkinCandidates)
                        SkinCands.Store4(16 + c * 16, uint4(id, mesh, asuint(dist), 0u));
                }
                for (uint i = 0; i < SkinSlotCount; ++i)
                {
                    if (SkinSlots.Load(i * 4) == id)
                    {
                        blas = SkinSlots.Load2(512 + i * 8);  // 0.5.11: 128 slots, addresses after 128 ids
                        valid = (blas.x | blas.y) != 0;
                        k = s;
                        // 0.5.12: characters (hit group 2) come back as candidates, so rays can ignore them right next to
                        // the ray start. Our skinned copy sits a little off the drawn face (no facial morphs), and rays
                        // leaving a face hit their own copy: splotchy faces and hair.
                        flags |= kForceNonOpaque;
                        hitGroup = 2;
                        // 0.2.67: the player in first person: an actor standing right under the camera (feet within
                        // 0.75 m sideways, 0.3..2.5 m below). The game hides its head in first person, but the RT copy
                        // has it: it showed up in every reflection. Non-opaque + tag, so rays can skip the head.
                        const float3 rel = PerSceneRows[0].xyz - t;
                        // 0.4.5: 1.5 m sideways (was 0.75): running, the body lags or leans and dropped out of the tag
                        // 0.4.6: parts anchored near head height (helmet, head, pack) sit level with the camera: rel.z
                        // down to -0.6 (0.4.5 started at 0.5 and missed them; they showed in windows). 1.2 m sideways.
                        if (length(rel.xy) < 1.2f && rel.z > -0.6f && rel.z < 2.5f)
                        {
                            flags |= kForceNonOpaque;
                            hitGroup = 1;
                        }
                        // 0.5.54: the player's unused first-person body in third person (the T-pose arms in shadows
                        // while moving). Characters stand upright (root rotation is yaw only); the first-person rig is
                        // tilted (quaternion y about -0.17 in both views). The game GPU-skins only what it draws
                        // (LOD position handle bit 30); in third person the first-person body isn't drawn, and our own
                        // skinning of its half-animated skeleton gave the T-pose. Tilted + not drawn + near = dropped.
                        const bool drawnSkinned = (Lods.Load(id * 80 + 8) & 0x40000000u) != 0;
                        if (!drawnSkinned && abs(q.x) + abs(q.y) > 0.03f && length(rel) < 6.0f)
                            valid = false;
                        break;
                    }
                }
            }
        }
        else if (valid)
        {
            blas = BlasAddr.Load2(mesh * 8);
            const uint mb = mesh * 52;
            const uint handle = Meshes.Load(mb);
            const float meshScale = asfloat(Meshes.Load(mb + 48));
            k = s * meshScale * 32767.0f * (((handle >> 30) & 1u) ? 2.0f : 1.0f);
            // |SNORM| <= 1, so k bounds the instance's half-extent in world units.
            valid = (blas.x | blas.y) != 0 && k > 0.0f && k <= MaxExtent;
            // 0.2.3: distance culling (k bounds the half-extent; sqrt(3) k covers the box corners).
            if (CullDistance > 0.0f && length(t - PerSceneRows[0].xyz) - k * 1.7320508f > CullDistance)
                valid = false;
            // 0.1.23: alpha-tested materials (leaves, grass, fences) go through the any-hit path.
            const uint bucket = Lods.Load(id * 4 * 20);
            const uint material = (bucket >> 13) & 0x7FFFu, type = bucket & 0x1Fu;
            const bool alpha = (type == 0 && material < MaterialCount && Materials.Load(material * 288 + 276) != 0) ||
                               (type == 3 && material < VegMaterialCount && VegMaterials.Load(material * 368 + 272) != 0);
            if ((AlphaTest & 1u) && alpha)
                flags |= kForceNonOpaque;
            // 0.2.44: glass. Standard materials whose flags (+108) have bit 0 clear are not drawn as solid surfaces
            // (window panes 0x12, solid geometry 0x11 in the probe data). Shadow and reflection rays skip mask 0x02.
            glass = (AlphaTest & 2u) && type == 0 && material < MaterialCount && (Materials.Load(material * 288 + 108) & 1u) == 0;
            // 0.2.64: LOD dword 0 = material 0, the game's blank default entry: smoke sheets, haze volumes and other effect
            // meshes (F8 dump in Neon). Not glass: they get their own mask 0x04, which no reflection or glass ray traces.
            effect = (AlphaTest & 2u) && bucket == 0u;
        }
    }

    // Rotation matrix of unit quaternion (x, y, z, w), same convention as the game's vertex shader.
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float3 r0 = float3(1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w));
    const float3 r1 = float3(2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w));
    const float3 r2 = float3(2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y));

    // 0.2.2: only active instances get a descriptor, packed to the front. InstanceID keeps the record
    // index (the shadow and probe shaders read the game's records through it).
    if (!valid)
        return;
    uint slot;
    Counter.InterlockedAdd(0, 1u, slot);
    if (slot >= Limit)
        return;  // over this frame's limit; the CPU raises it from the read-back count
    const uint o = slot * 64;
    Out.Store4(o + 0, asuint(float4(r0 * k, t.x)));
    Out.Store4(o + 16, asuint(float4(r1 * k, t.y)));
    Out.Store4(o + 32, asuint(float4(r2 * k, t.z)));
    if (glass && !effect)
        Counter.InterlockedAdd(4, 1u);
    if (hitGroup == 1u)
        Counter.InterlockedAdd(8, 1u);
    const uint mask = (AlphaTest & 2u) ? (effect ? 0x04u : glass ? 0x02u : 0x01u) : 0xFFu;  // 0.2.44, 0.2.64 effects
    Out.Store(o + 48, (id & 0xFFFFFFu) | (mask << 24));  // InstanceID = record index | InstanceMask
    Out.Store(o + 52, hitGroup | (flags << 24));          // hit group offset (0.2.67: 1 = player) | flags
    Out.Store2(o + 56, blas);
}
