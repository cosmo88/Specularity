// maps.h — image sampling + map-based shading passes (matcap, reflection, refraction,
// bump, depth). SDK-free. The caller supplies a TexFn: a raw bilinear sampler that,
// given (u,v) in [0,1], returns the layer's RGB (edge-clamped outside). Tile behavior
// is applied here in the passes, before calling TexFn.
#pragma once
#include "math3d.h"
#include "blendmodes.h"
#include <functional>

namespace nm {

// A raw texture sampler: (u,v) in [0,1] -> RGB. Provider clamps to edge outside [0,1].
using TexFn = std::function<Vec3(float u, float v)>;

enum class Tile { None = 0, Edge, Repeat, Mirror };

// Generalized Schlick fresnel: ~f0 when facing (ndv=1), ->strength at grazing.
// 'depth' is the falloff exponent (higher = effect concentrated at the rim).
inline float fresnelGen(float ndv, float f0, float depth, float strength) {
    float e = std::pow(clampf(1.0f - ndv, 0.0f, 1.0f), std::max(0.01f, depth));
    return clampf((f0 + (1.0f - f0) * e) * strength, 0.0f, 1.0f);
}

// Remap a single coordinate per tile mode. Returns false only when Tile::None and out of range.
inline bool tileCoord(float& t, Tile mode) {
    if (t >= 0.0f && t <= 1.0f) return true;
    switch (mode) {
        case Tile::None:   return false;
        case Tile::Edge:   t = clampf(t, 0.0f, 1.0f); return true;
        case Tile::Repeat: t = t - std::floor(t);     return true;
        case Tile::Mirror: {
            float f = std::fabs(t);
            float ip = std::floor(f);
            float fr = f - ip;
            t = (((int)ip) & 1) ? (1.0f - fr) : fr;
            return true;
        }
    }
    return true;
}
inline bool tileUV(float& u, float& v, Tile mode) {
    bool a = tileCoord(u, mode), b = tileCoord(v, mode);
    return a && b;
}

// ---------------- Matcap ----------------
// View-space normal -> matcap uv. Offset shifts the lookup. Mode tiles the lookup.
struct MatcapParams { bool enable=false; float offsetX=0, offsetY=0; Tile mode=Tile::Edge;
                      Blend blend=Blend::Normal; float opacity=1.0f; };

inline bool matcapUV(const Vec3& N, const MatcapParams& p, float& u, float& v) {
    u = N.x * 0.5f + 0.5f + p.offsetX;
    v = (-N.y) * 0.5f + 0.5f + p.offsetY;   // flip Y for image space
    return tileUV(u, v, p.mode);
}
inline Vec3 shadeMatcap(const Vec3& N, const MatcapParams& p, const TexFn& tex, bool& hit) {
    float u, v; hit = false;
    if (!p.enable || !tex) return {0,0,0};
    if (!matcapUV(N, p, u, v)) return {0,0,0};
    hit = true;
    return tex(u, v);
}

// ---------------- Reflection (environment) ----------------
enum class EnvMode { Spherical = 0, Panorama };
struct ReflectionParams {
    bool enable=false; EnvMode envMode=EnvMode::Spherical; Tile tile=Tile::Repeat;
    Vec3 color{1,1,1}; float gamma=1.0f;
    float inclination=0.0f; float azimuth=0.0f; float seam=0.0f; // orientation (radians)
    float ior=1.45f; float fresnel=1.0f; float fresnelDepth=5.0f;
    Blend blend=Blend::Add; float opacity=1.0f;
};

// Reflected view direction -> env uv.
inline void envUV(const Vec3& R, const ReflectionParams& p, float& u, float& v) {
    if (p.envMode == EnvMode::Panorama) {
        // equirectangular
        u = 0.5f + (std::atan2(R.x, R.z) + p.azimuth + p.seam) / (2.0f * kPi);
        v = 0.5f - (std::asin(clampf(R.y, -1.0f, 1.0f)) + p.inclination) / kPi;
    } else {
        // sphere map (mirror-ball)
        float m = 2.0f * std::sqrt(R.x * R.x + R.y * R.y + (R.z + 1.0f) * (R.z + 1.0f));
        if (m < 1e-4f) m = 1e-4f;
        u = R.x / m + 0.5f;
        v = -R.y / m + 0.5f;
    }
}

// Returns reflected color already fresnel-weighted; 'weight' outputs the fresnel factor.
inline Vec3 shadeReflection(const Vec3& N, const Vec3& V, const ReflectionParams& p,
                            const TexFn& tex, float& weight) {
    weight = 0.0f;
    if (!p.enable || !tex) return {0,0,0};
    Vec3 R = reflect(-V, N);           // incident = -V (from surface to viewer, reflected)
    float u, v; envUV(R, p, u, v);
    if (!tileUV(u, v, p.tile)) return {0,0,0};
    Vec3 c = tex(u, v);
    if (p.gamma != 1.0f) c = { std::pow(std::max(0.f,c.x), p.gamma), std::pow(std::max(0.f,c.y), p.gamma), std::pow(std::max(0.f,c.z), p.gamma) };
    float ndv = std::max(0.0f, dot(N, V));
    weight = fresnelGen(ndv, f0FromIOR(p.ior), p.fresnelDepth, p.fresnel);
    return c * p.color;
}

// ---------------- Refraction ----------------
struct RefractionParams {
    bool enable=false; float ior=1.10f; float gamma=1.0f; Tile tile=Tile::Edge;
    float offsetX=0, offsetY=0; float scale=0.1f; Vec3 color{1,1,1};
    float fresnel=1.0f; float fresnelDepth=5.0f;
    Blend blend=Blend::Normal; float opacity=1.0f;
};

// Samples a background/refraction map, distorted by the refracted vector. baseU/baseV is
// this pixel's own screen uv. Returns color; 'weight' outputs (1-fresnel) transmission.
inline Vec3 shadeRefraction(const Vec3& N, const Vec3& V, float baseU, float baseV,
                            const RefractionParams& p, const TexFn& tex, float& weight) {
    weight = 0.0f;
    if (!p.enable || !tex) return {0,0,0};
    Vec3 T;
    if (!refract(-V, N, 1.0f / std::max(1.001f, p.ior), T)) { T = reflect(-V, N); }
    float u = baseU + T.x * p.scale + p.offsetX;
    float v = baseV - T.y * p.scale + p.offsetY;
    if (!tileUV(u, v, p.tile)) return {0,0,0};
    Vec3 c = tex(u, v);
    if (p.gamma != 1.0f) c = { std::pow(std::max(0.f,c.x), p.gamma), std::pow(std::max(0.f,c.y), p.gamma), std::pow(std::max(0.f,c.z), p.gamma) };
    float ndv = std::max(0.0f, dot(N, V));
    float fr = fresnelGen(ndv, f0FromIOR(p.ior), p.fresnelDepth, p.fresnel);
    weight = clampf(1.0f - fr, 0.0f, 1.0f);   // transmission
    return c * p.color;
}

// ---------------- Bump ----------------
// Perturb a base normal using a height/bump map's local gradient (finite differences).
struct BumpParams { bool enable=false; float intensity=1.0f; Tile tile=Tile::Edge;
                    float offsetX=0, offsetY=0; float scale=1.0f; };

inline Vec3 perturbNormalFromBump(const Vec3& N, float u, float v, float du, float dv,
                                  const BumpParams& p, const TexFn& tex) {
    if (!p.enable || !tex) return N;
    auto h = [&](float uu, float vv) {
        float su = uu * p.scale + p.offsetX, sv = vv * p.scale + p.offsetY;
        if (!tileUV(su, sv, p.tile)) return 0.0f;
        return lum(tex(su, sv));
    };
    float hL = h(u - du, v), hR = h(u + du, v);
    float hD = h(u, v - dv), hU = h(u, v + dv);
    float dhdx = (hR - hL) * p.intensity;
    float dhdy = (hU - hD) * p.intensity;
    Vec3 perturbed{ N.x - dhdx, N.y - dhdy, N.z };
    return normalize(perturbed);
}

// ---------------- Depth ----------------
enum class DepthChannel { None = 0, Red, Green, Blue, Luminance };
struct DepthParams { bool invert=false; DepthChannel channel=DepthChannel::None;
                     float gamma=1.0f; float multiplier=1.0f; };

// Decode a depth value in [0,1] from a depth-map color; returns false if channel None.
inline bool decodeDepth(const Vec3& rgb, const DepthParams& p, float& depth) {
    if (p.channel == DepthChannel::None) return false;
    float d;
    switch (p.channel) {
        case DepthChannel::Red:       d = rgb.x; break;
        case DepthChannel::Green:     d = rgb.y; break;
        case DepthChannel::Blue:      d = rgb.z; break;
        case DepthChannel::Luminance: d = lum(rgb); break;
        default: return false;
    }
    if (p.invert) d = 1.0f - d;
    d = std::pow(clampf(d, 0.0f, 1.0f), 1.0f / std::max(1e-3f, p.gamma));
    depth = d * p.multiplier;
    return true;
}

} // namespace nm
