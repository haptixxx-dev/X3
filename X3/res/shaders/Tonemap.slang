// =============================================================================
// Tonemap -- AgX, once, over the finished frame.
//
// THE RASTER PATH WRITES LINEAR RADIANCE and this converts it for display. That
// split is not cosmetic: alpha compositing is only correct in linear light, so
// the transparent pass has to blend before any tonemap runs. A forward pass that
// tonemapped per fragment produced visibly washed-out transparency, because
// Reinhard had already compressed each layer before they were mixed.
//
// It calls the SAME function PBR.slang and Phong.slang call, from
// ToneMapping.slang -- the raster output has to land in the same space as the
// compute renderers or none of the cross-checks between them mean anything.
// Three copies of one curve is how they start to drift.
//
// Applied only in Forward+ mode. The compute renderers tonemap themselves and
// the path tracer deliberately does not, so running this over their output would
// double-apply it.
// =============================================================================

import GpuTypes;
import Bindings;
import ToneMapping;

[shader("compute")]
[numthreads(8, 4, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
    const int2 texelCoords = int2(threadId.xy);
    uint dimX, dimY;
    rayTracingTexture.GetDimensions(dimX, dimY);
    if (texelCoords.x >= int(dimX) || texelCoords.y >= int(dimY)) return;

    const float3 color = rayTracingTexture[texelCoords].rgb;
    rayTracingTexture[texelCoords] = float4(AgXTonemap(color), 1.0);
}
