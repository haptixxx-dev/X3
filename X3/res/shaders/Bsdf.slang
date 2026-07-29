// =============================================================================
// Bsdf -- the shading library, written ONCE and imported by every renderer.
//
// WHY THIS IS ONE MODULE. The path tracer and the Forward+ shaders of Phase 7
// must agree about what a material looks like, or the reference renderer stops
// being a reference. Sharing prose and hoping is how they diverge; sharing a
// compiled module is how they cannot. This is the payoff for doing Slang before
// the material system rather than after -- in GLSL this would have been a fourth
// copy-paste.
//
// WHAT THIS IS NOT. It is not OpenPBR. OpenPBR is a layered slab model with
// 40-50 channels and no engine ships it as a runtime evaluator; per locked
// decision 3 it is an AUTHORING interchange format that Phase 12 bakes down into
// the reduced parameter set below. What is here is a metallic-roughness base
// with optional clearcoat, sheen and anisotropy -- the reduction, not the spec.
//
// CONVENTIONS, stated once because getting these wrong is silent:
//   * All directions are in the LOCAL SHADING FRAME: +Z is the shading normal,
//     and both `wo` (towards the viewer) and `wi` (towards the light) point AWAY
//     from the surface. Callers convert; nothing in here touches world space.
//   * eval() returns the BSDF value f(wo,wi) WITHOUT the cosine term. The caller
//     multiplies by |wi.z|. Folding it in is the more convenient convention right
//     up until a sampled estimator divides by pdf and double-counts it.
//   * pdf is solid-angle measure, matching the sampling routines.
// =============================================================================

module Bsdf;

import GpuTypes;
import Bindings;
__exported import GpuTypes;

static const float BSDF_PI      = 3.14159265358979;
static const float BSDF_INV_PI  = 0.31830988618379;
static const float BSDF_EPS     = 1e-6;

// Roughness below this is treated as a perfect mirror: GGX's denominator goes to
// zero and the sampled direction collapses, so specular lobes are handled as a
// discrete case rather than integrated.
static const float MIN_ROUGHNESS = 1e-3;

// -----------------------------------------------------------------------------
// THE INTERFACE. Phase 7's fragment shader and the path tracer's hit shader are
// both written generically over this, so a lobe added here reaches both without
// either being edited.
//
// Slang specializes conformances at LINK time, so a shader instantiated with
// DiffuseBsdf compiles to a tight diffuse-only shader with every clearcoat and
// sheen branch eliminated -- which is what makes the tiering below cost nothing
// for materials that do not use it.
// -----------------------------------------------------------------------------
public interface IBsdf
{
    /// f(wo, wi), no cosine term. Zero when the pair is not in the same
    /// hemisphere as the lobe supports.
    float3 eval(float3 wo, float3 wi);

    /// Probability density of sample() having produced `wi`, solid angle
    /// measure. Needed separately from sample() for multiple importance
    /// sampling, which Phase 10's lightmap baker will want.
    float pdf(float3 wo, float3 wi);

    /// Importance-sample an incoming direction. Returns false when the sample is
    /// degenerate (below the horizon, zero weight); callers must terminate that
    /// path rather than treating it as black, because a zero-weight sample and a
    /// shadowed sample mean different things to an estimator.
    ///
    /// `weight` is f * cos / pdf -- the throughput multiplier, already divided,
    /// so the caller never sees the pdf of a discrete lobe.
    bool sample(float3 wo, float2 u, float uLobe, out float3 wi, out float3 weight);

    /// Rough hemispherical reflectance, for Russian roulette and for debug
    /// visualisation. Not required to be exact.
    float3 albedo();
}

// =============================================================================
// Microfacet building blocks. Trowbridge-Reitz (GGX) throughout.
// =============================================================================

/// GGX normal distribution, isotropic.
public float D_GGX(float NdotH, float alpha) {
    const float a2 = alpha * alpha;
    const float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(BSDF_PI * d * d, BSDF_EPS);
}

/// GGX normal distribution, anisotropic. `alphaT`/`alphaB` are the roughnesses
/// along the tangent and bitangent axes of the shading frame.
public float D_GGX_Aniso(float3 h, float alphaT, float alphaB) {
    const float t = h.x / max(alphaT, BSDF_EPS);
    const float b = h.y / max(alphaB, BSDF_EPS);
    const float d = t * t + b * b + h.z * h.z;
    return 1.0 / max(BSDF_PI * alphaT * alphaB * d * d, BSDF_EPS);
}

