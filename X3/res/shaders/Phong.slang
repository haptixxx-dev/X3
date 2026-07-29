// Blinn-Phong shading.
//
// The cheapest of the three entry points, kept as a fast preview mode. Like
// PBR.slang it is single-bounce direct lighting and shares everything below the
// BRDF -- traversal, intersection, attribute resolution, material sampling --
// with Trace.slang.

import GpuTypes;
import Bindings;
import Trace;

float3 CalculatePhongLighting(float3 hitPoint, float3 normal, float3 viewDir, Material mat) {
    float3 ambient  = 0.1 * mat.color.xyz;
    float3 diffuse  = float3(0.0);
    float3 specular = float3(0.0);

    const float shininess = 32.0;   // specular exponent

    for (uint i = 0; i < u_Settings.lightCount; i++) {
        LightData light = LightBuffer[i];
        // INITIALISED, and unknown types are skipped -- see the note in
        // PBR.slang. The pre-Slang GLSL left this uninitialised on that path.
        float3 lightDir = float3(0.0, 1.0, 0.0);
        float attenuation = 1.0;
        float maxDist = INF_T;

        int lightType = int(light.position.w);

        if (lightType == 0) {          // Directional
            lightDir = normalize(-light.direction.xyz);
            maxDist = INF_T;
        }
        else if (lightType == 1) {     // Point
            float3 lightVec = light.position.xyz - hitPoint;
            float dist = length(lightVec);
            lightDir = lightVec / dist;
            maxDist = dist;
            if (dist > light.color.w) continue;   // .w is range
            attenuation = 1.0 / (1.0 + light.params.x * dist * dist);
        }
        else if (lightType == 2) {     // Spot
            float3 lightVec = light.position.xyz - hitPoint;
            float dist = length(lightVec);
            lightDir = lightVec / dist;
            maxDist = dist;

            float theta = dot(lightDir, normalize(-light.direction.xyz));
            float innerCone = cos(light.params.y);
            float outerCone = cos(light.params.z);
            if (theta < outerCone) continue;

            float epsilon = innerCone - outerCone;
            float intensity = clamp((theta - outerCone) / epsilon, 0.0, 1.0);
            if (dist > light.color.w) continue;
            attenuation = intensity / (1.0 + light.params.x * dist * dist);
        }
        else { continue; }   // unknown light type

        if (IsInShadow(hitPoint + normal * SURFACE_BIAS, lightDir, maxDist)) continue;

        float diff = max(dot(normal, lightDir), 0.0);
        diffuse += diff * light.color.xyz * light.direction.w * attenuation;

        // Blinn-Phong specular
        float3 halfDir = normalize(lightDir + viewDir);
        float spec = pow(max(dot(normal, halfDir), 0.0), shininess);
        specular += spec * light.color.xyz * light.direction.w * attenuation;
    }

    float3 result = (ambient + diffuse) * mat.color.xyz + specular * 0.5;
    result += mat.emission.xyz * mat.emission.w;
    return result;
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
        float3 viewDir = -ray.dir;
        Material mat = SampleMaterial(MaterialBuffer[ray.materialIdx], ray.uv);
        pixelColor = CalculatePhongLighting(hitPoint, ray.normalShade, viewDir, mat);
    }
    else {
        pixelColor = GetSkyboxLight(ray.dir);
    }

    rayTracingTexture[texelCoords] = float4(pixelColor, 1.0f);
}
