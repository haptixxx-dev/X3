// =============================================================================
// Ddgi -- the shared half of dynamic diffuse global illumination.
//
// WHAT THIS IS. A grid of probes, each storing two small octahedral maps: the
// irradiance arriving at the probe from every direction, and the DISTANCE to the
// first surface in every direction. A shading pass looks up the eight probes
// around it, filters them trilinearly, and rejects the ones that a wall stands
// between -- which is what the distance map is for.
//
// WHY IT WORKS ON THIS ENGINE WITH NO RT HARDWARE. ENGINE_PLAN.md's Phase 10b
// point: DDGI is TRACER-AGNOSTIC. It needs exactly two things -- a ray/scene
// intersection oracle and a probe update compute pass. Trace.slang's
// CheckRayCollision IS that oracle, already compiled, already shared by three
// renderers. The reference implementation happens to call DXR; the algorithm
// does not care. Godot's SDFGI makes the same substitution with SDFs.
//
// THIS MODULE IS THE PART BOTH SIDES SHARE. DdgiProbeTrace.slang and
// DdgiProbeBlend.slang must agree, to the bit, on:
//   * which direction ray N of a probe points in,
//   * which texel of which tile of which atlas belongs to which probe,
//   * what a stored value MEANS.
// Every one of those lives here exactly once. Two copies of the ray-direction
// function that drift by one frame of rotation would attach each ray's radiance
// to the wrong cosine weight in the blend, and the probe would converge to a
// smeared, plausible-looking, WRONG answer -- with no error anywhere to find.
// That failure is the reason this module exists rather than a helper header.
//
// LEAK MITIGATION IS THE POINT OF THE SAMPLING FUNCTION. See the banner above
// DdgiSampleIrradiance.
// =============================================================================

module Ddgi;

import GpuTypes;
import Bindings;
import Trace;              // PI, and the intersection oracle the trace pass uses
__exported import GpuTypes;

// -----------------------------------------------------------------------------
// BINDINGS THIS MODULE ASSUMES.
//
// NONE OF THESE ARE DECLARED HERE. Bindings.slang is the single place any
// descriptor is declared in this engine -- one module, one C++ descriptor table
// generated from its reflection (scripts/gen_descriptor_tables.py), and every
// entry point declaring the same sets. Declaring a binding in a leaf module
// would either duplicate a slot or make one entry point's layout differ from
// the others, which the generator exits on rather than merging.
//
// Add these to Bindings.slang, and DdgiUBO to GpuTypes.slang, and this module
// compiles unchanged:
//
//   // ---- Set 0: images ----
//   [[vk::image_format("rgba16f")]]
//   [[vk::binding(11, 0)]] RWTexture2D<float4> u_DdgiIrradiance;
//   [[vk::image_format("rg32f")]]
//   [[vk::binding(12, 0)]] RWTexture2D<float2> u_DdgiDepth;
//
//   // ---- Set 1: uniform buffers ----
//   [[vk::binding(3, 1)]]  ConstantBuffer<DdgiUBO> u_Ddgi;
//
//   // ---- Set 2: storage buffers ----
//   [[vk::binding(15, 2)]] RWStructuredBuffer<float4> DdgiRayDataBuffer;
//
// SLOT NUMBERS ARE A REQUEST, NOT A CONSTRAINT. 11 and 12 in set 0 were the
// first free image slots when this was written -- TAA had just taken 9 and 10.
// Nothing in these three files names a slot; slangc warns loudly ("explicit
// binding overlap") if two declarations collide, so re-numbering is safe.
//
// WHY STORAGE IMAGES AND NOT SAMPLED TEXTURES, for both atlases: they are
// written by DdgiProbeBlend and read by the trace pass and by whatever shades.
// Binding the same VkImage both ways means two descriptors and a sampler for a
// 512x32 image whose bilinear filter is four loads and three lerps. This is the
// same call u_BsdfLut already made in Bindings.slang, for the same reason, and
// the hand-written filter is DdgiBilinearIrradiance below.
//
// WHY THE DEPTH ATLAS MUST BE 32-BIT FLOAT AND THE IRRADIANCE ATLAS NEED NOT BE.
// The Chebyshev test computes a variance as `meanSq - mean*mean`. Those two are
// large and nearly equal -- at a mean distance of 100 world units they are 10000
// and 10000 -- so the subtraction is catastrophic cancellation. In fp16, whose
// relative precision at 10^4 is about 1e-3, the result is NOISE rather than a
// variance, the Chebyshev weight becomes random per texel, and the GI flickers
// and blotches in a way that looks exactly like a race condition and is not one.
// Irradiance has no such subtraction and rgba16f is plenty for it.
// If rg32f is ever a problem on a target, widen to rgba32f and change ONLY
// DdgiLoadDepth/DdgiStoreDepth below -- nothing else touches the format.
//
// WHY float4 RAY DATA AND NOT A NAMED STRUCT: GpuTypes.slang is the mirrored-
// types file and nothing on the C++ side ever reads this buffer -- it is written
// and consumed entirely within one frame by two compute passes. A float4 needs
// no mirror and no static_assert. Lane meaning: .rgb = incident radiance along
// the ray, .w = signed hit distance (see DdgiEncodeRayDistance).
//
// THE STRUCT TO ADD TO GpuTypes.slang (std140, every member 16 B, so std430,
// std140 and scalar all agree -- the layout rule at the top of that file):
//
//   // std140 - 112 B (CPU: Renderer::DdgiUBOData)
//   public struct DdgiUBO {
//       public float4 gridOrigin;    // xyz world position of LOGICAL probe (0,0,0)
//       public float4 gridSpacing;   // xyz world distance between adjacent probes
//       public float4 rayRotation;   // xyzw UNIT quaternion, re-randomised per frame
//       public float4 params0;       // x hysteresis, y depthSharpness, z normalBias, w viewBias
//       public float4 params1;       // x maxRayDistance, y energyPreservation, z volumeFade, w pad
//       public int4   scroll;        // xyz probe-grid scroll offset, w pad
//       public uint4  counts;        // x frameIndex, y raysPerProbe, z enabled, w pad
//   };
//
// WHAT THE C++ MUST DO THAT NO SHADER CAN CHECK:
//   1. CLEAR BOTH ATLASES TO ZERO ONCE at creation. The blend pass's hysteresis
//      is a read-modify-write of its own texel; an uninitialised atlas seeds it
//      with NaN or garbage that hysteresis then preserves FOREVER -- a probe
//      that is permanently wrong and never converges.
//   2. Set counts.x (frameIndex) to 0 whenever the volume is re-placed or the
//      scene changes, so the blend pass takes the new value outright instead of
//      dragging a stale history through it.
//   3. rayRotation must be a UNIT quaternion. A non-unit one scales every ray
//      direction, which silently changes the sphere the rays sample into an
//      ellipsoid and biases the integral.
//   4. Order the passes DdgiProbeTrace -> barrier -> DdgiProbeBlend, and put a
//      barrier between the previous frame's blend and this frame's trace. The
//      trace pass READS the irradiance atlas the blend pass WROTE last frame --
//      that read is the entire "infinite bounces" mechanism, and it is only
//      last frame's data because of that ordering, not because of a copy.
//   5. Move gridOrigin only in WHOLE MULTIPLES of gridSpacing, and advance
//      `scroll` by the same integer step. See DdgiStorageCoord.
// -----------------------------------------------------------------------------