/// Smith height-correlated visibility term, ALREADY DIVIDED by the
/// 4*NdotV*NdotL of the microfacet denominator. Returning V rather than G is
/// what keeps the specular expression free of a division that cancels.
public float V_SmithGGXCorrelated(float NdotV, float NdotL, float alpha) {
    const float a2 = alpha * alpha;
    const float lv = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    const float ll = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(lv + ll, BSDF_EPS);
}

public float3 F_Schlick(float3 f0, float3 f90, float VdotH) {
    return f0 + (f90 - f0) * pow(saturate(1.0 - VdotH), 5.0);
}

public float F_SchlickScalar(float f0, float f90, float VdotH) {
    return f0 + (f90 - f0) * pow(saturate(1.0 - VdotH), 5.0);
}

/// Samples a GGX visible-normal (VNDF) half vector. Heitz 2018,
/// "Sampling the GGX Distribution of Visible Normals" -- strictly better than
/// sampling D directly, because it never generates a microfacet the viewer
/// cannot see and therefore never produces a zero-weight sample to throw away.
public float3 SampleGGXVNDF(float3 wo, float alphaT, float alphaB, float2 u) {
    // Stretch the view direction into the hemisphere configuration.
    float3 vh = normalize(float3(alphaT * wo.x, alphaB * wo.y, wo.z));

    // Orthonormal basis around vh, degenerate-safe at vh.z ~ 1.
    const float lensq = vh.x * vh.x + vh.y * vh.y;
    const float3 t1 = lensq > 0.0 ? float3(-vh.y, vh.x, 0.0) * rsqrt(lensq)
                                  : float3(1.0, 0.0, 0.0);
    const float3 t2 = cross(vh, t1);

    // Uniform point on the projected disk, warped for the hemisphere.
    const float r = sqrt(u.x);
    const float phi = 2.0 * BSDF_PI * u.y;
    const float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    const float s = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - s) * sqrt(1.0 - p1 * p1) + s * p2;

    float3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;

    // Unstretch.
    return normalize(float3(alphaT * nh.x, alphaB * nh.y, max(0.0, nh.z)));
}

public float3 SampleCosineHemisphere(float2 u) {
    const float r = sqrt(u.x);
    const float phi = 2.0 * BSDF_PI * u.y;
    return float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u.x)));
}

// -----------------------------------------------------------------------------
// SPECULAR DIRECTIONAL ALBEDO -- baked, not fitted.
//
// This was an analytic approximation (Karis' EnvBRDFApprox) and it was the
// weakest thing in this file. The furnace test measured two separate failures
// caused by it, both of which are now gone:
//
//   * Deriving multi-scatter compensation from the fit created 113% energy
//     (albedo 2.13). Compounding one approximation with another.
//   * Using it for kD left an 8.8% gain at grazing incidence -- a bias, not
//     noise: raising the sample count eightfold moved it by 0.003.
//
// BsdfLutBake.slang now integrates the true directional albedo using the SAME
// SampleGGXVNDF and V_SmithGGXCorrelated this file shades with, so the table is
// exact for this lobe by construction. Change the lobe and the table follows;
// there is no fit left to re-tune.
//
// MEASURED after the LUT landed (bsdf-furnace, white furnace should return 1.0):
//     metal      mean 0.9991   min 0.936   max 1.040
//     dielectric                            max 1.017
// The mean is the meaningful number -- it averages thousands of independent
// per-texel estimates. The residual spread is estimator noise: raising the
// furnace sample count sixteenfold took the max from 1.047 to 1.029 and left the
// mean unmoved. Before the LUT the mean was 0.829 and the minimum 0.303.
//
// tools/render-test's bsdf-furnace scenario is the check. Keep it green.
// -----------------------------------------------------------------------------

