// blendmodes.h — the 30 transfer modes Normality exposes on every pass' "Blend" popup.
// Portable, header-only. Operates per-channel on normalized [0,1] floats.
// a = backdrop (existing), b = source (the pass being blended in).
#pragma once
#include "math3d.h"

namespace nm {

// Order MUST match the popup string in SPEC.md (1-based in AE, 0-based here).
enum class Blend {
    Normal = 0, Average, Darken, Multiply, ColorBurn, InverseColorBurn,
    Subtract, Darker, Lighten, Add, Screen, ColorDodge, InverseColorDodge,
    Lighter, Divide, Overlay, SoftLight, HardLight, Reflect, Glow,
    Difference, Exclusion, GrainMerge, ExponentialAB, ExponentialBA,
    Hue, Saturation, Color, Luminosity, Phoenix,
    Count
};

// ---- per-channel primitives ----
inline float bmColorBurn(float a, float b)  { return b <= 0.0f ? 0.0f : 1.0f - std::min(1.0f, (1.0f - a) / b); }
inline float bmColorDodge(float a, float b)  { return b >= 1.0f ? 1.0f : std::min(1.0f, a / (1.0f - b)); }
inline float bmReflect(float a, float b)     { return b >= 1.0f ? 1.0f : std::min(1.0f, a * a / (1.0f - b)); }
inline float bmGlow(float a, float b)        { return a >= 1.0f ? 1.0f : std::min(1.0f, b * b / (1.0f - a)); }
inline float bmOverlay(float a, float b)     { return a < 0.5f ? 2.0f * a * b : 1.0f - 2.0f * (1.0f - a) * (1.0f - b); }
inline float bmSoftLight(float a, float b) {
    if (b < 0.5f) return a - (1.0f - 2.0f * b) * a * (1.0f - a);
    float d = (a < 0.25f) ? ((16.0f * a - 12.0f) * a + 4.0f) * a : std::sqrt(a);
    return a + (2.0f * b - 1.0f) * (d - a);
}

// Per-channel scalar blends (Hue/Sat/Color/Luminosity handled separately as they are
// non-separable). For those, this returns the source unchanged; use blendColor().
inline float blendChannel(Blend m, float a, float b) {
    switch (m) {
        case Blend::Normal:            return b;
        case Blend::Average:           return 0.5f * (a + b);
        case Blend::Darken:            return std::min(a, b);
        case Blend::Multiply:          return a * b;
        case Blend::ColorBurn:         return bmColorBurn(a, b);
        case Blend::InverseColorBurn:  return bmColorBurn(b, a);
        case Blend::Subtract:          return std::max(0.0f, a - b);
        case Blend::Darker:            return std::min(a, b);            // luminance form done in blendColor
        case Blend::Lighten:           return std::max(a, b);
        case Blend::Add:               return std::min(1.0f, a + b);
        case Blend::Screen:            return 1.0f - (1.0f - a) * (1.0f - b);
        case Blend::ColorDodge:        return bmColorDodge(a, b);
        case Blend::InverseColorDodge: return bmColorDodge(b, a);
        case Blend::Lighter:           return std::max(a, b);            // luminance form done in blendColor
        case Blend::Divide:            return b <= 0.0f ? 1.0f : std::min(1.0f, a / b);
        case Blend::Overlay:           return bmOverlay(a, b);
        case Blend::SoftLight:         return bmSoftLight(a, b);
        case Blend::HardLight:         return bmOverlay(b, a);
        case Blend::Reflect:           return bmReflect(a, b);
        case Blend::Glow:              return bmGlow(a, b);
        case Blend::Difference:        return std::fabs(a - b);
        case Blend::Exclusion:         return a + b - 2.0f * a * b;
        case Blend::GrainMerge:        return clampf(a + b - 0.5f, 0.0f, 1.0f);
        case Blend::ExponentialAB:     return std::pow(clampf(a,0,1), std::max(1e-3f, b));
        case Blend::ExponentialBA:     return std::pow(clampf(b,0,1), std::max(1e-3f, a));
        case Blend::Phoenix:           return std::min(a, b) - std::max(a, b) + 1.0f;
        default:                       return b; // non-separable handled in blendColor
    }
}

// ---- luminance / HSL helpers for non-separable modes ----
inline float lum(const Vec3& c) { return 0.3f * c.x + 0.59f * c.y + 0.11f * c.z; }

inline Vec3 clipColor(Vec3 c) {
    float l = lum(c);
    float n = std::min({c.x, c.y, c.z});
    float x = std::max({c.x, c.y, c.z});
    if (n < 0.0f) {
        float d = l - n; if (d > 1e-6f) c = Vec3{l,l,l} + (c - Vec3{l,l,l}) * (l / d);
    }
    if (x > 1.0f) {
        float d = x - l; if (d > 1e-6f) c = Vec3{l,l,l} + (c - Vec3{l,l,l}) * ((1.0f - l) / d);
    }
    return c;
}
inline Vec3 setLum(const Vec3& c, float l) { return clipColor(c + Vec3{1,1,1} * (l - lum(c))); }
inline float sat(const Vec3& c) { return std::max({c.x,c.y,c.z}) - std::min({c.x,c.y,c.z}); }
inline Vec3 setSat(Vec3 c, float s) {
    float* v[3] = {&c.x, &c.y, &c.z};
    // sort indices by value
    int mn = 0, md = 1, mx = 2;
    auto val = [&](int i){ return *v[i]; };
    if (val(mn) > val(md)) std::swap(mn, md);
    if (val(md) > val(mx)) std::swap(md, mx);
    if (val(mn) > val(md)) std::swap(mn, md);
    if (*v[mx] > *v[mn]) {
        *v[md] = (*v[md] - *v[mn]) * s / (*v[mx] - *v[mn]);
        *v[mx] = s;
    } else { *v[md] = 0; *v[mx] = 0; }
    *v[mn] = 0;
    return c;
}

// Full-color blend: handles both separable and non-separable (HSL) modes.
inline Vec3 blendColor(Blend m, const Vec3& a, const Vec3& b) {
    switch (m) {
        case Blend::Hue:        return setLum(setSat(b, sat(a)), lum(a));
        case Blend::Saturation: return setLum(setSat(a, sat(b)), lum(a));
        case Blend::Color:      return setLum(b, lum(a));
        case Blend::Luminosity: return setLum(a, lum(b));
        case Blend::Darker:     return lum(a) <= lum(b) ? a : b;   // "Darker" = darker color
        case Blend::Lighter:    return lum(a) >= lum(b) ? a : b;   // "Lighter" = lighter color
        default:
            return { blendChannel(m, a.x, b.x),
                     blendChannel(m, a.y, b.y),
                     blendChannel(m, a.z, b.z) };
    }
}

// Blend b over a by opacity (0..1) using transfer mode m.
inline Vec3 composite(Blend m, const Vec3& a, const Vec3& b, float opacity) {
    return lerp(a, blendColor(m, a, b), saturate(opacity));
}

} // namespace nm