// =============================================================================
// CONSTANTS
// =============================================================================

/// Probe grid: 8 x 4 x 8 = 256 probes. Y is deliberately the short axis -- an
/// interior is much wider than it is tall, and a probe layer costs 64 probes.
public static const uint X3_DDGI_PROBE_COUNT_X = 8;
public static const uint X3_DDGI_PROBE_COUNT_Y = 8;
public static const uint X3_DDGI_PROBE_COUNT_Z = 8;
public static const uint X3_DDGI_PROBE_COUNT =
    X3_DDGI_PROBE_COUNT_X * X3_DDGI_PROBE_COUNT_Y * X3_DDGI_PROBE_COUNT_Z;   // 256

/// Rays traced per probe per frame. ENGINE_PLAN.md calls this "the main
/// performance dial", and it is: total probe rays per frame is 256 * 64 = 16384,
/// which is a fifth of one 640x480 path-traced sample. That is the whole cost of
/// dynamic GI on this engine.
///
/// It is ALSO the accuracy dial, and 64 is low. See the note on depth-map
/// undersampling above DdgiProbeBlend's depth loop: 64 rays cannot resolve a
/// 14x14 direction map in one frame, which is precisely why the ray set is
/// rotated per frame and the result temporally accumulated rather than used.
public static const uint X3_DDGI_RAYS_PER_PROBE = 64;

/// INTERIOR texels per probe, per axis, for each atlas. The stored tile is one
/// texel LARGER on every side -- see the border banner below.
public static const uint X3_DDGI_IRRADIANCE_TEXELS = 6;    // tile 8x8 with border
public static const uint X3_DDGI_DEPTH_TEXELS      = 14;   // tile 16x16 with border

public static const uint X3_DDGI_IRRADIANCE_TILE = X3_DDGI_IRRADIANCE_TEXELS + 2;  // 8
public static const uint X3_DDGI_DEPTH_TILE      = X3_DDGI_DEPTH_TEXELS + 2;       // 16

// -----------------------------------------------------------------------------
// ATLAS LAYOUT
//
// Tiles are laid out with X and Z folded into atlas columns and Y into atlas
// rows: tile column = px + X * pz, tile row = py. So:
//
//   irradiance atlas = (8*8) * 8  x  4 * 8  = 512 x 32 texels   (rgba16f, 128 KB)
//   depth atlas      = (8*8) * 16 x  4 * 16 = 1024 x 64 texels  (rg32f,   512 KB)
//
// X and Z fold into columns rather than X and Y because the grid is wide and
// flat: folding the two LONG axes keeps the atlas a long thin strip instead of a
// nearly-square image with a mostly-empty right-hand side.
//
// THE BORDER, AND WHY ONE TEXEL IS EXACTLY THE RIGHT AMOUNT.
//
// An octahedral map is a SEAMED parametrisation. The square [-1,1]^2 covers the
// whole sphere, and the boundary of that square is a fold: the point (u, 1) on
// the top edge and the point (-u, 1) decode to the SAME direction (work it
// through OctDecode -- both give normalize(0, 1-|u|, -|u|)). Every edge folds
// onto itself with the opposite-axis coordinate mirrored, and all four corners
// collapse onto -Z.
//
// A bilinear fetch near an edge wants the texel one step OUTSIDE the interior.
// Concretely, DdgiBilinearIrradiance samples at texel-space position
// p = tileOrigin + 1 + uv*N with uv in [0,1], so its lower filter tap is
// floor(p - 0.5), which is tileOrigin + 0 at uv = 0 and tileOrigin + N + 1 at
// uv = 1. Both of those are OUTSIDE the interior -- so the filter reads them
// whether they exist or not.
//
// Without a border they are the NEIGHBOURING PROBE'S TILE. The visible failure
// is a bright or dark grid of seams: every probe bleeds its neighbour's
// backward-facing irradiance into its own -Z region, so lighting picks up a
// regular 8-texel checker that moves with the camera and looks like a filtering
// bug in whatever pass is downstream.
//
// One texel of border, holding the correctly-mirrored interior value, makes the
// bilinear filter continuous across the seam. TWO texels would be wasted: a
// bilinear filter reaches exactly one texel past the sample position, never
// more. This is why the tile sizes are 6+2 and 14+2 and not 8 and 16 outright.
//
// The border is not a copy pass. See DdgiTileTexelToInterior.
// -----------------------------------------------------------------------------