/// Bilinear fetch of the baked (A, B, A+B) triple. The table is tiny and read
/// once or twice per shading call, so the four loads and three lerps cost less
/// than a sampler and a second descriptor would.
///
/// Clamped at the edges: roughness and NdotV are both already in [0,1] by
/// construction, and clamping rather than wrapping stops a grazing NdotV from
/// sampling the mirror end of the table.
float3 SampleBsdfLut(float NdotV, float roughness) {
    uint w, h;
    u_BsdfLut.GetDimensions(w, h);

    // Texel CENTRES, matching how the bake wrote them.
    const float fx = clamp(roughness * float(w) - 0.5, 0.0, float(w) - 1.0);
    const float fy = clamp(NdotV     * float(h) - 0.5, 0.0, float(h) - 1.0);

    const int x0 = int(fx), y0 = int(fy);
    const int x1 = min(x0 + 1, int(w) - 1);
    const int y1 = min(y0 + 1, int(h) - 1);
    const float tx = fx - float(x0);
    const float ty = fy - float(y0);

    const float3 a = lerp(u_BsdfLut[int2(x0, y0)].rgb, u_BsdfLut[int2(x1, y0)].rgb, tx);
    const float3 b = lerp(u_BsdfLut[int2(x0, y1)].rgb, u_BsdfLut[int2(x1, y1)].rgb, tx);
    return lerp(a, b, ty);
}

/// Single-scatter directional albedo of the specular lobe: how much of the
/// incoming energy it reflects. EXACT for this file's lobe, because the bake
/// integrates the same sampling and visibility functions used to shade.
public float3 SpecularAlbedo(float3 f0, float NdotV, float roughness) {
    const float3 lut = SampleBsdfLut(NdotV, roughness);
    return f0 * lut.x + lut.y;   // the split-sum decomposition, E = f0*A + B
}

/// Energy compensation for the multiple microfacet bounces single-scatter GGX
/// never counts. Without it a rough metal is visibly too dark -- the furnace
/// test measured a mean albedo of 0.83, falling to 0.30 at the roughest.
///
/// Ess is the WHITE-furnace albedo (f0 = 1), which the bake stores in .z so it
/// does not have to be reconstructed. The missing energy re-enters tinted by f0
/// because every additional bounce is another Fresnel interaction.
public float3 SpecularEnergyCompensation(float3 f0, float NdotV, float roughness) {
    const float Ess = SampleBsdfLut(NdotV, roughness).z;
    return 1.0 + f0 * (1.0 / max(Ess, BSDF_EPS) - 1.0);
}

// =============================================================================
// LOBES
// =============================================================================

/// Lambertian. The base diffuse, and the reference every other lobe's energy is
/// checked against -- its directional albedo is exactly its colour.
public struct DiffuseBsdf : IBsdf
{
    public float3 baseColor;

    public float3 eval(float3 wo, float3 wi) {
        if (wo.z <= 0.0 || wi.z <= 0.0) return float3(0.0);
        return baseColor * BSDF_INV_PI;
    }

    public float pdf(float3 wo, float3 wi) {
        if (wo.z <= 0.0 || wi.z <= 0.0) return 0.0;
        return wi.z * BSDF_INV_PI;
    }

    public bool sample(float3 wo, float2 u, float uLobe, out float3 wi, out float3 weight) {
        wi = SampleCosineHemisphere(u);
        weight = baseColor;   // f*cos/pdf collapses exactly to the albedo
        return wo.z > 0.0 && wi.z > 0.0;
    }

    public float3 albedo() { return baseColor; }
}

/// The workhorse: glTF-style metallic-roughness, diffuse + GGX specular, with
/// optional anisotropy. Everything the engine renders today is one of these.
public struct MetalRoughBsdf : IBsdf
{
    public float3 baseColor;
    public float  metallic;
    public float  roughness;
    /// -1..1. Zero is isotropic; the sign selects which of the shading frame's
    /// tangent axes is stretched.
    public float  anisotropy;
    /// Dielectric F0 scale. 0.5 gives the usual 0.04; OpenPBR's specular weight
    /// maps here.
    public float  specularLevel;

    float3 f0() { return lerp(float3(0.08 * specularLevel), baseColor, metallic); }
    float3 diffuseColor() { return baseColor * (1.0 - metallic); }

    void alphas(out float alphaT, out float alphaB) {
        // Perceptual roughness squared, the convention every glTF asset and
        // every artist slider in this engine assumes.
        const float alpha = max(roughness * roughness, MIN_ROUGHNESS);
        const float aniso = clamp(anisotropy, -0.99, 0.99);
        alphaT = max(alpha * (1.0 + aniso), MIN_ROUGHNESS);
        alphaB = max(alpha * (1.0 - aniso), MIN_ROUGHNESS);
    }

