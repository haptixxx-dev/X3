// ray_tracing.glsl - BVH traversal and ray-primitive intersection

// Slab method Ray-AABB intersection
// https://tavianator.com/fast-branchless-raybounding-box-intersections/
float IntersectAABB(vec3 origin, vec3 invDir, vec3 bmin, vec3 bmax, float rayTMax) {
    vec3 t1 = (bmin - origin) * invDir;
    vec3 t2 = (bmax - origin) * invDir;
    vec3 tMin = min(t1, t2);
    vec3 tMax = max(t1, t2);
    float tNear = max(max(tMin.x, tMin.y), tMin.z);
    float tFar  = min(min(tMax.x, tMax.y), tMax.z);
    return (tFar >= tNear && tNear < rayTMax && tFar > 0.0) ? tNear : INF_T;
}

// Moller-Trumbore Ray-Triangle intersection
// Returns barycentric coordinates (u, v) via out parameters
bool IntersectTri(inout Ray r, Triangle tri, out float outU, out float outV) {
    vec3 E1 = tri.v1.xyz - tri.v0.xyz;
    vec3 E2 = tri.v2.xyz - tri.v0.xyz;
    vec3 Ng = cross(E1, E2);
    float det = -dot(r.dir, Ng);

    if (det < 1e-6) return false;

    float invdet = 1.0 / det;
    vec3 AO  = r.origin - tri.v0.xyz;
    vec3 DAO = cross(AO, r.dir);

    float t = dot(AO, Ng) * invdet;
    float u = dot(E2, DAO) * invdet;
    float v = -dot(E1, DAO) * invdet;

    if (t < 0.0 || u < 0.0 || v < 0.0 || (u + v) > 1.0) return false;
    if (t >= r.t) return false;

    r.t = t;
    r.normal = normalize(Ng);
    outU = u;
    outV = v;
    return true;
}

// Interpolate UV coordinates using barycentric weights
vec2 InterpolateUV(uint globalTriIdx, float u, float v) {
    uint base = globalTriIdx * 3;
    vec2 uv0 = UVBuffer[base + 0];
    vec2 uv1 = UVBuffer[base + 1];
    vec2 uv2 = UVBuffer[base + 2];
    return (1.0 - u - v) * uv0 + u * uv1 + v * uv2;
}

// BVH traversal for a single entity
void TraverseBVH(inout Ray ray, inout EntityHandle entityHandle,
                 out float hitU, out float hitV, out uint hitTriGlobalIdx) {
    uint nodeOffset = entityHandle.rootNodeIdx;
    uint nodeIdx = 0;
    uint stack[64];
    uint stackPtr = 0;
    vec3 origin = ray.origin;
    vec3 invDir = 1.0 / ray.dir;

    hitU = 0.0;
    hitV = 0.0;
    hitTriGlobalIdx = 0;

    while (true) {
        BVHNode node = NodeBuffer[nodeOffset + nodeIdx];

        if (node.triCount != 0) {
            // Leaf node - test triangles
            uint first = node.leftChild_Or_FirstTri;
            uint count = node.triCount;

            for (uint i = 0; i < count; i++) {
                uint triIndex = IndexBuffer[entityHandle.rootTriIdx + first + i];
                uint globalTriIdx = entityHandle.rootTriIdx + triIndex;
                Triangle tri = MeshBuffer[globalTriIdx];

                float u, v;
                if (IntersectTri(ray, tri, u, v)) {
                    g_TriIntersectionCount++;
                    ray.materialIdx = entityHandle.materialIdx;
                    hitU = u;
                    hitV = v;
                    hitTriGlobalIdx = globalTriIdx;
                }
            }

            if (stackPtr == 0) break;
            nodeIdx = stack[--stackPtr];
            continue;
        }

        // Internal node - test children
        uint child1Idx = node.leftChild_Or_FirstTri;
        uint child2Idx = node.leftChild_Or_FirstTri + 1;

        float dist1 = IntersectAABB(origin, invDir,
            NodeBuffer[nodeOffset + child1Idx].min,
            NodeBuffer[nodeOffset + child1Idx].max, ray.t);
        float dist2 = IntersectAABB(origin, invDir,
            NodeBuffer[nodeOffset + child2Idx].min,
            NodeBuffer[nodeOffset + child2Idx].max, ray.t);

        // Sort by distance (process closer child first)
        if (dist1 > dist2) {
            float tmpDist = dist1; dist1 = dist2; dist2 = tmpDist;
            uint tmpIdx = child1Idx; child1Idx = child2Idx; child2Idx = tmpIdx;
        }

        if (dist1 >= INF_T) {
            if (stackPtr == 0) break;
            nodeIdx = stack[--stackPtr];
        } else {
            g_AabbIntersectionCount++;
            nodeIdx = child1Idx;
            if (dist2 < INF_T) {
                g_AabbIntersectionCount++;
                stack[stackPtr++] = child2Idx;
            }
        }
    }
}

// Trace ray against all entities in the scene
void CheckRayCollision(inout Ray ray) {
    ray.t = INF_T;
    ray.uv = vec2(0.0);
    ray.hitTriIdx = 0;

    for (uint i = 0; i < u_EntityCount; i++) {
        EntityHandle entityHandle = EntityLookupTable[i];
        mat4 model = TransformBuffer[entityHandle.transformIdx];
        mat4 invTransform = inverse(model);

        // Transform ray to entity local space
        Ray rayLocal;
        rayLocal.t = INF_T;
        rayLocal.origin = (invTransform * vec4(ray.origin, 1.0)).xyz;
        rayLocal.dir = (invTransform * vec4(ray.dir, 0.0)).xyz;

        float hitU, hitV;
        uint hitTriGlobalIdx;
        TraverseBVH(rayLocal, entityHandle, hitU, hitV, hitTriGlobalIdx);

        if (rayLocal.t < ray.t) {
            // Transform normal back to world space
            mat3 normalMatrix = mat3(transpose(invTransform));
            vec3 worldNormal = normalize(normalMatrix * rayLocal.normal);

            ray.t = length(mat3(model) * (rayLocal.dir * rayLocal.t));
            ray.normal = faceforward(worldNormal, ray.dir, worldNormal);
            ray.materialIdx = rayLocal.materialIdx;
            ray.uv = InterpolateUV(hitTriGlobalIdx, hitU, hitV);
            ray.hitTriIdx = hitTriGlobalIdx;
        }
    }
}
