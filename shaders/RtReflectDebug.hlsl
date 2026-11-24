// 0.2.25 (R2): RT reflection debug views, written over the game's probe specular (Indirect#0 SV_Target1).
// 0.2.27 (R4): mode 5 is the real thing: RT reflections composited through the game's own split-sum weighting.
// 0.2.29 (R5): mode 5 is two passes. `main` traces one GGX-sampled (glossy) ray per pixel into Rad; `Resolve`
// filters Rad with an edge-aware kernel sized by roughness and composites into Out.
// Runs right after GatherIndirect's full-screen draw. Output goes to a plugin buffer laid out as an R11G11B10
// texture footprint; the C++ side copies it into the game's specular target (RT1).
//
// Inputs (RT_FINDINGS "R1 survey"): GatherIndirect root table 4 = space4 G-buffer table, offset = register.
//   t1 normal (R10G10B10A2, .xy encoded view-space normal), t2 (.x AO, .y roughness), t6 depth (reversed Z, sky 0).
// PerSceneConstants (same CBV as the shadow pass): row 0 camera position, rows 13..16 inverse view-projection
// (camera-relative), rows 17..19 view -> world rotation (Indirect#0 rotates its normal with these).

RaytracingAccelerationStructure Scene   : register(t0, space1);
static const uint kSolidMask = 0x01u;  // 0.2.44: instance mask of solid geometry (glass is 0x02)
Texture2D<float4>               GAlbedo : register(t0, space4);  // 0.2.27: albedo (sRGB view: reads linear)
Texture2D<float4>               GNormal : register(t1, space4);
Texture2D<float4>               GMat    : register(t2, space4);
Texture2D<uint>                 GType   : register(t4, space4);  // 0.2.27: material type (& 15)
Texture2D<float4>               GMatIdx : register(t5, space4);  // 0.2.65: .z x 255 = material table index (types 1/8), F8 dump only
Texture2D<float>                GDepth  : register(t6, space4);
Texture2D<float4>               BrdfLut : register(t13, space4); // 0.2.27: split-sum (A, B) at (N.V, roughness)
ByteAddressBuffer               GameSpec : register(t8, space1); // 0.2.27: copy of the game's probe specular (R11G11B10 footprint)
RWByteAddressBuffer             Out     : register(u0);
RWByteAddressBuffer             Rad     : register(u1);  // 0.2.29: 16 B/pixel: half rgb, half rough, f32 view dist, packed normal
// 0.2.26 (R3): scene records (global scene table) for hit shading. Bindless streams/textures via ResourceDescriptorHeap.
ByteAddressBuffer               Instances   : register(t1, space1);  // table +2, 96 B: +12 low 16 bits = tint index
ByteAddressBuffer               Lods        : register(t2, space1);  // table +4, 20 B, 4 per instance
ByteAddressBuffer               Meshes      : register(t3, space1);  // table +5, 52 B
ByteAddressBuffer               Tints       : register(t4, space1);  // table +6, 32 B: dword 0 = tint RGB (bytes 2,1,0)
ByteAddressBuffer               IndexPool   : register(t5, space1);  // table +7, uint16 indices
ByteAddressBuffer               Materials   : register(t6, space1);  // table +11, CommonMaterialEntry 288 B
ByteAddressBuffer               TextureSets : register(t7, space1);  // table +27, 40 B: +0 albedo texture heap index
ByteAddressBuffer               VegMaterials : register(t9, space1);  // 0.2.49: table +13, vegetation entries 368 B (alpha test)
ByteAddressBuffer               AlphaParams  : register(t10, space1); // 0.2.49: table +28, alpha params 32 B
SamplerState                    LinearWrap  : register(s0);
SamplerState                    LinearClamp : register(s1);

cbuffer ContactData : register(b2)
{
    float4 ContactRows[6];   // row 5 = direction toward the sun, camera space
};

// 0.2.58: the game's local lights (TiledDeferredLight cb2 space4, 64 KB = 512 lights x 8 rows, positions in camera space:
// x right, y up, z forward). LightCounts = its t0 space4 (48 B): [+0,+4) point, [+8,+12) box-bounded directional,
// [+16,+20) spot light index ranges. Layout decoded from PSO 11809 (see RT_FINDINGS.md "Local lights").
cbuffer LocalLightData : register(b3)
{
    float4 LightRows[4096];
};
ByteAddressBuffer LightCounts : register(t11, space1);
// 0.3.3: last frame's final lit image, 8 B per pixel: R11G11B10 colour (radiance x exposure), f32 view distance (1e9 = sky).
// Captured after the OIT composite (glass included); GI rays that hit something visible last frame take its real colour:
// neon signs, screens and lit walls light their surroundings (and it is a free extra bounce).
ByteAddressBuffer PrevFrame : register(t12, space1);

cbuffer PerScene : register(b0)
{
    float4 PerSceneRows[313];  // 0.2.27: rows 311..312 = exposure (Indirect#0 multiplies its specular by it)
};

cbuffer Params : register(b1)
{
    uint2 Size;          // output (RT1) size in pixels
    uint  Mode;          // 1 world normal, 2 mirror hit (instance colour, darker with distance), 3 roughness,
                         // 4 (0.2.26) RT reflection colour: albedo x tint x (sun x shadow + ambient) at the hit
    uint  PitchBytes;    // row pitch of the output footprint (256-aligned)
    float MaxDistance;   // mirror ray length, world units
    float Scale;         // output brightness
    uint  MaterialCount; // entries readable in Materials (0 = no records: grey hits)
    uint  Flags;         // bit 0 = sun direction valid; bit 1 (0.2.31) = replace the game's reflections everywhere;
                         // bit 2 (0.2.34) = no sun shadow rays at reflection hits (low quality)
    float SunIntensity;  // 0.2.26: placeholder lighting until R4 calibrates against the game's units
    float Ambient;
    float FadeStart;     // 0.2.27: roughness where RT starts fading back to the game's probe reflection
    float FadeEnd;       //         roughness where only the probe reflection is left
    float ProbeAmbient;  //         hit lighting: share of the probe radiance used as ambient
    float MinWeight;     // 0.2.28: skip the ray where the game's reflection weight W x k is below this (too faint to see)
    uint  Frame;         // 0.2.29: changes the glossy ray pattern every frame
    float BlurPx;        // 0.2.29: filter radius in pixels at roughness 1 (scaled by sqrt(roughness))
    uint  TraceScale;    // 0.2.34: 1 = a ray per pixel, 2 = a ray per 2x2 block (position rotates every frame)
    uint  SmoothTypes;   // 0.2.46: bit n set = G-buffer material type n is glass: its roughness is scaled by GlassRoughScale
    float GlassRoughScale;
    uint  VegMaterialCount;  // 0.2.49: entries readable in VegMaterials (alpha test)
    uint  AlphaParamsCount;  // 0.2.49: entries readable in AlphaParams
    uint  GlassDepthIndex;   // 0.2.51 (G3): heap index of the full-res depth SRV (OIT composite table 3 +0)
    uint  GlassMomentIndex;  // 0.2.51: heap index of the half-res b0 moment SRV (R32_FLOAT, table 3 +2)
    float GlassStrength;     // 0.2.51: scales the glass reflection (1 = physical Fresnel)
    float GlassMinCoverage;  // 0.2.51: b0 (optical depth) below this = no see-through surface at the pixel
    float LocalLightScale;   // 0.2.58: brightness of the game's local lights at reflection hits (Flags bit 4 = lights bound)
    uint  LocalShadowMax;    // 0.2.58: shadow rays per hit for local lights (the first few that contribute get one)
                             // 0.3.6: bits 0..7 = that count (0.4.5: the RT lamp pass), bit 8 = emission model (0 adaptive,
                             //        1 luminous emittance), bits 9..12 (0.4.5) = shadow rays per hit for lamps at
                             //        reflection / GI hits,
                             //        bits 16..31 = emission scale (half float; 0 = material emission off)
};

static const uint kBindlessBase = 32768;
// 0.3.7: material records can be half-written right after a load; a texture index past the descriptor heap (98304
// entries in this game) is undefined behaviour on the GPU, so such indices are treated as "no texture".
static const uint kMaxHeapIndex = 98304;

uint LoadU16(ByteAddressBuffer b, uint byteAddr)
{
    const uint w = b.Load(byteAddr & ~3u);
    return (byteAddr & 2u) ? (w >> 16) : (w & 0xFFFFu);
}

// 0.2.49: reflection rays run the shadow pass's alpha test. Without it, leaf cards (palms, plants, fences) were
// reflected as solid slabs: the smeared yellow-green streaks on the plaza doors.
#define RT_ALPHA_SHARED_DEFS
#define AlphaSampler LinearWrap
#include "AlphaTest.hlsli"
// Traverses `q`, committing alpha-tested candidates only where they are solid (at most 32 per ray: past that the
// candidate counts as solid, as in the shadow pass, so a ray through dense foliage cannot run for long).
// 0.2.67: candidates on the player's own body (hit group 1) are skipped within 0.45 m of the camera (the head the game
// hides in first person) and otherwise count as solid.
// 0.5.12: characters (hit group 2) don't block rays within kCharSelfGap of the ray start (their own skinned copy).
static const float kCharSelfGap = 0.2f;
#define PROCEED_ALPHA(q) { uint cand_ = 0; while (q.Proceed()) { if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) { \
    if (++cand_ > 32) q.CommitNonOpaqueTriangleHit(); /* 0.3.2: player candidates count toward the cap too */ \
    else if (q.CandidateInstanceContributionToHitGroupIndex() == 2u) { \
        if (q.CandidateTriangleRayT() > kCharSelfGap && AlphaPasses(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics())) \
            q.CommitNonOpaqueTriangleHit(); \
    } else if (q.CandidateInstanceContributionToHitGroupIndex() == 1u) { \
        /* 0.5.49: the first-person player casts no shadow (our copy isn't animated in first person: a T-pose shadow) */ \
    } else if (AlphaPasses(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics())) \
        q.CommitNonOpaqueTriangleHit(); } } }
// 0.4.4: reflection, GI and glass rays skip the first-person player body (hit group 1) entirely: the game doesn't
// animate it in first person, so our copy stood in a T-pose and showed up in windows and floors. Shadow rays keep it.
#define PROCEED_ALPHA_NOPLAYER(q) { uint cand_ = 0; while (q.Proceed()) { if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) { \
    if (++cand_ > 64) q.Abort(); /* safety cap, counts every candidate (0.3.2 hang) */ \
    else if (q.CandidateInstanceContributionToHitGroupIndex() == 1u) { /* the player: never */ } \
    else if (q.CandidateInstanceContributionToHitGroupIndex() == 2u && q.CandidateTriangleRayT() <= kCharSelfGap) { /* 0.5.12 */ } \
    else if (cand_ > 32 || AlphaPasses(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics())) \
        q.CommitNonOpaqueTriangleHit(); } } }