/// Atlas dimensions in texels, for the C++ that allocates them.
public static const uint X3_DDGI_IRRADIANCE_ATLAS_W =
    X3_DDGI_PROBE_COUNT_X * X3_DDGI_PROBE_COUNT_Z * X3_DDGI_IRRADIANCE_TILE;   // 512
public static const uint X3_DDGI_IRRADIANCE_ATLAS_H =
    X3_DDGI_PROBE_COUNT_Y * X3_DDGI_IRRADIANCE_TILE;                           // 32
public static const uint X3_DDGI_DEPTH_ATLAS_W =
    X3_DDGI_PROBE_COUNT_X * X3_DDGI_PROBE_COUNT_Z * X3_DDGI_DEPTH_TILE;        // 1024
public static const uint X3_DDGI_DEPTH_ATLAS_H =
    X3_DDGI_PROBE_COUNT_Y * X3_DDGI_DEPTH_TILE;                                // 64

// =============================================================================
// UBO ACCESSORS
//
// Every read of u_Ddgi goes through one of these, so a field that moves is a
// change to this block and nothing else. The alternative -- u_Ddgi.params0.w
// spelled out at eleven call sites -- is how a swizzle lane gets swapped in one
// of them and the GI develops a bias nobody can locate.
// =============================================================================

public uint  DdgiFrameIndex()        { return u_Ddgi.counts.x; }
public bool  DdgiEnabled()           { return u_Ddgi.counts.z != 0u; }
public float DdgiHysteresis()        { return saturate(u_Ddgi.params0.x); }
public float DdgiNormalBias()        { return u_Ddgi.params0.z; }
public float DdgiViewBias()          { return u_Ddgi.params0.w; }
public float DdgiMaxRayDistance()    { return max(u_Ddgi.params1.x, 1e-3); }
public float DdgiEnergyPreservation(){ return saturate(u_Ddgi.params1.y); }
public float DdgiVolumeFadeDistance(){ return max(u_Ddgi.params1.z, 1e-4); }

/// Sharpness of the cosine lobe the DEPTH blend weights rays by.
///
/// TUNE THIS WITH THE RAY COUNT, not independently. The effective fraction of
/// the sphere a cos^s lobe covers is 1/(2(s+1)), so the expected number of the
/// 64 rays contributing to one texel is 32/(s+1):
///
///     s = 50 -> 0.63 rays      s = 20 -> 1.5 rays
///     s = 12 -> 2.5 rays       s =  8 -> 3.6 rays
///
/// The reference DDGI value is 50, sized for its ~300-ray budget. At 64 rays it
/// leaves the MAJORITY of texels with no sample at all in a given frame. Around
/// 12 is the right order here. A too-sharp value is not a crash: it is a depth
/// map that updates only in the few directions a ray happened to land in, which
/// reads as GI that lags the scene by dozens of frames in some directions and
/// not others -- and a leak that flickers as the ray rotation moves.
public float DdgiDepthSharpness()    { return max(u_Ddgi.params0.y, 1.0); }

/// Rays actually traced this frame, clamped to what the ray buffer holds.
///
/// CALLED BY BOTH PASSES so they cannot disagree. The trace pass tracing 64 rays
/// while the blend pass integrates 32 is a silent 2x energy loss; the reverse
/// integrates 32 slots of last frame's data alongside 32 of this frame's, which
/// is worse because it looks correct.
public uint DdgiRayCount() {
    return clamp(u_Ddgi.counts.y, 1u, X3_DDGI_RAYS_PER_PROBE);
}

// =============================================================================
// OCTAHEDRAL MAPPING
//
// Cigolle et al., "A Survey of Efficient Representations for Independent Unit
// Vectors" (JCGT 2014). Equal-area-ish, no poles, and -- the property that
// matters here -- it maps the sphere to a SQUARE, so a probe's directional data
// is a rectangular tile that packs into an atlas with no wasted texels. A
// lat-long map would waste the poles and distort them; a cube map would be six
// tiles per probe and six times the border problem.
// =============================================================================

/// sign(), except that it returns +1 at zero instead of 0.
///
/// NOT sign(). The fold below multiplies by this, and a zero factor collapses an
/// entire half-edge of the octahedron onto the origin -- every direction on that
/// edge decodes to the same wrong vector. It costs nothing to get right and is
/// invisible until a ray lands exactly on an axis plane, which with a rotated
/// spherical-Fibonacci set happens somewhere every few frames.
float2 SignNotZero(float2 v) {
    return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

/// Unit direction -> [-1,1]^2.
public float2 OctEncode(float3 d) {
    const float l1 = abs(d.x) + abs(d.y) + abs(d.z);
    float2 p = d.xy * (1.0 / max(l1, 1e-8));
    if (d.z < 0.0) p = (1.0 - abs(float2(p.y, p.x))) * SignNotZero(p);
    return p;
}

/// [-1,1]^2 -> unit direction. Exact inverse of OctEncode.
public float3 OctDecode(float2 f) {
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0) {
        const float2 folded = (1.0 - abs(float2(n.y, n.x))) * SignNotZero(n.xy);
        n.x = folded.x;
        n.y = folded.y;
    }
    return normalize(n);
}

