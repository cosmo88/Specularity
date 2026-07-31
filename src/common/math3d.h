// math3d.h — portable 3D math for Normality Native.
// Header-only, no dependencies, no AE SDK. Compiles for arm64 + x86_64.
#pragma once
#include <cmath>
#include <algorithm>

namespace nm {

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float saturate(float v) { return clampf(v, 0.0f, 1.0f); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float smoothstepf(float e0, float e1, float x) {
    if (e1 <= e0) return x < e0 ? 0.0f : 1.0f;
    float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct Vec3 {
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator*(const Vec3& o) const { return {x * o.x, y * o.y, z * o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float lengthSqr(const Vec3& v) { return dot(v, v); }
inline float length(const Vec3& v) { return std::sqrt(lengthSqr(v)); }
inline Vec3 normalize(const Vec3& v) {
    float m = length(v);
    return m > 1e-8f ? Vec3{v.x / m, v.y / m, v.z / m} : Vec3{0, 0, 1};
}
inline Vec3 reflect(const Vec3& i, const Vec3& n) { return i - n * (2.0f * dot(i, n)); }
// Refract per Snell's law; returns false on total internal reflection.
inline bool refract(const Vec3& i, const Vec3& n, float eta, Vec3& out) {
    float ci = -dot(i, n);
    float k = 1.0f - eta * eta * (1.0f - ci * ci);
    if (k < 0.0f) return false;
    out = i * eta + n * (eta * ci - std::sqrt(k));
    return true;
}
inline Vec3 lerp(const Vec3& a, const Vec3& b, float t) {
    return {lerpf(a.x, b.x, t), lerpf(a.y, b.y, t), lerpf(a.z, b.z, t)};
}

// Schlick fresnel approximation. cosTheta = N·V (>=0). f0 from IOR.
inline float fresnelSchlick(float cosTheta, float f0) {
    float m = clampf(1.0f - cosTheta, 0.0f, 1.0f);
    float m2 = m * m;
    return f0 + (1.0f - f0) * (m2 * m2 * m);
}
inline float f0FromIOR(float ior) {
    float r = (ior - 1.0f) / (ior + 1.0f);
    return r * r;
}

constexpr float kPi = 3.14159265358979323846f;
inline float deg2rad(float d) { return d * (kPi / 180.0f); }

} // namespace nm