// 0.5.44: RT lamp shadow ray. A layer lying right on the shaded surface (the cover sheet over posters, instance 1445 in
// the Jemison probe, hit at 0.00-0.01 m by every lamp) blocked all lamps, so posters went dark. A hit within
// kLampCoverGap of the start is looked past once; anything behind it still blocks. Returns the blocking distance, or -1.
// 0.5.45: also passes a back-face hit within 0.1 m + 2% of the view distance (as reflections do since 0.5.33): past the
// game's level-of-detail switch the drawn wall is a simpler mesh than our RT copy, the ray started inside our copy and
// hit its inner face, so lamps lit posters only inside a circle around the camera.
static const float kLampCoverGap = 0.1f;
float LampShadowHit(float3 origin, float3 dir, float range, float viewDist, out uint hitInst)
{
    hitInst = 0xFFFFFFFFu;
    const float selfGap = 0.1f + viewDist * 0.02f;
    RayDesc sray;
    sray.Origin = origin;
    sray.Direction = dir;
    sray.TMin = 0.0f;
    sray.TMax = range;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> sq;
    [loop] for (uint tries = 0; tries < 3; ++tries)
    {
        sq.TraceRayInline(Scene, RAY_FLAG_NONE, kSolidMask, sray);
        PROCEED_ALPHA(sq)
        if (sq.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
            return -1.0f;
        const float t = sq.CommittedRayT();
        hitInst = sq.CommittedInstanceID();
        const bool skip = t <= kLampCoverGap || (!sq.CommittedTriangleFrontFace() && t < selfGap);
        if (!skip || tries == 2)
            return t;
        sray.TMin = t + 0.02f;
        if (sray.TMin >= range)
            return -1.0f;
    }
    return -1.0f;
}

// 0.5.47: GI probe (Numpad .): 15 GI rays from the centre surface, traced as GiTrace does. At dl: 'GIRY', raw N.V (the
// G-buffer normal against the direction to the camera; < 0 = facing away), view distance, flipped (1/0); then per ray
// (16 B): hit t (-1 = miss), front face | hit group << 1 | skipped self-hits << 4, instance, dot(direction, normal).
void DumpGiRays(uint dl, float3 posRel, float3 nRaw, float viewDist)
{
    const float3 toCam = -posRel / max(viewDist, 1e-4f);
    const float nv = dot(nRaw, toCam);
    const bool flip = nv < -0.25f;
    const float3 n = flip ? -nRaw : nRaw;
    Rad.Store4(dl, uint4(0x59524947u, asuint(nv), asuint(viewDist), flip ? 1u : 0u));
    const float3 up = abs(n.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
    const float3 tx = normalize(cross(up, n));
    const float3 ty = cross(n, tx);
    const float selfGap = 0.1f + viewDist * 0.02f;
    uint selfInst = 0xFFFFFFFFu;  // 0.5.48: as GiTrace finds it
    {
        RayDesc cr;
        cr.Origin = PerSceneRows[0].xyz;
        cr.Direction = posRel / max(viewDist, 1e-4f);
        cr.TMin = max(0.05f, viewDist - 0.5f);
        cr.TMax = viewDist + 0.3f;
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> cq;
        cq.TraceRayInline(Scene, RAY_FLAG_NONE, kSolidMask, cr);
        cq.Proceed();
        if (cq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            selfInst = cq.CommittedInstanceID();
    }
    [loop] for (uint i = 0; i < 15; ++i)
    {
        const float2 u = frac(float2(0.5f, 0.5f) + float(i) * float2(0.6180340f, 0.7548777f));
        const float r = sqrt(u.x), phi = 2.0f * 3.14159265f * u.y;
        const float3 dir = normalize(tx * (r * cos(phi)) + ty * (r * sin(phi)) + n * sqrt(max(1.0f - u.x, 0.0f)));
        RayDesc ray;
        ray.Origin = PerSceneRows[0].xyz + posRel + n * (0.05f + viewDist * 0.002f);
        ray.Direction = dir;
        ray.TMin = 0.0f;
        ray.TMax = 100.0f;
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
        uint skipped = 0;
        [loop] for (uint tries = 0; tries < 4; ++tries)
        {
            rq.TraceRayInline(Scene, RAY_FLAG_NONE, kSolidMask, ray);
            PROCEED_ALPHA_NOPLAYER(rq)
            if (tries < 3 && rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT && rq.CommittedRayT() < selfGap &&
                (!rq.CommittedTriangleFrontFace() || rq.CommittedInstanceID() == selfInst))
            {
                ray.TMin = rq.CommittedRayT() + 0.01f;
                ++skipped;
                continue;
            }
            break;
        }
        const bool hit = rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
        Rad.Store4(dl + 16 + i * 16, uint4(asuint(hit ? rq.CommittedRayT() : -1.0f),
            hit ? ((rq.CommittedTriangleFrontFace() ? 1u : 0u) | (rq.CommittedInstanceContributionToHitGroupIndex() << 1) | (skipped << 4)) : (skipped << 4),
            hit ? rq.CommittedInstanceID() : 0xFFFFFFFFu, asuint(dot(dir, nRaw))));
    }
}

uint3 TriangleIndices(uint meshB, uint prim)
{
    const uint firstIndex = Meshes.Load(meshB + 36);
    return uint3(LoadU16(IndexPool, (firstIndex + prim * 3) * 2), LoadU16(IndexPool, (firstIndex + prim * 3 + 1) * 2),
                 LoadU16(IndexPool, (firstIndex + prim * 3 + 2) * 2));
}

float3 LoadPosition(ByteAddressBuffer vb, uint base, uint i)
{
    const uint a = base + i * 6;
    const uint x = LoadU16(vb, a), y = LoadU16(vb, a + 2), z = LoadU16(vb, a + 4);
    return float3(int(x << 16) >> 16, int(y << 16) >> 16, int(z << 16) >> 16);
}

float3 SunDirectionWorld()
{
    const float3 f = normalize(PerSceneRows[16].xyz);
    const float3 r = normalize(PerSceneRows[13].xyz - dot(PerSceneRows[13].xyz, f) * f);
    const float3 u = cross(r, f);
    const float3 v = ContactRows[5].xyz;
    return normalize(r * v.x + u * v.y + f * v.z);
}

// Albedo at a hit, the G-buffer shader's way (StandardMaterial PSO 12209): texture set (material +116) entry +0,
// UV set from material +112 bit 0, UV = uv * half2(+168) + half2(+172), times the per-instance tint.
// 0.2.32: texture level from a ray cone (Akenine-Moller et al. 2019): footprint width at the hit times the triangle's
// texel density (texels per world unit, from its UV area x texture size over its world area).
// 0.3.6: also returns the material's own light (emission, scene units = radiance) for family-0 materials. Emission block
// of CommonMaterialEntry (decoded from Neon F8 dumps): +0 luminous emittance (float), +4 exposure offset (EV), +8 max
// offset, +16 flags (0 = no emission), +28 tint (RGB = bytes 2,1,0). Emissive texture = texture set +24.
float3 HitAlbedo(uint instance, uint meshB, uint3 idx, float2 bary, float coneWidth, float worldArea, float invExposure,
                 out float3 emission)
{
    emission = 0.0f;
    const uint bucket = Lods.Load(instance * 80);
    const uint material = (bucket >> 13) & 0x7FFFu, type = bucket & 0x1Fu;
    float3 albedo = (type == 3) ? float3(0.25f, 0.32f, 0.18f) : float3(0.5f, 0.5f, 0.5f);  // no material: grey / foliage green
    if (type == 0 && material < MaterialCount)
    {
        const uint b = material * 288;
        const uint texSet = Materials.Load(b + 116);
        const uint tex = TextureSets.Load(texSet * 40);
        const uint uvHandle = Meshes.Load(meshB + ((Materials.Load(b + 112) & 1u) ? 8 : 4));
        if (tex != 0 && tex < kMaxHeapIndex && (uvHandle & 0x3FFFu) >= 8)  // 0.3.7: bounds
        {
            ByteAddressBuffer uvBuf = ResourceDescriptorHeap[kBindlessBase + (uvHandle & 0x3FFFu)];
            const uint uvBase = (uvHandle >> 6) & 0xFFFF00u;
            const float w[3] = { 1.0f - bary.x - bary.y, bary.x, bary.y };
            float2 uv = 0, tuv[3];
            [unroll] for (uint j = 0; j < 3; ++j)
            {
                const uint h = uvBuf.Load(uvBase + idx[j] * 4);
                tuv[j] = float2(f16tof32(h), f16tof32(h >> 16));
                uv += w[j] * tuv[j];
            }
            const uint sc = Materials.Load(b + 168), of = Materials.Load(b + 172);
            const float2 scale = float2(f16tof32(sc >> 16), f16tof32(sc));
            uv = uv * scale + float2(f16tof32(of >> 16), f16tof32(of));
            Texture2D<float4> albedoTex = ResourceDescriptorHeap[tex];
            uint tw, th, levels;
            albedoTex.GetDimensions(0, tw, th, levels);
            const float2 e1 = (tuv[1] - tuv[0]) * scale, e2 = (tuv[2] - tuv[0]) * scale;
            const float texelArea = abs(e1.x * e2.y - e1.y * e2.x) * float(tw) * float(th);
            float mip = 0.0f;
            if (worldArea > 1e-12f && texelArea > 0.0f)
                mip = log2(max(coneWidth * sqrt(texelArea / worldArea), 1e-6f));
            albedo = albedoTex.SampleLevel(LinearWrap, uv, clamp(mip, 0.0f, float(levels) - 1.0f)).rgb;
            const float emScale = f16tof32(LocalShadowMax >> 16);
            if (emScale > 0.0f && Materials.Load(b + 16) != 0u)
            {
                const uint tw2 = Materials.Load(b + 28);
                const float3 tint = float3((tw2 >> 16) & 0xFFu, (tw2 >> 8) & 0xFFu, tw2 & 0xFFu) / 255.0f;
                const uint emTex = TextureSets.Load(texSet * 40 + 24);
                float3 em = 1.0f.xxx;
                if (emTex >= 32u && emTex < kMaxHeapIndex)  // small indices are the engine's default (placeholder) textures
                {
                    Texture2D<float4> emT = ResourceDescriptorHeap[emTex];
                    em = emT.SampleLevel(LinearWrap, uv, clamp(mip, 0.0f, float(levels) - 1.0f)).rgb;
                }
                const float lum = asfloat(Materials.Load(b + 0)), ev = asfloat(Materials.Load(b + 4));
                const float strength = (LocalShadowMax & 0x100u)
                    ? max(lum, 0.0f)                                           // luminous emittance, absolute
                    : exp2(clamp(ev, -16.0f, 16.0f)) * invExposure;            // adaptive: fixed brightness on screen
                emission = tint * em * strength * emScale;
                if (!all(isfinite(emission))) emission = 0.0f;
            }
        }
    }
    const uint tint = Tints.Load((Instances.Load(instance * 96 + 12) & 0xFFFFu) * 32);
    return albedo * float3((tint >> 16) & 0xFFu, (tint >> 8) & 0xFFu, tint & 0xFFu) / 255.0f;
}

// Positive floats only (debug colours): float -> half bits -> drop low mantissa bits.
uint PackR11G11B10(float3 c)
{
    c = clamp(c, 0.0f, 65000.0f);
    const uint r = (f32tof16(c.r) >> 4) & 0x7FFu;
    const uint g = (f32tof16(c.g) >> 4) & 0x7FFu;
    const uint b = (f32tof16(c.b) >> 5) & 0x3FFu;
    return r | (g << 11) | (b << 22);
}

float3 HashColour(uint v)
{
    v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16;
    return float3(v & 0xFFu, (v >> 8) & 0xFFu, (v >> 16) & 0xFFu) / 255.0f * 0.8f + 0.2f;
}

// Indirect#0's decode: v = xy*4-2, f = dot(v,v)/4, n = (v*sqrt(1-f), -(1-dot(v,v)/2)) in view space.
float3 DecodeNormalWorld(float2 xy)
{
    const float2 v = xy * 4.0f - 2.0f;
    const float d = dot(v, v);
    float3 n = float3(v * sqrt(max(1.0f - d * 0.25f, 0.0f)), -(1.0f - d * 0.5f));
    n = normalize(n);
    return normalize(n.x * PerSceneRows[17].xyz + n.y * PerSceneRows[18].xyz + n.z * PerSceneRows[19].xyz);
}

float3 UnpackR11G11B10(uint v)
{
    return float3(f16tof32((v & 0x7FFu) << 4), f16tof32(((v >> 11) & 0x7FFu) << 4), f16tof32(((v >> 22) & 0x3FFu) << 5));
}

// 0.2.32: angle one screen pixel covers. PerScene row 27.y = tan(vertical half FOV) (row 26.x = horizontal).
float PixelAngle()
{
    return 2.0f * PerSceneRows[27].y / float(Size.y);
}

// Indirect#0's exposure: row 311.y if set, else row 311.z / max(1.2 * row 311.x, eps); clamped to row 312 (.y, .z).
float Exposure()
{
    const float4 a = PerSceneRows[311], b = PerSceneRows[312];
    const float e = (a.y > 1e-10f) ? a.y : a.z / max(a.x * 1.2f, 1e-4f);
    return clamp(e, b.y, b.z);
}

// Radiance leaving a mirror-ray hit, toward the ray origin. `ambient` is the light that isn't the sun.
// `cone` = (width at the ray origin, spread angle): the ray's footprint for texture filtering (0.2.32).
// 0.4.4: pow(0, 0) is NaN on the GPU (exp2(log2(0) * 0)). The game's face lights are spots with a 0 cone exponent:
// every pixel outside their cone went NaN, and NaN turns a pixel black (16x16 black squares around heads).
float SafePow(float x, float y)
{
    return (x <= 0.0f) ? (y <= 0.0f ? 1.0f : 0.0f) : pow(x, y);
}

// 0.2.58: light from the game's local lights (point, spot, box-bounded directional) at world point `pw` with normal `nw`,
// following the falloff of the game's tiled lighting (PSO 11809). The first few that contribute get a shadow ray.
float3 LocalLights(float3 pw, float3 nw)
{
    if (!(Flags & 16u))
        return 0.0f;
    const float3 f = normalize(PerSceneRows[16].xyz);
    const float3 rr = normalize(PerSceneRows[13].xyz - dot(PerSceneRows[13].xyz, f) * f);
    const float3 uu = cross(rr, f);
    const float3 rel = pw - PerSceneRows[0].xyz;
    const float3 P = float3(dot(rel, rr), dot(rel, uu), dot(rel, f));
    const float3 N = float3(dot(nw, rr), dot(nw, uu), dot(nw, f));
    float3 sum = 0.0f;
    uint shadowsLeft = (LocalShadowMax >> 9) & 0xFu;  // 0.4.5: hit-shading shadow rays (bits 9..12; default 0 = none)
    uint budget = 512;
    [loop] for (uint type = 0; type < 3; ++type)
    {
        const uint b = min(LightCounts.Load(type * 8), 512u), e = min(LightCounts.Load(type * 8 + 4), 512u);
        [loop] for (uint i = b; i < e && budget > 0; ++i, --budget)
        {
            const uint row = i * 8;
            const uint4 r0 = asuint(LightRows[row]);
            const float4 r1 = LightRows[row + 1], r2 = LightRows[row + 2], r3 = LightRows[row + 3];
            const float4 r4 = LightRows[row + 4];
            const uint r5w = asuint(LightRows[row + 5].w);
            const float k = asfloat(r0.z);
            const float3 colour = r3.xyz;
            if (!(k >= 0.0f && k < 1.0e8f) || !all(colour >= 0.0f) || max(colour.r, max(colour.g, colour.b)) > 1.0e6f)
                continue;  // not a light record (stale or wrong buffer): skip
            const float pHi = f16tof32(r5w >> 16), pLo = f16tof32(r5w & 0xFFFFu);
            const float3 d = r2.xyz - P;  // surface -> light
            const float dist2 = dot(d, d);
            const bool noInvSq = (r0.w & 1u) != 0;
            float att;
            float3 L;
            float range;
            if (type == 1)
            {
                const float along = dot(-d, r1.xyz);
                const float a = saturate((along - r1.w) * 2.0f);
                const float fall = saturate(1.0f - SafePow(max(k * along * along, 0.0f), pHi));
                const float rad = length(cross(r1.xyz, -d));
                const float t = saturate((rad - r4.y) / (r4.x - r4.y));
                const float radial = SafePow(max(t * t * (3.0f - 2.0f * t), 0.0f), pLo);
                att = (noInvSq ? 1.0f : 1.0f / (along * along + 1.0f)) * (3.0f - 2.0f * a) * (a * fall) * (a * fall) * radial;
                L = -r1.xyz;
                range = 2000.0f;
            }
            else
            {
                const float dist = sqrt(dist2);
                L = d / max(dist, 1e-4f);
                const float fall = saturate(1.0f - SafePow(max(k * dist2, 0.0f), pHi));
                att = (noInvSq ? 1.0f : 1.0f / (dist2 + 1.0f)) * fall * fall;
                if (type == 2)
                {
                    const float along = dot(-d, r1.xyz);
                    const float a = saturate((along - r1.w) * 2.0f);
                    const float cosA = saturate(dot(-L, r1.xyz));
                    const float cone = saturate(SafePow(saturate((cosA - r4.y) / max(LightRows[row + 6].w - r4.y, 1e-4f)), pLo));
                    att *= (3.0f - 2.0f * a) * a * a * cone;
                }
                range = dist - 0.1f;
            }
            const float ndl = saturate(dot(N, L));
            const float3 c = colour * att * ndl;
            if (!(max(c.r, max(c.g, c.b)) > 1e-5f) || !all(isfinite(c)))
                continue;
            // 0.4.3: only lamps bright enough to matter get the shadow ray (the first few found were often faint ones,
            // leaving the bright lamp next to the surface unshadowed: fireflies in room reflections).
            if (shadowsLeft > 0 && range > 0.1f && dot(c, float3(0.2126f, 0.7152f, 0.0722f)) > 0.02f * LocalLightScale)
            {
                --shadowsLeft;
                RayDesc sray;
                sray.Origin = pw + nw * 0.05f;
                sray.Direction = rr * L.x + uu * L.y + f * L.z;
                sray.TMin = 0.0f;
                sray.TMax = range;
                RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> sq;
                sq.TraceRayInline(Scene, RAY_FLAG_NONE, kSolidMask, sray);
                PROCEED_ALPHA(sq)
                if (sq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
                    continue;
            }
            sum += c;
        }
    }
    return sum * LocalLightScale;
}

float3 ShadeHit(float3 origin, float3 r, float t, uint instance, uint prim, float2 bary, float3x4 o2w, float3 ambient,
                float sunScale, float2 cone)
{
    float3 albedo = float3(0.5f, 0.5f, 0.5f);
    float3 emission = 0.0f;  // 0.3.6
    float3 hn = -r;  // fallback: facing the ray
    float worldArea = 0.0f;
    if (MaterialCount != 0)
    {
        const uint meshB = Lods.Load(instance * 80 + 4) * 52;
        const uint3 idx = TriangleIndices(meshB, prim);
        const uint posHandle = Meshes.Load(meshB + 0);
        if ((posHandle & 0x3FFFu) >= 8)
        {
            ByteAddressBuffer vb = ResourceDescriptorHeap[kBindlessBase + (posHandle & 0x3FFFu)];
            const uint vbase = (posHandle >> 6) & 0xFFFF00u;
            const float3 p0 = LoadPosition(vb, vbase, idx.x), p1 = LoadPosition(vb, vbase, idx.y), p2 = LoadPosition(vb, vbase, idx.z);
            const float3 on = cross(p1 - p0, p2 - p0);
            if (dot(on, on) > 0.0f)
            {
                hn = normalize(mul((float3x3)o2w, on));
                if (dot(hn, r) > 0.0f) hn = -hn;  // face the incoming ray
            }
            // World-space triangle area: the transform maps SNORM units (int16 / 32767) to world units.
            const float3 w1 = mul((float3x3)o2w, p1 - p0), w2 = mul((float3x3)o2w, p2 - p0);
            worldArea = 0.5f * length(cross(w1, w2)) / (32767.0f * 32767.0f);
        }
        albedo = HitAlbedo(instance, meshB, idx, bary, cone.x + cone.y * t, worldArea, sunScale, emission);
    }
    float3 light = ambient;
    if ((Flags & 1u) && !(Flags & 4u))
    {
        const float3 L = SunDirectionWorld();
        const float ndl = saturate(dot(hn, L));
        if (ndl > 0.0f)
        {
            RayDesc sray;
            sray.Origin = origin + r * t + hn * 0.05f;
            sray.Direction = L;
            sray.TMin = 0.0f;
            sray.TMax = 2000.0f;
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> sq;
            sq.TraceRayInline(Scene, RAY_FLAG_NONE, kSolidMask, sray);
            PROCEED_ALPHA(sq)
            if (sq.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
                light += SunIntensity * sunScale * ndl;
        }
    }
    light += LocalLights(origin + r * t, hn);  // 0.2.58
    return albedo * light + emission;  // 0.3.6: + the material's own light
}

static const float kPi = 3.14159265f;

float Ign(float2 p)
{
    return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

// GGX visible-normal sample (Heitz 2018) in the tangent frame of `n`. `v` points from the surface to the camera.
float3 SampleGgxReflection(float3 n, float3 v, float alpha, float2 u)
{
    const float3 up = abs(n.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
    const float3 tx = normalize(cross(up, n));
    const float3 ty = cross(n, tx);
    const float3 ve = float3(dot(v, tx), dot(v, ty), dot(v, n));
    const float3 vh = normalize(float3(alpha * ve.x, alpha * ve.y, max(ve.z, 1e-4f)));
    const float lensq = vh.x * vh.x + vh.y * vh.y;
    const float3 t1 = lensq > 0.0f ? float3(-vh.y, vh.x, 0.0f) * rsqrt(lensq) : float3(1, 0, 0);
    const float3 t2 = cross(vh, t1);
    const float r = sqrt(u.x), phi = 2.0f * kPi * u.y;
    const float a = r * cos(phi);
    float b = r * sin(phi);
    const float sh = 0.5f * (1.0f + vh.z);
    b = (1.0f - sh) * sqrt(max(1.0f - a * a, 0.0f)) + sh * b;
    const float3 nh = a * t1 + b * t2 + sqrt(max(0.0f, 1.0f - a * a - b * b)) * vh;
    const float3 he = normalize(float3(alpha * nh.x, alpha * nh.y, max(0.0f, nh.z)));
    const float3 h = he.x * tx + he.y * ty + he.z * n;
    const float3 l = reflect(-v, h);
    return dot(l, n) > 1e-3f ? l : reflect(-v, n);
}

uint PackNormal(float3 n)
{
    const uint3 q = uint3(saturate(n * 0.5f + 0.5f) * 1023.0f + 0.5f);
    return q.x | (q.y << 10) | (q.z << 20);
}

float3 UnpackNormal(uint v)
{
    return float3(v & 1023u, (v >> 10) & 1023u, (v >> 20) & 1023u) / 1023.0f * 2.0f - 1.0f;
}

// Everything the composite needs about a pixel (shared by the trace and resolve passes).
struct Surface
{
    bool   valid;      // false: sky, or RT not used here (k = 0 or too faint)
    bool   keepGame;   // 0.2.31: when not valid, keep the game's value (else write no reflection)
    float3 n, view, game, w, probe;
    float  rough, k, e, viewDist, ao, ndv;
    float3 posRel;
};

void DumpLamps(uint dl, float3 posRel, float3 n, uint pixelChannels, float viewDist);  // 0.5.40, defined after DlAtten
Surface LoadSurface(uint2 p, uint addr)
{
    Surface sf = (Surface)0;
    uint2 gs;
    GDepth.GetDimensions(gs.x, gs.y);
    const float2 uv = (float2(p) + 0.5f) / float2(Size);
    const uint2 g = min(uint2(uv * float2(gs)), gs - 1);
    const float depth = GDepth[g];
    sf.game = UnpackR11G11B10(GameSpec.Load(addr));
    sf.keepGame = true;
    if (depth <= 0.0f)
        return sf;
    sf.n = DecodeNormalWorld(GNormal[g].xy);
    const float4 mat = GMat[g];
    sf.rough = saturate(mat.y);
    sf.ao = saturate(mat.x);
    const uint type = GType[g] & 15u;
    if ((SmoothTypes >> type) & 1u)
        sf.rough *= GlassRoughScale;  // 0.2.46: thick glass (doors) reflects sharply instead of like a smudged mirror
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 h = ndc.x * PerSceneRows[13] + ndc.y * PerSceneRows[14] + depth * PerSceneRows[15] + PerSceneRows[16];
    sf.posRel = h.xyz / h.w;
    sf.viewDist = length(sf.posRel);
    sf.view = sf.posRel / max(sf.viewDist, 1e-4f);
    const bool replaceAll = (Flags & 2u) != 0;
    // 0.5.24: art controls (replace-all mode, where the fade range is unused): FadeStart = roughness scale (lower = sharper,
    // glossier reflections), FadeEnd = strength. Starfield's floors are fairly rough dielectrics, so physically their
    // reflections are faint and blurry; the Cyberpunk-like look wants them glossier.
    if (replaceAll && type != 1 && type != 8)
        sf.rough *= lerp(saturate(FadeStart), 1.0f, saturate(mat.z));  // 0.5.27: non-metals only (metals turned into dark, smeary mirrors)
    if (type == 1 || type == 8)
        sf.k = 0.0f;  // skin/hair-like: the game's shader takes their roughness from a table; leave them to the game
    else
        sf.k = replaceAll ? 1.0f : 1.0f - smoothstep(FadeStart, max(FadeEnd, FadeStart + 1e-3f), sf.rough);
    if (sf.k <= 0.0f)
        return sf;
    sf.keepGame = !replaceAll;  // from here on, a skipped pixel gets no reflection in replace-all mode
    const float3 albedo = GAlbedo[g].rgb;
    const float3 f0 = lerp((type == 3 ? 0.028f : 0.04f).xxx, albedo, mat.z);
    sf.ndv = saturate(dot(sf.n, -sf.view));
    const float fres = pow(1.0f - sf.ndv, 5.0f);
    const float3 f0r = f0 + (max((1.0f - sf.rough).xxx, f0) - f0) * fres;
    const float2 ab = BrdfLut.SampleLevel(LinearClamp, float2(sf.ndv, clamp(sf.rough, 0.045f, 0.95f)), 0).xy;
    sf.w = max(f0r * ab.x + ab.y, 1e-4f);
    if (replaceAll)
        sf.w = min(sf.w * lerp(max(FadeEnd, 0.0f), 1.0f, saturate(mat.z)), 1.0f.xxx);  // 0.5.24 strength; 0.5.27: non-metals only
    // 0.2.34: fade (not cut) faint reflections: the weight follows the viewing angle, so on a flat wall a hard
    // threshold made a visible circle (RT inside, nothing outside). Faint pixels fade to the game's value.
    const float wmax = max(sf.w.r, max(sf.w.g, sf.w.b)) * sf.k;
    // 0.2.37: MinWeight 0 (replace-all default) traces every pixel: a faint cut-off still drew a ring near the
    // camera on floors (looking down = low Fresnel), with the game's reflection inside it.
    const float ramp = MinWeight > 0.0f ? smoothstep(0.5f * MinWeight, 1.5f * MinWeight, wmax) : 1.0f;
    if (ramp <= 0.0f)
    {
        sf.keepGame = true;
        return sf;
    }
    sf.k *= ramp;
    sf.e = Exposure();
    sf.probe = sf.game / (sf.w * sf.e);
    sf.valid = true;
    return sf;
}

// Lagarde & de Rousiers 2014 specular occlusion from AO (the game's own term is folded into `probe` on misses).
float SpecularOcclusion(float ndv, float ao, float rough)
{
    return saturate(pow(max(ndv + ao, 1e-4f), exp2(-16.0f * rough - 1.0f)) - 1.0f + ao);
}

// 0.2.34: `q` is the trace-grid position (TraceScale x TraceScale pixels per ray); the traced pixel inside the block
// rotates every frame so the upsample (and DLSS) sees every pixel over time.
uint2 TraceSize()
{
    return (Size + TraceScale - 1) / TraceScale;
}

void TraceGlossy(uint2 q)
{
    const uint2 ts = TraceSize();
    if (q.x >= ts.x || q.y >= ts.y)
        return;
    const uint radAddr = (q.y * ts.x + q.x) * 16;
    const uint f = Frame & 3u;
    const uint2 jitter = TraceScale > 1 ? uint2(f & 1u, f >> 1) : uint2(0, 0);
    const uint2 p = min(q * TraceScale + jitter, Size - 1);
    const uint addr = p.y * PitchBytes + p.x * 4;
    const Surface sf = LoadSurface(p, addr);
    if (!sf.valid)
    {
        Rad.Store4(radAddr, uint4(0, 0, 0, 0));  // view dist 0 = no sample here
        return;
    }
    const float alpha = max(sf.rough * sf.rough, 1e-3f);
    const float2 u = float2(Ign(float2(p) + 5.588238f * float(Frame & 63u)), Ign(float2(p.yx) + 3.1754f * float(Frame & 63u) + 17.0f));
    const float3 r = sf.rough < 0.03f ? reflect(sf.view, sf.n) : SampleGgxReflection(sf.n, -sf.view, alpha, u);
    RayDesc ray;
    ray.Origin = PerSceneRows[0].xyz + sf.posRel + sf.n * (0.05f + sf.viewDist * 0.002f);
    ray.Direction = r;
    ray.TMin = 0.0f;
    ray.TMax = MaxDistance;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    // 0.5.33: skip self-hits. Past the game's level-of-detail switch the drawn surface is a simpler mesh than our RT copy,
    // centimetres apart on curved shapes, so the ray started inside our copy and hit its inner face: copper columns
    // reflected copper (deep red) beyond a circle around the camera. A back-face hit close to the start is passed through.
    const float selfGap = 0.1f + sf.viewDist * 0.02f;
    [loop] for (uint tries = 0; tries < 3; ++tries)
    {
        rq.TraceRayInline(Scene, RAY_FLAG_NONE, kSolidMask, ray);
        PROCEED_ALPHA_NOPLAYER(rq)
        if (tries < 2 && rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT && !rq.CommittedTriangleFrontFace() &&
            rq.CommittedRayT() < selfGap)
        {
            ray.TMin = rq.CommittedRayT() + 0.01f;
            continue;
        }
        break;
    }
    float3 radiance = sf.probe;  // miss: what the game's probe sees in that direction
    bool onScreen = false;
    if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT && (Flags & 320u) == 320u)
    {
        // 0.4.4: a hit that was visible last frame takes its real lit colour from last frame's image (the GI capture),
        // like screen-space reflections: no per-ray shadow tests, so room reflections stop crawling. Off-screen hits
        // (and those behind glass or smoke, marked unusable in the capture) are ray-traced as before.
        const uint prev = asuint(GlassStrength);
        const float3 pr = ray.Origin + r * rq.CommittedRayT() - asfloat(Rad.Load3(prev));
        const float4 clip = pr.x * asfloat(Rad.Load4(prev + 80)) + pr.y * asfloat(Rad.Load4(prev + 96)) +
                            pr.z * asfloat(Rad.Load4(prev + 112)) + asfloat(Rad.Load4(prev + 128));
        if (clip.w > 1e-4f)
        {
            const float2 uvp = float2(clip.x / clip.w * 0.5f + 0.5f, 0.5f - clip.y / clip.w * 0.5f);
            if (all(uvp >= 0.0f) && all(uvp < 1.0f))
            {
                const uint2 pp = min(uint2(uvp * float2(Size)), Size - 1);
                const uint2 d = PrevFrame.Load2((pp.y * Size.x + pp.x) * 8);
                const float expect = length(pr);
                if (abs(asfloat(d.y) - expect) < 0.03f * expect + 0.1f)
                {
                    radiance = UnpackR11G11B10(d.x) / max(sf.e, 1e-6f);
                    radiance *= SpecularOcclusion(sf.ndv, sf.ao, sf.rough);
                    onScreen = true;
                }
            }
        }
    }
    if (!onScreen && rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        // Sun scaled by 1 / exposure: a sunlit white surface comes out about as bright as the exposure maps to 1.
        // `probe` is the light leaving surfaces the probe saw, so it already holds their albedo; ProbeAmbient
        // (default 2.5) makes up for counting albedo twice.
        const float pix = PixelAngle() * float(TraceScale);
        radiance = ShadeHit(ray.Origin, r, rq.CommittedRayT(), rq.CommittedInstanceID(), rq.CommittedPrimitiveIndex(),
            rq.CommittedTriangleBarycentrics(), rq.CommittedObjectToWorld3x4(), sf.probe * ProbeAmbient, 1.0f / max(sf.e, 1e-6f),
            float2(sf.viewDist * pix, pix + 0.25f * alpha));
        // 0.5.32: an off-screen hit is lit only by the sun, the game's lamps and a weak ambient term, while the game's
        // probe saw that direction fully lit (bounce light, emissives). Metals show nothing but their reflection, so they
        // went near-black (copper columns, The Viewport's facade) and the dark samples smeared across frames. Never darker
        // than the probe: RT keeps lamp highlights and sunlit hits, the probe fills the rest. Flags bit 10 (Ctrl+Numpad 5).
        if (Flags & 1024u)
            radiance = max(radiance, sf.probe);
        radiance *= SpecularOcclusion(sf.ndv, sf.ao, sf.rough);  // 0.2.29: was the plain AO
    }
    radiance = min(radiance, 16.0f / max(sf.e, 1e-6f));  // firefly guard (0.4.3: 16x the exposure's white, was 64x)
    Rad.Store4(radAddr, uint4(f32tof16(radiance.r) | (f32tof16(radiance.g) << 16), f32tof16(radiance.b) | (f32tof16(sf.rough) << 16),
                              asuint(sf.viewDist), PackNormal(sf.n)));
}

// Weight of one trace sample for the full-resolution pixel `sf` (normal, distance and roughness must agree).
float SampleWeight(uint4 d, Surface sf, out float3 L)
{
    L = float3(f16tof32(d.x), f16tof32(d.x >> 16), f16tof32(d.y));
    const float vd = asfloat(d.z);
    if (vd <= 0.0f)
        return 0.0f;
    float wt = pow(saturate(dot(UnpackNormal(d.w), sf.n)), 16.0f);
    wt *= saturate(1.0f - abs(vd - sf.viewDist) / (0.03f * sf.viewDist + 0.05f));
    wt *= saturate(1.0f - abs(f16tof32(d.y >> 16) - sf.rough) * 4.0f);
    return wt / (1.0f + dot(L * sf.e, float3(0.2126f, 0.7152f, 0.0722f)));  // Karis: tames bright outliers
}

[numthreads(8, 8, 1)]
void Resolve(uint3 tid : SV_DispatchThreadID)
{
    const uint2 p = tid.xy;
    if (p.x >= Size.x || p.y >= Size.y)
        return;
    const uint addr = p.y * PitchBytes + p.x * 4;
    const Surface sf = LoadSurface(p, addr);
    if ((Flags & 32u) && all(p == Size / 2))
    {
        // 0.2.65: F8 dump of the G-buffer and the reflection decision at the screen centre, into the unused tail of the
        // glossy-sample buffer (read back by RecordReflectionDebug): 0 'MDMP', 4 depth, 8 type word, 12..24 material
        // (ao, rough, metal, w), 28 flags (1 valid, 2 keep game), 32 k, 36 ndv, 40..48 weight, 52..60 game value,
        // 64..72 albedo, 76 material table index word (t5).
        uint2 gs;
        GDepth.GetDimensions(gs.x, gs.y);
        const uint2 g = min(uint2((float2(p) + 0.5f) / float2(Size) * float2(gs)), gs - 1);
        const uint d = Size.x * Size.y * 16 - 512;
        const float4 m = GMat[g];
        Rad.Store(d + 0, 0x504D444Du);
        // 0.5.40: and every lamp near the centre surface (Numpad . probe), 4096 bytes before the end of the buffer.
        if ((Flags & 16u) && sf.viewDist > 0.0f)
        {
            DumpLamps(Size.x * Size.y * 16 - 4096, sf.posRel, sf.n, (GType[g] >> 4) & 15u, sf.viewDist);
            DumpGiRays(Size.x * Size.y * 16 - 4096 + 3328, sf.posRel, DecodeNormalWorld(GNormal[g].xy), sf.viewDist);  // 0.5.47
        }
        else
            Rad.Store4(Size.x * Size.y * 16 - 4096, uint4(0x504D414Cu, 0u, (GType[g] >> 4) & 15u, asuint(sf.viewDist)));
        Rad.Store(d + 4, asuint(GDepth[g]));
        Rad.Store(d + 8, GType[g]);
        Rad.Store4(d + 12, asuint(m));
        Rad.Store(d + 28, (sf.valid ? 1u : 0u) | (sf.keepGame ? 2u : 0u));
        Rad.Store(d + 32, asuint(sf.k));
        Rad.Store(d + 36, asuint(sf.ndv));
        Rad.Store3(d + 40, asuint(sf.w));
        Rad.Store3(d + 52, asuint(sf.game));
        Rad.Store3(d + 64, asuint(GAlbedo[g].rgb));
        Rad.Store(d + 76, asuint(GMatIdx[g].z));
        // 0.3.5: what the crosshair ray hits: 96 instance (~0 = nothing), 100 LOD dword 0 (material | family), 104 distance,
        // 108 texture set index (material +116), 112 mesh, 128..415 standard material entry (family 0), 416..455 texture set.
        const float2 ndc = float2(((float2(p) + 0.5f) / float2(Size)).x * 2.0f - 1.0f, 1.0f - ((float2(p) + 0.5f) / float2(Size)).y * 2.0f);
        const float4 hn = ndc.x * PerSceneRows[13] + ndc.y * PerSceneRows[14] + PerSceneRows[15] + PerSceneRows[16];
        RayDesc cr;
        cr.Origin = PerSceneRows[0].xyz;
        cr.Direction = normalize(hn.xyz / hn.w);
        cr.TMin = 0.5f;
        cr.TMax = 1.0e5f;
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> cq;
        cq.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFBu, cr);
        cq.Proceed();
        Rad.Store(d + 96, 0xFFFFFFFFu);
        if (cq.CommittedStatus() == COMMITTED_TRIANGLE_HIT && MaterialCount != 0)
        {
            const uint inst = cq.CommittedInstanceID();
            const uint bucket = Lods.Load(inst * 80);
            const uint mat = (bucket >> 13) & 0x7FFFu;
            Rad.Store(d + 96, inst);
            Rad.Store(d + 100, bucket);
            Rad.Store(d + 104, asuint(cq.CommittedRayT()));
            Rad.Store(d + 112, Lods.Load(inst * 80 + 4));
            uint ts = 0xFFFFFFFFu;
            if ((bucket & 0x1Fu) == 0 && mat < MaterialCount)
            {
                for (uint b = 0; b < 288; b += 4) Rad.Store(d + 128 + b, Materials.Load(mat * 288 + b));
                ts = Materials.Load(mat * 288 + 116);
                if (ts < 8192)
                    for (uint t = 0; t < 40; t += 4) Rad.Store(d + 416 + t, TextureSets.Load(ts * 40 + t));
            }
            Rad.Store(d + 108, ts);
        }
    }
    if (!sf.valid)
    {
        Out.Store(addr, sf.keepGame ? GameSpec.Load(addr) : 0u);  // not traced: the game's value, or none (0.2.31)
        return;
    }
    // 0.2.34: joint upsample + filter. Samples live on the trace grid; each is weighted against THIS pixel's own
    // normal, distance and roughness, so edges stay sharp at half resolution. The 2x2 trace cells around the
    // pixel are always gathered, plus a Vogel disk (radius ~ sqrt(roughness)) on rough surfaces.
    const uint2 ts = TraceSize();
    const float2 c = (float2(p) + 0.5f) / float(TraceScale) - 0.5f;
    const int2 base = int2(floor(c));
    float3 sum = 0;
    float wsum = 0;
    float3 L;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        const int2 q = clamp(base + int2(k & 1, k >> 1), int2(0, 0), int2(ts) - 1);
        const float wt = SampleWeight(Rad.Load4(GlassMomentIndex + (uint(q.y) * ts.x + uint(q.x)) * 16), sf, L);  // 0.3.4: accumulated
        sum += L * wt;
        wsum += wt;
    }
    const float radius = BlurPx * sqrt(sf.rough) / float(TraceScale);
    if (radius >= 0.75f)
    {
        const int taps = sf.rough > 0.5f ? 20 : 12;  // 0.2.31: more taps on rough surfaces
        const float rot = Ign(float2(p)) * 2.0f * kPi;
        [loop] for (int i = 0; i < taps; ++i)
        {
            const float rr = radius * sqrt((float(i) + 0.5f) / float(taps));
            const float th = float(i) * 2.39996323f + rot;
            const int2 q = clamp(int2(round(c + rr * float2(cos(th), sin(th)))), int2(0, 0), int2(ts) - 1);
            const float wt = SampleWeight(Rad.Load4(GlassMomentIndex + (uint(q.y) * ts.x + uint(q.x)) * 16), sf, L);
            sum += L * wt;
            wsum += wt;
        }
    }
    const float3 Lf = wsum > 0.0f ? sum / wsum : sf.probe;
    const float3 rt = sf.w * Lf * sf.e * Scale;
    Out.Store(addr, PackR11G11B10(lerp(sf.game, rt, sf.k)));
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (Mode == 5)
    {
        TraceGlossy(tid.xy);  // 0.2.34: dispatched over the trace grid
        return;
    }
    const uint2 p = tid.xy;
    if (p.x >= Size.x || p.y >= Size.y)
        return;
    const uint addr = p.y * PitchBytes + p.x * 4;
    if (Mode == 7)
    {
        // 0.2.46: G-buffer material type legend. 0 black, 1 red, 2 green, 3 blue, 4 yellow, 5 cyan, 6 magenta, 7 white,
        // 8 orange, 9 purple, 10 teal, 11 pink, 12 grey, 13 brown, 14 lime, 15 navy.
        static const float3 kLegend[16] = {
            float3(0, 0, 0), float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1), float3(1, 1, 0), float3(0, 1, 1), float3(1, 0, 1), float3(1, 1, 1),
            float3(1, 0.5f, 0), float3(0.5f, 0, 1), float3(0, 0.5f, 0.5f), float3(1, 0.5f, 0.75f), float3(0.5f, 0.5f, 0.5f), float3(0.5f, 0.25f, 0),
            float3(0.5f, 1, 0), float3(0, 0, 0.5f) };
        uint2 gs3;
        GDepth.GetDimensions(gs3.x, gs3.y);
        const uint2 g3 = min(uint2((float2(p) + 0.5f) / float2(Size) * float2(gs3)), gs3 - 1);
        const float3 c = GDepth[g3] <= 0.0f ? float3(0, 0, 0) : kLegend[GType[g3] & 15u];
        Out.Store(addr, PackR11G11B10(c * Scale * 4.0f));
        return;
    }
    if (Mode == 6)
    {
        // 0.2.45: coverage. What the RT reflection pass does with each pixel: green = RT replaces the reflection
        // (brightness = blend weight), red = left to the game (material type 1/8), yellow = left to the game for another
        // reason, grey = no surface. Blue adds the G-buffer material type (0..15 -> 0..1).
        uint2 gs2;
        GDepth.GetDimensions(gs2.x, gs2.y);
        const float2 uv2 = (float2(p) + 0.5f) / float2(Size);
        const uint2 g2 = min(uint2(uv2 * float2(gs2)), gs2 - 1);
        const uint type = GType[g2] & 15u;
        const Surface sf = LoadSurface(p, addr);
        float3 c;
        if (GDepth[g2] <= 0.0f)
            c = float3(0.15f, 0.15f, 0.15f);
        else if (type == 1 || type == 8)
            c = float3(1.0f, 0.0f, 0.0f);
        else if (sf.valid)
            c = float3(0.0f, 0.25f + 0.75f * sf.k, 0.0f);
        else
            c = float3(1.0f, 1.0f, 0.0f);
        c.b = float(type) / 15.0f;
        Out.Store(addr, PackR11G11B10(c * Scale));
        return;
    }

    uint2 gs;
    GDepth.GetDimensions(gs.x, gs.y);
    const float2 uv = (float2(p) + 0.5f) / float2(Size);
    const uint2 g = min(uint2(uv * float2(gs)), gs - 1);
    const float depth = GDepth[g];
    if (depth <= 0.0f)
    {
        Out.Store(addr, 0u);
        return;
    }
    const float3 n = DecodeNormalWorld(GNormal[g].xy);
    const float rough = saturate(GMat[g].y);
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 h = ndc.x * PerSceneRows[13] + ndc.y * PerSceneRows[14] + depth * PerSceneRows[15] + PerSceneRows[16];
    const float3 posRel = h.xyz / h.w;
    const float viewDist = length(posRel);
    const float3 view = posRel / max(viewDist, 1e-4f);
    const float3 r = reflect(view, n);
    RayDesc ray;
    ray.Origin = PerSceneRows[0].xyz + posRel + n * (0.05f + viewDist * 0.002f);
    ray.Direction = r;
    ray.TMin = 0.0f;
    ray.TMax = MaxDistance;

    float3 colour;
    if (Mode == 1)
    {
        colour = n * 0.5f + 0.5f;
    }
    else if (Mode == 3)
    {
        colour = rough.xxx;
    }
    else
    {
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        q.TraceRayInline(Scene, RAY_FLAG_NONE, kSolidMask, ray);
        PROCEED_ALPHA_NOPLAYER(q)
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT && Mode == 4)
        {
            colour = ShadeHit(ray.Origin, r, q.CommittedRayT(), q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                q.CommittedTriangleBarycentrics(), q.CommittedObjectToWorld3x4(), Ambient.xxx, 1.0f,
                float2(viewDist * PixelAngle(), PixelAngle()));
        }
        else if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            const float t = q.CommittedRayT();
            colour = HashColour(q.CommittedInstanceID()) * (1.0f / (1.0f + t * 0.01f));
        }
        else if (Mode == 4)
        {
            colour = float3(0.35f, 0.5f, 0.8f) * Ambient * 2.0f;
        }
        else
        {
            colour = float3(0.25f, 0.45f, 0.9f) * saturate(r.z * 0.5f + 0.5f);
        }
    }
    Out.Store(addr, PackR11G11B10(colour * Scale));
}

// ---------------------------------------------------------------- 0.2.51 (G3): RT reflections on thin glass
// Thin glass is not in the G-buffer: it is drawn after the opaque scene with moment-based OIT. This pass runs right after
// CompositeMomentBasedOIT, on a copy of the HDR scene target (Out). Per pixel:
//   1. Skip unless the OIT moments say a blended surface covers the pixel (b0 > GlassMinCoverage). Additive effects
//      (light beams, glows) don't write moments, so they never get a reflection.
//   2. Trace a camera ray against the see-through instances only (mask 0x02: material flags +108 bit 0 clear) up to just
//      before the opaque surface. A hit there is the glass surface (thick glass in the G-buffer sits at the opaque depth
//      and is excluded by the margin: it already has its RT reflection).
//   3. Mirror ray from the glass (Schlick Fresnel, F0 0.04), shaded like the main reflection pass, added on top.
float3 GeometricNormal(uint instance, uint prim, float3x4 o2w, float3 dir)
{
    float3 n = -dir;
    if (MaterialCount == 0)
        return n;
    const uint meshB = Lods.Load(instance * 80 + 4) * 52;
    const uint posHandle = Meshes.Load(meshB + 0);
    if ((posHandle & 0x3FFFu) < 8)
        return n;
    const uint3 idx = TriangleIndices(meshB, prim);
    ByteAddressBuffer vb = ResourceDescriptorHeap[kBindlessBase + (posHandle & 0x3FFFu)];
    const uint vbase = (posHandle >> 6) & 0xFFFF00u;
    const float3 p0 = LoadPosition(vb, vbase, idx.x), p1 = LoadPosition(vb, vbase, idx.y), p2 = LoadPosition(vb, vbase, idx.z);
    const float3 on = cross(p1 - p0, p2 - p0);
    if (dot(on, on) <= 0.0f)
        return n;
    n = normalize(mul((float3x3)o2w, on));
    return dot(n, dir) > 0.0f ? -n : n;  // face the camera
}

[numthreads(8, 8, 1)]
void Glass(uint3 tid : SV_DispatchThreadID)
{
    const uint2 p = tid.xy;
    if (p.x >= Size.x || p.y >= Size.y)
        return;
    // 0.2.55: the coverage gate is off by default (GlassMinCoverage < 0): every see-through instance the camera ray finds
    // gets a reflection. Flags bit 8 = stage debug view (see the end of this function).
    const bool debugView = (Flags & 8u) != 0;
    Texture2D<float> moments = ResourceDescriptorHeap[GlassMomentIndex];
    uint2 ms;
    moments.GetDimensions(ms.x, ms.y);
    const uint2 mp = min(p * ms / Size, ms - 1);
    const float b0 = moments.Load(int3(mp, 0));
    const bool covered = b0 > max(GlassMinCoverage, 0.0f);
    // 0.4.10: fade the reflection in with the coverage (full from about 0.02 above the threshold) instead of switching it
    // on at the threshold: thin smoke drifting over a spot could flip a full-strength reflection on and off.
    const float coverFade = saturate((b0 - max(GlassMinCoverage, 0.0f)) / 0.02f);
    const uint addr = p.y * PitchBytes + p.x * 4;
    const float3 scene = UnpackR11G11B10(Out.Load(addr));
    const float sceneLum = dot(scene, float3(0.3f, 0.59f, 0.11f)) + 0.02f;
    if ((Flags & 32u) && all(p == Size / 2))
    {
        // 0.3.6: the final HDR colour and exposure at the crosshair (calibrates material emission against the screen).
        Rad.Store3(416, asuint(scene));
        Rad.Store(428, asuint(Exposure()));
    }
    // Debug colours (0.2.56): blue = not covered by OIT glass; magenta = covered, camera ray found nothing; pure red =
    // covered, but the depth buffer leaves the ray no room; yellow = hit a glass-tagged instance; white = hit an
    // instance not tagged as glass (still reflected).
    float3 dbg = float3(0.0f, 0.0f, 1.0f) + (covered ? float3(1.0f, 0.0f, 0.0f) : 0.0f);
    #define GLASS_DEBUG_OUT { if (debugView) Out.Store(addr, PackR11G11B10(lerp(scene, dbg * sceneLum * 2.0f, 0.6f))); }
    if (GlassMinCoverage >= 0.0f && !covered)
    {
        GLASS_DEBUG_OUT
        return;
    }

    Texture2D<float> depthTex = ResourceDescriptorHeap[GlassDepthIndex];
    const float depth = depthTex.Load(int3(p, 0));
    const float2 uv = (float2(p) + 0.5f) / float2(Size);
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 hNear = ndc.x * PerSceneRows[13] + ndc.y * PerSceneRows[14] + PerSceneRows[15] + PerSceneRows[16];  // depth 1 = near plane
    const float3 dir = normalize(hNear.xyz / hNear.w);
    float opaque = 1.0e5f;  // sky: nothing behind
    if (depth > 0.0f)
    {
        const float4 h = ndc.x * PerSceneRows[13] + ndc.y * PerSceneRows[14] + depth * PerSceneRows[15] + PerSceneRows[16];
        opaque = length(h.xyz / h.w);
    }
    RayDesc pr;
    pr.Origin = PerSceneRows[0].xyz;
    pr.Direction = dir;
    pr.TMin = 0.5f;  // 0.2.67: past the player's own head (first person), which the camera ray would otherwise hit
    pr.TMax = opaque - (0.05f + opaque * 0.01f);
    if (pr.TMax <= pr.TMin)
    {
        dbg = float3(1.0f, 0.0f, 0.0f);
        GLASS_DEBUG_OUT
        return;
    }
    // 0.2.56: glass-tagged instances first; inside an OIT-covered pixel, anything else in front of the opaque surface is
    // also the glass (some window panes are not tagged as see-through), so fall back to every instance.
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> gq;
    gq.TraceRayInline(Scene, RAY_FLAG_NONE, 0x02u, pr);
    gq.Proceed();
    bool tagged = gq.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    bool glassHit = tagged;
    if (!tagged && covered && !(Flags & 512u))  // 0.4.10: Flags bit 9 = untagged-window fallback off
    {
        gq.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFBu, pr);  // 0.2.64: everything except effects (mask 0x04)
        gq.Proceed();
        // 0.3.8: the fallback exists for untagged window panes (other material families). Drifting smoke turns the OIT
        // coverage on and off over the floor; at grazing angles the RT copy of the floor itself sits a little in front
        // of the drawn floor, so it was taken for a pane and got a flickering reflection. Reject ordinary solid
        // materials (family 0 with the solid flag) and hits that are not clearly in front of the surface behind.
        if (gq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            glassHit = (opaque - gq.CommittedRayT()) > 0.15f + 0.03f * min(opaque, 1000.0f);
            if (gq.CommittedInstanceContributionToHitGroupIndex() == 1u)
                glassHit = false;  // 0.4.4: the first-person player body is never a pane
            // 0.3.10: and clearly off the plane of the surface behind (G-buffer normal there). Along a grazing ray, a
            // centimetre between the RT floor and the drawn floor becomes tens of centimetres, so the distance along the
            // ray let the walkway floor (a non-standard material family) pass as a pane. Panes stand well off the wall
            // or floor behind them; a surface's own RT copy is within a few centimetres of its plane.
            if (glassHit && depth > 0.0f)
            {
                uint2 gs;
                GDepth.GetDimensions(gs.x, gs.y);
                const uint2 g = min(uint2(uv * float2(gs)), gs - 1);
                const float3 nb = DecodeNormalWorld(GNormal[g].xy);
                const float3 hitRel = dir * gq.CommittedRayT(), backRel = dir * opaque;
                if (abs(dot(hitRel - backRel, nb)) < 0.2f)
                    glassHit = false;
            }
            const uint bucket = Lods.Load(gq.CommittedInstanceID() * 80);
            const uint mat = (bucket >> 13) & 0x7FFFu;
            if ((bucket & 0x1Fu) == 0u && mat < MaterialCount && (Materials.Load(mat * 288 + 108) & 1u) != 0u)
                glassHit = false;
        }
    }
    if (!glassHit)
    {
        if ((Flags & 32u) && all(p == Size / 2))
        {
            Rad.Store(0, 0x504D4447u);
            Rad.Store(4, covered ? 1u : 0u);
            Rad.Store(8, 0xFFFFFFFFu);  // nothing hit
        }
        GLASS_DEBUG_OUT
        return;
    }
    dbg = tagged ? float3(1.0f, 1.0f, 0.0f) : float3(1.0f, 1.0f, 1.0f);
    if ((Flags & 32u) && all(p == Size / 2))
    {
        // 0.2.62: F8 dump of what the glass pass found at the screen centre (read back by RecordGlassReflections):
        // 0 'GDMP', 4 covered, 8 instance, 12 tagged, 16 distance, 20 LOD dword 0 (material | type), 24 primitive,
        // 32..127 instance record (96 B), 128..415 standard material entry (288 B, if type 0).
        const uint inst = gq.CommittedInstanceID();
        const uint bucket = Lods.Load(inst * 80);
        Rad.Store(0, 0x504D4447u);
        Rad.Store(4, covered ? 1u : 0u);
        Rad.Store(8, inst);
        Rad.Store(12, tagged ? 1u : 0u);
        Rad.Store(16, asuint(gq.CommittedRayT()));
        Rad.Store(20, bucket);
        Rad.Store(24, gq.CommittedPrimitiveIndex());
        for (uint b = 0; b < 96; b += 4) Rad.Store(32 + b, Instances.Load(inst * 96 + b));
        const uint mat = (bucket >> 13) & 0x7FFFu;
        if ((bucket & 0x1Fu) == 0 && mat < MaterialCount)
            for (uint m = 0; m < 288; m += 4) Rad.Store(128 + m, Materials.Load(mat * 288 + m));
    }

    const float tg = gq.CommittedRayT();
    const float3 n = GeometricNormal(gq.CommittedInstanceID(), gq.CommittedPrimitiveIndex(), gq.CommittedObjectToWorld3x4(), dir);
    const float ndv = saturate(dot(n, -dir));
    const float fresnel = 0.04f + 0.96f * pow(1.0f - ndv, 5.0f);
    const float3 r = reflect(dir, n);

    RayDesc ray;
    ray.Origin = pr.Origin + dir * tg + n * (0.02f + tg * 0.001f);
    ray.Direction = r;
    ray.TMin = 0.0f;
    ray.TMax = MaxDistance;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    rq.TraceRayInline(Scene, RAY_FLAG_NONE, kSolidMask, ray);
    PROCEED_ALPHA_NOPLAYER(rq)

    const float e = max(Exposure(), 1e-6f);
    const float3 ambient = Ambient.xxx * ProbeAmbient / e;  // no probe here: a flat ambient (scene units)
    float3 radiance;
    if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        const float pix = PixelAngle();
        radiance = ShadeHit(ray.Origin, r, rq.CommittedRayT(), rq.CommittedInstanceID(), rq.CommittedPrimitiveIndex(),
            rq.CommittedTriangleBarycentrics(), rq.CommittedObjectToWorld3x4(), ambient, 1.0f / e, float2(tg * pix, pix));
    }
    else
    {
        radiance = float3(0.35f, 0.5f, 0.8f) * Ambient * 2.0f * saturate(r.z * 0.5f + 0.5f) / e;  // sky-ish
    }
    radiance = min(radiance, 64.0f / e);
    if (debugView)
    {
        GLASS_DEBUG_OUT
        return;
    }
    Out.Store(addr, PackR11G11B10(scene + radiance * e * fresnel * GlassStrength * (GlassMinCoverage >= 0.0f ? coverFade : 1.0f)));
}