/// Centre of INTERIOR texel (i,j) of an N x N octahedral map, in [-1,1]^2.
///
/// The +0.5 is texel CENTRES, not corners. Sampling corners would make the two
/// texels either side of the seam decode to directions half a texel apart from
/// where the border-mirror rule puts them, and the seam would never quite close.
public float2 DdgiInteriorTexelToOct(int2 interior, uint n) {
    return ((float2(interior) + 0.5) / float(n)) * 2.0 - 1.0;
}

/// The direction INTERIOR texel (i,j) of an N x N map represents.
public float3 DdgiInteriorTexelDirection(int2 interior, uint n) {
    return OctDecode(DdgiInteriorTexelToOct(interior, n));
}

// -----------------------------------------------------------------------------
// BORDER MIRRORING, DERIVED RATHER THAN COPIED.
//
// Given a texel of the FULL (N+2)^2 tile, this returns which INTERIOR texel
// carries the same direction. For an interior texel that is itself; for a border
// texel it is the mirror across the seam.
//
// THE RULES, and where they come from. OctDecode identifies (u, 1) with (-u, 1)
// on the top edge, so the border row above the tile mirrors the first interior
// row in x. Same for each edge with the roles swapped. All four corners of the
// square decode to -Z, so a corner border texel takes the DIAGONALLY OPPOSITE
// interior corner, which is the nearest sample of the same neighbourhood.
//
// WHY THIS IS A FUNCTION AND NOT A SECOND COPY PASS. The obvious implementation
// -- write the interior, barrier, copy the border -- needs a device-wide barrier
// because a probe's interior and its border are in the same tile but the copy
// would run in a different workgroup after the whole grid is written. That is a
// second dispatch and a second read of both atlases.
//
// It is unnecessary, because the blend at a texel is a PURE FUNCTION of (its
// direction, this frame's rays, its own previous value). A border texel and its
// mirrored interior texel have identical directions and identical ray input, and
// both start at zero when the C++ clears the atlas, so by induction they hold
// identical values every frame -- bit-identical, since they execute the same
// instructions on the same inputs on the same device. So each border texel can
// simply compute the answer for itself, in the same dispatch, with no ordering
// between them at all.
//
// THAT INDUCTION IS WHY THE ZERO-CLEAR IS LISTED AS A C++ REQUIREMENT ABOVE. If
// the border texels start at a different value from their interior twins,
// hysteresis keeps the two apart forever and the seam never closes.
// -----------------------------------------------------------------------------
public int2 DdgiTileTexelToInterior(int2 t, uint nu) {
    const int n = int(nu);
    const bool left   = (t.x == 0);
    const bool right  = (t.x == n + 1);
    const bool top    = (t.y == 0);
    const bool bottom = (t.y == n + 1);

    if ((left || right) && (top || bottom)) {
        // Corner: the diagonally opposite interior corner.
        return int2(left ? n - 1 : 0, top ? n - 1 : 0);
    }
    if (top)    return int2(n - t.x, 0);
    if (bottom) return int2(n - t.x, n - 1);
    if (left)   return int2(0, n - t.y);
    if (right)  return int2(n - 1, n - t.y);
    return t - 1;   // interior
}

// =============================================================================
// PROBE GRID INDEXING
// =============================================================================

public static const int3 X3_DDGI_PROBE_COUNTS =
    int3(int(X3_DDGI_PROBE_COUNT_X), int(X3_DDGI_PROBE_COUNT_Y), int(X3_DDGI_PROBE_COUNT_Z));

/// Positive modulo. HLSL's % follows C and returns a NEGATIVE result for a
/// negative left operand, which would index a tile off the left edge of the
/// atlas the first time the volume scrolls backwards.
int3 PositiveMod(int3 v, int3 n) { return ((v % n) + n) % n; }

/// World position of the LOGICAL probe at grid coordinate `coord`.
///
/// Logical, not storage. gridOrigin moves with the volume; the storage slot a
/// probe occupies does not (see DdgiStorageCoord).
public float3 DdgiProbeWorldPosition(int3 coord) {
    return u_Ddgi.gridOrigin.xyz + u_Ddgi.gridSpacing.xyz * float3(coord);
}

/// Where a logical probe's data LIVES.
///
/// A camera-following volume must scroll. If it scrolled by re-placing
/// gridOrigin and leaving the storage alone, every probe's cached irradiance
/// would suddenly belong to a different world position -- the whole GI solution
/// would slide across the world as the camera walked, smearing for as many
/// frames as the hysteresis takes to forget, which at 0.97 is dozens.
///
/// Scrolling in WHOLE probe steps and wrapping the storage index means the
/// probes that did not leave the volume keep their data exactly, and only the
/// one new layer that entered is wrong -- and the C++ clears that layer's tiles.
///
/// scroll = 0 is a static volume, which is the correct value until the C++
/// actually implements scrolling. Nothing here breaks with it pinned at zero.
public int3 DdgiStorageCoord(int3 logical) {
    return PositiveMod(logical + u_Ddgi.scroll.xyz, X3_DDGI_PROBE_COUNTS);
}

/// Storage coordinate of linear storage slot `index`. Exact inverse of
/// DdgiStorageIndex, and the ONE place the dispatch's flat probe index is
/// unpacked -- both passes call it rather than each writing out the divides.
public int3 DdgiStorageCoordFromIndex(uint index) {
    return int3(int(index % X3_DDGI_PROBE_COUNT_X),
                int((index / X3_DDGI_PROBE_COUNT_X) % X3_DDGI_PROBE_COUNT_Y),
                int(index / (X3_DDGI_PROBE_COUNT_X * X3_DDGI_PROBE_COUNT_Y)));
}

