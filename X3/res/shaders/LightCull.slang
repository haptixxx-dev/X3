// =============================================================================
// LightCull -- which lights can reach which cluster.
//
// One thread per cluster; each walks the whole light list and keeps the ones
// whose bound intersects its box. The forward pass then loops that list instead
// of every light in the scene, which is the entire point of Forward+.
//
// THE TEST IS EXACT, NOT AN APPROXIMATION. Trace.slang's SampleLightDirect
// returns no contribution when `dist > light.color.w`, so a light's influence is
// exactly a sphere of radius `range` about its position. Closest-point-on-AABB
// against that sphere is therefore not conservative -- it is the same predicate
// the shading uses, evaluated on a box instead of a point. If the two ever
// disagree, shading loses a light with no warning, which is why debugMode 5
// exists to check them against each other rather than trusting this comment.
//
// DIRECTIONAL LIGHTS GO IN EVERY CLUSTER. They have no position and no range, so
// there is nothing to cull them against. The alternative -- sorting the light
// buffer so directionals lead and slicing them off in the shader -- would reorder
// the light list and therefore change the float summation order of every
// existing golden image, for no gain at the one or two directionals a scene has.
// =============================================================================

import GpuTypes;
import Bindings;

/// Squared distance from a point to the closest point on an AABB. Zero inside.
float DistSqPointAabb(float3 p, float3 lo, float3 hi) {
    const float3 d = max(max(lo - p, float3(0.0)), p - hi);
    return dot(d, d);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    const uint cluster = tid.x;
    if (cluster >= X3_CLUSTER_COUNT) return;

    const ClusterAABB box = ClusterAABBBuffer[cluster];
    const uint base = cluster * X3_MAX_LIGHTS_PER_CLUSTER;

    // THE CAP IS A REAL TRUNCATION. A cluster reached by more than
    // X3_MAX_LIGHTS_PER_CLUSTER lights silently drops the rest, and the result is
    // under-lit pixels rather than an error. Nothing here can detect it cheaply
    // -- but debugMode 5 can, because it checks the list against the shading
    // predicate per pixel and paints red wherever a light that should reach a
    // point is absent from its cluster. That is the gate on this number.
    uint count = 0;
    for (uint i = 0; i < u_Settings.lightCount && count < X3_MAX_LIGHTS_PER_CLUSTER; i++) {
        const LightData light = LightBuffer[i];
        const uint type = uint(light.position.w);

        bool hit;
        if (type == 0) {
            hit = true;                     // directional: unbounded
        } else {
            // Lights are authored in world space; the grid lives in view space.
            const float3 posView = mul(u_Camera.view, float4(light.position.xyz, 1.0)).xyz;
            const float  range   = light.color.w;
            hit = DistSqPointAabb(posView, box.minPoint.xyz, box.maxPoint.xyz) <= range * range;
        }

        if (hit) {
            ClusterLightIndices[base + count] = i;
            count++;
        }
    }

    // Ascending light index within a cluster, because that is the order the loop
    // appends in. The forward pass sums in this order, so it is deterministic
    // across runs -- which the golden-image tests rely on.
    ClusterLightGrid[cluster] = count;
}
