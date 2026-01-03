// mx_pbr.glsl - MaterialX-compatible PBR shading functions
// Based on MaterialX pbrlib (standard_surface/open_pbr_surface)
// Uses Cook-Torrance microfacet BRDF with GGX distribution

//-----------------------------------------------------------------------------
// GGX/Trowbridge-Reitz Normal Distribution Function
// D(H) = alpha^2 / (PI * ((N.H)^2 * (alpha^2 - 1) + 1)^2)
//-----------------------------------------------------------------------------
float mx_ggx_NDF(float NdotH, float alpha) {
    float alpha2 = alpha * alpha;
    float denom = NdotH * NdotH * (alpha2 - 1.0) + 1.0;
    return alpha2 / (PI * denom * denom);
}

//-----------------------------------------------------------------------------
// Smith G2 Geometry Function (height-correlated)
// Combined masking-shadowing for GGX
//-----------------------------------------------------------------------------
float mx_ggx_smith_G1(float NdotV, float alpha) {
    float alpha2 = alpha * alpha;
    return 2.0 * NdotV / (NdotV + sqrt(alpha2 + (1.0 - alpha2) * NdotV * NdotV));
}

float mx_ggx_smith_G2(float NdotL, float NdotV, float alpha) {
    return mx_ggx_smith_G1(NdotL, alpha) * mx_ggx_smith_G1(NdotV, alpha);
}

//-----------------------------------------------------------------------------
// Fresnel-Schlick Approximation
// F(theta) = F0 + (1 - F0) * (1 - cos(theta))^5
//-----------------------------------------------------------------------------
vec3 mx_fresnel_schlick(float cosTheta, vec3 F0) {
    float t = 1.0 - cosTheta;
    float t2 = t * t;
    float t5 = t2 * t2 * t;
    return F0 + (vec3(1.0) - F0) * t5;
}

// Fresnel with roughness-adjusted F90 for IBL
vec3 mx_fresnel_schlick_roughness(float cosTheta, vec3 F0, float roughness) {
    float t = 1.0 - cosTheta;
    float t2 = t * t;
    float t5 = t2 * t2 * t;
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * t5;
}

//-----------------------------------------------------------------------------
// Cook-Torrance Specular BRDF
// f_spec = D * G * F / (4 * NdotL * NdotV)
//-----------------------------------------------------------------------------
vec3 mx_cook_torrance_specular(vec3 N, vec3 V, vec3 L, vec3 F0, float roughness) {
    vec3 H = normalize(V + L);

    float NdotL = max(dot(N, L), 0.001);
    float NdotV = max(dot(N, V), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float alpha = roughness * roughness;

    float D = mx_ggx_NDF(NdotH, alpha);
    float G = mx_ggx_smith_G2(NdotL, NdotV, alpha);
    vec3 F = mx_fresnel_schlick(VdotH, F0);

    vec3 numerator = D * G * F;
    float denominator = 4.0 * NdotL * NdotV;

    return numerator / max(denominator, 0.001);
}

//-----------------------------------------------------------------------------
// Lambertian Diffuse BRDF with energy conservation
// Accounts for specular reflection reducing available energy for diffuse
//-----------------------------------------------------------------------------
vec3 mx_burley_diffuse(vec3 albedo, vec3 N, vec3 V, vec3 L, float roughness) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float LdotH = max(dot(L, H), 0.0);

    // Disney/Burley diffuse approximation
    float fd90 = 0.5 + 2.0 * roughness * LdotH * LdotH;
    float lightScatter = 1.0 + (fd90 - 1.0) * pow(1.0 - NdotL, 5.0);
    float viewScatter = 1.0 + (fd90 - 1.0) * pow(1.0 - NdotV, 5.0);

    return albedo * INV_PI * lightScatter * viewScatter;
}

// Simple Lambertian diffuse (faster)
vec3 mx_lambert_diffuse(vec3 albedo) {
    return albedo * INV_PI;
}

//-----------------------------------------------------------------------------
// Full PBR Surface Evaluation (Direct Lighting)
// Combines diffuse and specular with energy conservation
//-----------------------------------------------------------------------------
vec3 mx_evaluate_pbr_surface(PBRParams params, vec3 V, vec3 L, vec3 lightRadiance) {
    vec3 N = params.normal;
    float NdotL = max(dot(N, L), 0.0);

    if (NdotL <= 0.0) return vec3(0.0);

    // Specular BRDF (Cook-Torrance)
    vec3 specular = mx_cook_torrance_specular(N, V, L, params.F0, params.roughness);

    // Fresnel for energy conservation
    vec3 H = normalize(V + L);
    float VdotH = max(dot(V, H), 0.0);
    vec3 F = mx_fresnel_schlick(VdotH, params.F0);

    // Diffuse BRDF with energy conservation
    // Metals have no diffuse, dielectrics have diffuse weighted by (1 - F)
    vec3 kD = (vec3(1.0) - F) * (1.0 - params.metallic);
    vec3 diffuse = kD * mx_lambert_diffuse(params.albedo);

    // Combined BRDF * Li * NdotL * AO
    return (diffuse + specular) * lightRadiance * NdotL * params.ao;
}

//-----------------------------------------------------------------------------
// Image-Based Lighting (IBL) Approximation
// Uses skybox for both diffuse and specular environment lighting
//-----------------------------------------------------------------------------
vec3 mx_evaluate_pbr_indirect(PBRParams params, vec3 V, vec3 skyboxDiffuse, vec3 skyboxSpecular) {
    vec3 N = params.normal;
    float NdotV = max(dot(N, V), 0.0);

    // Fresnel for indirect
    vec3 F = mx_fresnel_schlick_roughness(NdotV, params.F0, params.roughness);

    // Diffuse IBL (hemispherical irradiance approximation)
    vec3 kD = (vec3(1.0) - F) * (1.0 - params.metallic);
    vec3 diffuseIBL = kD * params.albedo * skyboxDiffuse;

    // Specular IBL (rough approximation without prefiltered environment map)
    // Reduce specular intensity with roughness
    float specularWeight = 1.0 - params.roughness * 0.7;
    vec3 specularIBL = F * skyboxSpecular * specularWeight;

    return (diffuseIBL + specularIBL) * params.ao;
}

//-----------------------------------------------------------------------------
// Complete PBR Shading (Direct + Indirect)
//-----------------------------------------------------------------------------
vec3 mx_shade_pbr(PBRParams params, vec3 hitPoint, vec3 V) {
    vec3 N = params.normal;
    vec3 result = vec3(0.0);

    // Direct lighting from all lights
    for (uint i = 0; i < u_LightCount; i++) {
        LightSample ls = GetLightSample(LightBuffer[i], hitPoint, N);

        if (ls.visible) {
            // Shadow test
            if (!IsInShadow(hitPoint, N, ls.direction, ls.distance)) {
                result += mx_evaluate_pbr_surface(params, V, ls.direction, ls.radiance);
            }
        }
    }

    // Indirect lighting (IBL from skybox)
    vec3 R = reflect(-V, N);
    vec3 skyboxDiffuse = GetSkyboxLight(N) * 0.3;  // Crude diffuse irradiance
    vec3 skyboxSpecular = GetSkyboxLight(R);
    result += mx_evaluate_pbr_indirect(params, V, skyboxDiffuse, skyboxSpecular);

    // Add emission
    result += params.emission;

    return result;
}
