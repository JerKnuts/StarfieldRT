// Probe for debugging the TLAS: for each sample pixel, traces (a) a primary ray from the camera and
// (b) a sun ray from the surface, and dumps what each hit, including that instance's records, into a
// buffer the CPU reads back.
// 0.1.19: sample 0 is the screen centre (`Pixel`), samples 1..GridX*GridY are a grid over the screen.
// 0.1.25: the sun ray runs the same alpha test as the shadow pass (shared AlphaTest.hlsli) and records up
// to 4 alpha-tested candidates it met; each hit's material entry is dumped too.
// Each sample takes SampleBytes (1536) at sample * 1536. Layout within a sample (bytes):
//   0 magic, 4 primary status, 8 primary t, 12 depth distance, 16 primary instance index, 20 primary prim index,
//   24 depth (raw), 28 primary mesh index, 32 sun status, 36 sun t, 40 sun instance index, 44 sun prim index,
//   48 sun mesh index, 52 world x, 56 world y, 60 world z
//   64..159  primary instance record (96)   160..239 primary LOD entries (4 x 20)   240..291 primary mesh record (52)
//   292 toSun xyz (sample 0), 304 PerScene row 16 xyz (sample 0), 316 sun hit front face (1/0)
//   320..415 sun instance record (96)       416..495 sun LOD entries (80)           496..547 sun mesh record (52)
//   552 alpha candidate count
//   560..687 up to 4 alpha candidates, 32 B each: +0 instance, +4 type | material << 8 | alpha mode << 24, +8 opacity texture,
//            +12 threshold, +16 u, +20 v, +24 alpha (-1 = treated as solid), +28 ray t with bit 0 = counted as solid
//   704..1071  primary hit material entry (288 or 368 B)   1072..1439 sun hit material entry
//   1440..1503 0.1.28 UV fetch debug for the first alpha candidate: +0 instance, +4 prim, +8 mesh, +12 firstIndex,
//              +16..+24 vertex indices, +28 UV handle, +32..+40 raw UV words, +44 index window slot, +48 index dword,
//              +52 vertex count, +56 position handle

RaytracingAccelerationStructure Scene     : register(t0, space1);
Texture2D<float>                Depth     : register(t0);
ByteAddressBuffer               Instances : register(t1);
ByteAddressBuffer               Lods      : register(t2);
ByteAddressBuffer               Meshes    : register(t3);
ByteAddressBuffer               Materials : register(t4);
ByteAddressBuffer               VegMaterials : register(t5);
ByteAddressBuffer               AlphaParams : register(t6);  // 0.1.26
ByteAddressBuffer               IndexPool   : register(t7);  // 0.1.29: geometry pool (table +7)
RWByteAddressBuffer             Out       : register(u1);
SamplerState                    AlphaSampler : register(s0, space1);

cbuffer PerScene : register(b0) { float4 PerSceneRows[17]; };
cbuffer ContactData : register(b1) { float4 ContactRows[6]; };
cbuffer Params : register(b2)
{
    uint2 Extent;
    uint2 Pixel;
    float BiasBase;
    float BiasPerUnit;
    uint  GridX;
    uint  GridY;
    uint  MaterialCount;
    uint  VegMaterialCount;
    uint  AlphaParamsCount;  // 0.1.26
    uint  Pad;
};

static const uint SampleBytes = 1536;  // 0.1.28: 1440..1503 holds the UV fetch debug

#include "AlphaTest.hlsli"

// 0.1.21: the contact-shadow light vector (Data row 5) is in CAMERA space (x right, y up, z forward)
// and points toward the light. Rotate it to world space with the camera basis from the inverse
// view-projection rows (13 = right, 16 = forward).
float3 SunDirectionWorld()
{
    const float3 f = normalize(PerSceneRows[16].xyz);
    const float3 r = normalize(PerSceneRows[13].xyz - dot(PerSceneRows[13].xyz, f) * f);
    const float3 u = cross(r, f);
    const float3 v = ContactRows[5].xyz;
    return normalize(r * v.x + u * v.y + f * v.z);
}

