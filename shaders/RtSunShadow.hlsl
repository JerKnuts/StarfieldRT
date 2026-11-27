// Ray-traced sun shadow, written into Starfield's contact shadow mask (R8_UNORM, 1 = lit, 0 = shadowed)
// right after TiledDeferredContactShadowsBlurY, before TiledDeferredLight reads it.
// Constants (RT_FINDINGS.md "Camera and sun constants"):
//   PerSceneConstants row 0 = camera world position, rows 13..16 = inverse view-projection
//   (h = ndc.x*r13 + ndc.y*r14 + depth*r15 + r16; posRel = h.xyz / h.w), reversed-Z depth (sky = 0).
//   Contact-shadow Data row 5 = direction toward the light, in camera space (see SunDirectionWorld).

RaytracingAccelerationStructure Scene : register(t0, space1);  // our TLAS (root SRV)
// 0.2.44 tagged glass/effects as mask 0x02; 0.2.50: sun shadows hit everything again (only reflections skip 0x02).
// 0.5.60: except effect volumes (mask 0x04: LOD dword 0 = material 0, haze/smoke boxes and other invisible effect meshes,
// 16 m boxes over the Jemison plaza): they cast big solid sun shadows on open walkways where nothing visible stood.
static const uint kSolidMask = 0x03u;
Texture2D<float>                Depth : register(t0);          // engine depth (blur table +0)
RWTexture2D<unorm float>        Mask  : register(u0);          // engine contact shadow mask (blur table +2)
// 0.1.23: alpha test. Scene records (root SRVs) plus bindless engine streams and textures (SM 6.6 heap).
ByteAddressBuffer               Lods      : register(t1, space1);  // LOD entries, 20 B, 4 per instance
ByteAddressBuffer               Meshes    : register(t2, space1);  // mesh records, 52 B
ByteAddressBuffer               Materials : register(t3, space1);  // CommonMaterialEntry, 288 B (type 0)
ByteAddressBuffer               VegMaterials : register(t4, space1);  // vegetation material entry, 368 B (type 3)
ByteAddressBuffer               AlphaParams : register(t5, space1);   // 0.1.26: alpha params, 32 B (table +28)
ByteAddressBuffer               IndexPool   : register(t6, space1);   // 0.1.29: geometry pool (table +7), uint16 indices
SamplerState                    AlphaSampler : register(s0, space1);
// 0.1.30: soft shadows. Pass 1 (main) writes per-pixel results here; pass 2 (Denoise) filters them into Mask.
// 8 bytes per pixel: +0 half2 (visibility, mean blocker distance; distance -1 = pixel not traced), +4 view distance.
RWByteAddressBuffer             Vis         : register(u1, space1);
// 0.5.134: phantom detector. [0, 64 KB): 1 bit per instance record, "the game doesn't draw it" (read by the TLAS build:
// such objects get mask 0x10, traced by nothing but this detector). [64 KB, +4 B per record): phantom samples in a row.
RWByteAddressBuffer             Phantom     : register(u2, space1);

cbuffer PerScene : register(b0)
{
    float4 PerSceneRows[17];
};

cbuffer ContactData : register(b1)
{
    float4 ContactRows[6];
};

cbuffer Params : register(b2)
{
    uint2  Extent;        // dispatch bound in pixels (BlurY groups x 8); the real size comes from the mask texture
    uint   Mode;          // 1 = min(contact, RT), 2 = RT replaces contact (debug), 3 = left half RT only (A/B),
                          // 4 = hit-distance view (debug), 5 = TLAS/depth alignment test (debug)
    float  MaxDistance;   // ray length, world units
    float  BiasBase;      // ray start offset along the sun direction
    float  BiasPerUnit;   // extra offset per unit of view distance
    uint   AlphaFlags;    // 0.1.23: bit 0 = alpha test non-opaque candidates
    uint   MaterialCount; // 0.1.23: entries readable in Materials
    uint   VegMaterialCount;
    uint   AlphaParamsCount;  // 0.1.26
    float  SunTanRadius;  // 0.1.30: tan of the sun's angular radius (soft shadow cone)
    uint   Rays;          // 0.1.30: shadow rays per pixel when soft shadows are on
    uint   Frame;         // 0.1.30: frame counter (rotates the sampling pattern)
    uint   SoftFlags;     // 0.1.30: bit 0 = soft shadows (pass 1 writes Vis, pass 2 writes Mask); bits 8..15 = max blur radius px;
                          // 0.2.2: bits 16..19 = soft trace scale (2 = one sample per 2x2 pixels)
};

