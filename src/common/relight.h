// relight.h — per-pixel relighting composite (Specularity engine), SDK-free.
// SINGLE SOURCE OF TRUTH shared by the AE plugin and the offline render/unit tests.
#pragma once
#include "shading.h"
#include "maps.h"
#include <vector>

namespace nm {

// Display / output modes (values mirror the AE popup, 1-based).
// NOTE: new modes are appended AFTER DISPLAY_OFF so existing persisted popup
// indices (1..15) never shift.
enum DisplayMode {
	DISPLAY_ALL = 1,
	DISPLAY_ADJUSTED_NORMALS,
	DISPLAY_ORIGINAL_NORMALS,
	DISPLAY_DEPTH,
	DISPLAY_DIFFUSE,
	DISPLAY_SPECULAR,
	DISPLAY_INCIDENCE,
	DISPLAY_RIM,
	DISPLAY_TOON,
	DISPLAY_GRADIENT,
	DISPLAY_MATCAP,
	DISPLAY_REFLECTION,
	DISPLAY_REFRACTION,
	DISPLAY_BUMP,
	DISPLAY_OFF,
	// --- Beeble PBR passes (appended; keep order stable) ---
	DISPLAY_SOURCE,        // 16
	DISPLAY_BASECOLOR,     // 17
	DISPLAY_ROUGHNESS,     // 18
	DISPLAY_METALLIC,      // 19
	DISPLAY_SPECULAR_MAP,  // 20
	DISPLAY_ALPHA          // 21
};

// Per-pixel inputs (normal-map color, screen uv, and optional map samplers).
struct PixelCtx {
	Vec3  normalRGB{0.5f, 0.5f, 1.0f};
	float u = 0.5f, v = 0.5f, du = 0.0f, dv = 0.0f;
	bool  hasMatcap = false;     TexFn matcap;
	bool  hasEnv = false;        TexFn env;
	bool  hasRefraction = false; TexFn refractionMap;
	bool  hasBump = false;       TexFn bumpMap;
	bool  hasDepth = false;      Vec3 depthRGB{0, 0, 0};
	// Reconstructed per-pixel position in shading space (comp-pixel units, +X right,
	// +Y up, +Z toward viewer). X/Y come from screen position, Z from the depth pass.
	// Lets point lights compute a real per-pixel direction + distance (localized light).
	Vec3  worldPos{0, 0, 0};
	// --- Beeble PBR passes (per-pixel samples) ---
	bool  hasSource = false;      Vec3  sourceRGB{0, 0, 0};   // beauty / original render
	bool  hasBaseColor = false;   Vec3  baseColorRGB{1, 1, 1};// albedo
	bool  hasRoughness = false;   float roughness = 0.5f;     // grayscale (luminance)
	bool  hasMetallic = false;    float metallic = 0.0f;      // grayscale (luminance)
	bool  hasSpecularMap = false; float specularLevel = 1.0f; // grayscale (luminance)
	bool  hasAlpha = false;       float alpha = 1.0f;         // coverage matte (luminance)
};

// Toggle state for the Beeble PBR passes (mirrors the AE "PBR Passes" checkboxes).
struct PBRParams {
	bool baseColor = false;
	bool roughness = false;
	bool metallic  = false;
	bool specular  = false;   // specular-level map
	bool alpha     = false;
	bool source    = false;
	bool roughInvert = false;
	bool metalInvert = false;
	bool alphaInvert = false;
};

// Full shading state (parity feature set).
struct Shade {
	int                display = DISPLAY_ALL;
	NormalOptions      norm;
	// Lights: any mix of in-effect and comp 3D lights are pushed here by the host.
	std::vector<Light> lights;
	GlobalLightParams  global;
	// Passes
	DiffuseParams      diffuse;
	SpecularParams     spec;
	IncidenceParams    inc;
	RimParams          rim;
	ToonParams         toon;
	GradientParams     gradient;
	MatcapParams       matcap;
	ReflectionParams   reflection;
	RefractionParams   refraction;
	BumpParams         bump;
	DepthParams        depth;
	PBRParams          pbr;        // Beeble PBR pass toggles
	bool               enableShading = true;
	float              exposure = 0.0f;
	float              gamma    = 1.0f;
};

inline Vec3 lightDirFromAngles(float azimuthDeg, float elevationDeg) {
	float a = deg2rad(azimuthDeg), e = deg2rad(elevationDeg);
	return normalize(Vec3{ std::cos(e) * std::cos(a), std::cos(e) * std::sin(a), std::sin(e) });
}

inline Vec3 encodeNormal(const Vec3& n) { return n * 0.5f + Vec3{0.5f, 0.5f, 0.5f}; }

// Composite a pass contribution over the running color using its blend + opacity.
inline Vec3 applyPass(const Vec3& col, const Vec3& contrib, Blend blend, float opacity) {
	return composite(blend, col, contrib, opacity);
}

// Shade one pixel. `ctx` carries per-pixel data; `S` the parameters.
inline Vec3 ShadePixelCtx(const PixelCtx& ctx, const Shade& S) {
	NormalOptions plain;
	Vec3 origN = decodeNormal(ctx.normalRGB, plain);
	Vec3 N     = decodeNormal(ctx.normalRGB, S.norm);
	if (S.bump.enable && ctx.hasBump)
		N = perturbNormalFromBump(N, ctx.u, ctx.v, ctx.du, ctx.dv, S.bump, ctx.bumpMap);
	Vec3 V{0, 0, 1};

	// --- Beeble PBR pass modulation ---
	// BaseColor drives per-pixel albedo, Metallic kills diffuse + tints specular,
	// Roughness drives specular spread, the Specular map scales specular intensity.
	Vec3  albedo{1, 1, 1};
	bool  haveAlbedo = S.pbr.baseColor && ctx.hasBaseColor;
	if (haveAlbedo) albedo = ctx.baseColorRGB;
	float metal = (S.pbr.metallic && ctx.hasMetallic)
	              ? saturate(S.pbr.metalInvert ? 1.0f - ctx.metallic : ctx.metallic) : 0.0f;
	bool  haveRough = S.pbr.roughness && ctx.hasRoughness;
	float rough = haveRough ? saturate(S.pbr.roughInvert ? 1.0f - ctx.roughness : ctx.roughness) : 0.0f;
	bool  haveSpecMap = S.pbr.specular && ctx.hasSpecularMap;
	float specLevel = haveSpecMap ? saturate(ctx.specularLevel) : 1.0f;

	// Per-pixel diffuse params: tint by albedo, attenuate for metals.
	DiffuseParams dp = S.diffuse;
	if (haveAlbedo)   dp.color = dp.color * albedo;
	if (metal > 0.0f) dp.color = dp.color * (1.0f - metal);

	// Per-pixel specular params: roughness->spread, spec-level->intensity, metal tint.
	SpecularParams sp = S.spec;
	bool pbrSpec = haveRough || haveSpecMap || (metal > 0.0f);
	sp.enable = S.spec.enable || pbrSpec;
	if (haveRough)   sp.spread = lerpf(2.0f, std::max(1.0f, S.spec.spread), 1.0f - rough);
	if (haveSpecMap) sp.intensity = sp.intensity * specLevel;
	if (metal > 0.0f && haveAlbedo) sp.color = lerp(sp.color, albedo, metal);

	// Passes evaluated up-front where cheap.
	Vec3 diffuseCol = shadeDiffuse(N, ctx.worldPos, S.lights, S.global, dp);
	if (S.toon.enable) diffuseCol = shadeToon(diffuseCol, S.toon);

	switch (S.display) {
		case DISPLAY_OFF:              return ctx.normalRGB;
		case DISPLAY_ORIGINAL_NORMALS: return encodeNormal(origN);
		case DISPLAY_ADJUSTED_NORMALS: return encodeNormal(N);
		case DISPLAY_BUMP:             return encodeNormal(N);
		case DISPLAY_DEPTH: {
			float d = 0; if (ctx.hasDepth) decodeDepth(ctx.depthRGB, S.depth, d);
			d = saturate(d); return {d, d, d};
		}
		case DISPLAY_DIFFUSE:   return diffuseCol;
		case DISPLAY_SPECULAR:  return shadeSpecular(N, V, ctx.worldPos, S.lights, S.global, sp);
		case DISPLAY_INCIDENCE: return shadeIncidence(N, V, S.inc);
		case DISPLAY_RIM:       return shadeRim(N, V, S.lights, S.rim);
		case DISPLAY_TOON:      return diffuseCol;
		case DISPLAY_GRADIENT:  return shadeGradient(N, S.gradient);
		case DISPLAY_MATCAP: {
			bool hit; Vec3 c = shadeMatcap(N, S.matcap, ctx.matcap, hit); return hit ? c : Vec3{0,0,0};
		}
		case DISPLAY_REFLECTION: {
			float w; return shadeReflection(N, V, S.reflection, ctx.env, w) * w;
		}
		case DISPLAY_REFRACTION: {
			float w; return shadeRefraction(N, V, ctx.u, ctx.v, S.refraction, ctx.refractionMap, w) * w;
		}
		// --- Beeble PBR pass previews (show the raw pass, decoded) ---
		case DISPLAY_SOURCE:    return ctx.hasSource ? ctx.sourceRGB : ctx.normalRGB;
		case DISPLAY_BASECOLOR: return ctx.hasBaseColor ? ctx.baseColorRGB : Vec3{0,0,0};
		case DISPLAY_ROUGHNESS: { float r = ctx.hasRoughness
			? saturate(S.pbr.roughInvert ? 1.0f - ctx.roughness : ctx.roughness) : 0.0f; return {r,r,r}; }
		case DISPLAY_METALLIC:  { float m = ctx.hasMetallic
			? saturate(S.pbr.metalInvert ? 1.0f - ctx.metallic : ctx.metallic) : 0.0f; return {m,m,m}; }
		case DISPLAY_SPECULAR_MAP: { float s = ctx.hasSpecularMap ? saturate(ctx.specularLevel) : 0.0f; return {s,s,s}; }
		case DISPLAY_ALPHA:     { float a = ctx.hasAlpha
			? saturate(S.pbr.alphaInvert ? 1.0f - ctx.alpha : ctx.alpha) : 1.0f; return {a,a,a}; }
		case DISPLAY_ALL:
		default: break;
	}

	if (!S.enableShading) return ctx.normalRGB;

	// --- ALL: composite the full stack ---
	Vec3 col = diffuseCol;

	if (sp.enable) {
		Vec3 c = shadeSpecular(N, V, ctx.worldPos, S.lights, S.global, sp);
		col = applyPass(col, c, sp.blend, sp.opacity);
	}
	if (S.inc.enable)
		col = applyPass(col, shadeIncidence(N, V, S.inc), S.inc.blend, S.inc.opacity);
	if (S.rim.enable)
		col = applyPass(col, shadeRim(N, V, S.lights, S.rim), S.rim.blend, S.rim.opacity);
	if (S.gradient.enable)
		col = applyPass(col, shadeGradient(N, S.gradient), S.gradient.blend, S.gradient.opacity);
	if (S.matcap.enable && ctx.hasMatcap) {
		bool hit; Vec3 c = shadeMatcap(N, S.matcap, ctx.matcap, hit);
		if (hit) col = applyPass(col, c, S.matcap.blend, S.matcap.opacity);
	}
	if (S.reflection.enable && ctx.hasEnv) {
		float w; Vec3 c = shadeReflection(N, V, S.reflection, ctx.env, w);
		col = applyPass(col, c, S.reflection.blend, w * S.reflection.opacity);
	}
	if (S.refraction.enable && ctx.hasRefraction) {
		float w; Vec3 c = shadeRefraction(N, V, ctx.u, ctx.v, S.refraction, ctx.refractionMap, w);
		col = applyPass(col, c, S.refraction.blend, w * S.refraction.opacity);
	}

	return applyExposureGamma(col, S.exposure, S.gamma);
}

// Convenience overload for the no-maps case (used by simple callers/tests).
inline Vec3 ShadePixel(const Vec3& normalRGB, const Shade& S) {
	PixelCtx ctx; ctx.normalRGB = normalRGB;
	return ShadePixelCtx(ctx, S);
}

// Build a one-light Shade from azimuth/elevation UI angles (helper for tests/simple use).
inline void setSingleLight(Shade& S, float azimuthDeg, float elevationDeg, const Vec3& color, float intensity) {
	S.lights.clear();
	Light L; L.type = LightType::Directional;
	L.dir = lightDirFromAngles(azimuthDeg, elevationDeg);
	L.color = color; L.intensity = intensity;
	S.lights.push_back(L);
}

} // namespace nm