void DumpInstance(uint instance, uint base, out uint mesh)  // base includes the sample offset
{
    for (uint i = 0; i < 96; i += 4)
        Out.Store(base + i, Instances.Load(instance * 96 + i));
    for (uint j = 0; j < 80; j += 4)
        Out.Store(base + 96 + j, Lods.Load(instance * 80 + j));
    mesh = Lods.Load(instance * 80 + 4);
    for (uint k = 0; k < 52; k += 4)
        Out.Store(base + 176 + k, Meshes.Load(mesh * 52 + k));
}

void DumpMaterial(uint instance, uint base)
{
    const uint bucket = Lods.Load(instance * 80);
    const uint material = (bucket >> 13) & 0x7FFFu, type = bucket & 0x1Fu;
    if (type == 0 && material < MaterialCount)
    {
        for (uint i = 0; i < 288; i += 4)
            Out.Store(base + i, Materials.Load(material * 288 + i));
    }
    else if (type == 3 && material < VegMaterialCount)
    {
        for (uint i = 0; i < 368; i += 4)
            Out.Store(base + i, VegMaterials.Load(material * 368 + i));
    }
    else
    {
        for (uint i = 0; i < 368; i += 4)
            Out.Store(base + i, 0u);  // the readback buffer is reused between probes
    }
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint sample = tid.x;
    if (sample > GridX * GridY)
        return;
    const uint o = sample * SampleBytes;
    uint2 depthSize;
    Depth.GetDimensions(depthSize.x, depthSize.y);
    float2 uv;
    if (sample == 0)
        uv = (float2(min(Pixel, Extent - 1)) + 0.5f) / float2(Extent);
    else
        uv = (float2((sample - 1) % GridX, (sample - 1) / GridX) + 0.5f) / float2(GridX, GridY);
    const float depth = Depth[min(uint2(uv * float2(depthSize)), depthSize - 1)];
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 h = ndc.x * PerSceneRows[13] + ndc.y * PerSceneRows[14] + max(depth, 1e-7f) * PerSceneRows[15] + PerSceneRows[16];
    const float3 posRel = h.xyz / h.w;
    const float dist = length(posRel);
    const float3 world = posRel + PerSceneRows[0].xyz;

    Out.Store(o + 0, 0x52545052u);  // 'RTPR'
    Out.Store(o + 12, asuint(dist));
    Out.Store(o + 24, asuint(depth));
    Out.Store3(o + 52, asuint(world));

    RayDesc primary;
    primary.Origin = PerSceneRows[0].xyz;
    primary.Direction = posRel / dist;
    primary.TMin = 0.0f;
    primary.TMax = 100000.0f;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> pq;
    pq.TraceRayInline(Scene, RAY_FLAG_NONE, 0x03, primary);  // 0.5.60: solid + glass, as the sun shadow pass (0x04 effect volumes excluded)
    pq.Proceed();
    const bool pHit = pq.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    Out.Store(o + 4, pHit ? 1u : 0u);
    Out.Store(o + 8, asuint(pHit ? pq.CommittedRayT() : -1.0f));
    Out.Store(o + 16, pHit ? pq.CommittedInstanceID() : 0xFFFFFFFFu);
    Out.Store(o + 20, pHit ? pq.CommittedPrimitiveIndex() : 0xFFFFFFFFu);
    uint mesh = 0xFFFFFFFFu;
    if (pHit)
    {
        DumpInstance(pq.CommittedInstanceID(), o + 64, mesh);
        DumpMaterial(pq.CommittedInstanceID(), o + 704);
    }
    Out.Store(o + 28, mesh);

    const float3 toSun = SunDirectionWorld();
    RayDesc sun;
    // 0.1.22: same origin rule as the shadow pass (start just in front of the TLAS surface).
    sun.Origin = (pHit && abs(pq.CommittedRayT() - dist) < max(0.15f, dist * 0.05f))
        ? PerSceneRows[0].xyz + primary.Direction * (pq.CommittedRayT() - 0.01f - pq.CommittedRayT() * 0.0005f) + toSun * 0.02f
        : world + toSun * (BiasBase + dist * BiasPerUnit);
    sun.Direction = toSun;
    sun.TMin = 0.0f;
    sun.TMax = 100000.0f;
    // 0.1.25: alpha-aware closest hit, logging the alpha-tested candidates along the way.
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> sq;
    sq.TraceRayInline(Scene, RAY_FLAG_NONE, 0x03, sun);  // 0.5.60: as the sun shadow pass
    uint candidates = 0;
    while (sq.Proceed())
    {
        if (sq.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        const uint inst = sq.CandidateInstanceID();
        AlphaMaterial m;
        float2 cuv;
        const float a = AlphaValue(inst, sq.CandidatePrimitiveIndex(), sq.CandidateTriangleBarycentrics(), m, cuv);
        const bool passed = a < 0.0f || a >= m.threshold;
        if (candidates == 0)
        {
            // 0.1.28: raw fetch values behind the first candidate's UV.
            const uint prim = sq.CandidatePrimitiveIndex();
            const uint meshIdx = Lods.Load(inst * 80 + 4);
            const uint mb = meshIdx * 52;
            const uint first = Meshes.Load(mb + 36);
            const uint uvh = Meshes.Load(mb + 4);
            const uint dbg = o + 1440;
            Out.Store(dbg + 0, inst);
            Out.Store(dbg + 4, prim);
            Out.Store(dbg + 8, meshIdx);
            Out.Store(dbg + 12, first);
            uint vi[3];
            [unroll] for (uint k = 0; k < 3; ++k)
            {
                const uint byteAddr = (first + prim * 3 + k) * 2;
                vi[k] = LoadU16(IndexPool, byteAddr);
                Out.Store(dbg + 16 + k * 4, vi[k]);
                if (k == 0)
                {
                    Out.Store(dbg + 44, 0xFFFFFFFFu);  // 0.1.29: read from the pool root SRV, not a bindless window
                    Out.Store(dbg + 48, IndexPool.Load(byteAddr & ~3u));
                }
            }
            Out.Store(dbg + 28, uvh);
            ByteAddressBuffer uvBuf = ResourceDescriptorHeap[kBindlessBase + (uvh & 0x3FFFu)];
            [unroll] for (uint j = 0; j < 3; ++j)
                Out.Store(dbg + 32 + j * 4, uvBuf.Load(((uvh >> 6) & 0xFFFF00u) + vi[j] * 4));
            Out.Store(dbg + 52, Meshes.Load(mb + 44));
            Out.Store(dbg + 56, Meshes.Load(mb + 0));
        }
        if (candidates < 4)
        {
            const uint co = o + 560 + candidates * 32;
            const uint bucket = Lods.Load(inst * 80);
            Out.Store(co + 0, inst);
            Out.Store(co + 4, (bucket & 0x1Fu) | (((bucket >> 13) & 0x7FFFu) << 8) | (min(m.mode, 255u) << 24));
            Out.Store(co + 8, m.tex);
            Out.Store(co + 12, asuint(m.threshold));
            Out.Store(co + 16, asuint(cuv.x));
            Out.Store(co + 20, asuint(cuv.y));
            Out.Store(co + 24, asuint(a));
            Out.Store(co + 28, (passed ? 1u : 0u) | (asuint(sq.CandidateTriangleRayT()) & ~1u));
        }
        ++candidates;
        if (passed || candidates > 256)  // 0.2.12: bounded, like the shadow pass
        {
            sq.CommitNonOpaqueTriangleHit();
            if (candidates > 256)
                sq.Abort();
        }
    }
    Out.Store(o + 552, candidates);
    const bool sHit = sq.CommittedStatus() == COMMITTED_TRIANGLE_HIT && depth > 0.0f;
    Out.Store(o + 32, sHit ? 1u : 0u);
    Out.Store(o + 36, asuint(sHit ? sq.CommittedRayT() : -1.0f));
    Out.Store(o + 40, sHit ? sq.CommittedInstanceID() : 0xFFFFFFFFu);
    Out.Store(o + 44, sHit ? sq.CommittedPrimitiveIndex() : 0xFFFFFFFFu);
    Out.Store(o + 316, (sHit && sq.CommittedTriangleFrontFace()) ? 1u : 0u);
    if (sample == 0)
    {
        Out.Store3(o + 292, asuint(toSun));                            // 0.1.20: sun direction used
        Out.Store3(o + 304, asuint(normalize(PerSceneRows[16].xyz)));  // camera row 16 (forward)
    }
    uint sunMesh = 0xFFFFFFFFu;
    if (sHit)
    {
        DumpInstance(sq.CommittedInstanceID(), o + 320, sunMesh);
        DumpMaterial(sq.CommittedInstanceID(), o + 1072);
    }
    Out.Store(o + 48, sunMesh);
}
