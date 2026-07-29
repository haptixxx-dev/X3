// Physically Based Rendering (PBR)
// Metallic-roughness workflow with Cook-Torrance BRDF.
//
// Single-bounce direct lighting plus a crude skybox IBL -- not a path tracer.
// Traversal, intersection, attribute resolution and material sampling all come
// from Trace.slang, which is what stops this file and PathTracing.slang from
// drifting apart. Phase 6 replaces the BRDF below with the shared BSDF library.

import GpuTypes;
import Bindings;
import Trace;

float3 FresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

float DistributionGGX(float3 N, float3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return a2 / max(denom, 0.0001);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness) {
    float ggx2 = GeometrySchlickGGX(max(dot(N, V), 0.0), roughness);
    float ggx1 = GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
    return ggx1 * ggx2;
}

float3 CalculatePBRLighting(float3 hitPoint, float3 N, float3 V, Material mat) {
    float3 albedo   = mat.color.xyz;
    float metallic  = mat.pbrParams.x;
    float roughness = mat.pbrParams.y;
    float ao        = mat.pbrParams.z;

    // Reflectance at normal incidence.
    float3 F0 = lerp(float3(0.04), albedo, metallic);

    float3 Lo = float3(0.0);

    for (uint i = 0; i < u_Settings.lightCount; i++) {
        LightData light = LightBuffer[i];
        // INITIALISED, and unknown types are skipped. The pre-Slang GLSL
        // declared this bare and assigned it in an if/else-if chain with no
        // final else, so a light whose type was not 0/1/2 shaded with an
        // uninitialised direction. Slang's uninitialised-variable analysis is
        // what surfaced it.
        float3 L = float3(0.0, 1.0, 0.0);
        float attenuation = 1.0;
        float maxDist = INF_T;
        float3 radiance = light.color.xyz * light.direction.w;

        int lightType = int(light.position.w);

        if (lightType == 0) {          // Directional
            L = normalize(-light.direction.xyz);
            maxDist = INF_T;
        }
        else if (lightType == 1) {     // Point
            float3 lightVec = light.position.xyz - hitPoint;
            float dist = length(lightVec);
            L = lightVec / dist;
            maxDist = dist;
            if (dist > light.color.w) continue;   // .w is range
            attenuation = 1.0 / (dist * dist);
        }
        else if (lightType == 2) {     // Spot
            float3 lightVec = light.position.xyz - hitPoint;
            float dist = length(lightVec);
            L = lightVec / dist;
            maxDist = dist;

            float theta = dot(L, normalize(-light.direction.xyz));
            float innerCone = cos(light.params.y);
            float outerCone = cos(light.params.z);
            if (theta < outerCone) continue;

            float epsilon = innerCone - outerCone;
            float intensity = clamp((theta - outerCone) / epsilon, 0.0, 1.0);
            if (dist > light.color.w) continue;
            attenuation = intensity / (dist * dist);
        }
        else { continue; }   // unknown light type

        radiance *= attenuation;

        // The caller-side bias moved here from inside IsInShadow, which used to
        // offset by an UNINITIALISED member of the ray it was building.
        if (IsInShadow(hitPoint + N * SURFACE_BIAS, L, maxDist)) continue;

        // Cook-Torrance
        float3 H = normalize(V + L);
        float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
        float NDF = DistributionGGX(N, H, roughness);
        float G   = GeometrySmith(N, V, L, roughness);

        float3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0);
        float3 specular = numerator / max(denominator, 0.001);

        float3 kS = F;
        float3 kD = (float3(1.0) - kS) * (1.0 - metallic);

        float NdotL = max(dot(N, L), 0.0);
        Lo += (kD * albedo / PI + specular) * radiance * NdotL;
    }

    // Crude skybox IBL. Proper prefiltered environment maps are Phase 11.
    float3 ambient;
    {
        float3 diffuseIBL = GetSkyboxLight(N) * albedo;

        float3 R = reflect(-V, N);
        float3 specularIBL = GetSkyboxLight(R);
        // Rough surfaces get less sharp reflections. A stand-in for sampling a
        // prefiltered mip chain.
        specularIBL *= (1.0 - roughness * 0.5);

        float3 F = FresnelSchlick(max(dot(N, V), 0.0), F0);
        float3 kS = F;
        float3 kD = (float3(1.0) - kS) * (1.0 - metallic);   // pure metals have no diffuse

        ambient = (kD * diffuseIBL + kS * specularIBL) * ao;
    }

    float3 color = ambient + Lo;
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
        float3 hitPoint = ray.origin + ray.dir * ray.t;
        float3 V = -ray.dir;
        Material mat = SampleMaterial(MaterialBuffer[ray.materialIdx], ray.uv);
        pixelColor = CalculatePBRLighting(hitPoint, ray.normalShade, V, mat);
    }
    else {
        pixelColor = GetSkyboxLight(ray.dir);
    }

    // No accumulation: this is a single-sample raster-alike, so the image is
    // replaced rather than blended. Matches the pre-Slang behaviour.
    rayTracingTexture[texelCoords] = float4(pixelColor, 1.0f);
}