#include "AlphaTest.hlsli"

// 0.5.106: fade-in after a load (SoftFlags bits 20..27, 255 = done). Until it is done the game's sun cascades still draw,
// so RT shadows are added on top of the game's (min with the contact mask, fading in); after it they replace them.
float SunOut(float game, float rt)
{
    const float f = float((SoftFlags >> 20) & 255u) / 255.0f;
    if (Mode == 1 || f < 1.0f)
        return min(game, lerp(1.0f, rt, f));
    return rt;
}

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

// 0.1.22: start shadow rays from the TLAS surface, not the depth buffer. The two disagree by ~1% of the
// view distance (13 cm at 13 m), so a ray started at the depth point can begin just inside the
// surface and hit its own back face. Trace the camera ray first; if it hits near the depth distance,
// start just in front of that hit. Otherwise (surface not in the TLAS) fall back to the depth point.
float3 ShadowRayOrigin(float3 posRel, float viewDist, float3 toSun, out bool fromTlas)
{
    const float3 cam = PerSceneRows[0].xyz;
    const float3 dir = posRel / viewDist;
    RayDesc primary;
    primary.Origin = cam;
    primary.Direction = dir;
    primary.TMin = 0.0f;
    primary.TMax = viewDist * 1.1f + 1.0f;
    // 0.5.56: the camera ray runs the alpha test too. Forced opaque, it stopped on the see-through parts of leaf and
    // vine cards hanging in front of a wall (up to 5% of the view distance in front of it), and the sun ray then
    // started from the card: tree shadows on walls swelled and shrank as the camera moved, since which pixels hit a
    // card changed every frame. Characters' own copies and the first-person body are skipped here as well.
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> pq;
    pq.TraceRayInline(Scene, (AlphaFlags & 1u) ? RAY_FLAG_NONE : RAY_FLAG_FORCE_OPAQUE, kSolidMask, primary);
    uint cands = 0;
    while (pq.Proceed())
    {
        if (pq.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        if (pq.CandidateInstanceContributionToHitGroupIndex() == 1u)
            continue;
        if (++cands > 64)
        {
            pq.Abort();
            break;
        }
        if (AlphaPasses(pq.CandidateInstanceID(), pq.CandidatePrimitiveIndex(), pq.CandidateTriangleBarycentrics()))
            pq.CommitNonOpaqueTriangleHit();
    }
    if (pq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        const float t = pq.CommittedRayT();
        if (abs(t - viewDist) < max(0.15f, viewDist * 0.05f))
        {
            fromTlas = true;
            return cam + dir * (t - 0.01f - t * 0.0005f) + toSun * 0.02f;
        }
    }
    fromTlas = false;
    return cam + posRel + toSun * (BiasBase + viewDist * BiasPerUnit);
}

// ---- 0.1.30 soft shadows ----
float InterleavedGradientNoise(float2 p)
{
    return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

// Traces one shadow ray toward `dir`; returns true if blocked, with the blocker distance in `hitT`.
bool TraceSunOnce(RayDesc ray, out float hitT, out bool frontFace, out bool giant);

// 0.5.55: a back-face hit within selfGap of the start is passed through (up to twice), as reflections (0.5.33), GI
// (0.5.46) and lamp shadows (0.5.45) do. Far buildings are drawn with a simpler level-of-detail mesh than our RT copy
// (metres apart at a few hundred metres), so the ray could start inside our copy and hit its inner face: whole distant
// buildings went fully shadowed, and flipped back as the game switched detail levels while the player moved.
bool TraceSun(float3 origin, float3 dir, float selfGap, out float hitT)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.0f;
    ray.TMax = MaxDistance;
    bool front, giant;
    [loop] for (uint tries = 0; tries < 3; ++tries)
    {
        if (!TraceSunOnce(ray, hitT, front, giant))
            return false;
        // 0.5.132: a back face of a giant object (hit group 3: distant-detail blocks) never blocks, wherever it is.
        if (front || (hitT >= selfGap && !giant) || tries == 2)
            return true;
        ray.TMin = hitT + 0.01f;
    }
    return true;
}

bool TraceSunOnce(RayDesc ray, out float hitT, out bool frontFace, out bool giant)
{
    // 0.1.23: opaque geometry commits automatically; alpha-tested instances (FORCE_NON_OPAQUE in the TLAS)
    // come back as candidates and are committed only where their opacity texture is solid.
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(Scene, (AlphaFlags & 1u) ? RAY_FLAG_NONE : RAY_FLAG_FORCE_OPAQUE, kSolidMask, ray);
    // 0.2.12: at most 64 alpha-tested candidates per ray. Mid-load, the scene can briefly hold huge or misplaced
    // shapes, and one ray crossing thousands of leaf triangles (a texture fetch each) across millions of rays made
    // a frame take seconds: a GPU hang (TDR). Past the cap the ray counts as blocked.
    uint candidates = 0;
    while (q.Proceed())
    {
        if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        // 0.5.49: the first-person player's body (hit group 1) casts no shadow. The game doesn't animate it in first
        // person, so our copy cast a T-pose shadow (on counters with RT lamps). In third person it isn't tagged.
        if (q.CandidateInstanceContributionToHitGroupIndex() == 1u)
            continue;
        // 0.5.12: characters don't shadow themselves through their slightly-off skinned copy (splotchy faces).
        if (q.CandidateInstanceContributionToHitGroupIndex() == 2u && q.CandidateTriangleRayT() <= 0.2f)
            continue;
        if (++candidates > 64 ||
            AlphaPasses(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()))
        {
            q.CommitNonOpaqueTriangleHit();
            q.Abort();
        }
    }
    const bool hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    hitT = hit ? q.CommittedRayT() : 0.0f;
    frontFace = hit && q.CommittedTriangleFrontFace();
    giant = hit && q.CommittedInstanceContributionToHitGroupIndex() == 3u;  // 0.5.132
    return hit;
}

// 0.5.134: on 1 trace pixel in 16 per frame (rotating), a camera ray toward the visible surface (solid + phantom masks,
// alpha tested). A rigid object hit clearly in front of what the game shows there is drawn nowhere at that pixel: a
// phantom sample. One hit where the game shows it is a visible sample, which clears the object's count and bit. 16
// phantom samples in a row set the bit. Phantoms seen in probes: a settling 2-part object beside a Jemison bin (shadowed
// the floor), a glass-flagged 4 m object over the MAST floor.
void PhantomSample(uint2 q, float3 posRel, float viewDist)
{
    if (!(AlphaFlags & 256u) || viewDist > 120.0f || (((q.x & 3u) | ((q.y & 3u) << 2)) != (Frame & 15u)))
        return;
    RayDesc r;
    r.Origin = PerSceneRows[0].xyz;
    r.Direction = posRel / viewDist;
    r.TMin = 0.05f;
    r.TMax = viewDist * 1.05f + 0.5f;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> dq;
    dq.TraceRayInline(Scene, (AlphaFlags & 1u) ? RAY_FLAG_NONE : RAY_FLAG_FORCE_OPAQUE, 0x11u, r);
    uint cands = 0;
    while (dq.Proceed())
    {
        if (dq.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        if (dq.CandidateInstanceContributionToHitGroupIndex() == 1u)
            continue;
        if (++cands > 64)
        {
            dq.Abort();
            break;
        }
        if (AlphaPasses(dq.CandidateInstanceID(), dq.CandidatePrimitiveIndex(), dq.CandidateTriangleBarycentrics()))
            dq.CommitNonOpaqueTriangleHit();
    }
    if (dq.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return;
    const uint hg = dq.CommittedInstanceContributionToHitGroupIndex();
    const uint inst = dq.CommittedInstanceID();
    if (hg == 1u || hg == 2u || inst >= (1u << 19))
        return;  // characters and the player: their copies lag the game's pose
    const float t = dq.CommittedRayT();
    const uint word = (inst >> 5) * 4u, bit = 1u << (inst & 31u), ca = 65536u + inst * 4u;
    if (abs(t - viewDist) < max(0.15f, viewDist * 0.05f))
    {
        Phantom.Store(ca, 0u);
        Phantom.InterlockedAnd(word, ~bit);
    }
    else if (t < viewDist * 0.9f - 0.25f)
    {
        uint prev;
        Phantom.InterlockedAdd(ca, 1u, prev);
        if (prev + 1u >= 16u)
            Phantom.InterlockedOr(word, bit);
    }
}

void StoreVis(uint2 p, uint width, float vis, float blocker, float viewDist)
{
    Vis.Store2((p.y * width + p.x) * 8, uint2(f32tof16(vis) | (f32tof16(blocker) << 16), asuint(viewDist)));
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    // 0.1.18: cover the whole mask texture (it is output resolution, 3440x1440 here), and map to
    // the depth texture by UV in case it is a different size.
    uint2 size, depthSize;
    Mask.GetDimensions(size.x, size.y);
    Depth.GetDimensions(depthSize.x, depthSize.y);
    size = min(size, Extent);
    const bool soft = (SoftFlags & 1u) != 0 && (Mode == 1 || Mode == 2);
    // 0.2.2: soft shadows trace one sample per scale x scale block (pass 2 upsamples with depth weights).
    const uint scale = soft ? clamp((SoftFlags >> 16) & 0xFu, 1u, 4u) : 1u;
    const uint2 lowSize = (size + scale - 1) / scale;
    const uint2 q = tid.xy;
    if (q.x >= lowSize.x || q.y >= lowSize.y)
        return;
    const uint2 p = min(q * scale + scale / 2, size - 1);  // representative full-resolution pixel
    if (Mode == 3 && p.x >= size.x / 2)
        return;  // A/B: leave the right half untouched

    const float2 uv = (float2(p) + 0.5f) / float2(size);
    const float depth = Depth[min(uint2(uv * float2(depthSize)), depthSize - 1)];
    if (depth <= 0.0f)
    {
        if (soft)
            StoreVis(q, lowSize.x, 1.0f, -1.0f, 0.0f);  // sky: no sample here
        return;
    }

    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 h = ndc.x * PerSceneRows[13] + ndc.y * PerSceneRows[14] + depth * PerSceneRows[15] + PerSceneRows[16];
    const float3 posRel = h.xyz / h.w;
    const float  viewDist = length(posRel);
    const float3 world = posRel + PerSceneRows[0].xyz;

    if (Mode == 6)
    {
        // 0.2.4 lit test: RT says "fully lit" everywhere while the game's sun shadow maps stay off, so any
        // shadow still on screen comes from another game system, not from RT.
        Mask[p] = 1.0f;
        return;
    }

    if (Mode == 5)
    {
        // Alignment test: a primary ray from the camera through this pixel should hit our TLAS at the
        // distance the game's depth buffer reports. Match = lit (1), wrong distance = dark (0.15), miss = 0.5.
        RayDesc primary;
        primary.Origin = PerSceneRows[0].xyz;
        primary.Direction = posRel / viewDist;
        primary.TMin = 0.0f;
        primary.TMax = viewDist * 1.5f + 1.0f;
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> pq;  // closest hit
        pq.TraceRayInline(Scene, RAY_FLAG_NONE, kSolidMask, primary);
        pq.Proceed();
        float v = 0.5f;
        if (pq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            v = (abs(pq.CommittedRayT() - viewDist) < max(0.05f, viewDist * 0.02f)) ? 1.0f : 0.15f;
        Mask[p] = v;
        return;
    }

    PhantomSample(q, posRel, viewDist);  // 0.5.134
    const float3 toSun = SunDirectionWorld();
    bool fromTlas;
    const float3 origin = ShadowRayOrigin(posRel, viewDist, toSun, fromTlas);
    // 0.5.55: back-face hits this close to the start are self-hits (we started inside our RT copy). 0.5.57: wider when the
    // start point is the depth-buffer fallback (no RT surface found near it): buildings keep a coarse distance shell (a
    // 476-triangle, 64 m mesh in Jemison) in the RT scene that the game doesn't draw up close, up to ~5% of the view
    // distance in front of the real wall. Starting inside it, the sun ray hit its inner face: square shadows on walls
    // that appeared past ~10 m.
    const float selfGap = 0.1f + viewDist * (fromTlas ? 0.02f : 0.06f);

    if (Mode == 4)
    {
        // Hit distance: black = hit right at the start (self-intersection), grey = hit far away, white = no hit.
        // 0.5.59: the real shadow ray (alpha test, self-hit skips) toward the sun's centre, full resolution, no filter.
        float t;
        Mask[p] = TraceSun(origin, toSun, selfGap, t) ? saturate(t / 50.0f) * 0.8f : 1.0f;
        return;
    }

    if (!soft)
    {
        float t;
        const float visibility = TraceSun(origin, toSun, selfGap, t) ? 0.0f : 1.0f;
        Mask[p] = SunOut(Mask[p], visibility);  // 0.5.106
        return;
    }

    // 0.1.30: N rays toward random points on the sun's disc (cone of half-angle atan(SunTanRadius)),
    // with a per-pixel rotation that changes every frame. Pass 2 turns this into a smooth penumbra.
    float3 tangent = normalize(abs(toSun.z) < 0.99f ? cross(toSun, float3(0, 0, 1)) : cross(toSun, float3(1, 0, 0)));
    const float3 bitangent = cross(toSun, tangent);
    // 0.2.9: a fixed per-pixel pattern (no per-frame rotation), so soft edges do not shimmer.
    const float noise = InterleavedGradientNoise(float2(p));
    const uint rays = clamp(Rays, 1u, 16u);
    float lit = 0.0f, blockerSum = 0.0f, blockers = 0.0f;
    for (uint i = 0; i < rays; ++i)
    {
        // Vogel (golden-angle) disc, rotated by the per-pixel noise.
        const float r = sqrt((i + 0.5f) / rays);
        const float phi = i * 2.39996323f + noise * 6.28318531f;
        const float2 d = float2(cos(phi), sin(phi)) * r * SunTanRadius;
        const float3 dir = normalize(toSun + tangent * d.x + bitangent * d.y);
        float t;
        if (TraceSun(origin, dir, selfGap, t))
        {
            blockerSum += t;
            blockers += 1.0f;
        }
        else
        {
            lit += 1.0f;
        }
    }
    StoreVis(q, lowSize.x, lit / rays, blockers > 0.0f ? blockerSum / blockers : 0.0f, viewDist);
}

// ---- 0.1.30 pass 2: penumbra-aware, depth-aware filter (PCSS-style) from Vis into the engine mask ----
struct VisSample { float vis, blocker, dist; };

float3 ViewDir(float2 ndc)  // unit view ray through ndc (reconstructed at the near plane, depth 1)
{
    const float4 h = ndc.x * PerSceneRows[13] + ndc.y * PerSceneRows[14] + PerSceneRows[15] + PerSceneRows[16];
    return normalize(h.xyz / h.w);
}

VisSample LoadVis(int2 p, uint2 size)
{
    const int2 c = clamp(p, int2(0, 0), int2(size) - 1);
    const uint2 w = Vis.Load2((uint(c.y) * size.x + uint(c.x)) * 8);
    VisSample s;
    s.vis = f16tof32(w.x);
    s.blocker = f16tof32(w.x >> 16);
    s.dist = asfloat(w.y);
    return s;
}

[numthreads(8, 8, 1)]
void Denoise(uint3 tid : SV_DispatchThreadID)
{
    uint2 size, depthSize;
    Mask.GetDimensions(size.x, size.y);
    Depth.GetDimensions(depthSize.x, depthSize.y);
    size = min(size, Extent);
    const int2 p = int2(tid.xy);
    if (p.x >= int(size.x) || p.y >= int(size.y))
        return;
    if (Mode == 3 && p.x >= int(size.x) / 2)
        return;

    // 0.2.2: this pixel's own depth (pass 1 may have traced at lower resolution).
    const float2 uv = (float2(p) + 0.5f) / float2(size);
    const float depth = Depth[min(uint2(uv * float2(depthSize)), depthSize - 1)];
    if (depth <= 0.0f)
        return;  // sky
    const float2 ndc0 = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 h = ndc0.x * PerSceneRows[13] + ndc0.y * PerSceneRows[14] + depth * PerSceneRows[15] + PerSceneRows[16];
    const float dist = length(h.xyz / h.w);

    const uint scale = clamp((SoftFlags >> 16) & 0xFu, 1u, 4u);
    const uint2 lowSize = (size + scale - 1) / scale;
    const float2 lowPos = (float2(p) + 0.5f) / scale - 0.5f;  // this pixel in low-resolution sample space
    const int2 base = int2(floor(lowPos));
    const float2 f = lowPos - float2(base);
    const float depthTol = dist * 0.02f + 0.05f;

    // Bilateral upsample of the nearest 2x2 samples: bilinear weight x depth similarity.
    float vis0 = 0.0f, w0 = 0.0f, bSum = 0.0f, bN = 0.0f;
    float bestDiff = 1e30f, bestVis = 1.0f;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1u, k >> 1);
        const VisSample s = LoadVis(base + o, lowSize);
        if (s.blocker < 0.0f)
            continue;
        const float bw = (o.x ? f.x : 1.0f - f.x) * (o.y ? f.y : 1.0f - f.y) + 1e-3f;
        const float diff = abs(s.dist - dist);
        const float w = bw * exp(-diff / depthTol);
        vis0 += s.vis * w;
        w0 += w;
        if (diff < bestDiff) { bestDiff = diff; bestVis = s.vis; }
        if (s.vis < 1.0f && diff < dist * 0.05f + 0.1f) { bSum += s.blocker; bN += 1.0f; }
    }
    float result = w0 > 1e-4f ? vis0 / w0 : bestVis;

    const float maxRadius = float(max((SoftFlags >> 8) & 0xFFu, 1u));  // full-resolution pixels
    // World size of one full-resolution pixel at this distance: the angle between neighbouring view rays.
    const float pixelWorld = dist * length(ViewDir(ndc0 + float2(2.0f / size.x, 0.0f)) - ViewDir(ndc0));

    // Blocker search on a ring (like PCSS), in low-resolution samples.
    [unroll] for (uint r = 0; r < 8; ++r)
    {
        const float a = r * 0.78539816f;
        const VisSample s = LoadVis(base + int2(round(float2(cos(a), sin(a)) * (maxRadius / scale))), lowSize);
        if (s.blocker > 0.0f && s.vis < 1.0f && abs(s.dist - dist) < dist * 0.05f + 0.1f)
        {
            bSum += s.blocker;
            bN += 1.0f;
        }
    }
    if (bN > 0.0f)
    {
        // Penumbra width on the receiver ~ blocker distance x sun diameter; convert to low-res samples.
        const float penumbra = (bSum / bN) * 2.0f * SunTanRadius;
        const float radius = clamp(0.5f * penumbra / max(pixelWorld, 1e-4f), 1.0f, maxRadius) / scale;
        if (radius > 0.75f)
        {
            float sum = result * 2.0f, wsum = 2.0f;  // the upsampled centre counts double
            [unroll] for (uint j = 0; j < 16; ++j)
            {
                const float rr = sqrt((j + 0.5f) / 16.0f) * radius;
                const float phi = j * 2.39996323f;
                const VisSample s = LoadVis(int2(round(lowPos + float2(cos(phi), sin(phi)) * rr)), lowSize);
                if (s.blocker < 0.0f)
                    continue;
                const float w = exp(-2.0f * (rr * rr) / (radius * radius)) * exp(-abs(s.dist - dist) / depthTol);
                sum += s.vis * w;
                wsum += w;
            }
            result = sum / wsum;
        }
    }
    Mask[p] = SunOut(Mask[p], result);  // 0.5.106
}