// ---------------------------------------------------------------- 0.3.0: RT diffuse GI
// Replaces the game's indirect diffuse (GatherIndirect SV_Target0 = diffuse colour / pi x irradiance x exposure, with
// diffuse colour = albedo x (1 - metalness)) by ray-traced one-bounce light: one cosine-weighted ray per trace cell, hits
// shaded like reflection hits (sun with shadow, the game's local lights, plus a share of the game's own ambient as a
// multi-bounce stand-in), escaped rays see the game's ambient. Three passes over the same root signature:
//   GiTrace (trace grid) -> GiTemporal (trace grid, reprojected history) -> GiResolve (full res, edge-aware filter).
// Out = GI output footprint, GameSpec = copy of the game's indirect diffuse, Rad = GI scratch buffer.
// Constants the GI passes reuse (unused by them otherwise):
#define GiStrength    GlassStrength     // output scale (1 = physical)
#define GiBounce      GlassMinCoverage  // share of the game's ambient that lights ray hits (multi-bounce stand-in)
#define GiTraceOffset GlassDepthIndex   // byte offsets in Rad: this frame's ray results,
#define GiHistRead    GlassMomentIndex  //   last frame's history,
#define GiHistWrite   SmoothTypes       //   this frame's history,
#define GiPrevOffset  Mode              //   last frame's camera (PerScene rows 0..8)
#define GiRayLength   FadeStart
#define GiMaxHistory  FadeEnd
#define GiRadius      BlurPx            // spatial filter radius, trace cells
#define GiScreenLight GlassRoughScale   // 0.3.3: scale of light taken from last frame's image (1 = physical; >1 can run away)
// Flags: bit 6 (64) = history usable, bit 7 (128) = debug: show the GI light alone (grey surfaces),
//        bit 8 (256) = PrevFrame (last frame's lit image) usable.

