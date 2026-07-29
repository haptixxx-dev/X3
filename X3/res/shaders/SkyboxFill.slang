// =============================================================================
// SkyboxFill -- the background, for the raster path.
//
// The forward pass produces fragments only where geometry is, so without this
// every pixel the camera misses would keep whatever the clear left. That is not
// merely cosmetic: the gate on Phase 7c is "the forward output agrees with the
// reference", and a frame whose background disagrees can only be compared over
// a mask, which is a weaker test than comparing the whole image.
//
// IT IS PBR.slang'S MISS PATH, VERBATIM -- GetSkyboxLight(ray.dir) with no
// tonemap and no gamma, matching what the compute renderers write. Copying the
// expression rather than sharing it would be the usual mistake; GetSkyboxLight
// and MakeCameraRay both live in Trace.slang and both are called here.
//
// A COMPUTE DISPATCH, not a fullscreen triangle. The target is written as a
// storage image everywhere else in the engine, and adding a second raster
// pipeline whose only job is to write every pixel once would need the colour
// attachment set up one pass earlier for no gain.
// =============================================================================

import GpuTypes;
import Bindings;
import Trace;

[shader("compute")]
[numthreads(8, 4, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
    const int2 texelCoords = int2(threadId.xy);
    uint dimX, dimY;
    rayTracingTexture.GetDimensions(dimX, dimY);
    const int2 dims = int2(dimX, dimY);
    if (texelCoords.x >= dims.x || texelCoords.y >= dims.y) return;

    const Ray ray = MakeCameraRay(texelCoords, dims);
    rayTracingTexture[texelCoords] = float4(GetSkyboxLight(ray.dir), 1.0);
}
