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
static const uint kSkinCandidates = 2048;  // 0.5.119: was 256 (must match Blas.cpp)

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
    float MinAngle;      // 0.6.0 work (0.5.100): leave out rigid objects whose bounding radius / distance is below this (0 = off)
    uint  UseSolidBits;  // 0.5.126: 1 = SolidBits is bound
    uint  UsePhantomBits;  // 0.5.134: 1 = PhantomBits is bound
};
// 0.5.134: 1 bit per record: objects the game doesn't draw (phantom detector in the sun pass).
RWByteAddressBuffer PhantomBits : register(u4);
// 0.5.126: 1 bit per record: glass-tagged objects the game draws as solid surfaces (from the glass pass's scores).
RWByteAddressBuffer SolidBits : register(u3);

cbuffer PerScene : register(b1) { float4 PerSceneRows[1]; };  // 0.2.3: row 0 = camera world position

static const uint kTriangleCullDisable = 0x1;  // D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE
static const uint kForceNonOpaque = 0x8;       // D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE

// 0.5.102: duplicate check. Two hash tables after the near-camera dump in Counter: one keyed on place (position and
// rotation), one on place + BLAS. Counts at kDupCounts: same mesh at the same place, and any mesh at a place already taken.
static const uint kHashOff = 4096;
static const uint kHashSlots = 1u << 19;  // per table
static const uint kDupCounts = 4088;

uint Pcg(uint v)
{
    const uint s = v * 747796405u + 2891336453u;
    const uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}

