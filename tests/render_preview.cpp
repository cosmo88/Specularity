// render_preview.cpp — renders a synthetic normal-map sphere and relights it through
// the SAME ShadePixel() code the AE plugin uses, writing PPM images. Lets us verify the
// imaging (diffuse shading, moving highlight, rim) natively without launching AE.
#include "../src/common/relight.h"
#include <cstdio>
#include <vector>
#include <string>
using namespace nm;

static const int W = 256, H = 256;

// Build a normal map of a sphere centered in frame: RGB-encoded normals, alpha=coverage.
static void makeSphereNormalMap(std::vector<Vec3>& rgb, std::vector<float>& cov) {
	rgb.resize(W * H); cov.resize(W * H);
	float cx = W * 0.5f, cy = H * 0.5f, R = W * 0.42f;
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) {
			float nx = (x + 0.5f - cx) / R;
			float ny = (cy - (y + 0.5f)) / R; // +Y up
			float r2 = nx * nx + ny * ny;
			int i = y * W + x;
			if (r2 <= 1.0f) {
				float nz = std::sqrt(1.0f - r2);
				Vec3 n{nx, ny, nz};
				rgb[i] = n * 0.5f + Vec3{0.5f, 0.5f, 0.5f}; // encode to [0,1]
				cov[i] = 1.0f;
			} else {
				rgb[i] = Vec3{0.5f, 0.5f, 1.0f}; // flat/background
				cov[i] = 0.0f;
			}
		}
}

static void writePPM(const std::string& path, const std::vector<Vec3>& img) {
	FILE* f = std::fopen(path.c_str(), "wb");
	std::fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (auto& c : img) {
		unsigned char px[3] = {
			(unsigned char)(saturate(c.x) * 255.0f + 0.5f),
			(unsigned char)(saturate(c.y) * 255.0f + 0.5f),
			(unsigned char)(saturate(c.z) * 255.0f + 0.5f) };
		std::fwrite(px, 1, 3, f);
	}
	std::fclose(f);
}

// Average luminance over the sphere region, for automated sanity checks.
static float meanLum(const std::vector<Vec3>& img, const std::vector<float>& cov) {
	double s = 0; int n = 0;
	for (size_t i = 0; i < img.size(); ++i) if (cov[i] > 0.5f) { s += lum(img[i]); ++n; }
	return n ? (float)(s / n) : 0.0f;
}