struct GiPix
{
    bool   valid;
    float3 posRel, n, diffuse;
    float  viewDist;
};

GiPix LoadGiPixel(uint2 p)
{
    GiPix s = (GiPix)0;
    uint2 gs;
    GDepth.GetDimensions(gs.x, gs.y);
    const float2 uv = (float2(p) + 0.5f) / float2(Size);
    const uint2 g = min(uint2(uv * float2(gs)), gs - 1);
    const float depth = GDepth[g];
    if (depth <= 0.0f)
        return s;
    s.n = DecodeNormalWorld(GNormal[g].xy);
    s.diffuse = GAlbedo[g].rgb * (1.0f - saturate(GMat[g].z));
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 h = ndc.x * PerSceneRows[13] + ndc.y * PerSceneRows[14] + depth * PerSceneRows[15] + PerSceneRows[16];
    s.posRel = h.xyz / h.w;
    s.viewDist = length(s.posRel);
    s.valid = s.viewDist > 0.0f;
    // 0.5.47: a normal clearly facing away from the camera (two-sided thin surfaces such as the lobby chair backs) sent
    // the GI rays out of the back, into the chair frame: dark frame-shaped patterns on the chairs. Face it to the camera.
    if (s.valid && dot(s.n, s.posRel) > 0.25f * s.viewDist)
        s.n = -s.n;
    return s;
}

