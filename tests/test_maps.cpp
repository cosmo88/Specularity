// test_maps.cpp — exercises the map-based passes (sampler tiling, matcap, reflection
// fresnel, refraction, bump, depth). SDK-free; builds Universal.
#include "../src/common/maps.h"
#include <cstdio>
#include <cmath>
using namespace nm;

static int g_fail = 0;
static void check(bool c, const char* m) { std::printf(c ? "  ok:   %s\n" : "  FAIL: %s\n", m); if (!c) ++g_fail; }
static bool approx(float a, float b, float e = 1e-3f) { return std::fabs(a - b) < e; }

int main() {
    std::printf("== map-pass tests ==\n");

    // tile modes
    { float t = 1.25f; check(tileCoord(t, Tile::Repeat) && approx(t, 0.25f), "repeat wraps 1.25->0.25"); }
    { float t = 1.25f; check(tileCoord(t, Tile::Mirror) && approx(t, 0.75f), "mirror 1.25->0.75"); }
    { float t = 1.25f; check(tileCoord(t, Tile::Edge)   && approx(t, 1.0f),  "edge clamps 1.25->1.0"); }
    { float t = 1.25f; check(!tileCoord(t, Tile::None), "none rejects out-of-range"); }
    { float t = -0.25f; check(tileCoord(t, Tile::Repeat) && approx(t, 0.75f), "repeat wraps -0.25->0.75"); }

    // matcap uv: flat normal (0,0,1) -> center
    { MatcapParams mp; float u,v; matcapUV(Vec3{0,0,1}, mp, u, v);
      check(approx(u,0.5f)&&approx(v,0.5f), "matcap center for +Z normal"); }
    { MatcapParams mp; float u,v; matcapUV(Vec3{1,0,0}, mp, u, v);
      check(approx(u,1.0f)&&approx(v,0.5f), "matcap +X normal -> u=1"); }

    // reflection: spherical uv for R=+Z is center; fresnel higher at grazing
    { ReflectionParams rp; rp.enable=true; rp.fresnel=1.0f; rp.fresnelDepth=1.0f;
      TexFn white = [](float,float){ return Vec3{1,1,1}; };
      float wFace, wEdge;
      shadeReflection(Vec3{0,0,1}, Vec3{0,0,1}, rp, white, wFace);          // facing
      shadeReflection(normalize(Vec3{1,0,0.05f}), Vec3{0,0,1}, rp, white, wEdge); // grazing
      check(wEdge > wFace, "reflection fresnel: grazing > facing");
      check(wFace >= 0.0f && wFace < 0.2f, "reflection fresnel: facing is low"); }

    // reflection samples center texel for straight-on
    { ReflectionParams rp; rp.enable=true;
      TexFn probe = [](float u,float v){ return Vec3{u,v,0}; };
      float w; Vec3 c = shadeReflection(Vec3{0,0,1}, Vec3{0,0,1}, rp, probe, w);
      check(approx(c.x,0.5f)&&approx(c.y,0.5f), "reflection sphere-map center uv"); }

    // refraction: straight-on transmits, weight ~ (1-fresnel) high
    { RefractionParams rp; rp.enable=true; rp.ior=1.1f;
      TexFn white = [](float,float){ return Vec3{0.4f,0.4f,0.4f}; };
      float w; Vec3 c = shadeRefraction(Vec3{0,0,1}, Vec3{0,0,1}, 0.5f, 0.5f, rp, white, w);
      check(w > 0.8f, "refraction transmission high when facing");
      check(approx(c.x,0.4f), "refraction samples map color"); }

    // bump: flat height field leaves normal unchanged
    { BumpParams bp; bp.enable=true; bp.intensity=1.0f;
      TexFn flat = [](float,float){ return Vec3{0.5f,0.5f,0.5f}; };
      Vec3 N = perturbNormalFromBump(Vec3{0,0,1}, 0.5f,0.5f, 0.01f,0.01f, bp, flat);
      check(approx(N.z,1.0f,1e-3f), "bump: flat map keeps normal +Z"); }
    // bump: horizontal gradient tilts normal in X
    { BumpParams bp; bp.enable=true; bp.intensity=2.0f;
      TexFn ramp = [](float u,float){ return Vec3{u,u,u}; };
      Vec3 N = perturbNormalFromBump(Vec3{0,0,1}, 0.5f,0.5f, 0.05f,0.05f, bp, ramp);
      check(N.x < -0.01f || N.x > 0.01f, "bump: gradient perturbs normal.x"); }

    // depth decode
    { DepthParams dp; dp.channel=DepthChannel::Red; dp.multiplier=2.0f; float d;
      check(decodeDepth(Vec3{0.5f,0,0}, dp, d) && approx(d,1.0f), "depth red*mult"); }
    { DepthParams dp; dp.channel=DepthChannel::None; float d;
      check(!decodeDepth(Vec3{1,1,1}, dp, d), "depth None -> no decode"); }
    { DepthParams dp; dp.channel=DepthChannel::Luminance; dp.invert=true; float d;
      check(decodeDepth(Vec3{0,0,0}, dp, d) && approx(d,1.0f), "depth invert black->1"); }

    std::printf("== %s ==\n", g_fail ? "FAILURES" : "ALL PASSED");
    return g_fail ? 1 : 0;
}
