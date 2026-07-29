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


// -----------------------------------------------------------------------------
// Direct lighting, generic over the BSDF. Structurally identical to the path
// tracer's loop -- deliberately, since Phase 7 will hoist one of them into a
// shared module once the forward pass exists and can say what shape it needs.
// -----------------------------------------------------------------------------
float3 DirectLighting<T : IBsdf>(T bsdf, ShadingFrame frame, float3 woWorld,
                                 float3 hitPoint, float3 normalGeom) {
    float3 total = float3(0.0);
    const float3 wo = frame.toLocal(woWorld);
    if (wo.z <= 0.0) return total;

    for (uint i = 0; i < u_Settings.lightCount; i++) {
        const LightData light = LightBuffer[i];
        const uint type = uint(light.position.w);

        float3 toLight;
        float dist = 1e20;
        float attenuation = 1.0;

        if (type == 0) {
            toLight = -normalize(light.direction.xyz);
        } else {
            const float3 v = light.position.xyz - hitPoint;
            dist = length(v);
            if (dist > light.color.w) continue;
            toLight = v / dist;
            attenuation = 1.0 / max(dist * dist, 1e-4);

            if (type == 2) {
                const float cosAngle = dot(toLight, normalize(-light.direction.xyz));
                const float inner = cos(light.params.y);
                const float outer = cos(light.params.z);
                if (cosAngle < outer) continue;
                attenuation *= clamp((cosAngle - outer) / max(inner - outer, 1e-4), 0.0, 1.0);
            } else if (type != 1) {
                continue;   // unknown light type
            }
        }

        if (dot(normalGeom, toLight) <= 0.0) continue;
        if (IsInShadow(hitPoint + normalGeom * SURFACE_BIAS, toLight, dist)) continue;

        const float3 wi = frame.toLocal(toLight);
        if (wi.z <= 0.0) continue;

        const float3 radiance = light.color.xyz * light.direction.w * attenuation;
        total += bsdf.eval(wo, wi) * wi.z * radiance;
    }
    return total;
}

/// Crude skybox IBL. Split-sum without the prefiltered mip chain: the specular
/// term samples the mirror direction and is attenuated by roughness rather than
/// integrated. Proper prefiltered environment maps are Phase 11; this exists so
/// unlit sides of objects are not black.
float3 AmbientIBL(Material mat, MaterialExt ext, float3 N, float3 V) {
    const float NdotV = saturate(dot(N, V));
    const float metallic  = saturate(mat.pbrParams.x);
    const float roughness = clamp(mat.pbrParams.y, 0.0, 1.0);
    const float ao        = mat.pbrParams.z;
    const float3 f0 = lerp(float3(0.08 * ext.specular.x), mat.color.rgb, metallic);

    const float3 diffuseIBL  = GetSkyboxLight(N) * mat.color.rgb * (1.0 - metallic);
    const float3 specularIBL = GetSkyboxLight(reflect(-V, N)) * (1.0 - roughness * 0.5);

    // The same split-sum approximation the BSDF's energy compensation uses, so
    // the ambient term and the direct term agree about how reflective the
    // surface is instead of each guessing.
    const float2 dfg = EnvBRDFApprox(NdotV, roughness);
    const float3 kS = f0 * dfg.x + dfg.y;
    const float3 kD = (1.0 - kS) * (1.0 - metallic);

    return (kD * diffuseIBL + kS * specularIBL) * ao;
}

float3 Shade<T : IBsdf>(T bsdf, Material mat, MaterialExt ext, Ray ray) {
    const float3 hitPoint = ray.origin + ray.dir * ray.t;
    const float3 V = -ray.dir;
    const ShadingFrame frame = ShadingFrame.make(ray.normalShade, ray.tangent);

    float3 color = DirectLighting(bsdf, frame, V, hitPoint, ray.normalGeom);
    color += AmbientIBL(mat, ext, ray.normalShade, V);
    color += mat.emission.xyz * mat.emission.w;

    color = color / (color + float3(1.0));      // Reinhard
    color = pow(color, float3(1.0 / 2.2));      // gamma
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

    Ray ray = MakeCameraRay(texelCoords, dims);
    CheckRayCollision(ray);

    float3 pixelColor;
    if (ray.t < INF_T) {
        const Material mat = SampleMaterial(MaterialBuffer[ray.materialIdx], ray.uv);
        const MaterialExt ext = GetMaterialExt(mat);

        // The same tier split as the path tracer, for the same reason: Slang
        // specializes each instantiation at link time, so a plain
        // metallic-roughness material carries no clearcoat or sheen code.
        if (MaterialHasExtendedLobes(mat))
            pixelColor = Shade(MakeCoatedBsdf(mat, ext), mat, ext, ray);
        else
            pixelColor = Shade(MakeBaseBsdf(mat, ext), mat, ext, ray);
    }
    else {
        pixelColor = GetSkyboxLight(ray.dir);
    }

    // No accumulation: this is a single-sample raster-alike, so the image is
    // replaced rather than blended. Matches the pre-Slang behaviour.
    rayTracingTexture[texelCoords] = float4(pixelColor, 1.0f);
}
