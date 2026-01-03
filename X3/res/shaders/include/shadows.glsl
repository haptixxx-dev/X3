// shadows.glsl - Shadow ray testing

// Check if a point is in shadow by tracing a ray toward the light
// Returns true if something blocks the path to the light
bool IsInShadow(vec3 origin, vec3 normal, vec3 dirToLight, float distToLight) {
    Ray shadowRay;
    // Offset origin along normal to prevent self-intersection
    shadowRay.origin = origin + normal * SURFACE_BIAS;
    shadowRay.dir = dirToLight;
    shadowRay.t = distToLight - SURFACE_BIAS;

    CheckRayCollision(shadowRay);

    return (shadowRay.t < distToLight - SURFACE_BIAS);
}