    public float3 eval(float3 wo, float3 wi) {
        if (wo.z <= 0.0 || wi.z <= 0.0) return float3(0.0);

        const float3 h = normalize(wo + wi);
        const float VdotH = saturate(dot(wo, h));

        float alphaT, alphaB;
        alphas(alphaT, alphaB);
        const float alpha = max(roughness * roughness, MIN_ROUGHNESS);

        const float D = (abs(anisotropy) < 1e-4) ? D_GGX(saturate(h.z), alpha)
                                                 : D_GGX_Aniso(h, alphaT, alphaB);
        const float V = V_SmithGGXCorrelated(wo.z, wi.z, alpha);
        const float3 F = F_Schlick(f0(), float3(1.0), VdotH);

        // Multi-scatter compensation, from the baked table. The analytic version
        // of this created 113% energy; the LUT makes it exact for this lobe.
        float3 specular = D * V * F;
        specular *= SpecularEnergyCompensation(f0(), wo.z, roughness);

        // kD IS THE ENERGY THE SPECULAR LOBE DID NOT TAKE, and it must be
        // computed from the lobe's DIRECTIONAL ALBEDO, not from the Fresnel at
        // the half vector.
        //
        // Using (1 - F(VdotH)) is the common shortcut and it creates energy at
        // grazing incidence. At cos(theta_o) ~ 0 the specular lobe reflects
        // essentially everything, but VdotH for a cosine-sampled outgoing
        // direction stays moderate, so F(VdotH) stays near F0 and kD stays near
        // 1 -- the surface then returns a full diffuse albedo ON TOP OF a full
        // specular one. The furnace test measured exactly that: directional
        // albedo 1.89 for a dielectric at cos 0.008, unchanged when the sample
        // count was raised eightfold, which is what distinguishes a bias from
        // noise.
        //
        // Coupling to the split-sum albedo at NdotV instead makes the two lobes
        // partition the incoming energy rather than each claiming most of it.
        // The compensated lobe reflects Ess-worth of energy after compensation,
        // i.e. all of what the LUT says it takes; kD is the remainder.
        const float3 kD = saturate(1.0 - SpecularAlbedo(f0(), wo.z, roughness));

        return kD * diffuseColor() * BSDF_INV_PI + specular;
    }

    public float pdf(float3 wo, float3 wi) {
        if (wo.z <= 0.0 || wi.z <= 0.0) return 0.0;

        const float3 h = normalize(wo + wi);
        const float alpha = max(roughness * roughness, MIN_ROUGHNESS);

        const float diffusePdf = wi.z * BSDF_INV_PI;
        // VNDF pdf: D_visible(h) / (4 * VdotH).
        const float D = D_GGX(saturate(h.z), alpha);
        const float G1 = 2.0 * wo.z / max(wo.z + sqrt(alpha * alpha + (1.0 - alpha * alpha) * wo.z * wo.z), BSDF_EPS);
        const float specularPdf = D * G1 / max(4.0 * wo.z, BSDF_EPS);

        const float pSpec = lobeProbability(wo);
        return lerp(diffusePdf, specularPdf, pSpec);
    }

    /// Probability of choosing the specular lobe.
    ///
    /// ANGLE-DEPENDENT, and that matters more than it looks. Weighting by the
    /// normal-incidence F0 alone means a dielectric -- F0 = 0.04 -- is sampled
    /// specularly only ~4% of the time at EVERY angle. At grazing incidence
    /// Fresnel goes to 1 and the surface is almost entirely a mirror, so the
    /// estimator was putting 96% of its samples into a lobe carrying almost none
    /// of the energy and dividing the rest by a pdf that barely accounted for it.
    /// The furnace test found this as a directional albedo of 1.93 at cos 0.008:
    /// not energy creation, but variance so large the mean of 4096 samples was
    /// still nowhere near converged.
    float lobeProbability(float3 wo) {
        const float3 F = F_Schlick(f0(), float3(1.0), saturate(wo.z));
        const float lumF = dot(F, float3(0.2126, 0.7152, 0.0722));
        const float lumD = dot(diffuseColor(), float3(0.2126, 0.7152, 0.0722));
        return clamp(lumF / max(lumF + lumD, BSDF_EPS), 0.05, 0.95);
    }

