// =============================================================================
// Tonemap -- Reinhard and gamma, once, over the finished frame.
//
// THE RASTER PATH WRITES LINEAR RADIANCE and this converts it for display. That
// split is not cosmetic: alpha compositing is only correct in linear light, so
// the transparent pass has to blend before any tonemap runs. A forward pass that
// tonemapped per fragment produced visibly washed-out transparency, because
// Reinhard had already compressed each layer before they were mixed.
//
// It is the SAME curve PBR.slang and Phong.slang apply internally, deliberately
// -- the raster output has to land in the same space as the compute renderers or
// none of the cross-checks between them mean anything. Phase 11 replaces this
// with AgX through OpenColorIO and that is a change to one file.
//
// Applied only in Forward+ mode. The compute renderers tonemap themselves and
// the path tracer deliberately does not, so running this over their output would
// double-apply it.
// =============================================================================

import GpuTypes;
import Bindings;

[shader("compute")]
[numthreads(8, 4, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
    const int2 texelCoords = int2(threadId.xy);
    uint dimX, dimY;
    rayTracingTexture.GetDimensions(dimX, dimY);
    if (texelCoords.x >= int(dimX) || texelCoords.y >= int(dimY)) return;

    float3 color = rayTracingTexture[texelCoords].rgb;
    color = color / (color + float3(1.0));      // Reinhard
    color = pow(color, float3(1.0 / 2.2));      // gamma
    rayTracingTexture[texelCoords] = float4(color, 1.0);
}
