// Thanks:
// jakubg05 on github (This is from the Laura project)
// Sebastian Lague: https://www.youtube.com/watch?v=Qz0KTGYJtUk
// https://jacco.ompf2.com/2022/04/13/how-to-build-a-bvh-part-1-basics/
//
// The reference renderer. Everything below the entry point that is not
// path-tracing-specific -- traversal, intersection, attribute resolution,
// material sampling -- lives in Trace.slang and is shared with PBR and Phong.

import GpuTypes;
import Bindings;
import Trace;

static const float GAMMA = 0.8;

// Wavelength-ish ramp for the BVH heatmap debug modes.
float3 IntersectionsToRgb(uint intersections, uint cutoff) {
    float t = clamp(float(intersections) / max(1.0, float(cutoff)), 0.0, 1.0);
    float w = 380.0 + 370.0 * t;
    float3 color;
    if (w <= 380.0) { color = float3(0.0); }
    else if (w > 380.0 && w <= 440.0) { color = float3((440.0 - w) / (440.0 - 380.0) / 3.0, 0.0, 0.8); }
    else if (w <= 490.0) { color = float3(0.0, (w - 440.0) / (490.0 - 440.0), 1.0); }
    else if (w <= 510.0) { color = float3(0.0, 1.0, (510.0 - w) / (510.0 - 490.0)); }
    else if (w <= 580.0) { color = float3((w - 510.0) / (580.0 - 510.0), 1.0, 0.0); }
    else if (w <= 645.0) { color = float3(1.0, (645.0 - w) / (645.0 - 580.0), 0.0); }
    else if (w <= 780.0) { color = float3(1.0, 0.0, 0.0); }
    else { color = float3(1.0); }

    float factor;
    if (w >= 380.0 && w < 420.0) { factor = 0.3 + 0.7 * (w - 380.0) / (420.0 - 380.0); }
    else if (w >= 420.0 && w < 701.0) { factor = 1.0; }
    else if (w >= 701.0 && w < 781.0) {
        factor = 0.3 + 0.7 * (780.0 - w) / (780.0 - 700.0);
        return pow(color + factor * float3(1.0), float3(GAMMA));
    }
    else { factor = 1.0; }
    return pow(factor * color, float3(GAMMA));
}

uint pcg_hash(uint v) {
    v = v * 747796405u + 2891336453u;
    v ^= v >> ((v >> 28u) + 4u);
    v *= 277803737u;
    v ^= v >> 22u;
    return v;
}

uint InitRngState(int2 pixel, uint frame, uint raySampleIdx) {
    uint s = uint(pixel.x) * 1973u
           ^ uint(pixel.y) * 9277u
           ^ frame          * 26699u
           ^ raySampleIdx   * 374761393u;
    s ^= 0x9E3779B9u;   // golden ratio const
    return pcg_hash(s);
}

/// PCG-based uniform value in [0,1).
/// https://www.pcg-random.org, https://www.shadertoy.com/view/XlGcRh
float RandomValue(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint result = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    result = (result >> 22u) ^ result;
    return float(result) / 4294967295.0;
}

/// Box-Muller transform. Thanks to https://stackoverflow.com/a/6178290
float RandomValueNormalDistribution(inout uint state) {
    float theta = 2 * PI * RandomValue(state);
    float rho = sqrt(-2 * log(RandomValue(state)));
    return rho * cos(theta);
}

/// https://math.stackexchange.com/questions/1585975
float3 RandomDirection(inout uint state) {
    float x = RandomValueNormalDistribution(state);
    float y = RandomValueNormalDistribution(state);
    float z = RandomValueNormalDistribution(state);
    return normalize(float3(x, y, z));
}

// -----------------------------------------------------------------------------
// Direct lighting. Shared shape with PBR/Phong but a different BRDF (pure
// diffuse here), so it stays local rather than being hoisted into Trace.
// -----------------------------------------------------------------------------
float3 SampleDirectionalLight(LightData light, float3 hitPoint, float3 normal) {
    float3 lightDir = -normalize(light.direction.xyz);
    float ndotl = max(dot(normal, lightDir), 0.0);
    if (ndotl <= 0.0) return float3(0.0);

    // Directional lights are infinitely far away, so trace far.
    if (IsInShadow(hitPoint, lightDir, 1e20)) return float3(0.0);

    return light.color.xyz * light.direction.w * ndotl;   // colour * intensity * ndotl
}

float3 SamplePointLight(LightData light, float3 hitPoint, float3 normal) {
    float3 toLight = light.position.xyz - hitPoint;
    float distToLight = length(toLight);
    float3 lightDir = toLight / distToLight;

    float ndotl = max(dot(normal, lightDir), 0.0);
    if (ndotl <= 0.0) return float3(0.0);
    if (distToLight > light.color.w) return float3(0.0);   // .w is range
    if (IsInShadow(hitPoint, lightDir, distToLight)) return float3(0.0);

    float falloff = 1.0 / (1.0 + light.params.x * distToLight * distToLight);
    return light.color.xyz * light.direction.w * ndotl * falloff;
}