    public bool sample(float3 wo, float2 u, float uLobe, out float3 wi, out float3 weight) {
        wi = float3(0.0, 0.0, 1.0);
        weight = float3(0.0);
        if (wo.z <= 0.0) return false;

        const float pSpec = lobeProbability(wo);

        if (uLobe < pSpec) {
            float alphaT, alphaB;
            alphas(alphaT, alphaB);
            const float3 h = SampleGGXVNDF(wo, alphaT, alphaB, u);
            wi = reflect(-wo, h);
            if (wi.z <= 0.0) return false;
        } else {
            wi = SampleCosineHemisphere(u);
            if (wi.z <= 0.0) return false;
        }

        const float p = pdf(wo, wi);
        if (p < BSDF_EPS) return false;

        weight = eval(wo, wi) * wi.z / p;
        return true;
    }

    public float3 albedo() { return lerp(baseColor, f0(), metallic); }
}

/// Adds a clear coat and a sheen lobe over a MetalRoughBsdf base. This is the
/// "extended" tier -- a material that does not use it instantiates
/// MetalRoughBsdf directly and Slang eliminates all of this at link time, so it
/// genuinely costs nothing rather than being branched over.
public struct CoatedBsdf : IBsdf
{
    public MetalRoughBsdf base;
    public float  coatWeight;
    public float  coatRoughness;
    public float3 sheenColor;
    public float  sheenRoughness;

    static const float COAT_F0 = 0.04;   // IOR 1.5, the usual clear lacquer

    float coatFresnel(float VdotH) {
        return coatWeight * F_SchlickScalar(COAT_F0, 1.0, VdotH);
    }

    /// Charlie/Estevez-Kulla sheen distribution -- the inverted-Gaussian lobe
    /// that gives cloth its rim brightness. Cheaper and better behaved at
    /// grazing angles than a GGX lobe with huge roughness.
    float D_Charlie(float NdotH, float rough) {
        const float invR = 1.0 / max(rough, BSDF_EPS);
        const float cos2 = NdotH * NdotH;
        const float sin2 = max(1.0 - cos2, 0.0);
        return (2.0 + invR) * pow(sin2, invR * 0.5) / (2.0 * BSDF_PI);
    }

    public float3 eval(float3 wo, float3 wi) {
        if (wo.z <= 0.0 || wi.z <= 0.0) return float3(0.0);

        const float3 h = normalize(wo + wi);
        const float VdotH = saturate(dot(wo, h));
        const float NdotH = saturate(h.z);

        // The coat both adds a highlight and ATTENUATES what is underneath --
        // light reaching the substrate crossed the interface twice. Skipping the
        // attenuation is how a coated material ends up brighter than an uncoated
        // one, which is not physical and is the usual sign of a layered model
        // that was added rather than derived.
        const float Fc = coatFresnel(VdotH);
        const float coatAlpha = max(coatRoughness * coatRoughness, MIN_ROUGHNESS);
        const float coatSpec = D_GGX(NdotH, coatAlpha)
                             * V_SmithGGXCorrelated(wo.z, wi.z, coatAlpha) * Fc;

        const float3 sheen = sheenColor * D_Charlie(NdotH, max(sheenRoughness, MIN_ROUGHNESS))
                           / max(4.0 * (wo.z + wi.z - wo.z * wi.z), BSDF_EPS);

        return (base.eval(wo, wi) + sheen) * (1.0 - Fc) + float3(coatSpec);
    }

    public float pdf(float3 wo, float3 wi) {
        // The coat and sheen lobes are sampled through the base's distribution
        // rather than getting their own, which is a deliberate simplification:
        // both are narrow relative to the base, so the estimator stays unbiased
        // but converges slower on a strongly coated material. Revisit if a hero
        // material ever needs it.
        return base.pdf(wo, wi);
    }

    public bool sample(float3 wo, float2 u, float uLobe, out float3 wi, out float3 weight) {
        if (!base.sample(wo, u, uLobe, wi, weight)) return false;
        const float p = pdf(wo, wi);
        if (p < BSDF_EPS) return false;
        weight = eval(wo, wi) * wi.z / p;
        return true;
    }

    public float3 albedo() { return base.albedo() + sheenColor * 0.25; }
}

// =============================================================================
// SHADING FRAME
// =============================================================================

