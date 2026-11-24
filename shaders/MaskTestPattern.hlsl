// Debug pattern written into Starfield's contact shadow mask (R8_UNORM, 1 = lit, 0 = shadowed),
// right after TiledDeferredContactShadowsBlurY and before TiledDeferredLight reads it.
// Proves the plugin can feed its own shadow term into the game's lighting.

RWTexture2D<unorm float> Mask : register(u0);

cbuffer Params : register(b0)
{
    uint Mode;      // 1 = force shadow in checker cells, 2 = force lit in checker cells
    uint CellSize;  // checker cell size in pixels
    uint2 Extent;   // pixels to cover (width, height)
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Extent.x || id.y >= Extent.y)
        return;
    const bool cell = (((id.x / CellSize) + (id.y / CellSize)) & 1u) != 0u;
    if (!cell)
        return;
    Mask[id.xy] = (Mode == 2u) ? 1.0f : 0.0f;
}