uint GiHash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

// The game's own ambient at a pixel, as radiance (scene units): its indirect diffuse / (diffuse colour x exposure).
float3 GiGameRadiance(uint addr, float3 diffuse, float e)
{
    return UnpackR11G11B10(GameSpec.Load(addr)) / (max(diffuse, 0.04f.xxx) * e);
}

uint4 GiPackCell(float3 L, float count, float viewDist, float3 n)
{
    return uint4(f32tof16(L.r) | (f32tof16(L.g) << 16), f32tof16(L.b) | (f32tof16(count) << 16), asuint(viewDist), PackNormal(n));
}

void GiUnpackCell(uint4 d, out float3 L, out float count, out float viewDist, out float3 n)
{
    L = float3(f16tof32(d.x & 0xFFFFu), f16tof32(d.x >> 16), f16tof32(d.y & 0xFFFFu));
    count = f16tof32(d.y >> 16);
    viewDist = asfloat(d.z);
    n = UnpackNormal(d.w);
}

uint2 GiCellPixel(uint2 q)
{
    const uint f = Frame & 3u;
    const uint2 jitter = TraceScale > 1 ? uint2(f & 1u, f >> 1) : uint2(0, 0);
    return min(q * TraceScale + jitter, Size - 1);
}

// 0.4.7: world position of last frame's sample in trace cell pq: last frame's camera (rows 0 and 13..16 stored at
// `prev`), the pixel last frame's jitter picked in that cell, and the view distance stored with it.
float3 PrevSampleWorld(uint prev, uint2 pq, float dist)
{
    const uint f = (Frame - 1u) & 3u;
    const uint2 jitter = TraceScale > 1 ? uint2(f & 1u, f >> 1) : uint2(0, 0);
    const uint2 pp = min(pq * TraceScale + jitter, Size - 1);
    const float2 uv = (float2(pp) + 0.5f) / float2(Size);
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 h = ndc.x * asfloat(Rad.Load4(prev + 208)) + ndc.y * asfloat(Rad.Load4(prev + 224)) +
                     asfloat(Rad.Load4(prev + 240)) + asfloat(Rad.Load4(prev + 256));  // depth 1 = near plane
    return asfloat(Rad.Load3(prev)) + normalize(h.xyz / h.w) * dist;
}

// 0.4.7: is last frame's sample on this surface? Same plane (distance along the normal), not the same distance from the
// camera: on walls and ceilings seen at a slant, neighbouring pixels differ by more than 5% in camera distance, so the
// old test kept throwing their history away while moving (raw noise, blurred wide: the "waving").
bool SameSurface(float3 prevWorld, float3 world, float3 n, float dist)
{
    const float3 d = prevWorld - world;
    return abs(dot(d, n)) < 0.03f + 0.01f * dist && dot(d, d) < (0.3f + 0.05f * dist) * (0.3f + 0.05f * dist);
}