/// World <-> local tangent-space conversion around a shading normal.
///
/// The frame is built from the geometric tangent when one exists, because an
/// arbitrary basis rotates freely around N and ANISOTROPY IS DEFINED RELATIVE TO
/// THE TANGENT. An isotropic material cannot tell the difference; an anisotropic
/// one shows the arbitrary basis as brushed metal whose grain swims as the
/// camera moves.
public struct ShadingFrame
{
    public float3 t, b, n;

    public static ShadingFrame make(float3 normal, float3 tangent) {
        ShadingFrame f;
        f.n = normalize(normal);
        float3 t = tangent - f.n * dot(f.n, tangent);
        if (dot(t, t) < 1e-8) {
            // No usable tangent: any orthonormal basis will do, built
            // branchlessly (Duff et al.) rather than by picking an up vector
            // that degenerates when N happens to be parallel to it.
            const float sgn = f.n.z >= 0.0 ? 1.0 : -1.0;
            const float a = -1.0 / (sgn + f.n.z);
            const float bb = f.n.x * f.n.y * a;
            t = float3(1.0 + sgn * f.n.x * f.n.x * a, sgn * bb, -sgn * f.n.x);
            f.b = float3(bb, sgn + f.n.y * f.n.y * a, -f.n.y);
            f.t = t;
            return f;
        }
        f.t = normalize(t);
        f.b = cross(f.n, f.t);
        return f;
    }

    public float3 toLocal(float3 v) { return float3(dot(v, t), dot(v, b), dot(v, n)); }
    public float3 toWorld(float3 v) { return v.x * t + v.y * b + v.z * n; }
}

// =============================================================================
// MATERIAL -> BSDF
// =============================================================================

/// Feature bits in Gpu::Material::flags.x. Mirrored by MaterialFeature in
/// X3/src/Renderer/GpuTypes.h.
public static const uint MATERIAL_FEATURE_CLEARCOAT  = 1u << 0;
public static const uint MATERIAL_FEATURE_SHEEN      = 1u << 1;
public static const uint MATERIAL_FEATURE_ANISOTROPY = 1u << 2;

public bool MaterialHasExtendedLobes(Material mat) {
    return (mat.flags.x & (MATERIAL_FEATURE_CLEARCOAT
                         | MATERIAL_FEATURE_SHEEN
                         | MATERIAL_FEATURE_ANISOTROPY)) != 0u;
}

public MetalRoughBsdf MakeBaseBsdf(Material mat, MaterialExt ext) {
    MetalRoughBsdf bsdf;
    bsdf.baseColor     = mat.color.rgb;
    bsdf.metallic      = saturate(mat.pbrParams.x);
    bsdf.roughness     = clamp(mat.pbrParams.y, MIN_ROUGHNESS, 1.0);
    bsdf.anisotropy    = (mat.flags.x & MATERIAL_FEATURE_ANISOTROPY) != 0u ? ext.aniso.x : 0.0;
    bsdf.specularLevel = ext.specular.x;
    return bsdf;
}

public CoatedBsdf MakeCoatedBsdf(Material mat, MaterialExt ext) {
    CoatedBsdf bsdf;
    bsdf.base = MakeBaseBsdf(mat, ext);
    const bool hasCoat  = (mat.flags.x & MATERIAL_FEATURE_CLEARCOAT) != 0u;
    const bool hasSheen = (mat.flags.x & MATERIAL_FEATURE_SHEEN) != 0u;
    bsdf.coatWeight     = hasCoat ? ext.clearcoat.x : 0.0;
    bsdf.coatRoughness  = hasCoat ? max(ext.clearcoat.y, MIN_ROUGHNESS) : 1.0;
    bsdf.sheenColor     = hasSheen ? ext.sheen.rgb : float3(0.0);
    bsdf.sheenRoughness = hasSheen ? max(ext.sheen.w, MIN_ROUGHNESS) : 1.0;
    return bsdf;
}

/// The default extended block, for materials whose flags name no extended
/// lobes and which therefore have no entry in the MaterialExt buffer.
public MaterialExt DefaultMaterialExt() {
    MaterialExt ext;
    ext.clearcoat = float4(0.0, 0.1, 1.5, 0.0);
    ext.sheen     = float4(0.0, 0.0, 0.0, 0.3);
    ext.aniso     = float4(0.0, 0.0, 0.0, 1.5);
    ext.specular  = float4(0.5, 1.0, 1.0, 1.0);   // x = 0.5 -> the usual F0 0.04
    return ext;
}
