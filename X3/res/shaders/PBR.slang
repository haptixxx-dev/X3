// Physically Based Rendering (PBR)
//
// Single-bounce direct lighting plus a crude skybox IBL -- not a path tracer.
//
// SINCE PHASE 6 THIS FILE OWNS NO BRDF OF ITS OWN. It evaluates Bsdf.slang,
// exactly as the path tracer's hit shader does, so the two cannot disagree about
// what a material looks like. Previously each carried its own Cook-Torrance
// implementation and the path tracer's was purely diffuse -- a metal rendered
// two completely different ways depending on which mode you were in, which makes
// "validate the raster path against the reference" meaningless.
//
// What remains here is the LIGHTING INTEGRATION: which lights, how attenuated,
// what ambient. Phase 7's forward pass replaces this half and keeps the BSDF.

import GpuTypes;
import Bindings;
import Trace;
import ToneMapping;


float3 Shade<T : IBsdf>(T bsdf, Material mat, MaterialExt ext, Ray ray) {
    const float3 hitPoint = ray.origin + ray.dir * ray.t;
    const float3 V = -ray.dir;
    const ShadingFrame frame = ShadingFrame.make(ray.normalShade, ray.tangent);

    float3 color = DirectLighting(bsdf, frame, V, hitPoint, ray.normalGeom);
    color += AmbientIBL(mat, ext, ray.normalShade, V);
    color += mat.emission.xyz * mat.emission.w;

    // LINEAR OUT. The tonemap moved to the end of main(), because alpha
    // compositing is only correct in linear light and this function is now one
    // layer of a composite rather than the whole pixel.
    //
    // For an opaque pixel the two are identical -- one layer, weight 1 -- which
    // is what let the opaque goldens survive this change untouched.
    return color;
}

[shader("compute")]
[numthreads(8, 4, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
    int2 texelCoords = int2(threadId.xy);
    uint dimX, dimY;
    rayTracingTexture.GetDimensions(dimX, dimY);
    int2 dims = int2(dimX, dimY);
    if (texelCoords.x >= dims.x || texelCoords.y >= dims.y) { return; }

    // FRONT-TO-BACK ALPHA COMPOSITING, and it is what makes this the reference
    // for the transparent raster pass rather than the path tracer.
    //
    // The path tracer models alpha as stochastic COVERAGE, which is correct but
    // also carries full multi-bounce GI -- so comparing the single-bounce
    // rasterizer against it measures the GI difference, not the transparency.
    // This shader is the single-bounce raster-alike, so it is the like-for-like
    // reference, and it composites ANALYTICALLY: no noise, no sample count.
    //
    // Front-to-back `C += T*a*c ; T *= 1-a` is algebraically identical to
    // back-to-front `dst = a*src + (1-a)*dst`, which is exactly the blend the
    // transparent pipeline is configured with. That equivalence is the reason a
    // ray marcher and a sorted rasterizer can be expected to agree at all.
    Ray ray = MakeCameraRay(texelCoords, dims);

    float3 pixelColor = float3(0.0);
    float  T = 1.0;                       // remaining transmittance

    // Bounded, like the tracer's coverage loop: a stack of transparent surfaces
    // would otherwise run until the shader was killed. Hitting the cap leaves
    // the pixel slightly too transparent rather than wrong.
    for (uint layer = 0; layer < 8; layer++) {
        CheckRayCollision(ray);

        if (ray.t >= INF_T) {
            pixelColor += T * GetSkyboxLight(ray.dir);
            T = 0.0;
            break;
        }

        const Material mat = SampleMaterial(MaterialBuffer[ray.materialIdx], ray.uv);
        const MaterialExt ext = GetMaterialExt(mat);

        float3 layerColor;
        // The same tier split as the path tracer, for the same reason: Slang
        // specializes each instantiation at link time, so a plain
        // metallic-roughness material carries no clearcoat or sheen code.
        if (MaterialHasExtendedLobes(mat))
            layerColor = Shade(MakeCoatedBsdf(mat, ext), mat, ext, ray);
        else
            layerColor = Shade(MakeBaseBsdf(mat, ext), mat, ext, ray);

        const float a = saturate(mat.color.a);
        pixelColor += T * a * layerColor;
        T *= (1.0 - a);
        if (T <= 0.002) break;            // nothing behind this can matter

        // Continue from just past the surface, same direction. The bias is along
        // the RAY rather than the normal: the ray is passing through rather than
        // reflecting off.
        ray.origin = ray.origin + ray.dir * (ray.t + SURFACE_BIAS);
        ray.anyHit = false;
    }

    // ONE TONEMAP, over the composited result. Tonemapping each layer before
    // mixing compresses each one separately and the result is a washed-out
    // picture of neither -- the same mistake the forward pass made first.
    //
    // The shared curve, not a local copy: the raster path calls the identical
    // function and every forward-vs-pbr comparison in the test suite depends on
    // the two landing in the same colour space.
    pixelColor = AgXTonemap(pixelColor);

    // No accumulation: this is a single-sample raster-alike, so the image is
    // replaced rather than blended. Matches the pre-Slang behaviour.
    rayTracingTexture[texelCoords] = float4(pixelColor, 1.0f);
}
