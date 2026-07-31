// test_core.cpp — exercises the SDK-independent shading core.
// Build: see tests/run.sh (compiles Universal arm64+x86_64 and runs both).
#include "../src/common/shading.h"
#include <cstdio>
#include <cmath>
#include <cassert>

using namespace nm;

static int g_fail = 0;
static void check(bool cond, const char* msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg); ++g_fail; }
    else       { std::printf("  ok:   %s\n", msg); }
}
static bool approx(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

int main() {
    std::printf("== Normality Native core tests ==\n");

    // --- normal decoding ---
    NormalOptions no;
    Vec3 flat = decodeNormal(Vec3{0.5f, 0.5f, 1.0f}, no);
    check(approx(flat.x, 0) && approx(flat.y, 0) && approx(flat.z, 1), "flat normal (0.5,0.5,1) -> +Z");
    Vec3 right = decodeNormal(Vec3{1.0f, 0.5f, 0.5f}, no);
    check(right.x > 0.9f, "red=1 -> +X normal");
    no.invX = true;
    Vec3 rightInv = decodeNormal(Vec3{1.0f, 0.5f, 0.5f}, no);
    check(rightInv.x < -0.9f, "invert X flips normal.x");

    // --- blend modes ---
    check(approx(blendChannel(Blend::Multiply, 0.5f, 0.5f), 0.25f), "multiply 0.5*0.5=0.25");
    check(approx(blendChannel(Blend::Screen, 0.5f, 0.5f), 0.75f), "screen 0.5,0.5=0.75");
    check(approx(blendChannel(Blend::Add, 0.6f, 0.6f), 1.0f), "add clamps to 1");
    check(approx(blendChannel(Blend::Difference, 0.7f, 0.2f), 0.5f), "difference |0.7-0.2|=0.5");
    check(approx(blendChannel(Blend::Normal, 0.3f, 0.9f), 0.9f), "normal returns source");
    // Luminosity: result has luma of b, chroma of a
    Vec3 lc = blendColor(Blend::Luminosity, Vec3{0.8f, 0.2f, 0.2f}, Vec3{0.5f, 0.5f, 0.5f});
    check(approx(lum(lc), 0.5f, 1e-3f), "luminosity sets luma to source");

    // --- diffuse Lambert ---
    std::vector<Light> lights;
    Light key; key.type = LightType::Directional; key.dir = Vec3{0, 0, 1}; key.intensity = 1.0f; // straight on
    lights.push_back(key);
    GlobalLightParams g;
    DiffuseParams d;
    Vec3 dc = shadeDiffuse(Vec3{0,0,1}, Vec3{0,0,0}, lights, g, d);
    check(approx(dc.x, 1.0f), "diffuse: N=L -> full 1.0");
    Vec3 dc2 = shadeDiffuse(Vec3{1,0,0}, Vec3{0,0,0}, lights, g, d); // normal perpendicular
    check(approx(dc2.x, 0.0f), "diffuse: N perp L -> 0");
    Vec3 dc3 = shadeDiffuse(Vec3{0,0,-1}, Vec3{0,0,0}, lights, g, d); // facing away
    check(approx(dc3.x, 0.0f), "diffuse: N away from L clamps to 0");

    // 45-degree
    Vec3 n45 = normalize(Vec3{0, 1, 1});
    Vec3 dc4 = shadeDiffuse(n45, Vec3{0,0,0}, lights, g, d);
    check(approx(dc4.z, std::cos(deg2rad(45.0f)), 1e-3f), "diffuse: 45deg -> cos45");

    // --- specular ---
    SpecularParams s; s.enable = true; s.spread = 20.0f; s.intensity = 1.0f;
    Vec3 V{0, 0, 1};
    Vec3 sc = shadeSpecular(Vec3{0,0,1}, V, Vec3{0,0,0}, lights, g, s);
    check(sc.x > 0.9f, "specular: aligned N,L,V -> strong highlight");
    Vec3 sc2 = shadeSpecular(normalize(Vec3{1,0,1}), V, Vec3{0,0,0}, lights, g, s);
    check(sc2.x < sc.x, "specular: off-angle -> weaker");

    // --- incidence (fresnel-like) ---
    IncidenceParams ip; ip.enable = true; ip.falloff = 2.0f; ip.intensity = 1.0f;
    Vec3 icFace = shadeIncidence(Vec3{0,0,1}, V, ip);
    Vec3 icEdge = shadeIncidence(normalize(Vec3{1,0,0.05f}), V, ip);
    check(icFace.x < 0.05f, "incidence: facing viewer -> ~0");
    check(icEdge.x > icFace.x, "incidence: grazing -> brighter");

    // --- point light falloff ---
    std::vector<Light> plights;
    Light pl; pl.type = LightType::Point; pl.pos = Vec3{0,0,2}; pl.intensity = 1.0f;
    plights.push_back(pl);
    GlobalLightParams gq; gq.falloff = Falloff::Quadratic;
    Vec3 near = shadeDiffuse(Vec3{0,0,1}, Vec3{0,0,1}, plights, gq, d); // dist 1
    Vec3 far  = shadeDiffuse(Vec3{0,0,1}, Vec3{0,0,0}, plights, gq, d); // dist 2
    check(near.z > far.z, "point light: closer point is brighter (quadratic falloff)");

    // --- exposure / gamma ---
    Vec3 eg = applyExposureGamma(Vec3{0.25f, 0.25f, 0.25f}, 1.0f, 1.0f); // *2
    check(approx(eg.x, 0.5f), "exposure +1 stop doubles value");
    Vec3 eg2 = applyExposureGamma(Vec3{0.25f,0.25f,0.25f}, 0.0f, 2.0f);  // sqrt
    check(approx(eg2.x, 0.5f), "gamma 2.0 -> sqrt(0.25)=0.5");

    // --- refract sanity (Snell) ---
    Vec3 rout;
    bool ok = refract(Vec3{0,0,-1}, Vec3{0,0,1}, 1.0f/1.5f, rout);
    check(ok && approx(rout.z, -1.0f, 1e-3f), "refract straight-on passes through");

    std::printf("== %s ==\n", g_fail == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return g_fail == 0 ? 0 : 1;
}