/// Which LOGICAL probe occupies storage slot `index`.
///
/// The trace pass dispatches over STORAGE slots (one workgroup per slot, so the
/// atlas writes are contiguous) and needs the logical coordinate to know where
/// in the world to put the ray origin.
public int3 DdgiLogicalCoordFromStorageIndex(uint index) {
    return PositiveMod(DdgiStorageCoordFromIndex(index) - u_Ddgi.scroll.xyz,
                       X3_DDGI_PROBE_COUNTS);
}

/// Linear storage slot of a STORAGE coordinate. This is the ray buffer's probe
/// stride index and the order the trace pass dispatches in.
public uint DdgiStorageIndex(int3 storage) {
    return uint(storage.x)
         + X3_DDGI_PROBE_COUNT_X * (uint(storage.y)
         + X3_DDGI_PROBE_COUNT_Y *  uint(storage.z));
}

/// Top-left texel of a storage coordinate's tile in an atlas of `tile`-sized
/// tiles. X and Z fold into columns; Y is the row.
public int2 DdgiTileOrigin(int3 storage, uint tile) {
    const int col = storage.x + int(X3_DDGI_PROBE_COUNT_X) * storage.z;
    const int row = storage.y;
    return int2(col, row) * int(tile);
}

// =============================================================================
// RAY DIRECTIONS -- THE ONE DEFINITION BOTH PASSES USE
// =============================================================================

