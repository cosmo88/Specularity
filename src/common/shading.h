// shading.h — portable relighting engine for Normality Native.
// No AE SDK. All math is fresh (standard CG techniques). arm64 + x86_64.
#pragma once
#include "math3d.h"
#include "blendmodes.h"
#include <vector>

namespace nm {

// ---------------- Normal decoding ----------------
enum class AxisSrc { Red = 0, Green, Blue, Full, None };

struct NormalOptions {
    AxisSrc xSrc = AxisSrc::Red;   bool invX = false;
    AxisSrc ySrc = AxisSrc::Green; bool invY = false;
    AxisSrc zSrc = AxisSrc::Blue;  bool invZ = false;
};

inline float pickAxis(AxisSrc s, const Vec3& rgb, float dflt) {
    switch (s) {
        case AxisSrc::Red:   return rgb.x;
        case AxisSrc::Green: return rgb.y;
        case AxisSrc::Blue:  return rgb.z;
        case AxisSrc::Full:  return (rgb.x + rgb.y + rgb.z) / 3.0f;
        case AxisSrc::None:  return dflt;
    }
    return dflt;
}

// rgb in [0,1]. Returns unit surface normal in tangent/view space (Y up, Z toward viewer).
inline Vec3 decodeNormal(const Vec3& rgb, const NormalOptions& o) {
    float nx = pickAxis(o.xSrc, rgb, 0.5f) * 2.0f - 1.0f;
    float ny = pickAxis(o.ySrc, rgb, 0.5f) * 2.0f - 1.0f;
    float nz = pickAxis(o.zSrc, rgb, 1.0f) * 2.0f - 1.0f;
    if (o.invX) nx = -nx;
    if (o.invY) ny = -ny;
    if (o.invZ) nz = -nz;
    Vec3 n{nx, ny, nz};
    if (lengthSqr(n) < 1e-8f) return {0, 0, 1};
    return normalize(n);
}

// ---------------- Lights ----------------
enum class LightType { Directional = 0, Point, Ambient };
enum class Falloff   { Constant = 0, Linear, Quadratic, Cubic };

struct Light {
    LightType type = LightType::Directional;
    Vec3 dir{0.0f, 0.0f, 1.0f};   // direction TO light (directional), normalized
    Vec3 pos{0.0f, 0.0f, 1.0f};   // world/view position (point)
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    bool visible = true;
};

inline float falloffAtten(Falloff f, float dist) {
    float d = std::max(1e-3f, dist);
    switch (f) {
        case Falloff::Constant:  return 1.0f;
        case Falloff::Linear:    return 1.0f / d;
        case Falloff::Quadratic: return 1.0f / (d * d);
        case Falloff::Cubic:     return 1.0f / (d * d * d);
    }
    return 1.0f;
}

// Normalized, bounded distance attenuation for point lights. nd = dist/refDist
// (refDist ~ subject radius, so distance reads in "subject sizes", not raw pixels);
// k scales how fast it drops. atten in (0,1], = 1 at the light — never explodes to
// infinity up close nor collapses to ~0 at pixel-scale distances (both of which made
// raw inverse-square unusable once P is reconstructed in comp-pixel space).
inline float falloffAttenNorm(Falloff f, float dist, float refDist, float k) {
    float nd = dist / std::max(1e-3f, refDist);
    float kk = std::max(0.0f, k);
    switch (f) {
        case Falloff::Constant:  return 1.0f;
        case Falloff::Linear:    return 1.0f / (1.0f + kk * nd);
        case Falloff::Quadratic: return 1.0f / (1.0f + kk * nd * nd);
        case Falloff::Cubic:     return 1.0f / (1.0f + kk * nd * nd * nd);
    }
    return 1.0f;
}

// Resolve light direction (to light) and attenuation at a shaded point P.
inline Vec3 lightDirTo(const Light& L, const Vec3& P, float& atten, Falloff fo, float foScale, float refDist) {
    if (L.type == LightType::Point) {
        Vec3 d = L.pos - P;
        float dist = length(d);
        atten = falloffAttenNorm(fo, dist, refDist, foScale);
        return dist > 1e-6f ? d * (1.0f / dist) : Vec3{0, 0, 1};
    }
    atten = 1.0f; // directional/ambient: no distance falloff
    return normalize(L.dir);
}

// ---------------- Pass parameters ----------------
struct PassBlend { bool enable = true; Vec3 color{1,1,1}; Blend blend = Blend::Normal; float opacity = 1.0f; };

struct DiffuseParams  { Vec3 color{1,1,1}; float ambient = 0.0f; float incandescence = 0.0f; Blend blend = Blend::Normal; float opacity = 1.0f; };
struct SpecularParams { bool enable=false; float intensity=1.0f; float spread=20.0f; Vec3 color{1,1,1}; Blend blend=Blend::Add; float opacity=1.0f; };
struct IncidenceParams{ bool enable=false; float intensity=1.0f; float falloff=2.0f; Vec3 color{1,1,1}; Blend blend=Blend::Normal; float opacity=1.0f; };
struct RimParams      { bool enable=false; float size=1.0f; float width=0.5f; Vec3 color{1,1,1}; float angle=0.0f; Blend blend=Blend::Add; float opacity=1.0f; };
struct ToonParams     { bool enable=false; float smoothing=0.05f; float shadowIntensity=0.5f; float shadowWidth=0.5f; Vec3 shadowColor{0,0,0};
                        float highlightIntensity=1.0f; float highlightWidth=0.2f; Blend blend=Blend::Normal; float opacity=1.0f; };
struct GradientParams { bool enable=false; Vec3 topColor{1,1,1}; Vec3 botColor{0,0,0}; Blend blend=Blend::Normal; float opacity=1.0f; };

struct GlobalLightParams { Falloff falloff = Falloff::Constant; float falloffScale = 1.0f; float exposure = 0.0f; float gamma = 1.0f;
                           float refDist = 1.0f; };  // ~subject radius (px), set per-render from layer size

// ---------------- Shading passes ----------------
// All passes take the surface normal N, view vector V (to viewer), and lights.

// Lambert diffuse accumulated over lights.
inline Vec3 shadeDiffuse(const Vec3& N, const Vec3& P, const std::vector<Light>& lights,
                         const GlobalLightParams& g, const DiffuseParams& d) {
    Vec3 acc{0, 0, 0};
    for (const auto& L : lights) {
        if (L.type == LightType::Ambient) { acc = acc + L.color * (L.intensity); continue; }
        float atten;
        Vec3 Ld = lightDirTo(L, P, atten, g.falloff, g.falloffScale, g.refDist);
        float ndl = std::max(0.0f, dot(N, Ld));
        acc = acc + L.color * (ndl * L.intensity * atten);
    }
    Vec3 col = acc * d.color;
    col = col + d.color * d.ambient;            // ambient term
    col = col + d.color * d.incandescence;      // self-emission
    return col;
}

// Blinn-Phong specular accumulated over lights.
inline Vec3 shadeSpecular(const Vec3& N, const Vec3& V, const Vec3& P, const std::vector<Light>& lights,
                          const GlobalLightParams& g, const SpecularParams& s) {
    if (!s.enable) return {0, 0, 0};
    Vec3 acc{0, 0, 0};
    for (const auto& L : lights) {
        if (L.type == LightType::Ambient) continue;
        float atten;
        Vec3 Ld = lightDirTo(L, P, atten, g.falloff, g.falloffScale, g.refDist);
        if (dot(N, Ld) <= 0.0f) continue;
        Vec3 H = normalize(Ld + V);
        float spec = std::pow(std::max(0.0f, dot(N, H)), std::max(1.0f, s.spread));
        acc = acc + L.color * (spec * s.intensity * atten);
    }
    return acc * s.color;
}

// Incidence / facing ratio (fresnel-like): bright at grazing angles.
inline Vec3 shadeIncidence(const Vec3& N, const Vec3& V, const IncidenceParams& p) {
    if (!p.enable) return {0, 0, 0};
    float ndv = std::max(0.0f, dot(N, V));
    float f = std::pow(1.0f - ndv, std::max(0.01f, p.falloff)) * p.intensity;
    return p.color * f;
}

// Rim light: incidence gated by a light direction + angular width.
inline Vec3 shadeRim(const Vec3& N, const Vec3& V, const std::vector<Light>& lights, const RimParams& p) {
    if (!p.enable) return {0, 0, 0};
    float rim = std::pow(saturate(1.0f - std::max(0.0f, dot(N, V))), std::max(0.01f, p.width));
    rim *= p.size;
    // gate by nearest light direction if any directional light present
    float gate = 1.0f;
    for (const auto& L : lights) {
        if (L.type == LightType::Ambient) continue;
        Vec3 Ld = normalize(L.dir);
        float a = dot(N, Ld);
        gate = std::max(gate, saturate(a + std::cos(deg2rad(p.angle))));
        break;
    }
    return p.color * (rim * gate);
}

// Toon / cel banding of a diffuse luminance.
inline Vec3 shadeToon(const Vec3& litColor, const ToonParams& t) {
    if (!t.enable) return litColor;
    float l = lum(litColor);
    float sw = clampf(t.shadowWidth, 0.0f, 1.0f);
    float sm = std::max(1e-4f, t.smoothing);
    float shadow = 1.0f - smoothstepf(sw - sm, sw + sm, l);
    Vec3 base = lerp(litColor, t.shadowColor, shadow * t.shadowIntensity);
    float hw = 1.0f - clampf(t.highlightWidth, 0.0f, 1.0f);
    float hi = smoothstepf(hw - sm, hw + sm, l) * t.highlightIntensity;
    return base + Vec3{1,1,1} * hi;
}

// Directional gradient (top→bottom by normal.y).
inline Vec3 shadeGradient(const Vec3& N, const GradientParams& p) {
    if (!p.enable) return {0, 0, 0};
    float t = saturate(N.y * 0.5f + 0.5f);
    return lerp(p.botColor, p.topColor, t);
}

// ---------------- Exposure / gamma ----------------
inline Vec3 applyExposureGamma(const Vec3& c, float exposure, float gamma) {
    float e = std::pow(2.0f, exposure);
    float ig = 1.0f / std::max(1e-3f, gamma);
    return { std::pow(std::max(0.0f, c.x * e), ig),
             std::pow(std::max(0.0f, c.y * e), ig),
             std::pow(std::max(0.0f, c.z * e), ig) };
}

} // namespace nm
