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
// APPLIED TO EVERY RENDERER, and that unification is the point.
//
// PBR and Phong used to tonemap internally and the path tracer did not tonemap
// at all, so switching Shader Type changed the EXPOSURE CONVENTION as well as
// the renderer. The path-traced view came out roughly half as bright as the
// other two on the same scene -- measured on a one-light fixture, a pool centre
// of 65 against 136 -- and read as the raster path being blown out when in fact
// it was the only one being displayed correctly. Forward+ and PBR agreed to 0.1
// levels the whole time.
//
// It runs only when debugMode is 0. The heatmaps, depth, cluster and velocity
// views already write display-ready values; pushing those through a tonemap
// would change what they are showing.
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