float3 SampleSpotLight(LightData light, float3 hitPoint, float3 normal) {
    float3 toLight = light.position.xyz - hitPoint;
    float distToLight = length(toLight);
    float3 lightDir = toLight / distToLight;

    float ndotl = max(dot(normal, lightDir), 0.0);
    if (ndotl <= 0.0) return float3(0.0);
    if (distToLight > light.color.w) return float3(0.0);

    float3 spotDir = normalize(light.direction.xyz);
    float cosAngle = dot(-lightDir, spotDir);
    float innerCone = cos(light.params.y);
    float outerCone = cos(light.params.z);
    if (cosAngle < outerCone) return float3(0.0);

    if (IsInShadow(hitPoint, lightDir, distToLight)) return float3(0.0);

    float falloff = 1.0 / (1.0 + light.params.x * distToLight * distToLight);
    float spotFalloff = smoothstep(outerCone, innerCone, cosAngle);
    return light.color.xyz * light.direction.w * ndotl * falloff * spotFalloff;
}

float3 SampleLights(float3 hitPoint, float3 normal) {
    float3 totalLight = float3(0.0);
    for (uint i = 0; i < u_Settings.lightCount; i++) {
        LightData light = LightBuffer[i];
        uint lightType = uint(light.position.w);
        if      (lightType == 0) totalLight += SampleDirectionalLight(light, hitPoint, normal);
        else if (lightType == 1) totalLight += SamplePointLight(light, hitPoint, normal);
        else if (lightType == 2) totalLight += SampleSpotLight(light, hitPoint, normal);
    }
    return totalLight;
}

float3 TraceRay(inout Ray ray, inout uint state) {
    float3 rayColor = float3(1.0);
    float3 brightness_score = float3(0.0);

    for (uint i = 0; i < u_Settings.bouncesPerRay; i++) {
        CheckRayCollision(ray);

        if (ray.t < INF_T) {
            float3 hitPoint = ray.origin + ray.dir * ray.t;

            Material mat = SampleMaterial(MaterialBuffer[ray.materialIdx], ray.uv);
            float3 albedo = mat.color.rgb;

            // metallic (pbrParams.x) and roughness (.y) are sampled and
            // available but not consumed: this loop is pure diffuse and the
            // BSDF work is Phase 6. PBR.slang does consume them.

            brightness_score += mat.emission.rgb * mat.emission.w * rayColor;

            // OFFSET ALONG THE GEOMETRIC NORMAL, SHADE ALONG THE SHADING NORMAL.
            // That asymmetry is deliberate: offsetting along a normal-mapped
            // shading normal self-intersects on grazing surfaces, because the
            // shading normal can tilt far enough that the offset point lands
            // back inside the triangle it came from.
            float3 directLight = SampleLights(hitPoint + ray.normalGeom * SURFACE_BIAS, ray.normalShade);
            brightness_score += directLight * albedo * rayColor;

            rayColor *= albedo;

            ray.origin = hitPoint + ray.normalGeom * SURFACE_BIAS;
            // Cosine-weighted hemisphere sampling around the shading normal.
            ray.dir = normalize(ray.normalShade + RandomDirection(state));
            ray.anyHit = false;
        }
        else {
            brightness_score += GetSkyboxLight(ray.dir) * rayColor;
            break;
        }
    }
    return brightness_score;
}

[shader("compute")]
[numthreads(8, 4, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
    int2 texelCoords = int2(threadId.xy);
    uint dimX, dimY;
    rayTracingTexture.GetDimensions(dimX, dimY);
    int2 dims = int2(dimX, dimY);
    if (texelCoords.x >= dims.x || texelCoords.y >= dims.y) { return; }

    Ray cameraRay = MakeCameraRay(texelCoords, dims);

    float3 pixelColor = float3(0.0);
    for (uint i = 0; i < u_Settings.raysPerPixel; i++) {
        uint state = InitRngState(texelCoords, u_Settings.numAccumulatedFrames, i);
        Ray ray = cameraRay;
        pixelColor += TraceRay(ray, state);
    }

    if (u_Settings.debugMode == 0)      { pixelColor = pixelColor / float(u_Settings.raysPerPixel); }
    else if (u_Settings.debugMode == 1) { pixelColor = IntersectionsToRgb(g_AabbIntersectionCount, u_Settings.aabbHeatmapCutoff); }
    else                                { pixelColor = IntersectionsToRgb(g_TriIntersectionCount, u_Settings.triHeatmapCutoff); }

    // Accumulate over multiple frames. THIS IS A READ-MODIFY-WRITE ON THE SAME
    // IMAGE, which is why Renderer::writeSlot pins the slot to 0 while
    // accumulating and why Draw() records a barrier before the dispatch.
    float weight = 1.0f / float(u_Settings.numAccumulatedFrames + 1);
    float3 previous = rayTracingTexture[texelCoords].rgb;
    float3 outputColor = previous * (1 - weight) + pixelColor * weight;
    rayTracingTexture[texelCoords] = float4(outputColor, 1.0f);
}