/// Spherical Fibonacci: `n` points spread as evenly over the sphere as a closed
/// form can manage, for ANY n. That "any n" is what makes the ray count a
/// runtime dial at all -- a fixed table would only be uniform at one count.
///
/// cosTheta is stepped linearly, not theta, because area on a sphere is uniform
/// in cos(theta). Stepping theta would crowd the poles, and a probe would
/// integrate the ceiling and floor at several times their true solid angle.
public float3 DdgiSphericalFibonacci(uint i, uint n) {
    const float PHI_FRAC = 0.6180339887498949;    // (sqrt(5)*0.5 + 0.5) - 1
    const float phi      = 2.0 * PI * frac(float(i) * PHI_FRAC);
    const float cosTheta = 1.0 - (2.0 * float(i) + 1.0) / float(n);
    const float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

/// Rotate v by unit quaternion q. The two-cross-product form -- no matrix built,
/// no trig.
public float3 DdgiQuatRotate(float4 q, float3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

// -----------------------------------------------------------------------------
/// Direction of ray `rayIndex` of EVERY probe, this frame.
///
/// THE PER-FRAME ROTATION IS THE LOAD-BEARING PART, and it is worth being
/// precise about why, because it looks like a cheap dither and is not.
///
/// Without it, all 256 probes trace the same fixed 64 directions every frame
/// forever. 64 directions is a very coarse sampling of a sphere -- one sample
/// per 0.2 steradians -- so any feature smaller than that is missed. Crucially
/// it is missed THE SAME WAY EVERY FRAME. That makes the error a STATIONARY
/// BIAS, not noise, and temporal accumulation is powerless against it: averaging
/// a constant gives the constant back. The visible result is banding -- the 64
/// lobes of the sampling pattern baked permanently into every probe's
/// irradiance, showing as blotchy facets on large smooth surfaces that do not
/// improve no matter how long the camera stands still.
///
/// Rotating the whole set by a fresh random rotation each frame does not make
/// any single frame better. It makes the error DIFFERENT each frame -- it turns
/// the bias into noise. And noise is exactly what the blend pass's hysteresis
/// removes: averaging over N frames of independent rotations converges on the
/// true integral at the usual 1/sqrt(N). Sixty-four rays a frame becomes an
/// effective few thousand within a second of standing still.
///
/// "A stationary error survives averaging; a moving one does not" is the whole
/// argument, and it is also why the DEPTH map -- 196 texels fed by 64 rays --
/// is viable at all.
///
/// THE ROTATION COMES FROM THE UBO, NOT FROM A HASH OF THE FRAME INDEX. The
/// trace pass and the blend pass must produce IDENTICAL directions for the same
/// index; deriving it in two places from a frame counter that differs by one
/// between the passes would pair each ray's radiance with a slightly wrong
/// cosine weight. Nothing would look broken -- the probes would just converge
/// to a smoothed, incorrect answer. One CPU-supplied quaternion, read by both,
/// makes that unrepresentable. It also keeps the pass deterministic for the
/// golden-image tests, which a per-dispatch hash would not.
// -----------------------------------------------------------------------------
public float3 DdgiRayDirection(uint rayIndex) {
    return DdgiQuatRotate(u_Ddgi.rayRotation,
                          DdgiSphericalFibonacci(rayIndex, DdgiRayCount()));
}

// =============================================================================
// RAY BUFFER
//
// Layout: DdgiRayDataBuffer[storageProbeIndex * X3_DDGI_RAYS_PER_PROBE + rayIndex].
// Stride is the COMPILE-TIME constant, not DdgiRayCount(), so lowering the
// runtime ray count leaves holes rather than re-packing -- which would make the
// buffer's meaning depend on a UBO field and break the moment the dial moved
// between the two dispatches.
//
// 256 probes * 64 rays * 16 B = 256 KB.
// =============================================================================

public uint DdgiRaySlot(uint storageProbeIndex, uint rayIndex) {
    return storageProbeIndex * X3_DDGI_RAYS_PER_PROBE + rayIndex;
}

// -----------------------------------------------------------------------------
/// DISTANCE ENCODING, and the one trick in it.
///
/// A NEGATIVE stored distance means the ray hit a BACKFACE -- it started inside
/// geometry, or the surface is one-sided and the probe is behind it. The value
/// stored is -0.2 * t: negative to flag it, and SHORTENED because the honest
/// thing for the visibility map to report is "there is a surface extremely close
/// to me in this direction", which is what makes the Chebyshev test reject this
/// probe for essentially every shading point.
///
/// THIS IS THE PRIMARY DEFENCE AGAINST A PROBE INSIDE A WALL, which is the other
/// half of the leak problem (the first half is a probe on the wrong side of a
/// wall, handled by Chebyshev directly). A probe buried in geometry sees only
/// backfaces, its depth map fills with near-zero distances, and every lookup
/// weights it to zero. Without the shortening it would report the distance to
/// the far side of the wall, look unoccluded, and pour black -- or worse,
/// outdoor -- irradiance into the room.
///
/// It is a MITIGATION, NOT A CURE, exactly as ENGINE_PLAN.md says. A probe fully
/// inside geometry still burns 64 rays a frame producing nothing. The complete
/// fix is a per-probe "inactive" flag written by the blend pass and read by the
/// trace pass to skip it -- another resource and another dependency between the
/// two passes, deliberately not built here.
///
/// A MISS stores +maxRayDistance, NOT infinity. INF_T is 1e30; feeding it into
/// the mean-of-squares would produce 1e60, which overflows fp32 to +inf, and one
/// missed ray would poison a probe's entire depth map with NaN the moment the
/// variance subtraction ran. Clamping to a finite "far away" distance is both
/// numerically safe and physically what the map should say.
// -----------------------------------------------------------------------------
public float DdgiEncodeRayDistance(float t, bool hit, bool frontFacing) {
    if (!hit)          return DdgiMaxRayDistance();
    if (!frontFacing)  return -0.2 * t;
    return min(t, DdgiMaxRayDistance());
}

/// The distance the DEPTH map should record for an encoded value: always
/// positive, backface hits already shortened by the encode above.
public float DdgiDecodeDepthDistance(float encoded) {
    return min(abs(encoded), DdgiMaxRayDistance());
}

/// True when this ray carries no light (it hit the inside of something).
public bool DdgiRayIsBackface(float encoded) { return encoded < 0.0; }

// =============================================================================
// ATLAS ACCESS
//
// FORMAT IS ISOLATED TO THESE FOUR FUNCTIONS. Widening the depth atlas to
// rgba32f, or adding a perceptual encoding to irradiance, is a change here and
// nowhere else.
//
// NO PERCEPTUAL (gamma) ENCODING ON IRRADIANCE, deliberately, though the
// reference implementation has one. It exists there to fight quantisation in a
// 10-bit unorm format; this atlas is fp16 and has no such problem. The cost of
// adding it would be a pow() in the blend and a matching pow() in the sample,
// and the failure mode of getting the pair out of step -- one exponent changed,
// the other not -- is GI that is uniformly and subtly wrong in contrast
// everywhere at once, which is close to undetectable by eye and does not look
// like a bug in the pass that caused it.
// =============================================================================

public float3 DdgiLoadIrradiance(int2 texel)            { return u_DdgiIrradiance[texel].rgb; }

/// PROBE VALIDITY RIDES IN THE ALPHA LANE.
///
/// A probe inside solid geometry must not contribute, and Chebyshev alone does
/// not stop it: the visibility test asks "is there an occluder between the probe
/// and this point", which is a different question from "is this probe inside a
/// wall". A buried probe whose depth map says "surface right next to me in every
/// direction" is rejected for points it can see past -- but its irradiance is
/// still interpolated wherever the geometry happens not to occlude.
///
/// The alpha channel was already there and always written as 1, so this costs
/// nothing: no new binding, no second atlas, and the value is fetched by the
/// same load the irradiance uses.
public void   DdgiStoreIrradiance(int2 texel, float3 v, float validity = 1.0) {
    u_DdgiIrradiance[texel] = float4(v, validity);
}
public float  DdgiLoadValidity(int2 texel)              { return u_DdgiIrradiance[texel].a; }
public float2 DdgiLoadDepth(int2 texel)                 { return u_DdgiDepth[texel]; }
public void   DdgiStoreDepth(int2 texel, float2 v)      { u_DdgiDepth[texel] = v; }

/// Hand-written bilinear over a probe's irradiance tile.
///
/// Position derivation: interior texel i sits at texel-space centre
/// tileOrigin + 1 + i + 0.5, and uv = (i+0.5)/N, so the sample position is
/// tileOrigin + 1 + uv*N. The `+1` skips the border row/column; the border is
/// then reachable by the filter's lower and upper taps, which is exactly what
/// it is there for.
float3 DdgiBilinearIrradiance(int3 storage, float3 dir) {
    const int2 origin = DdgiTileOrigin(storage, X3_DDGI_IRRADIANCE_TILE);
    const float2 uv   = OctEncode(dir) * 0.5 + 0.5;
    const float2 p    = float2(origin) + 1.0 + uv * float(X3_DDGI_IRRADIANCE_TEXELS) - 0.5;

    const int2  base = int2(floor(p));
    const float2 f   = p - float2(base);

    const float3 c00 = DdgiLoadIrradiance(base + int2(0, 0));
    const float3 c10 = DdgiLoadIrradiance(base + int2(1, 0));
    const float3 c01 = DdgiLoadIrradiance(base + int2(0, 1));
    const float3 c11 = DdgiLoadIrradiance(base + int2(1, 1));

    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
}

/// The same filter over a probe's depth tile. Returns (mean, meanSquared).
float2 DdgiBilinearDepth(int3 storage, float3 dir) {
    const int2 origin = DdgiTileOrigin(storage, X3_DDGI_DEPTH_TILE);
    const float2 uv   = OctEncode(dir) * 0.5 + 0.5;
    const float2 p    = float2(origin) + 1.0 + uv * float(X3_DDGI_DEPTH_TEXELS) - 0.5;

    const int2  base = int2(floor(p));
    const float2 f   = p - float2(base);

    const float2 c00 = DdgiLoadDepth(base + int2(0, 0));
    const float2 c10 = DdgiLoadDepth(base + int2(1, 0));
    const float2 c01 = DdgiLoadDepth(base + int2(0, 1));
    const float2 c11 = DdgiLoadDepth(base + int2(1, 1));

    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
}

// =============================================================================
// SAMPLING -- THE FUNCTION A SHADING PASS CALLS
//
// WHAT A LEAK IS, since preventing it is the entire design of what follows.
//
// Probes sit on a fixed grid that knows nothing about the geometry. Put an
// 8-probe cell astride a wall, with a bright sunlit exterior on one side and a
// dark interior on the other, and four of the eight probes see sunlight. A
// shading point on the dark side of the wall is inside that cell, so plain
// trilinear filtering hands it a blend that is roughly half sunlight. The
// result is a soft glow bleeding through solid geometry -- brightest near the
// wall, fading inward over one probe spacing, and looking uncannily like
// ambient light from nowhere. It is the classic and defining DDGI failure, it
// gets worse as probe spacing grows, and it is worst exactly where it is most
// obvious: thin walls in dark rooms.
//
// THE MITIGATION IS TO ASK EACH PROBE WHETHER IT CAN SEE THE SHADING POINT, and
// to answer that question with the probe's own depth map. That is the Chebyshev
// test below. ENGINE_PLAN.md is explicit that it is a mitigation and not a cure
// -- a wall thinner than the depth map's angular resolution at that distance
// still leaks, and the remedies for that are more probes or a manual volume.
//
// THREE WEIGHTS, multiplied:
//   1. trilinear   -- the plain interpolation weight, so the result is smooth
//                     as the shading point moves through the cell.
//   2. normal-facing -- a probe BEHIND the surface being shaded contributes
//                     light the surface cannot receive.
//   3. Chebyshev   -- a probe on the far side of an occluder is rejected.
// =============================================================================

/// 1 inside the probe volume, ramping to 0 over volumeFadeDistance outside it.
public float DdgiVolumeFade(float3 p) {
    const float3 lo = u_Ddgi.gridOrigin.xyz;
    const float3 hi = lo + u_Ddgi.gridSpacing.xyz * float3(X3_DDGI_PROBE_COUNTS - 1);
    const float3 d  = max(max(lo - p, float3(0.0)), p - hi);   // zero inside the box
    return saturate(1.0 - length(d) / DdgiVolumeFadeDistance());
}

/// Value returned is IRRADIANCE DIVIDED BY PI -- that is, the number a
/// Lambertian surface multiplies its albedo by directly.
///
/// STATE THIS ONCE AND OBEY IT EVERYWHERE. The blend pass stores the
/// cosine-weighted MEAN RADIANCE over the sphere, and the identity
/// E = pi * meanRadiance means storing the mean and multiplying by albedo is the
/// same as storing E and multiplying by albedo/pi. Choosing the former removes a
/// pi and a 1/pi that would otherwise have to cancel across two shader files.
/// The failure a lost factor of pi produces is GI that is 3.14x too bright or
/// too dim -- large enough to be obviously wrong, small enough that it gets
/// "fixed" by turning down an intensity slider and then hides forever.
/// Irradiance AND a confidence in [0,1], which callers must honour.
///
/// THE ZERO RETURN IS NOT AN ANSWER, IT IS THE ABSENCE OF ONE. This function
/// returns zero in three legitimate cases: DDGI disabled, every probe rejected
/// (inside geometry, tight concave corners), and outside the probe volume. A
/// caller that treats those zeros as "no indirect light arrives here" renders
/// them BLACK -- and since the volume is fitted to the scene's entities, a large
/// ground plane extends well past it, so most of that plane goes black while a
/// patch in the middle stays lit. That was a real bug, reported from a scene
/// with the sun turned to zero, where ambient was all there was.
///
/// `confidence` is what distinguishes "no light" from "no opinion". Blend the
/// caller's own fallback -- the skybox ambient -- in by (1 - confidence).
public float3 DdgiSampleIrradianceConfident(float3 worldPos, float3 normal,
                                            float3 viewDir, out float confidence) {
    confidence = 0.0;
    if (!DdgiEnabled()) return float3(0.0);

    // SURFACE BIAS. The shading point sits ON a surface, so the probe's depth
    // map reports an occluder at essentially zero distance in that direction and
    // the Chebyshev test rejects every probe -- the surface renders with no GI
    // at all, which is a black-crease artifact along every wall. Pushing the
    // sample point off the surface, along a mix of the normal and the view
    // direction, is what gets it out of its own shadow.
    //
    // BOTH TERMS ARE NEEDED. Normal-only fails at grazing angles, where the
    // offset barely moves the point away from the camera's line of sight and
    // the surface still occludes itself. View-only fails on surfaces facing the
    // camera. The reference implementation uses a 0.2/0.8 split of a single
    // scalar; two separate scalars here so the two can be tuned apart.
    const float3 p = worldPos + normal * DdgiNormalBias() + viewDir * DdgiViewBias();

    const float3 spacing   = u_Ddgi.gridSpacing.xyz;
    const float3 gridCoord = (p - u_Ddgi.gridOrigin.xyz) / spacing;
    const int3   baseCoord = int3(floor(gridCoord));
    const float3 alpha     = saturate(gridCoord - float3(baseCoord));

    float3 sumIrradiance = float3(0.0);
    float  sumWeight     = 0.0;

    for (uint i = 0; i < 8; i++) {
        const int3 offset  = int3(int(i & 1u), int((i >> 1u) & 1u), int((i >> 2u) & 1u));
        // CLAMPED, not rejected. A point at the very edge of the volume computes
        // a base coord one short of the last probe and asks for the one past it.
        // Rejecting would black out a one-cell border around the entire volume;
        // clamping reuses the edge probe, which is what the volume fade below is
        // for. Same reasoning as ClusterIndexFromView's saturating clamp.
        const int3 logical = clamp(baseCoord + offset, int3(0), X3_DDGI_PROBE_COUNTS - 1);
        const int3 storage = DdgiStorageCoord(logical);

        const float3 probePos    = DdgiProbeWorldPosition(logical);
        const float3 pointToProbe = probePos - p;
        const float  distToProbe = length(pointToProbe);
        const float3 dirToProbe  = pointToProbe / max(distToProbe, 1e-6);

        // ---- weight 2: normal-facing -------------------------------------
        // "Wrap shading": (dot+1)/2 rather than max(dot,0), so a probe exactly
        // in the surface plane still contributes a little. A hard cosine cutoff
        // makes the weight discontinuous as the shading point rotates past a
        // probe, which shows up as a hard line across curved geometry. The
        // squaring sharpens the falloff; the +0.2 floor keeps a probe that is
        // nearly behind the surface from being dropped entirely, because with
        // only eight probes dropping one is a visible discontinuity.
        const float wrap = (dot(dirToProbe, normal) + 1.0) * 0.5;
        float weight = wrap * wrap + 0.2;

        // ---- weight 3: Chebyshev visibility -------------------------------
        // The probe's depth map, sampled in the direction FROM THE PROBE TOWARDS
        // THE POINT (hence -dirToProbe), gives the mean and mean-square distance
        // to the first surface that way. Those two moments are a variance:
        //
        //     mean  = E[r]
        //     msq   = E[r^2]
        //     var   = |E[r^2] - E[r]^2|
        //
        // Chebyshev's inequality bounds the probability that a random variable
        // with that mean and variance exceeds a given value:
        //
        //     P(r >= d) <= var / (var + (d - mean)^2)     for d > mean
        //
        // Read as visibility: if the shading point is FARTHER from the probe
        // than the mean occluder distance in its direction, that bound is the
        // most this probe can claim to see it. Where the depth map is
        // consistent (var ~ 0, a flat wall) the bound collapses to zero and the
        // probe is rejected outright; where it is inconsistent (var large, a
        // texel straddling an edge) the bound stays near one and the probe is
        // kept, which is the correct conservative behaviour at a silhouette.
        //
        // abs() on the variance because catastrophic cancellation can make
        // E[r^2] - E[r]^2 very slightly negative; a negative variance makes the
        // ratio negative and a negative weight subtracts light, which shows as
        // black speckles.
        //
        // ONLY WHEN d > mean. If the point is closer than the mean occluder
        // there is nothing between it and the probe and the weight stays 1.
        //
        // CUBED. The raw bound is a soft, wide ramp -- it leaves partial weight
        // on a probe well behind a wall, and partial weight is still a leak.
        // Cubing pushes everything below ~0.7 towards zero while leaving a fully
        // visible probe at 1, which sharpens the wall's shadow onto the wall
        // instead of spreading it a probe-spacing deep into the room.
        const float2 moments = DdgiBilinearDepth(storage, -dirToProbe);
        const float  mean    = moments.x;
        if (distToProbe > mean) {
            const float delta    = distToProbe - mean;
            const float variance = abs(moments.y - mean * mean);
            float cheb = variance / (variance + delta * delta);
            cheb = max(cheb * cheb * cheb, 0.0);
            weight *= cheb;
        }

        // A tiny weight is worse than no weight: it keeps a probe alive in the
        // normalisation without contributing anything meaningful, which makes
        // the result jitter as the point moves. Crush the small tail smoothly
        // rather than with a hard threshold, which would band.
        //
        // BEFORE THE TRILINEAR MULTIPLY, not after, and that ordering matters. A
        // corner probe of the cell legitimately has a trilinear weight near zero
        // when the point is at the opposite corner; crushing after would treat
        // that geometric fact as though the probe were occluded and would
        // destroy the interpolation's smoothness at every cell boundary.
        const float kCrush = 0.2;
        if (weight < kCrush) weight *= (weight * weight) / (kCrush * kCrush);

        // ---- weight 1: trilinear -----------------------------------------
        const float3 tri = lerp(1.0 - alpha, alpha, float3(offset));
        weight *= tri.x * tri.y * tri.z;

        if (weight <= 0.0) continue;

        sumIrradiance += DdgiBilinearIrradiance(storage, normal) * weight;
        sumWeight     += weight;
    }

    // Every probe rejected -- the point is sealed off from all eight, which
    // happens inside geometry and in tight concave corners. Returning zero is
    // right: there is genuinely no probe qualified to answer, and inventing a
    // fallback here is how a leak gets re-introduced through the back door.
    if (sumWeight <= 1e-6) return float3(0.0);   // confidence stays 0

    // FADE OUT AT THE VOLUME BOUNDARY. Without it, geometry crossing the edge of
    // the probe volume switches from full GI to none in one pixel -- a hard
    // straight line across the floor that moves with the volume and reads as a
    // clipping bug.
    // The fade IS the confidence outside the volume, and the probes having
    // answered at all is the confidence inside it.
    confidence = DdgiVolumeFade(p);
    return (sumIrradiance / sumWeight) * confidence;
}

/// Backwards-compatible form. Prefer the confident one: this cannot tell a
/// caller whether its zero means "no light" or "no opinion".
public float3 DdgiSampleIrradiance(float3 worldPos, float3 normal, float3 viewDir) {
    float ignored;
    return DdgiSampleIrradianceConfident(worldPos, normal, viewDir, ignored);
}
