// =============================================================================
// ClusterBuild -- the view-space AABB of every cluster in the frustum grid.
//
// One thread per cluster, 3456 of them. Runs before LightCull, which tests each
// light's bounding sphere against these boxes.
//
// NO MATRIX INVERSE. The obvious implementation unprojects the eight corners of
// each cluster through inverse(proj), which would mean adding invProj to the
// camera UBO and growing it again. It is unnecessary: the projection is a plain
// perspective, so
//
//     ndc.x = proj[0][0] * x_view / z_view   =>   x_view = ndc.x * z_view / proj[0][0]
//
// and the same for y. Two scalars off the diagonal give exactly what the inverse
// would, and they are already in the UBO.
//
// x_view scales LINEARLY with depth, so the extreme x over a slice is at one of
// the two slice depths -- which is why all four products are taken rather than
// assuming the far plane always dominates. It does not: for a tile straddling
// the centre of the screen, ndc.x changes sign across the tile.
// =============================================================================

import GpuTypes;
import Bindings;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    const uint cluster = tid.x;
    if (cluster >= X3_CLUSTER_COUNT) return;

    const uint tx = cluster % X3_CLUSTER_X;
    const uint ty = (cluster / X3_CLUSTER_X) % X3_CLUSTER_Y;
    const uint tz = cluster / (X3_CLUSTER_X * X3_CLUSTER_Y);

    // The tile's NDC extent. Matches ClusterIndexFromView's mapping exactly --
    // if these two ever disagree, lights are culled against a box the shading
    // never looks up, and the failure is a few objects unlit at certain camera
    // angles rather than anything that names itself.
    const float ndcX0 = float(tx)     / float(X3_CLUSTER_X) * 2.0 - 1.0;
    const float ndcX1 = float(tx + 1) / float(X3_CLUSTER_X) * 2.0 - 1.0;
    const float ndcY0 = float(ty)     / float(X3_CLUSTER_Y) * 2.0 - 1.0;
    const float ndcY1 = float(ty + 1) / float(X3_CLUSTER_Y) * 2.0 - 1.0;

    const float2 zRange = ClusterSliceDepth(tz, u_Camera.nearPlane, u_Camera.farPlane);

    const float invPx = 1.0 / u_Camera.proj[0][0];
    const float invPy = 1.0 / u_Camera.proj[1][1];

    const float x00 = ndcX0 * zRange.x * invPx;
    const float x01 = ndcX0 * zRange.y * invPx;
    const float x10 = ndcX1 * zRange.x * invPx;
    const float x11 = ndcX1 * zRange.y * invPx;

    const float y00 = ndcY0 * zRange.x * invPy;
    const float y01 = ndcY0 * zRange.y * invPy;
    const float y10 = ndcY1 * zRange.x * invPy;
    const float y11 = ndcY1 * zRange.y * invPy;

    ClusterAABB box;
    box.minPoint = float4(min(min(x00, x01), min(x10, x11)),
                          min(min(y00, y01), min(y10, y11)),
                          zRange.x, 0.0);
    box.maxPoint = float4(max(max(x00, x01), max(x10, x11)),
                          max(max(y00, y01), max(y10, y11)),
                          zRange.y, 0.0);

    ClusterAABBBuffer[cluster] = box;
}