[numthreads(8, 8, 1)]
void GiTrace(uint3 tid : SV_DispatchThreadID)
{
    const uint2 q = tid.xy;
    const uint2 ts = TraceSize();
    if (q.x >= ts.x || q.y >= ts.y)
        return;
    const uint cell = GiTraceOffset + (q.y * ts.x + q.x) * 16;
    const uint2 p = GiCellPixel(q);
    const GiPix s = LoadGiPixel(p);
    if (!s.valid)
    {
        Rad.Store4(cell, uint4(0, 0, asuint(-1.0f), 0));
        return;
    }
    const float e = max(Exposure(), 1e-6f);
    const float3 gameL = GiGameRadiance(p.y * PitchBytes + p.x * 4, s.diffuse, e);

    // 0.5.15: characters keep the game's own indirect light. Faces and hair animate every frame, so our GI history never
    // builds up there and the raw ray noise showed as a dot grid on faces (gone with GI off in the tester's check). One
    // camera ray: if the surface this pixel shows is a character (hit group 2), store the game's value instead of tracing.
    uint selfInst = 0xFFFFFFFFu;  // 0.5.48: the object this pixel shows, as found by the camera ray
    {
        RayDesc cr;
        cr.Origin = PerSceneRows[0].xyz;
        cr.Direction = s.posRel / max(length(s.posRel), 1e-4f);
        // 0.5.19: start just in front of the surface. From the camera, the ray first hit the player's own first-person body
        // copy (hit group 1, around the camera) at some angles, so the NPC behind it wasn't recognised: blotchy faces
        // "depending on where they stood".
        cr.TMin = max(0.05f, s.viewDist - 0.5f);
        cr.TMax = s.viewDist + 0.3f;
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> cq;
        cq.TraceRayInline(Scene, RAY_FLAG_NONE, kSolidMask, cr);
        cq.Proceed();
        if (cq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            selfInst = cq.CommittedInstanceID();
        bool character = cq.CommittedStatus() == COMMITTED_TRIANGLE_HIT && cq.CommittedInstanceContributionToHitGroupIndex() == 2u &&
                         cq.CommittedRayT() > s.viewDist - 0.3f;
        if (!character)
        {
            // 0.5.21: and a short ray back into the surface along its normal. On the side of a face turning away from the
            // camera, the camera ray grazes the face and slipped past our (morph-less) copy to the background: dots on
            // the shadowed side of faces. The normal ray hits the copy whatever the view angle.
            RayDesc nr;
            nr.Origin = PerSceneRows[0].xyz + s.posRel + s.n * 0.12f;
            nr.Direction = -s.n;
            nr.TMin = 0.0f;
            nr.TMax = 0.3f;
            RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> nq;
            nq.TraceRayInline(Scene, RAY_FLAG_NONE, kSolidMask, nr);
            nq.Proceed();
            character = nq.CommittedStatus() == COMMITTED_TRIANGLE_HIT && nq.CommittedInstanceContributionToHitGroupIndex() == 2u;
        }
        if (character)
        {
            Rad.Store4(cell, GiPackCell(gameL / max(GiStrength, 1e-3f), 1.0f, s.viewDist, s.n));  // resolve scales by GiStrength
            return;
        }
    }

    // Cosine-weighted direction around the normal.
    // 0.4.8: Scale = rays per cell (Ultra: 2). Each extra ray walks the same sequence from a shifted start.
    const uint nRays = clamp(uint(Scale + 0.5f), 1u, 8u);
    float3 Lsum = 0.0f;
    [loop] for (uint ri = 0; ri < nRays; ++ri)
    {
        // 0.3.4: low-discrepancy over time (each pixel walks a golden-ratio sequence from its own offset): the 16-32 frames
        // the temporal pass blends cover the hemisphere evenly instead of clumping, so the result settles with less noise.
        const float2 u = frac(float2(Ign(float2(p)), Ign(float2(p.yx) + 37.0f)) + (float(Frame & 1023u) * float(nRays) + float(ri)) * float2(0.6180340f, 0.7548777f));
        const float3 up = abs(s.n.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
        const float3 tx = normalize(cross(up, s.n));
        const float3 ty = cross(s.n, tx);
        const float r = sqrt(u.x), phi = 2.0f * kPi * u.y;
        const float3 dir = normalize(tx * (r * cos(phi)) + ty * (r * sin(phi)) + s.n * sqrt(max(1.0f - u.x, 0.0f)));

        RayDesc ray;
        ray.Origin = PerSceneRows[0].xyz + s.posRel + s.n * (0.05f + s.viewDist * 0.002f);
        ray.Direction = dir;
        ray.TMin = 0.0f;
        ray.TMax = GiRayLength;
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
        // 0.5.46: skip self-hits, as reflections (0.5.33) and lamp shadows (0.5.45) do. Past the game's level-of-detail
        // switch the drawn surface is a simpler mesh than our RT copy, the ray started inside our copy and hit its inner
        // face, so surfaces went dark beyond a ring around the camera (stepped shadows on the lobby ceiling).
        // 0.5.48: and any hit on the pixel's own object within that gap, front face too. Probe on a lobby chair at 5.3 m:
        // our RT chair sat 7 cm in front of the chair the game draws at that distance, so all 15 GI rays hit the chair's own
        // front face 1-6 cm out: dark spots on the chair backs that grew with distance.
        const float selfGap = 0.1f + s.viewDist * 0.02f;
        [loop] for (uint tries = 0; tries < 4; ++tries)
        {
            rq.TraceRayInline(Scene, RAY_FLAG_NONE, kSolidMask, ray);
            PROCEED_ALPHA_NOPLAYER(rq)
            if (tries < 3 && rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT && rq.CommittedRayT() < selfGap &&
                (!rq.CommittedTriangleFrontFace() || rq.CommittedInstanceID() == selfInst))
            {
                ray.TMin = rq.CommittedRayT() + 0.01f;
                continue;
            }
            break;
        }
        float3 L;
        if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            const float3 hitPos = ray.Origin + dir * rq.CommittedRayT();
            bool onScreen = false;
            if ((Flags & 320u) == 320u)  // last frame's image and camera both usable
            {
                // 0.3.3: was the hit visible last frame? Project it with last frame's camera and compare distances.
                const float3 prevCam = asfloat(Rad.Load3(GiPrevOffset));
                const float3 pr = hitPos - prevCam;
                const float4 r5 = asfloat(Rad.Load4(GiPrevOffset + 5 * 16)), r6 = asfloat(Rad.Load4(GiPrevOffset + 6 * 16));
                const float4 r7 = asfloat(Rad.Load4(GiPrevOffset + 7 * 16)), r8 = asfloat(Rad.Load4(GiPrevOffset + 8 * 16));
                const float4 clip = pr.x * r5 + pr.y * r6 + pr.z * r7 + r8;
                if (clip.w > 1e-4f)
                {
                    const float2 uvp = float2(clip.x / clip.w * 0.5f + 0.5f, 0.5f - clip.y / clip.w * 0.5f);
                    if (all(uvp >= 0.0f) && all(uvp < 1.0f))
                    {
                        const uint2 pp = min(uint2(uvp * float2(Size)), Size - 1);
                        const uint2 d = PrevFrame.Load2((pp.y * Size.x + pp.x) * 8);
                        const float expect = length(pr);
                        if (abs(asfloat(d.y) - expect) < 0.03f * expect + 0.1f)
                        {
                            L = UnpackR11G11B10(d.x) / (e * max(GiStrength, 1e-3f)) * GiScreenLight;
                            onScreen = true;
                        }
                    }
                }
            }
            if (!onScreen)
            {
                L = ShadeHit(ray.Origin, dir, rq.CommittedRayT(), rq.CommittedInstanceID(), rq.CommittedPrimitiveIndex(),
                    rq.CommittedTriangleBarycentrics(), rq.CommittedObjectToWorld3x4(), gameL * GiBounce, 1.0f / e, float2(0.0f, 0.02f));
            }
        }
        else
        {
            L = gameL;  // escaped: the game's ambient (sky / probe light)
        }
        // 0.4.6: was 32x the exposure's white. A GI ray that hits a small bright lamp panel brings back a huge value and its
        // neighbour misses it: after blurring and blending, the spikes became slowly moving blobs ("waving" walls). 4x keeps
        // lamps lighting the room evenly instead of in rare spikes.
        L = min(max(L, 0.0f), 4.0f / e);
        Lsum += L;
    }
    const float3 Lavg = Lsum / float(nRays);
    Rad.Store4(cell, GiPackCell(Lavg, 1.0f, s.viewDist, s.n));
}

[numthreads(8, 8, 1)]
void GiTemporal(uint3 tid : SV_DispatchThreadID)
{
    const uint2 q = tid.xy;
    const uint2 ts = TraceSize();
    if (q.x >= ts.x || q.y >= ts.y)
        return;
    const uint idx = (q.y * ts.x + q.x) * 16;
    float3 L, n;
    float count, dist;
    GiUnpackCell(Rad.Load4(GiTraceOffset + idx), L, count, dist, n);
    if (dist < 0.0f)
    {
        Rad.Store4(GiHistWrite + idx, uint4(0, 0, asuint(-1.0f), 0));
        return;
    }
    count = 1.0f;
    if (Flags & 64u)
    {
        // Reproject this cell's surface into last frame's view (rows 0 = camera, 5..8 = camera-relative view-projection).
        const GiPix s = LoadGiPixel(GiCellPixel(q));
        const float3 world = PerSceneRows[0].xyz + s.posRel;
        const float3 prevCam = asfloat(Rad.Load3(GiPrevOffset));
        const float3 pr = world - prevCam;
        const float4 r5 = asfloat(Rad.Load4(GiPrevOffset + 5 * 16)), r6 = asfloat(Rad.Load4(GiPrevOffset + 6 * 16));
        const float4 r7 = asfloat(Rad.Load4(GiPrevOffset + 7 * 16)), r8 = asfloat(Rad.Load4(GiPrevOffset + 8 * 16));
        const float4 clip = pr.x * r5 + pr.y * r6 + pr.z * r7 + r8;
        if (s.valid && clip.w > 1e-4f)
        {
            const float2 uvp = float2(clip.x / clip.w * 0.5f + 0.5f, 0.5f - clip.y / clip.w * 0.5f);
            if (all(uvp >= 0.0f) && all(uvp < 1.0f))
            {
                const uint2 pq = min(uint2(uvp * float2(Size) / float(TraceScale)), ts - 1);
                float3 hL, hn;
                float hc, hd;
                GiUnpackCell(Rad.Load4(GiHistRead + (pq.y * ts.x + pq.x) * 16), hL, hc, hd, hn);
                if (hd > 0.0f && SameSurface(PrevSampleWorld(GiPrevOffset, pq, hd), world, n, dist) && dot(hn, n) > 0.8f &&
                    all(isfinite(hL)))
                {
                    // 0.3.4: clamp the history to what this frame's neighbourhood allows (mean +- 3 sigma of the 3x3 ray
                    // results): stops lights that moved or switched off from lingering, without making it noisier.
                    float3 m1 = 0.0f, m2 = 0.0f;
                    float nn = 0.0f;
                    [unroll] for (int y = -1; y <= 1; ++y)
                    [unroll] for (int x = -1; x <= 1; ++x)
                    {
                        const int2 qq = clamp(int2(q) + int2(x, y), int2(0, 0), int2(ts) - 1);
                        float3 cL, cn;
                        float cc, cd;
                        GiUnpackCell(Rad.Load4(GiTraceOffset + (uint(qq.y) * ts.x + uint(qq.x)) * 16), cL, cc, cd, cn);
                        if (cd > 0.0f && abs(cd - dist) < 0.1f * dist + 0.2f)
                        {
                            m1 += cL;
                            m2 += cL * cL;
                            nn += 1.0f;
                        }
                    }
                    if (nn > 0.0f)
                    {
                        const float3 mean = m1 / nn, sigma = sqrt(max(m2 / nn - mean * mean, 0.0f));
                        hL = clamp(hL, mean - 3.0f * sigma, mean + 3.0f * sigma);
                    }
                    count = min(hc + 1.0f, max(GiMaxHistory, 1.0f));
                    L = lerp(hL, L, 1.0f / count);
                }
            }
        }
    }
    Rad.Store4(GiHistWrite + idx, GiPackCell(L, count, dist, n));
}

[numthreads(8, 8, 1)]
void GiResolve(uint3 tid : SV_DispatchThreadID)
{
    const uint2 p = tid.xy;
    if (p.x >= Size.x || p.y >= Size.y)
        return;
    const uint addr = p.y * PitchBytes + p.x * 4;
    const GiPix s = LoadGiPixel(p);
    if (!s.valid)
    {
        Out.Store(addr, GameSpec.Load(addr));  // sky: the game's value
        return;
    }
    const float e = max(Exposure(), 1e-6f);
    const uint2 ts = TraceSize();
    const float2 c = (float2(p) + 0.5f) / float(TraceScale) - 0.5f;
    float3 sum = 0.0f;
    float wsum = 0.0f;
    const float rot = Ign(float2(p)) * 2.0f * kPi;
    const int taps = 16;
    // 0.3.4: little history here (just revealed or fast motion) -> blur wider to hide the raw noise.
    float centreCount;
    {
        float3 cL, cn;
        float cd;
        const int2 cq = clamp(int2(round(c)), int2(0, 0), int2(ts) - 1);
        GiUnpackCell(Rad.Load4(GiHistWrite + (uint(cq.y) * ts.x + uint(cq.x)) * 16), cL, centreCount, cd, cn);
    }
    const float radius = GiRadius * lerp(2.5f, 1.0f, saturate((centreCount - 1.0f) / 7.0f));
    [loop] for (int i = 0; i < taps + 4; ++i)
    {
        int2 q;
        if (i < 4)
        {
            q = int2(floor(c)) + int2(i & 1, i >> 1);  // the 2x2 cells around the pixel
        }
        else
        {
            const float rr = radius * sqrt((float(i - 4) + 0.5f) / float(taps));
            const float th = float(i) * 2.39996323f + rot;
            q = int2(round(c + rr * float2(cos(th), sin(th))));
        }
        q = clamp(q, int2(0, 0), int2(ts) - 1);
        float3 L, n;
        float count, dist;
        GiUnpackCell(Rad.Load4(GiHistWrite + (uint(q.y) * ts.x + uint(q.x)) * 16), L, count, dist, n);
        if (dist <= 0.0f || !all(isfinite(L)))
            continue;
        const float wd = saturate(1.0f - abs(dist - s.viewDist) / (0.05f * s.viewDist + 0.05f));
        const float wn = pow(saturate(dot(n, s.n)), 8.0f);
        const float w = wd * wn * (i < 4 ? 2.0f : 1.0f) * min(count, 8.0f);
        sum += L * w;
        wsum += w;
    }
    const float3 gameL = GiGameRadiance(addr, s.diffuse, e);
    const float3 L = wsum > 1e-4f ? sum / wsum : gameL;
    const float3 diffuse = (Flags & 128u) ? float3(0.5f, 0.5f, 0.5f) : s.diffuse;
    Out.Store(addr, PackR11G11B10(diffuse * L * e * GiStrength));
}

// 0.3.3: after the OIT composite: last frame's lit image for GI. GameSpec = copy of the HDR target (footprint), the depth
// SRV is at heap index GlassDepthIndex; Out = PrevFrame buffer (8 B per pixel).
[numthreads(8, 8, 1)]
void GiCapture(uint3 tid : SV_DispatchThreadID)
{
    const uint2 p = tid.xy;
    if (p.x >= Size.x || p.y >= Size.y)
        return;
    Texture2D<float> depthTex = ResourceDescriptorHeap[GlassDepthIndex];
    const float depth = depthTex.Load(int3(p, 0));
    float dist = 1.0e9f;
    if (depth > 0.0f)
    {
        const float2 uv = (float2(p) + 0.5f) / float2(Size);
        const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        const float4 h = ndc.x * PerSceneRows[13] + ndc.y * PerSceneRows[14] + depth * PerSceneRows[15] + PerSceneRows[16];
        dist = length(h.xyz / h.w);
    }
    // 0.3.9: pixels covered by transparency (smoke, glass, effects) hold their colour, not the surface's: a GI ray hitting
    // the floor behind drifting smoke read the smoke and the GI flickered with it. Mark them unusable (ray falls back to
    // normal hit shading). GlassMomentIndex = the OIT optical-depth texture (half res), like the glass pass.
    if (GlassMomentIndex != 0u)
    {
        Texture2D<float> moments = ResourceDescriptorHeap[GlassMomentIndex];
        uint2 ms;
        moments.GetDimensions(ms.x, ms.y);
        if (moments.Load(int3(min(p * ms / Size, ms - 1), 0)) > 0.001f)
            dist = -1.0f;
    }
    // 0.5.29: metal pixels are mostly their own reflection. A reflection ray that took a metal surface's colour from this
    // image reflected its reflection, frame after frame: copper columns facing each other darkened to a deep red, like an
    // endless mirror tunnel. Metal hits are shaded directly instead (one bounce).
    if (dist > 0.0f)
    {
        uint2 gs;
        GDepth.GetDimensions(gs.x, gs.y);
        const uint2 g = min(uint2((float2(p) + 0.5f) / float2(Size) * float2(gs)), gs - 1);
        if (saturate(GMat[g].z) > 0.5f)
            dist = -1.0f;
    }
    Out.Store2((p.y * Size.x + p.x) * 8, uint2(GameSpec.Load(p.y * PitchBytes + p.x * 4), asuint(dist)));
}

// 0.3.4: temporal accumulation for RT reflections (trace grid). Reprojects each cell's surface into last frame's view,
// clamps last frame's value to this frame's 3x3 neighbourhood (mean +- 2.5 sigma) and blends: rough surfaces keep more
// history (they are the noisy ones), mirror-like ones less (their reflection moves with the view). Same 16-byte cell
// format as TraceGlossy. GlassDepthIndex = last frame's history, GlassMomentIndex = this frame's, GlassStrength (as
// uint) = last frame's camera rows; Flags bit 6 = history usable.
[numthreads(8, 8, 1)]
void ReflTemporal(uint3 tid : SV_DispatchThreadID)
{
    const uint2 q = tid.xy;
    const uint2 ts = TraceSize();
    if (q.x >= ts.x || q.y >= ts.y)
        return;
    const uint idx = (q.y * ts.x + q.x) * 16;
    const uint4 cur = Rad.Load4(idx);
    const float dist = asfloat(cur.z);
    if (dist <= 0.0f)
    {
        Rad.Store4(GlassMomentIndex + idx, cur);
        return;
    }
    float3 L = float3(f16tof32(cur.x), f16tof32(cur.x >> 16), f16tof32(cur.y));
    const float rough = f16tof32(cur.y >> 16);
    const float3 n = UnpackNormal(cur.w);
    if (Flags & 64u)
    {
        const GiPix s = LoadGiPixel(GiCellPixel(q));
        const uint prev = asuint(GlassStrength);
        const float3 pr = PerSceneRows[0].xyz + s.posRel - asfloat(Rad.Load3(prev));
        const float4 clip = pr.x * asfloat(Rad.Load4(prev + 80)) + pr.y * asfloat(Rad.Load4(prev + 96)) +
                            pr.z * asfloat(Rad.Load4(prev + 112)) + asfloat(Rad.Load4(prev + 128));
        if (s.valid && clip.w > 1e-4f)
        {
            const float2 uvp = float2(clip.x / clip.w * 0.5f + 0.5f, 0.5f - clip.y / clip.w * 0.5f);
            if (all(uvp >= 0.0f) && all(uvp < 1.0f))
            {
                const uint2 pq = min(uint2(uvp * float2(Size) / float(TraceScale)), ts - 1);
                const uint4 h = Rad.Load4(GlassDepthIndex + (pq.y * ts.x + pq.x) * 16);
                const float hd = asfloat(h.z);
                float3 hL = float3(f16tof32(h.x), f16tof32(h.x >> 16), f16tof32(h.y));
                if (hd > 0.0f && SameSurface(PrevSampleWorld(prev, pq, hd), PerSceneRows[0].xyz + s.posRel, n, dist) &&
                    dot(UnpackNormal(h.w), n) > 0.9f &&
                    abs(f16tof32(h.y >> 16) - rough) < 0.1f && all(isfinite(hL)))
                {
                    float3 m1 = 0.0f, m2 = 0.0f;
                    float nn = 0.0f;
                    [unroll] for (int y = -1; y <= 1; ++y)
                    [unroll] for (int x = -1; x <= 1; ++x)
                    {
                        const int2 qq = clamp(int2(q) + int2(x, y), int2(0, 0), int2(ts) - 1);
                        const uint4 c = Rad.Load4((uint(qq.y) * ts.x + uint(qq.x)) * 16);
                        const float cd = asfloat(c.z);
                        if (cd > 0.0f && abs(cd - dist) < 0.05f * dist + 0.1f)
                        {
                            const float3 cL = float3(f16tof32(c.x), f16tof32(c.x >> 16), f16tof32(c.y));
                            m1 += cL;
                            m2 += cL * cL;
                            nn += 1.0f;
                        }
                    }
                    if (nn > 0.0f)
                    {
                        const float3 mean = m1 / nn, sigma = sqrt(max(m2 / nn - mean * mean, 0.0f));
                        hL = clamp(hL, mean - 2.5f * sigma, mean + 2.5f * sigma);
                    }
                    const float a = lerp(0.5f, 0.08f, saturate(rough * 2.5f));  // weight of this frame
                    L = lerp(hL, L, a);
                }
            }
        }
    }
    Rad.Store4(GlassMomentIndex + idx, uint4(f32tof16(L.r) | (f32tof16(L.g) << 16), f32tof16(L.b) | (cur.y & 0xFFFF0000u), cur.z, cur.w));
}

// ---------------------------------------------------------------- 0.4.0: RT direct lighting from the game's lamps
// The game's tiled lighting no longer draws its local lights (their index ranges are zeroed, see CaptureLocalLights);
// instead, every lamp's direct light is computed here with the game's own falloff and BRDF conventions (Lambert with
// 1/pi, GGX specular, F0 = lerp(0.04, albedo, metal), as in TiledDeferredLight PSO 11809) and ray-traced shadows for the
// strongest few lamps per pixel. Diffuse goes into the indirect-diffuse target (RT0) and specular into the specular
// target (RT1) of GatherIndirect, which the game's composite adds to its own (sun) direct light.
// Pass 1, DlCull: lamp lists per 16x16-pixel tile (Out = tile buffer, 260 B per tile: count + 64 indices).
// Pass 2, DlShade: Out = RT0 copy (read-modify-write), Rad = RT1 copy, GameSpec = tile buffer.
#define DlScale   GlassStrength      // brightness of RT lamp light (1 = the game's)
#define DlTilesX  GlassDepthIndex    // tiles per row
static const uint kDlTile = 16;
static const uint kDlMaxPerTile = 64;

// Decodes light i (camera-space record) into: position (camera space), axis, falloff parameters. Returns false if the
// record isn't a plausible light.
struct DlLight
{
    float3 pos, axis, colour;
    float  k, nearOff, pHi, pLo, r4x, r4y, coneEnd;
    bool   noInvSq;
    uint   channels;  // 0.4.4: row 0.y, light-channel mask (ANDed with the pixel's G-buffer type word bits 4..7)
    bool   gameShadow;  // 0.5.38: row 0.x bit 20 = the game casts shadows from this lamp (has shadow data in cb0)
};

bool DlLoad(uint i, out DlLight l)
{
    const uint row = i * 8;
    const uint4 r0 = asuint(LightRows[row]);
    const float4 r1 = LightRows[row + 1], r2 = LightRows[row + 2], r3 = LightRows[row + 3], r4 = LightRows[row + 4];
    const uint r5w = asuint(LightRows[row + 5].w);
    l.pos = r2.xyz;
    l.axis = r1.xyz;
    l.nearOff = r1.w;
    l.colour = r3.xyz;
    l.k = asfloat(r0.z);
    l.pHi = f16tof32(r5w >> 16);
    l.pLo = f16tof32(r5w & 0xFFFFu);
    l.r4x = r4.x;
    l.r4y = r4.y;
    l.coneEnd = LightRows[row + 6].w;
    l.noInvSq = (r0.w & 1u) != 0;
    l.channels = r0.y;
    l.gameShadow = ((r0.x >> 20) & 1u) != 0;
    return l.k >= 0.0f && l.k < 1.0e8f && all(l.colour >= 0.0f) && max(l.colour.r, max(l.colour.g, l.colour.b)) < 1.0e6f &&
           all(isfinite(l.pos));
}

// Irradiance-scale attenuation and direction (camera space) of light `l` of `type` at camera-space point P, as the game
// computes it. range = shadow-ray length.
float DlAtten(uint type, DlLight l, float3 P, out float3 L, out float range)
{
    const float3 d = l.pos - P;
    const float dist2 = dot(d, d);
    float att;
    if (type == 1)
    {
        const float along = dot(-d, l.axis);
        const float a = saturate((along - l.nearOff) * 2.0f);
        const float fall = saturate(1.0f - SafePow(max(l.k * along * along, 0.0f), l.pHi));
        const float rad = length(cross(l.axis, -d));
        const float t = saturate((rad - l.r4y) / (l.r4x - l.r4y));
        const float radial = SafePow(max(t * t * (3.0f - 2.0f * t), 0.0f), l.pLo);
        att = (l.noInvSq ? 1.0f : 1.0f / (along * along + 1.0f)) * (3.0f - 2.0f * a) * (a * fall) * (a * fall) * radial;
        L = -l.axis;
        range = 2000.0f;
    }
    else
    {
        const float dist = sqrt(dist2);
        L = d / max(dist, 1e-4f);
        const float fall = saturate(1.0f - SafePow(max(l.k * dist2, 0.0f), l.pHi));
        att = (l.noInvSq ? 1.0f : 1.0f / (dist2 + 1.0f)) * fall * fall;
        if (type == 2)
        {
            const float along = dot(-d, l.axis);
            const float a = saturate((along - l.nearOff) * 2.0f);
            const float cosA = saturate(dot(-L, l.axis));
            const float cone = saturate(SafePow(saturate((cosA - l.r4y) / max(l.coneEnd - l.r4y, 1e-4f)), l.pLo));
            att *= (3.0f - 2.0f * a) * a * a * cone;
        }
        // 0.5.39: stop the shadow ray short of the lamp by its near offset (the game's shadows start there too), and by at
        // least 30 cm. Lamps sit inside their fixtures (strip lights above Jemison's posters): stopping 10 cm short, the
        // ray hit the lamp's own housing and the posters it lights went dark.
        // 0.5.42: and nothing in the last 30% of the way to the lamp. The 0.5.41 probe at a Jemison poster: its main lamp
        // (game-shadowed) was blocked by trim 1.8 m from the lamp, 25% of the way, while the game lit the poster. Shadow maps
        // lose such near-light occluders (near plane, bias, resolution); occluders nearer the surface still shadow.
        range = dist - max(max(0.3f, l.nearOff + 0.1f), 0.3f * dist);
    }
    return att;
}

// 0.5.40: lamp probe. Every lamp within 8 m of the centre surface (box lights at any distance), as our RT lamp pass sees
// it, for comparing with the game's lighting. Header: 'LAMP', count, pixel light channels, view distance. Then 64 bytes
// per lamp: index | type << 16, flags (row 0.x), channels, k / near offset, pHi, pLo, r4x / r4y, cone end, distance,
// attenuation / N.L, luminance, cos(axis, lamp->surface), noInvSq | gameShadow << 1.
void DumpLamps(uint dl, float3 posRel, float3 n, uint pixelChannels, float viewDist)
{
    const float3 f = normalize(PerSceneRows[16].xyz);
    const float3 rr = normalize(PerSceneRows[13].xyz - dot(PerSceneRows[13].xyz, f) * f);
    const float3 uu = cross(rr, f);
    const float3 P = float3(dot(posRel, rr), dot(posRel, uu), dot(posRel, f));
    const float3 N = float3(dot(n, rr), dot(n, uu), dot(n, f));
    uint nL = 0;
    [loop] for (uint type = 0; type < 3; ++type)
    {
        const uint b = min(LightCounts.Load(type * 8), 512u), e = min(LightCounts.Load(type * 8 + 4), 512u);
        [loop] for (uint i = b; i < e && nL < 40; ++i)
        {
            DlLight l;
            if (!DlLoad(i, l))
                continue;
            const float dist = length(l.pos - P);
            if (dist > 8.0f && type != 1)
                continue;
            float3 L;
            float range;
            const float att = DlAtten(type, l, P, L, range);
            const uint o = dl + 64 + nL * 80;  // 0.5.41: 80 bytes per lamp (+ shadow ray result)
            Rad.Store4(o, uint4(i | (type << 16), asuint(LightRows[i * 8].x), l.channels, asuint(l.k)));
            Rad.Store4(o + 16, asuint(float4(l.nearOff, l.pHi, l.pLo, l.r4x)));
            Rad.Store4(o + 32, asuint(float4(l.r4y, l.coneEnd, dist, att)));
            Rad.Store4(o + 48, asuint(float4(saturate(dot(N, L)), dot(l.colour, float3(0.2126f, 0.7152f, 0.0722f)),
                dot(normalize(P - l.pos), l.axis), (l.noInvSq ? 1.0f : 0.0f) + (l.gameShadow ? 2.0f : 0.0f))));
            // 0.5.41: the shadow ray our RT lamp pass would trace (same origin, direction and length): hit distance (-1 =
            // reached the lamp), the instance and the LOD dword of what blocked it, and the ray length.
            float hitT = -1.0f;
            uint hitInst = 0xFFFFFFFFu, hitBucket = 0;
            if (att > 0.0f && range > 0.1f)
            {
                hitT = LampShadowHit(PerSceneRows[0].xyz + posRel + n * (0.03f + viewDist * 0.001f),
                                     rr * L.x + uu * L.y + f * L.z, range, viewDist, hitInst);
                if (hitT >= 0.0f)
                    hitBucket = Lods.Load(hitInst * 80);
                else
                    hitInst = 0xFFFFFFFFu;
            }
            Rad.Store4(o + 64, uint4(asuint(hitT), hitInst, hitBucket, asuint(range)));
            ++nL;
        }
    }
    Rad.Store4(dl, uint4(0x504D414Cu, nL, pixelChannels, asuint(viewDist)));
}

uint DlType(uint i)
{
    // index ranges: [0,1) point, [2,3) box-directional, [4,5) spot (dwords of LightCounts)
    if (i >= LightCounts.Load(16) && i < LightCounts.Load(20)) return 2u;
    if (i >= LightCounts.Load(8) && i < LightCounts.Load(12)) return 1u;
    return 0u;
}

// 0.5.18: each tile keeps two lamp lists, one for its near pixels and one for its far pixels, split at the middle of its
// depth range. A tile across a silhouette (hair or a face against the street) used one box spanning both, ranked its
// lamps for that box and dropped lamps that only mattered for the near surface: squares along hair and profiles.
// Tile record (kDlTileStride bytes): [0] split depth, [4] near count, [8..264) near list, [264] far count, [268..524) far list.
static const uint kDlTileStride = 528;
static const uint kDlMaxCand = 512;
groupshared uint  gsDepthMin, gsDepthMax;
groupshared uint  gsBMin[2], gsBMax[2], gsCount[2];
groupshared uint  gsCand[2][kDlMaxCand];
groupshared float gsScore[2][kDlMaxCand];

[numthreads(16, 16, 1)]
void DlCull(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0)
    {
        gsDepthMin = 0x7F7FFFFFu;
        gsDepthMax = 0u;
        gsBMin[0] = gsBMin[1] = 0x7F7FFFFFu;
        gsBMax[0] = gsBMax[1] = 0u;
        gsCount[0] = gsCount[1] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    const uint2 p = gid.xy * kDlTile + gtid.xy;
    uint2 gs;
    GDepth.GetDimensions(gs.x, gs.y);
    float depth = 0.0f;
    if (p.x < Size.x && p.y < Size.y)
    {
        const uint2 g = min(uint2((float2(p) + 0.5f) / float2(Size) * float2(gs)), gs - 1);
        depth = GDepth[g];
        if (depth > 0.0f)
        {
            InterlockedMin(gsDepthMin, asuint(depth));  // positive floats order like uints
            InterlockedMax(gsDepthMax, asuint(depth));
        }
    }
    GroupMemoryBarrierWithGroupSync();
    const bool any = gsDepthMax != 0u;
    const float split = any ? 0.5f * (asfloat(gsDepthMin) + asfloat(gsDepthMax)) : 0.0f;
    if (depth > 0.0f)
    {
        const uint b = depth >= split ? 0u : 1u;  // reversed Z: larger = nearer; 0 = near bucket
        InterlockedMin(gsBMin[b], asuint(depth));
        InterlockedMax(gsBMax[b], asuint(depth));
    }
    GroupMemoryBarrierWithGroupSync();

    // Each bucket's box in camera-relative world space from the tile's 4 corners at its nearest and farthest depth.
    float3 bmin[2], bmax[2];
    bool has[2];
    [unroll] for (uint bk = 0; bk < 2; ++bk)
    {
        has[bk] = gsBMax[bk] != 0u;
        bmin[bk] = 1e30f;
        bmax[bk] = -1e30f;
        if (has[bk])
        {
            const float dn = asfloat(gsBMax[bk]), df = asfloat(gsBMin[bk]);
            [unroll] for (uint c = 0; c < 8; ++c)
            {
                const float2 px = float2(gid.xy * kDlTile + uint2((c & 1u) ? kDlTile : 0u, (c & 2u) ? kDlTile : 0u));
                const float2 uv = px / float2(Size);
                const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
                const float dd = (c & 4u) ? df : dn;
                const float4 h = ndc.x * PerSceneRows[13] + ndc.y * PerSceneRows[14] + dd * PerSceneRows[15] + PerSceneRows[16];
                const float3 w = h.xyz / h.w;
                bmin[bk] = min(bmin[bk], w);
                bmax[bk] = max(bmax[bk], w);
            }
        }
    }
    const float3 f = normalize(PerSceneRows[16].xyz);
    const float3 rr = normalize(PerSceneRows[13].xyz - dot(PerSceneRows[13].xyz, f) * f);
    const float3 uu = cross(rr, f);
    if (any && (Flags & 16u))
    {
        [loop] for (uint type = 0; type < 3; ++type)
        {
            const uint b = min(LightCounts.Load(type * 8), 512u), e = min(LightCounts.Load(type * 8 + 4), 512u);
            [loop] for (uint i = b + gi; i < e; i += 256)
            {
                DlLight l;
                if (!DlLoad(i, l))
                    continue;
                const float3 c = rr * l.pos.x + uu * l.pos.y + f * l.pos.z;  // camera-relative world
                const float lum = dot(l.colour, float3(0.3f, 0.59f, 0.11f));
                [unroll] for (uint bk = 0; bk < 2; ++bk)
                {
                    if (!has[bk])
                        continue;
                    bool hit = true;
                    float score = 1e30f;  // box-directional lights: always kept first
                    if (type != 1 && l.k > 0.0f)
                    {
                        const float radius = rsqrt(l.k) * 1.05f;  // falloff reaches 0 at k d^2 = 1
                        const float3 q = clamp(c, bmin[bk], bmax[bk]);
                        const float d2 = dot(q - c, q - c);
                        hit = d2 <= radius * radius;
                        score = lum * saturate(1.0f - l.k * d2) / max(d2, 0.25f);  // brightest at the box's nearest point
                    }
                    if (hit)
                    {
                        uint slot;
                        InterlockedAdd(gsCount[bk], 1u, slot);
                        if (slot < kDlMaxCand)
                        {
                            gsCand[bk][slot] = i;
                            gsScore[bk][slot] = score;
                        }
                    }
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    const uint tile = gid.y * DlTilesX + gid.x;
    const uint base = tile * kDlTileStride;
    if (gi == 0)
        Out.Store(base, asuint(split));
    [unroll] for (uint bk = 0; bk < 2; ++bk)
    {
        const uint nAll = min(gsCount[bk], kDlMaxCand);
        const uint n = min(nAll, kDlMaxPerTile);
        const uint lb = base + 4 + bk * 260;
        if (gi == 0)
            Out.Store(lb, n);
        [loop] for (uint c = gi; c < nAll; c += 256)
        {
            uint rank = c;  // few enough: keep the order
            if (nAll > kDlMaxPerTile)
            {
                rank = 0;  // strongest first; ties by candidate order
                const float sc = gsScore[bk][c];
                [loop] for (uint j = 0; j < nAll; ++j)
                    rank += (gsScore[bk][j] > sc || (gsScore[bk][j] == sc && j < c)) ? 1u : 0u;
            }
            if (rank < kDlMaxPerTile)
                Out.Store(lb + 4 + rank * 4, gsCand[bk][c]);
        }
    }
}

[numthreads(8, 8, 1)]
void DlShade(uint3 tid : SV_DispatchThreadID)
{
    const uint2 p = tid.xy;
    if (p.x >= Size.x || p.y >= Size.y || !(Flags & 16u))
        return;
    uint2 gs;
    GDepth.GetDimensions(gs.x, gs.y);
    const float2 uv = (float2(p) + 0.5f) / float2(Size);
    const uint2 g = min(uint2(uv * float2(gs)), gs - 1);
    const float depth = GDepth[g];
    if (depth <= 0.0f)
        return;
    const float3 n = DecodeNormalWorld(GNormal[g].xy);
    const float4 mat = GMat[g];
    const float rough = clamp(saturate(mat.y), 0.03f, 1.0f), metal = saturate(mat.z);
    const float3 albedo = GAlbedo[g].rgb;
    const float3 diffuseColour = albedo * (1.0f - metal);
    const float3 f0 = lerp(0.04f.xxx, albedo, metal);
    const uint pixelChannels = (GType[g] >> 4) & 15u;  // 0.4.4: which light channels light this pixel (as the game does)
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 h = ndc.x * PerSceneRows[13] + ndc.y * PerSceneRows[14] + depth * PerSceneRows[15] + PerSceneRows[16];
    const float3 posRel = h.xyz / h.w;
    const float viewDist = length(posRel);
    const float3 f = normalize(PerSceneRows[16].xyz);
    const float3 rr = normalize(PerSceneRows[13].xyz - dot(PerSceneRows[13].xyz, f) * f);
    const float3 uu = cross(rr, f);
    const float3 P = float3(dot(posRel, rr), dot(posRel, uu), dot(posRel, f));
    const float3 N = float3(dot(n, rr), dot(n, uu), dot(n, f));
    const float3 V = -normalize(P);
    const float ndv = max(dot(N, V), 1e-4f);
    const float a = rough * rough, a2 = a * a;

    // Strongest few lamps get a shadow ray; the rest are added unshadowed.
    const uint shadowMax = min(LocalShadowMax & 0xFFu, 8u);  // 0.4.8: up to 8 (Ultra)
    float3 topD[8], topS[8], topL[8];
    float topW[8], topRange[8];
    [unroll] for (uint z = 0; z < 8; ++z) { topD[z] = 0; topS[z] = 0; topL[z] = 0; topW[z] = 0; topRange[z] = 0; }
    float3 sumD = 0.0f, sumS = 0.0f;
    const uint tile = (p.y / kDlTile) * DlTilesX + (p.x / kDlTile);
    // 0.5.18: the near or far list of this tile, by this pixel's depth against the tile's split (same test as DlCull)
    const uint tileBase = tile * kDlTileStride;
    const uint lb = tileBase + 4 + (depth >= asfloat(GameSpec.Load(tileBase)) ? 0u : 260u);
    const uint count = min(GameSpec.Load(lb), kDlMaxPerTile);
    [loop] for (uint j = 0; j < count; ++j)
    {
        const uint i = GameSpec.Load(lb + 4 + j * 4);
        DlLight l;
        if (!DlLoad(i, l) || (l.channels & pixelChannels) == 0u)
            continue;  // 0.4.4: e.g. dialogue face lights only light characters
        float3 L;
        float range;
        const float att = DlAtten(DlType(i), l, P, L, range);
        const float ndl = saturate(dot(N, L));
        if (!(att * ndl > 1e-6f))
            continue;
        const float3 E = l.colour * att * ndl;  // irradiance
        const float3 H = normalize(L + V);
        const float ndh = saturate(dot(N, H)), vdh = saturate(dot(V, H));
        const float dd = ndh * ndh * (a2 - 1.0f) + 1.0f;
        const float D = a2 / (kPi * dd * dd);
        const float vis = 0.5f / (ndl * sqrt(ndv * ndv * (1.0f - a2) + a2) + ndv * sqrt(ndl * ndl * (1.0f - a2) + a2) + 1e-5f);
        const float3 F = f0 + (1.0f - f0) * pow(1.0f - vdh, 5.0f);
        const float3 d = E / kPi;            // x diffuse colour later
        const float3 s = E * D * vis * F;
        if (!all(isfinite(d)) || !all(isfinite(s)))
            continue;  // 0.4.4
        const float w = dot(E, float3(0.2126f, 0.7152f, 0.0722f));
        // insert into the top list if stronger than its weakest entry
        uint weakest = 0;
        // 0.4.9: only the first shadowMax slots are used, so the list always holds the strongest shadowMax lamps. 0.4.8 kept
        // 8 and shadow-tested slots 0..3 at High: unsorted, so which lamps got shadows depended on the tile's list order,
        // which differs per 16x16 tile (built in parallel): blocky squares.
        [unroll] for (uint t = 1; t < 8; ++t) if (t < shadowMax && topW[t] < topW[weakest]) weakest = t;
        // 0.5.38: only lamps the game itself shadows get a shadow ray. The others are fill lamps (poster light boxes, room
        // fill) the level designers left unshadowed, often placed inside or behind geometry: shadow-testing them blocked
        // them, and posters and whole rooms went dark with RT lamps on.
        if (shadowMax > 0 && l.gameShadow && w > topW[weakest])
        {
            sumD += topD[weakest];  // the entry it replaces goes in unshadowed
            sumS += topS[weakest];
            topD[weakest] = d; topS[weakest] = s; topW[weakest] = w; topL[weakest] = L; topRange[weakest] = range;
        }
        else
        {
            sumD += d;
            sumS += s;
        }
    }
    const float3 origin = PerSceneRows[0].xyz + posRel + n * (0.03f + viewDist * 0.001f);
    [unroll] for (uint t = 0; t < 8; ++t)
    {
        if (!(topW[t] > 0.0f))
            continue;
        bool lit = true;
        if (t < shadowMax && topRange[t] > 0.1f)
        {
            uint blocker;
            lit = LampShadowHit(origin, rr * topL[t].x + uu * topL[t].y + f * topL[t].z, topRange[t], viewDist, blocker) < 0.0f;
        }
        if (lit)
        {
            sumD += topD[t];
            sumS += topS[t];
        }
    }
    const float e = max(Exposure(), 1e-6f);
    const float3 addD = diffuseColour * sumD * e * DlScale * LocalLightScale;
    const float3 addS = sumS * e * DlScale * LocalLightScale;
    const uint addr = p.y * PitchBytes + p.x * 4;
    if (any(addD > 0.0f))
        Out.Store(addr, PackR11G11B10(UnpackR11G11B10(Out.Load(addr)) + addD));
    if (any(addS > 0.0f))
        Rad.Store(addr, PackR11G11B10(UnpackR11G11B10(Rad.Load(addr)) + addS));
}

// 0.4.3: copies the game's lamp list on the GPU, at the moment its lighting table is bound (the buffers are not
// CPU-reachable): LightRows (b3) and LightCounts (t11 space1) are bound straight at the game's addresses. Out = our slot:
// [0, 65536) lamp records, [65536, 65584) index ranges, [65760, 65808) the same ranges with point/box/spot emptied (the
// game's lighting table is pointed there while RT lamps run; 65760 = 48 x 1370, a whole structure element).
[numthreads(256, 1, 1)]
void LightCopy(uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (i < 4096)
        Out.Store4(i * 16, asuint(LightRows[i]));
    if (i < 12)
    {
        const uint v = LightCounts.Load(i * 4);
        Out.Store(65536 + i * 4, v);
        Out.Store(65760 + i * 4, i < 6 ? 0u : v);
    }
}

// ---------------------------------------------------------------- 0.5.0: guide images for DLSS Ray Reconstruction
// Writes, per pixel at the DLSS input size, into Out as four texture footprints (copied into textures by the host):
//   region 0: diffuse albedo, RGBA8 (pitch = PitchBytes)           = albedo x (1 - metalness), linear
//   region 1: specular albedo, RGBA8 (pitch = PitchBytes)          = F0 x A + B from the game's split-sum LUT
//   region 2: normal + roughness, RGBA16F (pitch = RrPitch8)       = world normal xyz, linear roughness w
//   region 3: specular hit distance, R32F (pitch = PitchBytes)     = world distance of a mirror ray (1000 if it escapes)
// Region offsets: RrOffset1..3 (bytes, 512-aligned), passed in reused constant fields.
#define RrPitch8  GlassDepthIndex
#define RrOffset1 GlassMomentIndex
#define RrOffset2 SmoothTypes
#define RrOffset3 Mode

uint PackUnorm4(float4 v)
{
    const uint4 q = uint4(saturate(v) * 255.0f + 0.5f);
    return q.x | (q.y << 8) | (q.z << 16) | (q.w << 24);
}

[numthreads(8, 8, 1)]
void RrGuides(uint3 tid : SV_DispatchThreadID)
{
    const uint2 p = tid.xy;
    if (p.x >= Size.x || p.y >= Size.y)
        return;
    const uint a4 = p.y * PitchBytes + p.x * 4;
    const uint a8 = RrOffset2 + p.y * RrPitch8 + p.x * 8;
    uint2 gs;
    GDepth.GetDimensions(gs.x, gs.y);
    const float2 uv = (float2(p) + 0.5f) / float2(Size);
    const uint2 g = min(uint2(uv * float2(gs)), gs - 1);
    const float depth = GDepth[g];
    if (depth <= 0.0f)
    {
        Out.Store(a4, 0u);
        Out.Store(RrOffset1 + a4, 0u);
        Out.Store2(a8, uint2(0u, f32tof16(1.0f) << 16));
        Out.Store(RrOffset3 + a4, asuint(1000.0f));
        return;
    }
    const float3 n = DecodeNormalWorld(GNormal[g].xy);
    const float4 mat = GMat[g];
    const float rough = saturate(mat.y), metal = saturate(mat.z);
    const float3 albedo = GAlbedo[g].rgb;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 h = ndc.x * PerSceneRows[13] + ndc.y * PerSceneRows[14] + depth * PerSceneRows[15] + PerSceneRows[16];
    const float3 posRel = h.xyz / h.w;
    const float viewDist = length(posRel);
    const float3 v = -posRel / max(viewDist, 1e-4f);
    const float ndv = saturate(dot(n, v));
    const float3 f0 = lerp(0.04f.xxx, albedo, metal);
    const float2 ab = BrdfLut.SampleLevel(LinearClamp, float2(ndv, clamp(rough, 0.045f, 0.95f)), 0).xy;

    Out.Store(a4, PackUnorm4(float4(albedo * (1.0f - metal), 1.0f)));
    Out.Store(RrOffset1 + a4, PackUnorm4(float4(f0 * ab.x + ab.y, 1.0f)));
    Out.Store2(a8, uint2(f32tof16(n.x) | (f32tof16(n.y) << 16), f32tof16(n.z) | (f32tof16(rough) << 16)));

    // Specular hit distance: one mirror ray (solid geometry, no alpha test: a guide, not an image).
    RayDesc ray;
    ray.Origin = PerSceneRows[0].xyz + posRel + n * (0.05f + viewDist * 0.002f);
    ray.Direction = reflect(-v, n);
    ray.TMin = 0.0f;
    ray.TMax = 1000.0f;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_NON_OPAQUE> q;
    q.TraceRayInline(Scene, RAY_FLAG_NONE, kSolidMask, ray);
    q.Proceed();
    const float hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : 1000.0f;
    Out.Store(RrOffset3 + a4, asuint(hit));
}
