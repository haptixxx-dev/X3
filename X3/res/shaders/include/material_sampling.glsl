// material_sampling.glsl - Material property and texture sampling utilities

// TODO: When bindless textures are implemented, these will sample from texture arrays
// For now, they return scalar values from the Material struct

vec3 SampleAlbedo(Material mat, vec2 uv) {
    // if (mat.albedoTexIdx >= 0) return texture(textureArray, vec3(uv, mat.albedoTexIdx)).rgb;
    return mat.color.xyz;
}

vec3 SampleNormal(Material mat, vec2 uv, vec3 geometryNormal, vec3 tangent, vec3 bitangent) {
    // if (mat.normalTexIdx >= 0) {
    //     vec3 normalMap = texture(textureArray, vec3(uv, mat.normalTexIdx)).rgb * 2.0 - 1.0;
    //     mat3 TBN = mat3(tangent, bitangent, geometryNormal);
    //     return normalize(TBN * normalMap);
    // }
    return geometryNormal;
}

// Simplified version when tangent space is not available
vec3 SampleNormalSimple(Material mat, vec2 uv, vec3 geometryNormal) {
    return geometryNormal;
}

float SampleMetallic(Material mat, vec2 uv) {
    // if (mat.metallicTexIdx >= 0) return texture(textureArray, vec3(uv, mat.metallicTexIdx)).r;
    return mat.pbrParams.x;
}

float SampleRoughness(Material mat, vec2 uv) {
    // if (mat.roughnessTexIdx >= 0) return texture(textureArray, vec3(uv, mat.roughnessTexIdx)).r;
    return mat.pbrParams.y;
}

float SampleAO(Material mat, vec2 uv) {
    // if (mat.aoTexIdx >= 0) return texture(textureArray, vec3(uv, mat.aoTexIdx)).r;
    return mat.pbrParams.z;
}

vec3 SampleEmission(Material mat, vec2 uv) {
    // if (mat.emissionTexIdx >= 0) return texture(textureArray, vec3(uv, mat.emissionTexIdx)).rgb * mat.emission.w;
    return mat.emission.xyz * mat.emission.w;
}

// PBR material parameters struct for shading functions
struct PBRParams {
    vec3 albedo;
    vec3 normal;
    float metallic;
    float roughness;
    float ao;
    vec3 emission;
    vec3 F0;  // Fresnel reflectance at normal incidence
};

// Extract PBR parameters from material
PBRParams GetPBRParams(Material mat, vec2 uv, vec3 geometryNormal) {
    PBRParams params;
    params.albedo = SampleAlbedo(mat, uv);
    params.normal = SampleNormalSimple(mat, uv, geometryNormal);
    params.metallic = SampleMetallic(mat, uv);
    params.roughness = max(SampleRoughness(mat, uv), 0.04); // Prevent divide by zero
    params.ao = SampleAO(mat, uv);
    params.emission = SampleEmission(mat, uv);

    // Calculate F0 (Fresnel reflectance at normal incidence)
    // Dielectrics have F0 around 0.04, metals use albedo
    params.F0 = mix(vec3(0.04), params.albedo, params.metallic);

    return params;
}