int main() {
	std::vector<Vec3> nrm; std::vector<float> cov;
	makeSphereNormalMap(nrm, cov);

	auto render = [&](const Shade& S, std::vector<Vec3>& out) {
		out.resize(W * H);
		for (int i = 0; i < W * H; ++i)
			out[i] = cov[i] > 0.5f ? ShadePixel(nrm[i], S) : Vec3{0, 0, 0};
	};

	std::printf("== Normality Native render preview ==\n");
	int fail = 0;

	// 1) Diffuse, light from the RIGHT (azimuth 0, elevation 20): right side should be brighter.
	Shade s1; s1.display = DISPLAY_DIFFUSE;
	setSingleLight(s1, 0, 20, Vec3{1,1,1}, 1.0f);
	std::vector<Vec3> imgR; render(s1, imgR);
	writePPM("build/preview_diffuse_right.ppm", imgR);

	// 2) Same but light from the LEFT (azimuth 180).
	Shade s2 = s1; setSingleLight(s2, 180, 20, Vec3{1,1,1}, 1.0f);
	std::vector<Vec3> imgL; render(s2, imgL);
	writePPM("build/preview_diffuse_left.ppm", imgL);

	// Check: with right light, mean luma of right half > left half.
	auto halfLum = [&](const std::vector<Vec3>& img, bool rightHalf) {
		double s = 0; int n = 0;
		for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
			int i = y * W + x; if (cov[i] < 0.5f) continue;
			bool r = x > W / 2; if (r == rightHalf) { s += lum(img[i]); ++n; }
		}
		return n ? (float)(s / n) : 0.0f;
	};
	float rR = halfLum(imgR, true), rL = halfLum(imgR, false);
	std::printf("  diffuse (light right): right=%.3f left=%.3f -> %s\n", rR, rL, rR > rL ? "ok" : "FAIL");
	if (!(rR > rL)) fail = 1;
	float lR = halfLum(imgL, true), lL = halfLum(imgL, false);
	std::printf("  diffuse (light left):  right=%.3f left=%.3f -> %s\n", lR, lL, lL > lR ? "ok" : "FAIL");
	if (!(lL > lR)) fail = 1;

    // 3) Full composite: diffuse + specular + rim, front-top light.
	Shade s3; s3.display = DISPLAY_ALL;
	setSingleLight(s3, 60, 45, Vec3{1,1,1}, 1.0f);
	s3.diffuse.ambient = 0.08f;
	s3.spec.enable = true; s3.spec.intensity = 1.2f; s3.spec.spread = 40.0f;
	s3.rim.enable = true;  s3.rim.size = 1.0f; s3.rim.width = 3.0f; s3.rim.color = Vec3{0.5f,0.7f,1.0f};
	std::vector<Vec3> img3; render(s3, img3);
	writePPM("build/preview_composite.ppm", img3);
	float m3 = meanLum(img3, cov);
	std::printf("  composite mean luma:   %.3f -> %s\n", m3, (m3 > 0.05f && m3 < 0.95f) ? "ok" : "FAIL");
	if (!(m3 > 0.05f && m3 < 0.95f)) fail = 1;

	// 4) Specular highlight must exist (some pixels near white) and be localized.
	Shade s4; s4.display = DISPLAY_SPECULAR;
	setSingleLight(s4, 0, 90, Vec3{1,1,1}, 1.0f); // straight on -> highlight near center
	s4.spec.enable = true; s4.spec.intensity = 1.0f; s4.spec.spread = 60.0f;
	std::vector<Vec3> img4; render(s4, img4);
	writePPM("build/preview_specular.ppm", img4);
	int bright = 0; for (int i = 0; i < W*H; ++i) if (cov[i] > 0.5f && lum(img4[i]) > 0.8f) ++bright;
	std::printf("  specular bright px:    %d -> %s\n", bright, (bright > 5 && bright < W*H/4) ? "ok" : "FAIL");
	if (!(bright > 5 && bright < W*H/4)) fail = 1;

	// 5) Toon: diffuse should quantize into flat bands (few distinct luma levels).
	Shade s5; s5.display = DISPLAY_DIFFUSE;
	setSingleLight(s5, 30, 40, Vec3{1,1,1}, 1.0f);
	s5.toon.enable = true; s5.toon.smoothing = 0.02f; s5.toon.shadowIntensity = 0.8f; s5.toon.shadowWidth = 0.5f;
	std::vector<Vec3> img5; render(s5, img5);
	writePPM("build/preview_toon.ppm", img5);

	// 6) Matcap: a procedural radial "clay" matcap sampled by the normal.
	Shade s6; s6.display = DISPLAY_MATCAP; s6.matcap.enable = true;
	TexFn matTex = [](float u, float v){
		float dx = u - 0.5f, dy = v - 0.35f; float r = std::sqrt(dx*dx + dy*dy) * 2.0f;
		float k = saturate(1.0f - r); return Vec3{0.9f*k + 0.1f, 0.55f*k + 0.08f, 0.4f*k + 0.06f};
	};
	{ std::vector<Vec3> out(W*H);
	  for (int i=0;i<W*H;++i){ if (cov[i]<0.5f){ out[i]=Vec3{0,0,0}; continue; }
	    PixelCtx c; c.normalRGB=nrm[i]; c.hasMatcap=true; c.matcap=matTex; out[i]=ShadePixelCtx(c, s6); }
	  writePPM("build/preview_matcap.ppm", out);
	  float mm = meanLum(out, cov);
	  std::printf("  matcap mean luma:      %.3f -> %s\n", mm, (mm > 0.02f && mm < 0.9f) ? "ok" : "FAIL");
	  if (!(mm > 0.02f && mm < 0.9f)) fail = 1; }

	// 7) Reflection: procedural sky/ground env; edges (grazing) should pick up more reflection.
	Shade s7; s7.display = DISPLAY_ALL;
	setSingleLight(s7, 45, 45, Vec3{1,1,1}, 1.0f);
	s7.diffuse.color = Vec3{0.2f,0.2f,0.22f}; s7.diffuse.ambient = 0.05f;
	s7.reflection.enable = true; s7.reflection.envMode = EnvMode::Spherical;
	s7.reflection.fresnel = 1.0f; s7.reflection.fresnelDepth = 3.0f; s7.reflection.ior = 1.6f;
	s7.reflection.blend = Blend::Add;
	TexFn envTex = [](float u, float v){
		// sky (top) to ground (bottom)
		Vec3 sky{0.5f,0.7f,1.0f}, ground{0.25f,0.2f,0.15f};
		return lerp(ground, sky, saturate(v));
	};
	{ std::vector<Vec3> out(W*H);
	  for (int i=0;i<W*H;++i){ if (cov[i]<0.5f){ out[i]=Vec3{0,0,0}; continue; }
	    PixelCtx c; c.normalRGB=nrm[i]; c.hasEnv=true; c.env=envTex; out[i]=ShadePixelCtx(c, s7); }
	  writePPM("build/preview_reflection.ppm", out); }

	std::printf("\nWrote build/preview_*.ppm\n== %s ==\n", fail ? "FAILURES" : "RENDER CHECKS PASSED");
	return fail;
}
