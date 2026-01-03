// mx_phong.glsl - MaterialX-compatible Blinn-Phong shading functions
// Classic Blinn-Phong model for fast, simple shading

// Phong material parameters
struct PhongParams {
    vec3 diffuseColor;
    vec3 specularColor;
    float shininess;    // Specular exponent (typically 1-256)
    vec3 normal;
    float ao;
    vec3 emission;
};

// Convert PBR roughness to Phong shininess
float RoughnessToShininess(float roughness) {
    // Approximate mapping: shininess = 2 / roughness^4 - 2
    float r = max(roughness, 0.01);
    return 2.0 / (r * r * r * r) - 2.0;
}

// Create PhongParams from a Material
PhongParams GetPhongParams(Material mat, vec2 uv, vec3 geometryNormal) {
    PhongParams params;

    params.diffuseColor = SampleAlbedo(mat, uv);

    // For Phong, we derive specular from albedo and metallic
    float metallic = SampleMetallic(mat, uv);
    params.specularColor = mix(vec3(0.04), params.diffuseColor, metallic);

    // Convert roughness to shininess
    float roughness = SampleRoughness(mat, uv);
    params.shininess = RoughnessToShininess(roughness);

    params.normal = SampleNormalSimple(mat, uv, geometryNormal);
    params.ao = SampleAO(mat, uv);
    params.emission = SampleEmission(mat, uv);

    return params;
}

//-----------------------------------------------------------------------------
// Blinn-Phong BRDF Evaluation
//-----------------------------------------------------------------------------
vec3 mx_evaluate_phong(PhongParams params, vec3 V, vec3 L, vec3 lightRadiance) {
    vec3 N = params.normal;
    float NdotL = max(dot(N, L), 0.0);

    if (NdotL <= 0.0) return vec3(0.0);

    // Diffuse term
    vec3 diffuse = params.diffuseColor * NdotL;

    // Blinn-Phong specular
    vec3 H = normalize(V + L);
    float NdotH = max(dot(N, H), 0.0);
    vec3 specular = params.specularColor * pow(NdotH, params.shininess);

    // Normalization factor for energy conservation
    float normalization = (params.shininess + 8.0) / (8.0 * PI);
    specular *= normalization;

    return (diffuse + specular) * lightRadiance * params.ao;
}

//-----------------------------------------------------------------------------
// Phong Ambient/Indirect Lighting
//-----------------------------------------------------------------------------
vec3 mx_evaluate_phong_indirect(PhongParams params, vec3 V, vec3 ambientLight) {
    // Simple ambient term
    vec3 ambient = params.diffuseColor * ambientLight;

    // Very crude specular reflection from environment
    vec3 R = reflect(-V, params.normal);
    vec3 envSpecular = GetSkyboxLight(R) * params.specularColor * 0.3;

    return (ambient + envSpecular) * params.ao;
}

//-----------------------------------------------------------------------------
// Complete Phong Shading (Direct + Ambient)
//-----------------------------------------------------------------------------
vec3 mx_shade_phong(PhongParams params, vec3 hitPoint, vec3 V) {
    vec3 N = params.normal;
    vec3 result = vec3(0.0);

    // Direct lighting from all lights
    for (uint i = 0; i < u_LightCount; i++) {
        LightSample ls = GetLightSample(LightBuffer[i], hitPoint, N);

        if (ls.visible) {
            // Shadow test
            if (!IsInShadow(hitPoint, N, ls.direction, ls.distance)) {
                result += mx_evaluate_phong(params, V, ls.direction, ls.radiance);
            }
        }
    }

    // Indirect/ambient lighting from skybox
    vec3 ambientLight = GetSkyboxLight(N) * 0.2;
    result += mx_evaluate_phong_indirect(params, V, ambientLight);

    // Add emission
    result += params.emission;

    return result;
}
