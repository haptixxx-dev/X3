// =============================================================================
// ToneMapping -- one curve, every renderer.
//
// A MODULE RATHER THAN A COPY IN EACH SHADER, for exactly the reason Bsdf.slang
// is one: PBR.slang, Phong.slang and the raster Tonemap pass all have to land in
// the same colour space or every raster-vs-reference comparison in the test
// suite stops meaning anything. Reinhard used to be written out three times and
// they happened to agree; a change to one of them would not have been caught by
// anything.
//
// AgX, replacing Reinhard. ENGINE_PLAN.md Phase 11 calls it "a few dozen lines,
// enormous gain over naive clamping" and that is accurate. Reinhard's failure is
// specific and visible: it desaturates nothing and compresses everything, so a
// bright saturated light source washes toward white while never actually
// reaching it, and highlights lose all hue separation. AgX instead applies its
// sigmoid in a log-encoded working space and deliberately shifts hue toward the
// achromatic axis as values clip, which is what film does and what makes a bright
// blue sky read as bright blue rather than as pale cyan.
//
// THIS IS THE APPROXIMATION, NOT OpenColorIO. The plan names OCIO; pulling it in
// brings a large dependency tree and a config-file runtime for what is, at this
// stage, a fixed transform with no per-project grading. The matrices and the
// polynomial below are the published AgX approximation and produce the same
// image to well within a quantisation step. When per-project grading or a real
// view transform is wanted, OCIO replaces the body of AgXTonemap and nothing
// else -- which is the whole reason this is one function in one module.
// =============================================================================

module ToneMapping;

// sRGB -> AgX working space. AgX does its compression in a space whose primaries
// are deliberately wider than sRGB's, which is what stops the sigmoid from
// clipping a saturated channel long before the other two.
static const float3x3 kAgXInset = float3x3(
    0.8424790, 0.0784336, 0.0792237,
    0.0423282, 0.8784686, 0.0791661,
    0.0423756, 0.0784336, 0.8791430);

// And back. NOT the exact inverse of the inset: the outset is deliberately
// slightly different, and that asymmetry is what produces AgX's characteristic
// desaturation of very bright values instead of a hue shift.
static const float3x3 kAgXOutset = float3x3(
     1.1968790, -0.0980209, -0.0990297,
    -0.0528969,  1.1519031, -0.0989612,
    -0.0529716, -0.0980435,  1.1510737);

/// The sigmoid, as a degree-6 polynomial fit on log-encoded values.
///
/// A polynomial rather than an exp/pow chain because it is evaluated per pixel
/// per channel and the fit is accurate to well under a quantisation step across
/// the whole domain. The domain is deliberately clamped first: the fit diverges
/// outside [0,1] and an unclamped input produces colours that are not merely
/// wrong but negative, which then propagate as black pixels through anything
/// downstream.
float3 AgXSigmoid(float3 x) {
    const float3 x2 = x * x;
    const float3 x4 = x2 * x2;
    return 15.5 * x4 * x2
         - 40.14 * x4 * x
         + 31.96 * x4
         - 6.868 * x2 * x
         + 0.4298 * x2
         + 0.1191 * x
         - 0.00232;
}

/// SCENE EXPOSURE, and it is not optional when moving off Reinhard.
///
/// Reinhard maps radiance 1.0 to 0.5 -- mid-grey -- so every light intensity,
/// material albedo and skybox in this project was authored against that
/// response. AgX is scene-referred and expects 0.18 to be mid-grey, which puts
/// radiance 1.0 about two and a half stops HIGH. Dropping AgX in without this
/// scale blows the whole image to white and reads as "AgX is broken" rather than
/// "the exposure is wrong".
///
/// 0.18 is therefore not a taste value: it is the factor that maps this engine's
/// existing 1.0-is-normal convention onto AgX's 0.18-is-mid-grey one. A real
/// exposure control -- auto-exposure, or a per-camera EV -- replaces it, and
/// that is Phase 11 work that has not happened.
public static const float kSceneExposure = 0.18;

/// Linear radiance -> display-referred sRGB.
///
/// The log2 range is the exposure window: -12.47 to +4.026 stops, which is the
/// published AgX default and covers roughly 16 stops. Values outside it clamp,
/// so a light bright enough to exceed +4 stops saturates rather than wrapping.
public float3 AgXTonemap(float3 color) {
    const float kMinEv = -12.47393;
    const float kMaxEv =   4.026069;

    color = max(color, float3(0.0)) * kSceneExposure;
    color = mul(kAgXInset, color);

    // Log2 encode into [0,1] across the exposure window. The 1e-10 floor is what
    // stops log2(0) producing -inf, which the polynomial turns into NaN and which
    // then survives all the way to the swapchain as a black or garbage pixel.
    color = clamp(log2(max(color, float3(1e-10))), kMinEv, kMaxEv);
    color = (color - kMinEv) / (kMaxEv - kMinEv);

    color = AgXSigmoid(color);
    color = mul(kAgXOutset, color);

    // The sigmoid's output is already display-referred, so this is the sRGB
    // transfer function rather than a second tonemap. 2.2 rather than the exact
    // piecewise sRGB curve, matching what the rest of the engine assumes.
    return pow(max(color, float3(0.0)), float3(1.0 / 2.2));
}
