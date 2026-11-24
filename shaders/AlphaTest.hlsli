// Shared alpha test for RT shadow rays (0.1.25), included by RtSunShadow.hlsl and RtProbe.hlsl.
// The including shader declares: ByteAddressBuffer Lods, Meshes, Materials, VegMaterials; SamplerState
// AlphaSampler; and uint MaterialCount, VegMaterialCount, AlphaParamsCount in a cbuffer, plus
// ByteAddressBuffer AlphaParams (table +28, 32 B entries) and ByteAddressBuffer IndexPool (the 4 GB geometry
// pool, table +7, as a root SRV). Needs SM 6.6 (ResourceDescriptorHeap).
// ---- 0.1.23 alpha test (RT_FINDINGS.md "Alpha-tested materials") ----
// Alpha = opacity texture (red channel) at the material-transformed UV, times the vertex colour channel
// when the material asks for it; the surface is solid where alpha >= the material threshold (the same test
// as the game's StandardMaterial and Vegetation shadow pixel shaders, PSO 971 and 980).
static const uint kPoolFirstSlot = 14;
#ifndef RT_ALPHA_SHARED_DEFS  // 0.2.49: RtReflectDebug.hlsl defines these itself
static const uint kBindlessBase = 32768;

uint LoadU16(ByteAddressBuffer b, uint byteAddr)
{
    const uint w = b.Load(byteAddr & ~3u);
    return (byteAddr & 2u) ? (w >> 16) : (w & 0xFFFFu);
}
#endif

struct AlphaMaterial
{
    uint  tex;            // opacity texture heap index
    float threshold;
    uint  uvScale, uvOffset, uvSet, vertexAlpha, channel;
    uint  params;         // 0.1.26: alpha-params entry index (standard +280, vegetation +276)
    uint  mode;           // 0.1.26: alpha-params +4 (3 = remapped alpha)
};

// Material index = LOD entry 0 dword 0 bits 13..27, family = bits 0..4 (0 standard, 3 vegetation).
bool LoadAlphaMaterial(uint instance, out AlphaMaterial m)
{
    m = (AlphaMaterial)0;
    const uint bucket = Lods.Load(instance * 80);
    const uint material = (bucket >> 13) & 0x7FFFu, type = bucket & 0x1Fu;
    if (type == 0 && material < MaterialCount)
    {
        const uint b = material * 288;
        m.tex = Materials.Load(b + 276);
        m.threshold = asfloat(Materials.Load(b + 104));
        m.uvScale = Materials.Load(b + 56);
        m.uvOffset = Materials.Load(b + 60);
        m.uvSet = Materials.Load(b + 64);
        m.vertexAlpha = Materials.Load(b + 68);
        m.channel = Materials.Load(b + 72);
        m.params = Materials.Load(b + 280);
    }
    else if (type == 3 && material < VegMaterialCount)
    {
        const uint b = material * 368;
        m.tex = VegMaterials.Load(b + 272);
        m.threshold = asfloat(VegMaterials.Load(b + 72));
        m.uvScale = VegMaterials.Load(b + 24);
        m.uvOffset = VegMaterials.Load(b + 28);
        m.uvSet = VegMaterials.Load(b + 32);
        m.vertexAlpha = VegMaterials.Load(b + 36);
        m.channel = VegMaterials.Load(b + 40);
        m.params = VegMaterials.Load(b + 276);
    }
    return m.tex != 0;
}