// Returns true when the key was already in the table.
bool HashInsert(uint table, uint h)
{
    const uint key = Pcg(h ^ 0x9E3779B9u) | 1u;  // never 0 (empty)
    uint i = h & (kHashSlots - 1u);
    [loop] for (uint n = 0; n < 16u; ++n)
    {
        uint prev;
        Counter.InterlockedCompareExchange(kHashOff + (table * kHashSlots + i) * 4u, 0u, key, prev);
        if (prev == 0u)
            return false;
        if (prev == key)
            return true;
        i = (i + 1u) & (kHashSlots - 1u);
    }
    return false;
}

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
        if (id < 2u * kHashSlots)
            Counter.Store(kHashOff + id * 4u, 0u);  // 0.5.102
        if (id == 0)
        {
            Counter.Store4(0, uint4(0u, 0u, 0u, 0u));  // active count, glass count (0.2.44), player instances (0.2.67), size-culled (0.5.100)
            Counter.Store(16, 0u);                  // 0.5.66: near-camera dump count
            Counter.Store3(20, uint3(0u, 0u, 0u));  // 0.5.101: breakdown counters 20..63
            Counter.Store4(32, uint4(0u, 0u, 0u, 0u));
            Counter.Store4(48, uint4(0u, 0u, 0u, 0u));
            Counter.Store2(kDupCounts, uint2(0u, 0u));
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
    bool glowCard = false, screen = false;  // 0.5.121
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
                        // 0.5.65: ...and in first person that tilted rig IS drawn: the arms you see. It is anchored away
                        // from "right under the camera", so the 0.2.67 test missed it and the arms showed in reflections.
                        // NPCs stand upright (yaw only), so a drawn tilted rig this close is the player's.
                        if (drawnSkinned && abs(q.x) + abs(q.y) > 0.03f && length(rel) < 2.5f)
                        {
                            flags |= kForceNonOpaque;
                            hitGroup = 1;
                        }
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
            // 0.5.65: the first-person weapon (rigid, not skinned) showed in reflections. 0.5.68: the near-camera probe
            // showed it is ~50 records (63136..63185 in Neon) right next to the player's own skinned records
            // (63159..63165), and that some parts have a size bound of 2 or 10 m (the bound is loose), so 0.5.65's
            // "under 1.5 m" rule missed them. Now: a rigid object within 1.5 m of the camera whose record index is
            // within 64 of a skinned copy that stands where the player does (same test as 0.2.67). Walls and props are
            // thousands of records away from the player. In third person the camera is metres behind the character,
            // so the test fails and the weapon stays in. Tagged as the player (hit group 1): reflection, GI and glass
            // rays skip it, as do sun and lamp shadows (no first-person shadows, 0.5.49).
            if (valid && SkinRadius > 0.0f && length(t - PerSceneRows[0].xyz) < 1.5f)  // SkinRadius > 0: camera bound
            {
                for (uint i = 0; i < SkinSlotCount; ++i)
                {
                    const uint sid = SkinSlots.Load(i * 4);
                    if (sid >= InstanceRecords || (sid > id ? sid - id : id - sid) > 64u)
                        continue;
                    const float3 srel = PerSceneRows[0].xyz - asfloat(Instances.Load3(sid * 96 + 32));
                    if (length(srel.xy) < 1.2f && srel.z > -0.6f && srel.z < 2.5f)
                    {
                        flags |= kForceNonOpaque;
                        hitGroup = 1;
                        break;
                    }
                }
            }
            // 0.2.3: distance culling (k bounds the half-extent; sqrt(3) k covers the box corners).
            if (CullDistance > 0.0f && length(t - PerSceneRows[0].xyz) - k * 1.7320508f > CullDistance)
                valid = false;
            // 0.5.100: size on screen. Outside on the Neon walkways the scene went from 65k to 275k objects (4 km range),
            // most of them small clutter far away covering a pixel or two: the frame time went from 17 to 61 ms. An
            // object whose bounding radius / distance is below MinAngle (about 3-8 px at 1440p) is left out. Buildings
            // and large props stay in to the full range.
            if (valid && MinAngle > 0.0f && hitGroup == 0u && k * 1.7320508f < MinAngle * length(t - PerSceneRows[0].xyz))
            {
                valid = false;
                Counter.InterlockedAdd(12, 1u);
            }
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
            // 0.5.121: glowing cards and screens. Probe on the Jemison/MAST walkway: a long hard sun shadow came from two
            // invisible 16 m panels 0.4 m above the floor: instance 180 (standard material with emission, not drawn as a
            // solid surface: a light-shaft / hologram card) and instance 173 (family 7, the screen/hologram family).
            glowCard = glass && Materials.Load(material * 288 + 16) != 0u;  // emission flags set
            screen = (AlphaTest & 2u) && type == 7u;
        }
    }

    // Rotation matrix of unit quaternion (x, y, z, w), same convention as the game's vertex shader.
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float3 r0 = float3(1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w));
    const float3 r1 = float3(2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w));
    const float3 r2 = float3(2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y));

    // 0.2.2: only active instances get a descriptor, packed to the front. InstanceID keeps the record
    // index (the shadow and probe shaders read the game's records through it).
    // 0.5.66: every record whose origin is within 1.5 m of the camera (0.5.67: was 3 m, which filled the list with walls), for the Numpad . probe (the first-person gun still
    // showed in reflections in 0.5.65). 64 B at Counter + 64 + 64 i, at most 48: record, bones, mesh, packed
    // (flags | hit group << 8 | valid << 16 | drawn-skinned << 17 | glass << 18 | effect << 19), k, distance,
    // camera - origin (xyz), quaternion (xyzw).
    if (id < InstanceRecords && SkinRadius > 0.0f && all(isfinite(t)) && length(t - PerSceneRows[0].xyz) < 1.5f)
    {
        uint n;
        Counter.InterlockedAdd(16, 1u, n);
        if (n < 48u)
        {
            const uint o = 64 + n * 64;
            const uint drawnBit = (Lods.Load(id * 80 + 8) & 0x40000000u) != 0 ? 1u : 0u;
            Counter.Store4(o, uint4(id, Instances.Load(id * 96 + 4) >> 24, Lods.Load(id * 80 + 4),
                (flags & 0xFFu) | (hitGroup << 8) | ((valid ? 1u : 0u) << 16) | (drawnBit << 17) | ((glass ? 1u : 0u) << 18) |
                ((effect ? 1u : 0u) << 19)));
            const float3 rel = PerSceneRows[0].xyz - t;
            Counter.Store4(o + 16, asuint(float4(k, length(rel), rel.x, rel.y)));
            Counter.Store4(o + 32, asuint(float4(rel.z, q.x, q.y, q.z)));
            Counter.Store(o + 48, asuint(q.w));
        }
    }
    if (!valid)
        return;
    // 0.5.132: hit group 3 = a giant rigid object (over 150 m across): the city's distant-detail blocks (a 512 m block
    // at round coordinates under Jemison, never drawn up close). Sun rays pass their back faces at any distance: up
    // close, our copy of such a block sits up to 0.4 m off the real floor and shadowed it in big curved patches.
    if (hitGroup == 0u && k * 1.7320508f > 150.0f)
        hitGroup = 3u;
    uint slot;
    Counter.InterlockedAdd(0, 1u, slot);
    if (slot >= Limit)
        return;  // over this frame's limit; the CPU raises it from the read-back count
    const uint o = slot * 64;
    Out.Store4(o + 0, asuint(float4(r0 * k, t.x)));
    Out.Store4(o + 16, asuint(float4(r1 * k, t.y)));
    Out.Store4(o + 32, asuint(float4(r2 * k, t.z)));
    if (glass && !effect && !glowCard)
        Counter.InterlockedAdd(4, 1u);
    // 0.5.101: breakdown of what stays in (the 0.5.100 size cut left 265k of 269k on the Neon walkways). Distance from
    // the camera to the origin (m): < 50, < 200, < 1000, more. Bounding radius k (m): < 0.5, < 2, < 10, more. Then
    // alpha-tested (any-hit), effect meshes and characters.
    {
        const float d = length(t - PerSceneRows[0].xyz);
        Counter.InterlockedAdd(d < 50.0f ? 20 : d < 200.0f ? 24 : d < 1000.0f ? 28 : 32, 1u);
        Counter.InterlockedAdd(k < 0.5f ? 36 : k < 2.0f ? 40 : k < 10.0f ? 44 : 48, 1u);
        if (flags & kForceNonOpaque)
            Counter.InterlockedAdd(52, 1u);
        if (effect || glowCard)
            Counter.InterlockedAdd(56, 1u);  // 0.5.121: + glowing cards
        if (hitGroup == 2u)
            Counter.InterlockedAdd(60, 1u);
        // 0.5.102: position to 1 cm, rotation to 1/1000.
        const int3 tp = int3(round(t * 100.0f));
        const int4 qp = int4(round(q * 1000.0f));
        uint h = Pcg(asuint(tp.x));
        h = Pcg(h ^ asuint(tp.y)); h = Pcg(h ^ asuint(tp.z));
        h = Pcg(h ^ asuint(qp.x)); h = Pcg(h ^ asuint(qp.y)); h = Pcg(h ^ asuint(qp.z)); h = Pcg(h ^ asuint(qp.w));
        h = Pcg(h ^ asuint(int(round(k * 1000.0f))));
        const bool samePlace = HashInsert(0u, h);
        const bool sameMesh = HashInsert(1u, Pcg(Pcg(h ^ blas.x) ^ blas.y));
        if (sameMesh)
            Counter.InterlockedAdd(kDupCounts, 1u);
        if (samePlace)
            Counter.InterlockedAdd(kDupCounts + 4u, 1u);
    }
    if (hitGroup == 1u)
        Counter.InterlockedAdd(8, 1u);
    // 0.5.121: 0x10 = glowing card (traced by nothing: invisible in the game), 0x08 = family 7 screen (reflections, GI and
    // glass reflections only: no sun or lamp shadows).
    // 0.5.126: a glass-tagged object the game draws solid (Jemison Mercantile wall panel, material flags 0x12 like a window
    // pane) also gets the solid mask: reflection rays went through it and picked up the far hall or the game's cubemap.
    // It keeps the glass bit, so the glass pass goes on scoring it.
    const bool drawnSolid = glass && UseSolidBits != 0u && id < (1u << 19) && ((SolidBits.Load((id >> 5) * 4u) >> (id & 31u)) & 1u) != 0u;
    // 0.5.134: an object the game doesn't draw (phantom bit) gets mask 0x10: no shadow, reflection or GI ray traces it,
    // only the sun pass's phantom detector (mask 0x11), which clears the bit once the game draws it again.
    const bool phantom = UsePhantomBits != 0u && (hitGroup == 0u || hitGroup == 3u) && id < (1u << 19) &&
                         ((PhantomBits.Load((id >> 5) * 4u) >> (id & 31u)) & 1u) != 0u;
    const uint mask = (AlphaTest & 2u) ? (effect ? 0x04u : glowCard ? 0x10u : phantom ? 0x10u : drawnSolid ? 0x03u : glass ? 0x02u : screen ? 0x08u : 0x01u) : 0xFFu;  // 0.2.44, 0.2.64 effects
    Out.Store(o + 48, (id & 0xFFFFFFu) | (mask << 24));  // InstanceID = record index | InstanceMask
    Out.Store(o + 52, hitGroup | (flags << 24));          // hit group offset (0.2.67: 1 = player) | flags
    Out.Store2(o + 56, blas);
}
