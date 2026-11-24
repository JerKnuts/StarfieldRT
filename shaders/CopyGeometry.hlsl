// 0.1.34: copies one mesh's geometry from the engine's buffers into our staging buffer before its BLAS
// build, clamping every index into [0, VertexCount). Building straight from the engine's buffers hung the
// GPU on save load (DRED: stopped inside BuildRaytracingAccelerationStructure): right after a load a mesh
// record can exist before its index data is uploaded, and the DXR spec leaves out-of-range indices undefined.
// With this copy the acceleration-structure builder only ever reads our own, sanitized data.

ByteAddressBuffer   Vertices : register(t0);  // engine vertex buffer at the mesh's position offset (int16 x3, stride 6)
ByteAddressBuffer   Pool     : register(t1);  // engine geometry pool (uint16 indices)
RWByteAddressBuffer Staging  : register(u0);

cbuffer Params : register(b0)
{
    uint VertexDwords;    // ceil((VertexCount * 6 + 2) / 4): positions plus the 2 bytes R16G16B16A16 reads past the last
    uint IndexCount;
    uint VertexCount;
    uint IndexByteOffset; // first index * 2, from the pool start (may be 2 mod 4)
    uint VertexDst;       // byte offset in Staging (256-aligned)
    uint IndexDst;        // byte offset in Staging (256-aligned)
};

uint LoadIndex(uint i)
{
    const uint byteAddr = IndexByteOffset + i * 2;
    const uint w = Pool.Load(byteAddr & ~3u);
    const uint v = (byteAddr & 2u) ? (w >> 16) : (w & 0xFFFFu);
    return min(v, VertexCount - 1);
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (i < VertexDwords)
        Staging.Store(VertexDst + i * 4, Vertices.Load(i * 4));
    const uint pairs = (IndexCount + 1) / 2;
    if (i < pairs)
    {
        const uint lo = LoadIndex(i * 2);
        const uint hi = (i * 2 + 1 < IndexCount) ? LoadIndex(i * 2 + 1) : 0u;
        Staging.Store(IndexDst + i * 4, lo | (hi << 16));
    }
}