// Returns the alpha at this hit, or -1 if the instance is not alpha tested (solid). `m` and `uvOut` report
// what was used (for the F8 probe).
float AlphaValue(uint instance, uint prim, float2 bary, out AlphaMaterial m, out float2 uvOut)
{
    uvOut = 0;
    if (!LoadAlphaMaterial(instance, m))
        return -1.0f;
    const uint meshB = Lods.Load(instance * 80 + 4) * 52;
    const uint uvHandle = Meshes.Load(meshB + (m.uvSet == 0 ? 4 : 8));
    if ((uvHandle & 0x3FFFu) < 8)
        return -1.0f;  // no UV stream

    // 0.1.29: indices come straight from the pool (root SRV), like the BLAS builds. Bindless slots past 14
    // are NOT consecutive 16 MB pool windows (slot 39 read zeros for an index 425 MB into the pool).
    const uint firstIndex = Meshes.Load(meshB + 36);
    uint idx[3];
    [unroll] for (uint k = 0; k < 3; ++k)
        idx[k] = LoadU16(IndexPool, (firstIndex + prim * 3 + k) * 2);
    const float w[3] = { 1.0f - bary.x - bary.y, bary.x, bary.y };

    ByteAddressBuffer uvBuf = ResourceDescriptorHeap[kBindlessBase + (uvHandle & 0x3FFFu)];
    const uint uvBase = (uvHandle >> 6) & 0xFFFF00u;
    float2 uv = 0;
    [unroll] for (uint j = 0; j < 3; ++j)
    {
        const uint h = uvBuf.Load(uvBase + idx[j] * 4);
        uv += w[j] * float2(f16tof32(h), f16tof32(h >> 16));
    }
    uv = uv * float2(f16tof32(m.uvScale >> 16), f16tof32(m.uvScale)) + float2(f16tof32(m.uvOffset >> 16), f16tof32(m.uvOffset));

    float vertexAlpha = 1.0f;
    if (m.vertexAlpha != 0)
    {
        const uint cHandle = Meshes.Load(meshB + 12);
        if ((cHandle & 0x3FFFu) >= 8)
        {
            ByteAddressBuffer cBuf = ResourceDescriptorHeap[kBindlessBase + (cHandle & 0x3FFFu)];
            const uint cBase = (cHandle >> 6) & 0xFFFF00u;
            const uint shift[4] = { 16, 8, 0, 24 };  // channel order used by the game's vertex shader
            const uint sh = shift[min(m.channel, 3u)];
            vertexAlpha = 0;
            [unroll] for (uint c = 0; c < 3; ++c)
                vertexAlpha += w[c] * (((cBuf.Load(cBase + idx[c] * 4) >> sh) & 0xFFu) / 255.0f);
        }
    }

    uvOut = uv;
    // 0.3.7: an index past the descriptor heap (98304 entries) is a half-written record (right after a load): solid.
    float texAlpha = 1.0f;
    if (m.tex < 98304u)
    {
        Texture2D<float4> opacity = ResourceDescriptorHeap[m.tex];
        texAlpha = opacity.SampleLevel(AlphaSampler, uv, 0).r;
    }

    // 0.1.26: alpha-params mode 3 remaps the texture alpha with the vertex alpha as a fade control
    // (decoded from the game's StandardMaterial/Vegetation shadow pixel shaders, PSO 971/980).
    if (m.params < AlphaParamsCount)
    {
        const uint pb = m.params * 32;
        m.mode = AlphaParams.Load(pb + 4);
        if (m.mode == 3)
        {
            const float p = asfloat(AlphaParams.Load(pb + 24)), q = asfloat(AlphaParams.Load(pb + 28));
            const float lo = min(max((p - 0.5f) * 2.0f, 0.0f), 0.99902f);
            const float w = max(min(p * 2.0f, 1.0f) - lo, 0.001f);
            const float fade = min(max(vertexAlpha - lo, 0.0f), w) / w;
            const float t2 = saturate(texAlpha - 1.0f + fade * 2.0f);
            const float c = w * 0.5f + lo, hw = q * 0.5f * w;
            const float hi2 = clamp(c + hw, 0.001f, 1.0f);
            const float lo2 = clamp(c - hw, 0.0f, 0.99902f);
            const float w2 = max(hi2 - lo2, 0.001f);
            return saturate(min(max(t2 - lo2, 0.0f), w2) / w2);
        }
    }
    return saturate(texAlpha * vertexAlpha);
}

bool AlphaPasses(uint instance, uint prim, float2 bary)
{
    AlphaMaterial m;
    float2 uv;
    const float a = AlphaValue(instance, prim, bary, m, uv);
    return a < 0.0f || a >= m.threshold;
}
