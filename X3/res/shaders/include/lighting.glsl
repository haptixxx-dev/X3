// lighting.glsl - Light sampling for all light types

// Sample a directional light
vec3 SampleDirectionalLight(LightData light, vec3 hitPoint, vec3 normal) {
    vec3 lightDir = -normalize(light.direction.xyz);
    float ndotl = max(dot(normal, lightDir), 0.0);

    if (ndotl <= 0.0) return vec3(0.0);

    // Directional lights are infinitely far away
    if (IsInShadow(hitPoint, normal, lightDir, 1e20)) return vec3(0.0);

    return light.color.xyz * light.direction.w * ndotl;
}

// Sample a point light
vec3 SamplePointLight(LightData light, vec3 hitPoint, vec3 normal) {
    vec3 lightPos = light.position.xyz;
    vec3 toLight = lightPos - hitPoint;
    float distToLight = length(toLight);
    vec3 lightDir = toLight / distToLight;

    float ndotl = max(dot(normal, lightDir), 0.0);
    if (ndotl <= 0.0) return vec3(0.0);

    // Check range
    float range = light.color.w;
    if (distToLight > range) return vec3(0.0);

    // Shadow test
    if (IsInShadow(hitPoint, normal, lightDir, distToLight)) return vec3(0.0);

    // Distance attenuation
    float attenuation = light.params.x;
    float falloff = 1.0 / (1.0 + attenuation * distToLight * distToLight);

    return light.color.xyz * light.direction.w * ndotl * falloff;
}

// Sample a spot light
vec3 SampleSpotLight(LightData light, vec3 hitPoint, vec3 normal) {
    vec3 lightPos = light.position.xyz;
    vec3 toLight = lightPos - hitPoint;
    float distToLight = length(toLight);
    vec3 lightDir = toLight / distToLight;

    float ndotl = max(dot(normal, lightDir), 0.0);
    if (ndotl <= 0.0) return vec3(0.0);

    // Check range
    float range = light.color.w;
    if (distToLight > range) return vec3(0.0);

    // Check spot cone
    vec3 spotDir = normalize(light.direction.xyz);
    float cosAngle = dot(-lightDir, spotDir);
    float innerCone = cos(light.params.y);
    float outerCone = cos(light.params.z);

    if (cosAngle < outerCone) return vec3(0.0);

    // Shadow test
    if (IsInShadow(hitPoint, normal, lightDir, distToLight)) return vec3(0.0);

    // Distance attenuation
    float attenuation = light.params.x;
    float falloff = 1.0 / (1.0 + attenuation * distToLight * distToLight);

    // Spot cone falloff
    float spotFalloff = smoothstep(outerCone, innerCone, cosAngle);

    return light.color.xyz * light.direction.w * ndotl * falloff * spotFalloff;
}

// Sample all lights in the scene (returns total irradiance)
vec3 SampleLights(vec3 hitPoint, vec3 normal) {
    vec3 totalLight = vec3(0.0);

    for (uint i = 0; i < u_LightCount; i++) {
        LightData light = LightBuffer[i];
        uint lightType = uint(light.position.w);

        if (lightType == LIGHT_TYPE_DIRECTIONAL) {
            totalLight += SampleDirectionalLight(light, hitPoint, normal);
        } else if (lightType == LIGHT_TYPE_POINT) {
            totalLight += SamplePointLight(light, hitPoint, normal);
        } else if (lightType == LIGHT_TYPE_SPOT) {
            totalLight += SampleSpotLight(light, hitPoint, normal);
        }
    }

    return totalLight;
}

// Light sample result for PBR shading (provides more detail)
struct LightSample {
    vec3 direction;   // Direction TO light (normalized)
    float distance;   // Distance to light
    vec3 radiance;    // Light color * intensity * attenuation
    bool visible;     // Passes geometric test (not shadowed yet)
};

LightSample GetLightSample(LightData light, vec3 hitPoint, vec3 normal) {
    LightSample ls;
    uint lightType = uint(light.position.w);

    if (lightType == LIGHT_TYPE_DIRECTIONAL) {
        ls.direction = -normalize(light.direction.xyz);
        ls.distance = 1e20;
        ls.radiance = light.color.xyz * light.direction.w;
        ls.visible = dot(normal, ls.direction) > 0.0;
    }
    else if (lightType == LIGHT_TYPE_POINT) {
        vec3 toLight = light.position.xyz - hitPoint;
        ls.distance = length(toLight);
        ls.direction = toLight / ls.distance;

        float range = light.color.w;
        float attenuation = light.params.x;
        float falloff = 1.0 / (1.0 + attenuation * ls.distance * ls.distance);

        ls.radiance = light.color.xyz * light.direction.w * falloff;
        ls.visible = dot(normal, ls.direction) > 0.0 && ls.distance < range;
    }
    else { // LIGHT_TYPE_SPOT
        vec3 toLight = light.position.xyz - hitPoint;
        ls.distance = length(toLight);
        ls.direction = toLight / ls.distance;

        float range = light.color.w;
        vec3 spotDir = normalize(light.direction.xyz);
        float cosAngle = dot(-ls.direction, spotDir);
        float innerCone = cos(light.params.y);
        float outerCone = cos(light.params.z);

        float attenuation = light.params.x;
        float falloff = 1.0 / (1.0 + attenuation * ls.distance * ls.distance);
        float spotFalloff = smoothstep(outerCone, innerCone, cosAngle);

        ls.radiance = light.color.xyz * light.direction.w * falloff * spotFalloff;
        ls.visible = dot(normal, ls.direction) > 0.0 && ls.distance < range && cosAngle >= outerCone;
    }

    return ls;
}
