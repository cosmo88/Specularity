/*
	Specularity.cpp — native Universal After Effects effect.
	Normal-pass / PBR relighting: multi-pass shading + map-based passes (matcap, reflection,
	refraction, bump, depth) + Beeble PBR passes. Uses the portable, unit-tested engine in
	../common. Clean-room, original code; inspired by Stefan Minning's Normality.
*/
#include "Specularity.h"
#include "relight.h"   // ../common — shared relighting engine (single source of truth)
#include <type_traits>
#include <cstring>

using namespace nm;

/* ---------------- pixel traits ---------------- */
template<typename PIX> struct PixTraits;
template<> struct PixTraits<PF_Pixel8>    { static constexpr float MAXV = (float)PF_MAX_CHAN8;  };
template<> struct PixTraits<PF_Pixel16>   { static constexpr float MAXV = (float)PF_MAX_CHAN16; };
template<> struct PixTraits<PF_PixelFloat>{ static constexpr float MAXV = 1.0f; };

/* Bilinear-sample a checked-out layer world at uv in [0,1] -> RGB (edge-clamped). */
template<typename PIX>
static Vec3 sampleWorld(const PF_EffectWorld* w, float u, float v) {
	const float MAXV = PixTraits<PIX>::MAXV;
	int W = w->width, H = w->height;
	float fx = clampf(u, 0.f, 1.f) * (W - 1);
	float fy = clampf(v, 0.f, 1.f) * (H - 1);
	int x0 = (int)fx, y0 = (int)fy;
	int x1 = x0 + 1 < W ? x0 + 1 : W - 1;
	int y1 = y0 + 1 < H ? y0 + 1 : H - 1;
	float tx = fx - x0, ty = fy - y0;
	auto at = [&](int x, int y) {
		const PIX* row = (const PIX*)((const char*)w->data + (size_t)y * w->rowbytes);
		const PIX& p = row[x];
		return Vec3{ (float)p.red / MAXV, (float)p.green / MAXV, (float)p.blue / MAXV };
	};
	Vec3 a = at(x0, y0), b = at(x1, y0), c = at(x0, y1), d = at(x1, y1);
	return lerp(lerp(a, b, tx), lerp(c, d, tx), ty);
}

/* ---------------- enum converters (popup 1-based -> engine enums) ---------------- */
static Blend toBlend(A_long v) { int i = (int)v - 1; if (i < 0) i = 0; if (i >= (int)Blend::Count) i = (int)Blend::Count - 1; return (Blend)i; }
static Falloff toFalloff(A_long v) { int i = (int)v - 1; if (i < 0) i = 0; if (i > 3) i = 3; return (Falloff)i; }
static Tile toTile(A_long v) { int i = (int)v - 1; if (i < 0) i = 0; if (i > 3) i = 3; return (Tile)i; }
static EnvMode toEnv(A_long v) { return v == 2 ? EnvMode::Panorama : EnvMode::Spherical; }
static DepthChannel toDChan(A_long v) { int i = (int)v - 1; if (i < 0) i = 0; if (i > 4) i = 4; return (DepthChannel)i; }
static Vec3 colVec(const PF_Pixel8& c) { return { c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f }; }

/* ---------------- AE command handlers ---------------- */
static PF_Err About(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef*[], PF_LayerDef*) {
	PF_SPRINTF(out_data->return_msg, "%s v%d.%d\r%s", NM_NAME, MAJOR_VERSION, MINOR_VERSION, NM_DESCRIPTION);
	return PF_Err_NONE;
}

static PF_Err GlobalSetup(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef*[], PF_LayerDef*) {
	out_data->my_version = PF_VERSION(MAJOR_VERSION, MINOR_VERSION, BUG_VERSION, STAGE_VERSION, BUILD_VERSION);
	out_data->out_flags  = PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_PIX_INDEPENDENT | PF_OutFlag_USE_OUTPUT_EXTENT;
	// Comp 3D lights are read in PreRender (single-threaded), snapshotted into
	// pre_render_data, and mixed into the frame GUID (I_MIX_GUID_DEPENDENCIES) so moving a
	// light invalidates the cache. SmartRender then touches no AEGP suites -> safe to thread.
	out_data->out_flags2 = PF_OutFlag2_SUPPORTS_SMART_RENDER |
	                       PF_OutFlag2_FLOAT_COLOR_AWARE |
	                       PF_OutFlag2_I_MIX_GUID_DEPENDENCIES |
	                       PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
	return PF_Err_NONE;
}

/* Convenience wrappers (all rely on a `PF_ParamDef def;` in scope, and `in_data`). */
#define D(idx) (DISK_BASE + (idx))
#define ADD_TOPIC(NM_, IDX)  do{ AEFX_CLR_STRUCT(def); PF_ADD_TOPIC(NM_, D(IDX)); }while(0)
#define END_TOPIC(IDX)       do{ AEFX_CLR_STRUCT(def); PF_END_TOPIC(D(IDX)); }while(0)
#define ADD_POPUP(NM_,CNT,DFLT,STR,IDX) do{ AEFX_CLR_STRUCT(def); PF_ADD_POPUP(NM_,CNT,DFLT,STR,D(IDX)); }while(0)
#define ADD_CHECK(NM_,DFLT,IDX) do{ AEFX_CLR_STRUCT(def); PF_ADD_CHECKBOXX(NM_,DFLT,0,D(IDX)); }while(0)
#define ADD_COLOR(NM_,R,G,B,IDX) do{ AEFX_CLR_STRUCT(def); PF_ADD_COLOR(NM_,R,G,B,D(IDX)); }while(0)
#define ADD_ANGLE(NM_,DFLT,IDX) do{ AEFX_CLR_STRUCT(def); PF_ADD_ANGLE(NM_,DFLT,D(IDX)); }while(0)
#define ADD_FLOAT(NM_,VMIN,VMAX,SMIN,SMAX,DFLT,PREC,IDX) do{ AEFX_CLR_STRUCT(def); \
	PF_ADD_FLOAT_SLIDERX(NM_,VMIN,VMAX,SMIN,SMAX,DFLT,PREC,PF_ValueDisplayFlag_NONE,0,D(IDX)); }while(0)
#define ADD_LAYER_MYSELF(NM_,IDX) do{ AEFX_CLR_STRUCT(def); PF_ADD_LAYER(NM_,PF_LayerDefault_MYSELF,D(IDX)); }while(0)
#define ADD_LAYER_NONE(NM_,IDX)   do{ AEFX_CLR_STRUCT(def); PF_ADD_LAYER(NM_,PF_LayerDefault_NONE,D(IDX)); }while(0)

static PF_Err ParamsSetup(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef*[], PF_LayerDef*) {
	PF_Err err = PF_Err_NONE;
	PF_ParamDef def;

	ADD_POPUP("Display", NM_DISPLAY_COUNT, DISPLAY_ALL, NM_DISPLAY_CHOICES, NM_DISPLAY);

	// --- Passes (all layer inputs in one group: Normals, Depth, + Beeble PBR) ---
	// Feed a Beeble export as separate layers. Each pass is selectable (its own layer
	// picker) and toggleable (Use checkbox); the toggle only bites if a layer is connected.
	ADD_TOPIC("PBR Passes (Beeble)", NM_PBR_TOPIC);
	// Normals
	ADD_LAYER_MYSELF("Normal Map", NM_NORMAL_MAP);
	ADD_CHECK("Invert X", FALSE, NM_INVERT_X);
	ADD_CHECK("Invert Y", FALSE, NM_INVERT_Y);
	ADD_CHECK("Invert Z", FALSE, NM_INVERT_Z);
	// Depth
	ADD_LAYER_NONE("Depth Map", NM_DEPTH_MAP);
	ADD_CHECK("Invert Depth", FALSE, NM_DEPTH_INVERT);
	ADD_POPUP("Depth Channel", 5, 1, NM_DCHAN_CHOICES, NM_DEPTH_CHANNEL);
	ADD_FLOAT("Depth Gamma", 0.1, 4, 0.1, 4, 1.0, 2, NM_DEPTH_GAMMA);
	ADD_FLOAT("Depth Multiplier", 0, 8, 0, 4, 1.0, 2, NM_DEPTH_MULT);
	// Beeble PBR passes
	ADD_CHECK("Use Source", FALSE, NM_PBR_SOURCE_USE);
	ADD_LAYER_NONE("Source RGB", NM_PBR_SOURCE_MAP);
	ADD_CHECK("Use BaseColor", FALSE, NM_PBR_BASECOLOR_USE);
	ADD_LAYER_NONE("BaseColor", NM_PBR_BASECOLOR_MAP);
	ADD_CHECK("Use Roughness", FALSE, NM_PBR_ROUGH_USE);
	ADD_LAYER_NONE("Roughness", NM_PBR_ROUGH_MAP);
	ADD_CHECK("Invert Roughness", FALSE, NM_PBR_ROUGH_INVERT);
	ADD_CHECK("Use Metallic", FALSE, NM_PBR_METAL_USE);
	ADD_LAYER_NONE("Metallic", NM_PBR_METAL_MAP);
	ADD_CHECK("Invert Metallic", FALSE, NM_PBR_METAL_INVERT);
	ADD_CHECK("Use Specular Map", FALSE, NM_PBR_SPEC_USE);
	ADD_LAYER_NONE("Specular", NM_PBR_SPEC_MAP);
	ADD_CHECK("Use Alpha", FALSE, NM_PBR_ALPHA_USE);
	ADD_LAYER_NONE("Alpha", NM_PBR_ALPHA_MAP);
	ADD_CHECK("Invert Alpha", FALSE, NM_PBR_ALPHA_INVERT);
	END_TOPIC(NM_PBR_TOPIC_END);

	// --- Lights ---
	ADD_TOPIC("Lights", NM_LIGHT_TOPIC);
	ADD_CHECK("Use Comp 3D Lights", FALSE, NM_USE_COMP_LIGHTS);
	ADD_ANGLE("Azimuth", 45, NM_LIGHT_AZIMUTH);
	ADD_ANGLE("Elevation", 45, NM_LIGHT_ELEVATION);
	ADD_FLOAT("Intensity", 0, 10, 0, 4, 1.0, 2, NM_LIGHT_INTENSITY);
	ADD_COLOR("Light Color", 255, 255, 255, NM_LIGHT_COLOR);
	ADD_FLOAT("Ambient", 0, 1, 0, 1, 0.05, 3, NM_AMBIENT);
	ADD_POPUP("Falloff Mode", 4, 1, NM_FALLOFF_CHOICES, NM_FALLOFF_MODE);
	ADD_FLOAT("Falloff", 0, 4, 0, 2, 1.0, 2, NM_FALLOFF);
	ADD_FLOAT("Exposure", -5, 5, -5, 5, 0.0, 2, NM_EXPOSURE);
	ADD_FLOAT("Gamma", 0.1, 4, 0.1, 4, 1.0, 2, NM_GAMMA);
	END_TOPIC(NM_LIGHT_TOPIC_END);

	// --- Shading ---
	ADD_TOPIC("Shading", NM_SHADE_TOPIC);
	ADD_CHECK("Enable Shading", TRUE, NM_ENABLE_SHADING);
	// Diffuse
	ADD_COLOR("Diffuse Color", 255, 255, 255, NM_DIFF_COLOR);
	ADD_FLOAT("Incandescence", 0, 1, 0, 1, 0.0, 3, NM_DIFF_INCAND);
	ADD_POPUP("Diffuse Blend", NM_BLEND_COUNT, 1, NM_BLEND_CHOICES, NM_DIFF_BLEND);
	// Specular
	ADD_CHECK("Specular", FALSE, NM_SPEC_ENABLE);
	ADD_FLOAT("Specular Intensity", 0, 4, 0, 4, 1.0, 2, NM_SPEC_INTENSITY);
	ADD_FLOAT("Specular Spread", 1, 200, 1, 100, 20.0, 1, NM_SPEC_SPREAD);
	ADD_COLOR("Specular Color", 255, 255, 255, NM_SPEC_COLOR);
	ADD_POPUP("Specular Blend", NM_BLEND_COUNT, 10, NM_BLEND_CHOICES, NM_SPEC_BLEND); // Add
	// Incidence
	ADD_CHECK("Incidence", FALSE, NM_INC_ENABLE);
	ADD_FLOAT("Incidence Intensity", 0, 4, 0, 4, 1.0, 2, NM_INC_INTENSITY);
	ADD_FLOAT("Incidence Falloff", 0.1, 8, 0.1, 8, 2.0, 2, NM_INC_FALLOFF);
	ADD_COLOR("Incidence Color", 255, 255, 255, NM_INC_COLOR);
	ADD_POPUP("Incidence Blend", NM_BLEND_COUNT, 1, NM_BLEND_CHOICES, NM_INC_BLEND);
	// Rim
	ADD_CHECK("Rim Light", FALSE, NM_RIM_ENABLE);
	ADD_FLOAT("Rim Size", 0, 4, 0, 4, 1.0, 2, NM_RIM_SIZE);
	ADD_FLOAT("Rim Width", 0.1, 8, 0.1, 8, 2.0, 2, NM_RIM_WIDTH);
	ADD_ANGLE("Rim Angle", 0, NM_RIM_ANGLE);
	ADD_COLOR("Rim Color", 255, 255, 255, NM_RIM_COLOR);
	ADD_POPUP("Rim Blend", NM_BLEND_COUNT, 10, NM_BLEND_CHOICES, NM_RIM_BLEND); // Add
	// Toon
	ADD_CHECK("Toon", FALSE, NM_TOON_ENABLE);
	ADD_FLOAT("Smoothing", 0.001, 0.5, 0.001, 0.3, 0.03, 3, NM_TOON_SMOOTH);
	ADD_FLOAT("Shadow Intensity", 0, 1, 0, 1, 0.6, 2, NM_TOON_SHADOW_INT);
	ADD_FLOAT("Shadow Width", 0, 1, 0, 1, 0.5, 2, NM_TOON_SHADOW_WIDTH);
	ADD_COLOR("Shadow Color", 0, 0, 0, NM_TOON_SHADOW_COLOR);
	ADD_FLOAT("Highlight Intensity", 0, 2, 0, 2, 0.0, 2, NM_TOON_HILIGHT_INT);
	ADD_POPUP("Toon Blend", NM_BLEND_COUNT, 1, NM_BLEND_CHOICES, NM_TOON_BLEND);
	// Gradient
	ADD_CHECK("Gradient", FALSE, NM_GRAD_ENABLE);
	ADD_COLOR("Gradient Top", 180, 200, 255, NM_GRAD_TOP);
	ADD_COLOR("Gradient Bottom", 40, 30, 20, NM_GRAD_BOT);
	ADD_POPUP("Gradient Blend", NM_BLEND_COUNT, 1, NM_BLEND_CHOICES, NM_GRAD_BLEND);
	// Matcap
	ADD_CHECK("Matcap", FALSE, NM_MATCAP_ENABLE);
	ADD_LAYER_NONE("Matcap Map", NM_MATCAP_MAP);
	ADD_FLOAT("Matcap Offset X", -1, 1, -1, 1, 0.0, 3, NM_MATCAP_OFFX);
	ADD_FLOAT("Matcap Offset Y", -1, 1, -1, 1, 0.0, 3, NM_MATCAP_OFFY);
	ADD_POPUP("Matcap Tile", 4, 2, NM_TILE_CHOICES, NM_MATCAP_TILE); // Edge
	ADD_POPUP("Matcap Blend", NM_BLEND_COUNT, 1, NM_BLEND_CHOICES, NM_MATCAP_BLEND);
	// Reflection
	ADD_CHECK("Reflection", FALSE, NM_REFL_ENABLE);
	ADD_LAYER_NONE("Environment Map", NM_REFL_MAP);
	ADD_POPUP("Environment Mode", 2, 1, NM_ENVMODE_CHOICES, NM_REFL_ENVMODE);
	ADD_FLOAT("Reflection Gamma", 0.1, 4, 0.1, 4, 1.0, 2, NM_REFL_GAMMA);
	ADD_POPUP("Reflection Tile", 4, 3, NM_TILE_CHOICES, NM_REFL_TILE); // Repeat
	ADD_COLOR("Reflection Color", 255, 255, 255, NM_REFL_COLOR);
	ADD_FLOAT("Reflection IOR", 1, 3, 1, 3, 1.45, 2, NM_REFL_IOR);
	ADD_FLOAT("Reflection Fresnel", 0, 1, 0, 1, 1.0, 2, NM_REFL_FRESNEL);
	ADD_FLOAT("Reflection Fresnel Depth", 0.1, 10, 0.1, 10, 5.0, 2, NM_REFL_FRESNEL_DEPTH);
	ADD_POPUP("Reflection Blend", NM_BLEND_COUNT, 10, NM_BLEND_CHOICES, NM_REFL_BLEND); // Add
	// Refraction
	ADD_CHECK("Refraction", FALSE, NM_REFR_ENABLE);
	ADD_LAYER_NONE("Refraction Map", NM_REFR_MAP);
	ADD_FLOAT("Refraction IOR", 1, 3, 1, 2, 1.1, 2, NM_REFR_IOR);
	ADD_FLOAT("Refraction Gamma", 0.1, 4, 0.1, 4, 1.0, 2, NM_REFR_GAMMA);
	ADD_POPUP("Refraction Tile", 4, 2, NM_TILE_CHOICES, NM_REFR_TILE); // Edge
	ADD_FLOAT("Refraction Scale", 0, 1, 0, 0.5, 0.1, 3, NM_REFR_SCALE);
	ADD_FLOAT("Refraction Offset X", -1, 1, -1, 1, 0.0, 3, NM_REFR_OFFX);
	ADD_FLOAT("Refraction Offset Y", -1, 1, -1, 1, 0.0, 3, NM_REFR_OFFY);
	ADD_COLOR("Refraction Color", 255, 255, 255, NM_REFR_COLOR);
	ADD_FLOAT("Refraction Fresnel", 0, 1, 0, 1, 1.0, 2, NM_REFR_FRESNEL);
	ADD_FLOAT("Refraction Fresnel Depth", 0.1, 10, 0.1, 10, 5.0, 2, NM_REFR_FRESNEL_DEPTH);
	ADD_POPUP("Refraction Blend", NM_BLEND_COUNT, 1, NM_BLEND_CHOICES, NM_REFR_BLEND);
	// Bump
	ADD_CHECK("Bump", FALSE, NM_BUMP_ENABLE);
	ADD_LAYER_NONE("Bump Map", NM_BUMP_MAP);
	ADD_FLOAT("Bump Intensity", 0, 8, 0, 4, 1.0, 2, NM_BUMP_INTENSITY);
	ADD_POPUP("Bump Tile", 4, 2, NM_TILE_CHOICES, NM_BUMP_TILE); // Edge
	ADD_FLOAT("Bump Scale", 0.1, 4, 0.1, 4, 1.0, 2, NM_BUMP_SCALE);
	END_TOPIC(NM_SHADE_TOPIC_END);

	out_data->num_params = NM_NUM_PARAMS;
	return err;
}

/* Read a single scalar param helper via PF_CHECKOUT_PARAM. */
struct PReader {
	PF_InData* in;
	PF_Err err = PF_Err_NONE;
	bool have = false;
	PReader(PF_InData* i) : in(i) {}
	// Checks in the previously checked-out param (still resident in `p`) before checking
	// out the next, so every checkout is balanced by exactly one checkin.
	bool get(int idx, PF_ParamDef& p) {
		if (have) { PF_CHECKIN_PARAM(in, &p); have = false; }
		AEFX_CLR_STRUCT(p);
		err |= PF_CHECKOUT_PARAM(in, idx, in->current_time, in->time_step, in->time_scale, &p);
		have = true;
		return err == PF_Err_NONE;
	}
	void finish(PF_ParamDef& p) { if (have) { PF_CHECKIN_PARAM(in, &p); have = false; } }
};

static PF_Err ReadParams(PF_InData *in_data, Shade& S) {
	PReader R(in_data);
	PF_ParamDef p;
	#define GET(IDX) R.get((IDX), p)
	#define FVAL (p.u.fs_d.value)
	#define BVAL (p.u.bd.value != 0)
	#define PVAL (p.u.pd.value)
	#define AVAL (p.u.ad.value / 65536.0f)
	#define CVAL (colVec(p.u.cd.value))

	GET(NM_DISPLAY);        S.display = (int)PVAL;

	GET(NM_INVERT_X);       S.norm.invX = BVAL;
	GET(NM_INVERT_Y);       S.norm.invY = BVAL;
	GET(NM_INVERT_Z);       S.norm.invZ = BVAL;

	GET(NM_DEPTH_INVERT);   S.depth.invert = BVAL;
	GET(NM_DEPTH_CHANNEL);  S.depth.channel = toDChan(PVAL);
	GET(NM_DEPTH_GAMMA);    S.depth.gamma = (float)FVAL;
	GET(NM_DEPTH_MULT);     S.depth.multiplier = (float)FVAL;

	float az = 45, el = 45; Vec3 lcol{1,1,1}; float lint = 1;
	GET(NM_LIGHT_AZIMUTH);  az = AVAL;
	GET(NM_LIGHT_ELEVATION);el = AVAL;
	GET(NM_LIGHT_INTENSITY);lint = (float)FVAL;
	GET(NM_LIGHT_COLOR);    lcol = CVAL;
	GET(NM_AMBIENT);        S.diffuse.ambient = (float)FVAL;
	GET(NM_FALLOFF_MODE);   S.global.falloff = toFalloff(PVAL);
	GET(NM_FALLOFF);        S.global.falloffScale = (float)FVAL;
	GET(NM_EXPOSURE);       S.exposure = (float)FVAL;
	GET(NM_GAMMA);          S.gamma = (float)FVAL;
	setSingleLight(S, az, el, lcol, lint);   // in-effect key light (comp lights appended later)

	GET(NM_ENABLE_SHADING); S.enableShading = BVAL;
	GET(NM_DIFF_COLOR);     S.diffuse.color = CVAL;
	GET(NM_DIFF_INCAND);    S.diffuse.incandescence = (float)FVAL;
	GET(NM_DIFF_BLEND);     S.diffuse.blend = toBlend(PVAL);

	GET(NM_SPEC_ENABLE);    S.spec.enable = BVAL;
	GET(NM_SPEC_INTENSITY); S.spec.intensity = (float)FVAL;
	GET(NM_SPEC_SPREAD);    S.spec.spread = (float)FVAL;
	GET(NM_SPEC_COLOR);     S.spec.color = CVAL;
	GET(NM_SPEC_BLEND);     S.spec.blend = toBlend(PVAL);

	GET(NM_INC_ENABLE);     S.inc.enable = BVAL;
	GET(NM_INC_INTENSITY);  S.inc.intensity = (float)FVAL;
	GET(NM_INC_FALLOFF);    S.inc.falloff = (float)FVAL;
	GET(NM_INC_COLOR);      S.inc.color = CVAL;
	GET(NM_INC_BLEND);      S.inc.blend = toBlend(PVAL);

	GET(NM_RIM_ENABLE);     S.rim.enable = BVAL;
	GET(NM_RIM_SIZE);       S.rim.size = (float)FVAL;
	GET(NM_RIM_WIDTH);      S.rim.width = (float)FVAL;
	GET(NM_RIM_ANGLE);      S.rim.angle = AVAL;
	GET(NM_RIM_COLOR);      S.rim.color = CVAL;
	GET(NM_RIM_BLEND);      S.rim.blend = toBlend(PVAL);

	GET(NM_TOON_ENABLE);        S.toon.enable = BVAL;
	GET(NM_TOON_SMOOTH);        S.toon.smoothing = (float)FVAL;
	GET(NM_TOON_SHADOW_INT);    S.toon.shadowIntensity = (float)FVAL;
	GET(NM_TOON_SHADOW_WIDTH);  S.toon.shadowWidth = (float)FVAL;
	GET(NM_TOON_SHADOW_COLOR);  S.toon.shadowColor = CVAL;
	GET(NM_TOON_HILIGHT_INT);   S.toon.highlightIntensity = (float)FVAL;
	GET(NM_TOON_BLEND);         S.toon.blend = toBlend(PVAL);

	GET(NM_GRAD_ENABLE);    S.gradient.enable = BVAL;
	GET(NM_GRAD_TOP);       S.gradient.topColor = CVAL;
	GET(NM_GRAD_BOT);       S.gradient.botColor = CVAL;
	GET(NM_GRAD_BLEND);     S.gradient.blend = toBlend(PVAL);

	GET(NM_MATCAP_ENABLE);  S.matcap.enable = BVAL;
	GET(NM_MATCAP_OFFX);    S.matcap.offsetX = (float)FVAL;
	GET(NM_MATCAP_OFFY);    S.matcap.offsetY = (float)FVAL;
	GET(NM_MATCAP_TILE);    S.matcap.mode = toTile(PVAL);
	GET(NM_MATCAP_BLEND);   S.matcap.blend = toBlend(PVAL);

	GET(NM_REFL_ENABLE);        S.reflection.enable = BVAL;
	GET(NM_REFL_ENVMODE);       S.reflection.envMode = toEnv(PVAL);
	GET(NM_REFL_GAMMA);         S.reflection.gamma = (float)FVAL;
	GET(NM_REFL_TILE);          S.reflection.tile = toTile(PVAL);
	GET(NM_REFL_COLOR);         S.reflection.color = CVAL;
	GET(NM_REFL_IOR);           S.reflection.ior = (float)FVAL;
	GET(NM_REFL_FRESNEL);       S.reflection.fresnel = (float)FVAL;
	GET(NM_REFL_FRESNEL_DEPTH); S.reflection.fresnelDepth = (float)FVAL;
	GET(NM_REFL_BLEND);         S.reflection.blend = toBlend(PVAL);

	GET(NM_REFR_ENABLE);        S.refraction.enable = BVAL;
	GET(NM_REFR_IOR);           S.refraction.ior = (float)FVAL;
	GET(NM_REFR_GAMMA);         S.refraction.gamma = (float)FVAL;
	GET(NM_REFR_TILE);          S.refraction.tile = toTile(PVAL);
	GET(NM_REFR_SCALE);         S.refraction.scale = (float)FVAL;
	GET(NM_REFR_OFFX);          S.refraction.offsetX = (float)FVAL;
	GET(NM_REFR_OFFY);          S.refraction.offsetY = (float)FVAL;
	GET(NM_REFR_COLOR);         S.refraction.color = CVAL;
	GET(NM_REFR_FRESNEL);       S.refraction.fresnel = (float)FVAL;
	GET(NM_REFR_FRESNEL_DEPTH); S.refraction.fresnelDepth = (float)FVAL;
	GET(NM_REFR_BLEND);         S.refraction.blend = toBlend(PVAL);

	GET(NM_BUMP_ENABLE);    S.bump.enable = BVAL;
	GET(NM_BUMP_INTENSITY); S.bump.intensity = (float)FVAL;
	GET(NM_BUMP_TILE);      S.bump.tile = toTile(PVAL);
	GET(NM_BUMP_SCALE);     S.bump.scale = (float)FVAL;

	// Beeble PBR passes (per-pass Use toggles + inverts; the layers themselves are
	// checked out in Pre/SmartRender). Toggle only takes effect if a layer is connected.
	GET(NM_PBR_SOURCE_USE);    S.pbr.source = BVAL;
	GET(NM_PBR_BASECOLOR_USE); S.pbr.baseColor = BVAL;
	GET(NM_PBR_ROUGH_USE);     S.pbr.roughness = BVAL;
	GET(NM_PBR_ROUGH_INVERT);  S.pbr.roughInvert = BVAL;
	GET(NM_PBR_METAL_USE);     S.pbr.metallic = BVAL;
	GET(NM_PBR_METAL_INVERT);  S.pbr.metalInvert = BVAL;
	GET(NM_PBR_SPEC_USE);      S.pbr.specular = BVAL;
	GET(NM_PBR_ALPHA_USE);     S.pbr.alpha = BVAL;
	GET(NM_PBR_ALPHA_INVERT);  S.pbr.alphaInvert = BVAL;

	R.finish(p);   // balance the final checkout
	#undef GET
	return R.err;
}

/* ---------------- comp 3D lights (AEGP) ---------------- */
#define NM_MAX_COMP_LIGHTS 32

// POD snapshot passed from PreRender -> SmartRender via pre_render_data (AE owns/frees it).
// Also fed to GuidMixInPtr so cache invalidates when a comp light changes.
struct CompLightData {
	A_long useComp;   // "Use Comp 3D Lights" checkbox state (snapshotted in PreRender)
	A_long count;
	Light  lights[NM_MAX_COMP_LIGHTS];
};

// Transform a world (comp) point into the effect layer's LOCAL space (layer pixels,
// top-left origin) using the inverse of its layer->world matrix. Row-vector convention:
// p_world = p_layer * M, with translation in row 3. This is the piece the original does
// via the QueryXform suite / CalInvTransSpaceMatrix — it makes a 3D light's position line
// up with the layer's pixels even when the layer is scaled / rotated / repositioned or its
// anchor isn't centered. Returns false if the 3x3 is degenerate.
static bool WorldToLayer(const A_Matrix4& M, const Vec3& w, Vec3& out) {
	double a=M.mat[0][0], b=M.mat[0][1], c=M.mat[0][2];
	double d=M.mat[1][0], e=M.mat[1][1], f=M.mat[1][2];
	double g=M.mat[2][0], h=M.mat[2][1], i=M.mat[2][2];
	double det = a*(e*i - f*h) - b*(d*i - f*g) + c*(d*h - e*g);
	if (std::fabs(det) < 1e-9) return false;
	double id = 1.0 / det;
	double r00=(e*i-f*h)*id, r01=(c*h-b*i)*id, r02=(b*f-c*e)*id;   // inverse of the 3x3
	double r10=(f*g-d*i)*id, r11=(a*i-c*g)*id, r12=(c*d-a*f)*id;
	double r20=(d*h-e*g)*id, r21=(b*g-a*h)*id, r22=(a*e-b*d)*id;
	double vx = w.x - M.mat[3][0], vy = w.y - M.mat[3][1], vz = w.z - M.mat[3][2];
	out.x = (float)(vx*r00 + vy*r10 + vz*r20);   // (w - T) * Rinv   (row vector)
	out.y = (float)(vx*r01 + vy*r11 + vz*r21);
	out.z = (float)(vx*r02 + vy*r12 + vz*r22);
	return true;
}

// Read the effect comp's 3D lights and map them into shading space.
// Each light's world position is transformed into the effect layer's LOCAL space (so it
// aligns with the layer's pixels regardless of the layer's transform), then flipped from
// AE convention (+X right, +Y down, +Z into screen) to shading (+X right, +Y up, +Z toward
// viewer). If the layer matrix is unusable, the light degrades to a global directional
// light rather than being placed wrongly. Robust: any failure -> no lights.
static void ReadCompLights(PF_InData* in_data, std::vector<Light>& out) {
	out.clear();
	try {
		AEFX_SuiteScoper<AEGP_PFInterfaceSuite1> pfi(in_data, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1);
		AEFX_SuiteScoper<AEGP_LayerSuite9>       ls (in_data, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
		AEFX_SuiteScoper<AEGP_StreamSuite6>      ss (in_data, kAEGPStreamSuite, kAEGPStreamSuiteVersion6);
		AEFX_SuiteScoper<AEGP_LightSuite2>       lgt(in_data, kAEGPLightSuite, kAEGPLightSuiteVersion2);

		AEGP_LayerH effLayer = NULL;
		if (pfi->AEGP_GetEffectLayer(in_data->effect_ref, &effLayer) || !effLayer) return;
		AEGP_CompH comp = NULL;
		if (ls->AEGP_GetLayerParentComp(effLayer, &comp) || !comp) return;
		A_long n = 0;
		if (ls->AEGP_GetCompNumLayers(comp, &n)) return;

		A_Time t; t.value = in_data->current_time; t.scale = in_data->time_scale;

		Vec3 ref{0, 0, 0};
		A_Matrix4 me; bool haveEff = false;
		if (!ls->AEGP_GetLayerToWorldXform(effLayer, &t, &me)) {
			haveEff = true;
			ref = Vec3{ (float)me.mat[3][0], (float)me.mat[3][1], (float)me.mat[3][2] };
		}

		for (A_long i = 0; i < n && (A_long)out.size() < NM_MAX_COMP_LIGHTS; ++i) {
			AEGP_LayerH lyr = NULL;
			if (ls->AEGP_GetCompLayerByIndex(comp, i, &lyr) || !lyr) continue;
			AEGP_ObjectType ot;
			if (ls->AEGP_GetLayerObjectType(lyr, &ot) || ot != AEGP_ObjectType_LIGHT) continue;

			// Respect the layer's video (eyeball) switch: a light that's turned off in the
			// comp must not contribute. Only skip when we can positively read the flag as off.
			AEGP_LayerFlags lflags = 0;
			if (!ls->AEGP_GetLayerFlags(lyr, &lflags) && !(lflags & AEGP_LayerFlag_VIDEO_ACTIVE))
				continue;

			AEGP_LightType lt = AEGP_LightType_POINT;
			lgt->AEGP_GetLightType(lyr, &lt);

			// Light world position (its own layer->world translation).
			Vec3 lw = ref; A_Matrix4 m; bool haveLw = false;
			if (!ls->AEGP_GetLayerToWorldXform(lyr, &t, &m)) {
				lw = Vec3{ (float)m.mat[3][0], (float)m.mat[3][1], (float)m.mat[3][2] };
				haveLw = true;
			}
			// Bring the light into the effect layer's LOCAL space, then flip to shading
			// space. `localized` = we have a real per-pixel-comparable position.
			Vec3 ll;
			bool localized = haveEff && haveLw && WorldToLayer(me, lw, ll);
			Vec3 shade = localized ? Vec3{ ll.x, -ll.y, -ll.z }
			                       : Vec3{ (lw - ref).x, -(lw - ref).y, -(lw - ref).z };

			AEGP_StreamVal2 sv; AEGP_StreamType stype;
			float intensity = 1.0f;
			if (!ss->AEGP_GetLayerStreamValue(lyr, AEGP_LayerStream_INTENSITY, AEGP_LTimeMode_CompTime, &t, FALSE, &sv, &stype))
				intensity = (float)(sv.one_d / 100.0);
			Vec3 color{1, 1, 1};
			if (!ss->AEGP_GetLayerStreamValue(lyr, AEGP_LayerStream_COLOR, AEGP_LTimeMode_CompTime, &t, FALSE, &sv, &stype))
				color = Vec3{ (float)sv.color.redF, (float)sv.color.greenF, (float)sv.color.blueF };

			Light L; L.color = color; L.intensity = intensity;
			if (lt == AEGP_LightType_AMBIENT) {
				L.type = LightType::Ambient;
			} else if ((lt == AEGP_LightType_POINT || lt == AEGP_LightType_SPOT) && localized) {
				// Real point light: keep its layer-space position for per-pixel direction+falloff.
				L.type = LightType::Point; L.pos = shade; L.dir = normalize(shade);
			} else {
				// Parallel/environment, or a point light we couldn't localize -> directional.
				L.type = LightType::Directional; L.dir = normalize(shade);
			}
			out.push_back(L);
		}
	} catch (...) { out.clear(); }
}

/* ---------------- render ---------------- */
// Map (layer) slots checked out alongside the input. Order MUST match kMapParams.
enum {
	MAP_NORMAL = 0, MAP_DEPTH, MAP_MATCAP, MAP_REFL, MAP_REFR, MAP_BUMP,
	MAP_SOURCE, MAP_BASECOLOR, MAP_ROUGH, MAP_METAL, MAP_SPEC, MAP_ALPHA,
	NM_MAP_COUNT
};
static const int kMapParams[NM_MAP_COUNT] = {
	NM_NORMAL_MAP, NM_DEPTH_MAP, NM_MATCAP_MAP, NM_REFL_MAP, NM_REFR_MAP, NM_BUMP_MAP,
	NM_PBR_SOURCE_MAP, NM_PBR_BASECOLOR_MAP, NM_PBR_ROUGH_MAP, NM_PBR_METAL_MAP,
	NM_PBR_SPEC_MAP, NM_PBR_ALPHA_MAP
};

template<typename PIX>
static void ProcessWorld(PF_EffectWorld* input, PF_EffectWorld* output,
                         PF_EffectWorld* maps[NM_MAP_COUNT], const Shade& S) {
	const float MAXV = PixTraits<PIX>::MAXV;
	const bool  isFloat = std::is_same<PIX, PF_PixelFloat>::value;
	const A_long W = output->width, H = output->height;
	const A_long inW = input->width, inH = input->height;

	PF_EffectWorld* normalW = maps[MAP_NORMAL] ? maps[MAP_NORMAL] : input;
	PF_EffectWorld* depthW  = maps[MAP_DEPTH];
	PF_EffectWorld* matcapW = maps[MAP_MATCAP];
	PF_EffectWorld* envW    = maps[MAP_REFL];
	PF_EffectWorld* refrW   = maps[MAP_REFR];
	PF_EffectWorld* bumpW   = maps[MAP_BUMP];
	// Beeble PBR pass worlds — only consumed when the matching "Use" toggle is on.
	PF_EffectWorld* sourceW = (S.pbr.source    && maps[MAP_SOURCE])    ? maps[MAP_SOURCE]    : NULL;
	PF_EffectWorld* baseW   = (S.pbr.baseColor && maps[MAP_BASECOLOR]) ? maps[MAP_BASECOLOR] : NULL;
	PF_EffectWorld* roughW  = (S.pbr.roughness && maps[MAP_ROUGH])     ? maps[MAP_ROUGH]     : NULL;
	PF_EffectWorld* metalW  = (S.pbr.metallic  && maps[MAP_METAL])     ? maps[MAP_METAL]     : NULL;
	PF_EffectWorld* specW   = (S.pbr.specular  && maps[MAP_SPEC])      ? maps[MAP_SPEC]      : NULL;
	PF_EffectWorld* alphaW  = (S.pbr.alpha     && maps[MAP_ALPHA])     ? maps[MAP_ALPHA]     : NULL;

	TexFn matcapTex = matcapW ? TexFn([matcapW](float u,float v){ return sampleWorld<PIX>(matcapW,u,v); }) : TexFn();
	TexFn envTex    = envW    ? TexFn([envW](float u,float v){ return sampleWorld<PIX>(envW,u,v); }) : TexFn();
	TexFn refrTex   = refrW   ? TexFn([refrW](float u,float v){ return sampleWorld<PIX>(refrW,u,v); }) : TexFn();
	TexFn bumpTex   = bumpW   ? TexFn([bumpW](float u,float v){ return sampleWorld<PIX>(bumpW,u,v); }) : TexFn();

	const float du = W > 1 ? 1.0f / W : 0.0f;
	const float dv = H > 1 ? 1.0f / H : 0.0f;

	for (A_long y = 0; y < H; ++y) {
		PIX* outRow = (PIX*)((char*)output->data + (size_t)y * output->rowbytes);
		A_long iy = y < inH ? y : inH - 1;
		PIX* inRow = (PIX*)((char*)input->data + (size_t)iy * input->rowbytes);
		float v = (y + 0.5f) / H;
		for (A_long x = 0; x < W; ++x) {
			A_long ix = x < inW ? x : inW - 1;
			PIX& op = outRow[x];
			float u = (x + 0.5f) / W;

			PixelCtx ctx;
			ctx.normalRGB = (normalW == input) ? sampleWorld<PIX>(input, u, v) : sampleWorld<PIX>(normalW, u, v);
			ctx.u = u; ctx.v = v; ctx.du = du; ctx.dv = dv;
			if (matcapW) { ctx.hasMatcap = true; ctx.matcap = matcapTex; }
			if (envW)    { ctx.hasEnv = true; ctx.env = envTex; }
			if (refrW)   { ctx.hasRefraction = true; ctx.refractionMap = refrTex; }
			if (bumpW)   { ctx.hasBump = true; ctx.bumpMap = bumpTex; }
			if (depthW)  { ctx.hasDepth = true; ctx.depthRGB = sampleWorld<PIX>(depthW, u, v); }

			// Per-pixel position in shading space, in the SAME frame as the lights:
			// absolute layer pixels (top-left origin), flipped to +Y up / +Z toward viewer.
			// X/Y from screen position; Z is depth relief so point lights get a real
			// per-pixel direction + distance instead of one global direction.
			float px = u * (float)inW;
			float py = -(v * (float)inH);
			float pz = 0.0f;
			if (depthW) {
				DepthParams dpz = S.depth; dpz.multiplier = 1.0f;   // normalized depth in [0,1]
				float dn = 0.0f;
				if (decodeDepth(ctx.depthRGB, dpz, dn))
					pz = (dn - 0.5f) * (0.5f * (float)inH) * S.depth.multiplier;  // + toward viewer
			}
			ctx.worldPos = Vec3{ px, py, pz };

			// Beeble PBR passes: color passes keep RGB; scalar passes use luminance.
			if (sourceW) { ctx.hasSource = true;    ctx.sourceRGB = sampleWorld<PIX>(sourceW, u, v); }
			if (baseW)   { ctx.hasBaseColor = true; ctx.baseColorRGB = sampleWorld<PIX>(baseW, u, v); }
			if (roughW)  { ctx.hasRoughness = true; ctx.roughness = lum(sampleWorld<PIX>(roughW, u, v)); }
			if (metalW)  { ctx.hasMetallic = true;  ctx.metallic = lum(sampleWorld<PIX>(metalW, u, v)); }
			if (specW)   { ctx.hasSpecularMap = true; ctx.specularLevel = lum(sampleWorld<PIX>(specW, u, v)); }
			float alphaMatte = 1.0f; bool haveAlphaMatte = false;
			if (alphaW)  {
				ctx.hasAlpha = true;
				ctx.alpha = lum(sampleWorld<PIX>(alphaW, u, v));
				alphaMatte = saturate(S.pbr.alphaInvert ? 1.0f - ctx.alpha : ctx.alpha);
				haveAlphaMatte = true;
			}

			Vec3 c = ShadePixelCtx(ctx, S);

			if (isFloat) {
				op.red = (decltype(op.red))c.x; op.green = (decltype(op.green))c.y; op.blue = (decltype(op.blue))c.z;
			} else {
				op.red   = (decltype(op.red))(clampf(c.x, 0.f, 1.f) * MAXV + 0.5f);
				op.green = (decltype(op.green))(clampf(c.y, 0.f, 1.f) * MAXV + 0.5f);
				op.blue  = (decltype(op.blue))(clampf(c.z, 0.f, 1.f) * MAXV + 0.5f);
			}
			// Coverage: the Beeble Alpha pass (when enabled) drives output alpha; else
			// pass the input layer's own alpha through unchanged.
			if (haveAlphaMatte) {
				op.alpha = isFloat ? (decltype(op.alpha))alphaMatte
				                   : (decltype(op.alpha))(alphaMatte * MAXV + 0.5f);
			} else {
				op.alpha = inRow[ix].alpha;
			}
		}
	}
}

static PF_Err PreRender(PF_InData *in_data, PF_OutData *out_data, PF_PreRenderExtra *extra) {
	PF_Err err = PF_Err_NONE;
	PF_RenderRequest req = extra->input->output_request;
	PF_CheckoutResult in_result;

	ERR(extra->cb->checkout_layer(in_data->effect_ref, NM_INPUT, NM_INPUT, &req,
	                              in_data->current_time, in_data->time_step, in_data->time_scale, &in_result));
	UnionLRect(&in_result.result_rect,     &extra->output->result_rect);
	UnionLRect(&in_result.max_result_rect, &extra->output->max_result_rect);

	// Best-effort checkout of optional map layers (ignore individual errors / unconnected).
	for (int i = 0; i < NM_MAP_COUNT; ++i) {
		PF_CheckoutResult r;
		PF_RenderRequest req2 = extra->input->output_request;
		extra->cb->checkout_layer(in_data->effect_ref, kMapParams[i], kMapParams[i], &req2,
		                          in_data->current_time, in_data->time_step, in_data->time_scale, &r);
	}

	// Snapshot comp 3D lights here (single-threaded), pass to SmartRender via pre_render_data,
	// and mix into the GUID so moving/altering a light invalidates the cached frame.
	AEFX_SuiteScoper<PF_HandleSuite1> hs(in_data, kPFHandleSuite, kPFHandleSuiteVersion1, out_data);
	PF_Handle h = hs->host_new_handle(sizeof(CompLightData));
	if (h) {
		CompLightData* d = (CompLightData*)hs->host_lock_handle(h);
		if (d) {
			std::memset(d, 0, sizeof(CompLightData));
			PF_ParamDef ucl; AEFX_CLR_STRUCT(ucl);
			if (PF_CHECKOUT_PARAM(in_data, NM_USE_COMP_LIGHTS, in_data->current_time,
			                      in_data->time_step, in_data->time_scale, &ucl) == PF_Err_NONE) {
				d->useComp = ucl.u.bd.value ? 1 : 0;
				if (ucl.u.bd.value) {
					std::vector<Light> comp;
					ReadCompLights(in_data, comp);
					A_long n = (A_long)comp.size();
					if (n > NM_MAX_COMP_LIGHTS) n = NM_MAX_COMP_LIGHTS;
					for (A_long i = 0; i < n; ++i) d->lights[i] = comp[i];
					d->count = n;
				}
				PF_CHECKIN_PARAM(in_data, &ucl);
			}
			// Mix the (zero-padded, deterministic) snapshot into the frame GUID.
			if (extra->cb->GuidMixInPtr)
				extra->cb->GuidMixInPtr(in_data->effect_ref, sizeof(CompLightData), d);
			hs->host_unlock_handle(h);
		}
		extra->output->pre_render_data = h;   // AE takes ownership and frees it.
	}
	return err;
}

static PF_Err SmartRender(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra) {
	PF_Err err = PF_Err_NONE;
	PF_EffectWorld *input = NULL, *output = NULL;
	PF_EffectWorld *maps[NM_MAP_COUNT] = { NULL };

	ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, NM_INPUT, &input));
	ERR(extra->cb->checkout_output(in_data->effect_ref, &output));

	// Optional map layers (null if unconnected).
	for (int i = 0; i < NM_MAP_COUNT; ++i) {
		PF_EffectWorld* w = NULL;
		if (extra->cb->checkout_layer_pixels(in_data->effect_ref, kMapParams[i], &w) == PF_Err_NONE)
			maps[i] = (w && w->data) ? w : NULL;
	}

	Shade S;
	ERR(ReadParams(in_data, S));

	// Comp 3D lights come from the PreRender snapshot (no AEGP calls here -> thread-safe).
	if (extra->input->pre_render_data) {
		AEFX_SuiteScoper<PF_HandleSuite1> hs(in_data, kPFHandleSuite, kPFHandleSuiteVersion1, out_data);
		PF_Handle h = (PF_Handle)extra->input->pre_render_data;
		CompLightData* d = (CompLightData*)hs->host_lock_handle(h);
		// When "Use Comp 3D Lights" is on, the comp is the ONLY light source: replace the
		// in-effect key light with the comp snapshot even if it's empty. Zero comp lights
		// then reads as "unlit" (ambient only) instead of falling back to a white built-in
		// light. When the toggle is off, keep the in-effect light from ReadParams.
		if (d && d->useComp) S.lights.assign(d->lights, d->lights + d->count);
		if (d) hs->host_unlock_handle(h);
	}

	// Point-light falloff reference distance ≈ half the layer diagonal (~subject scale),
	// so distances read in subject-sizes and the falloff modes stay usable in pixel space.
	if (input) {
		float rw = (float)input->width, rh = (float)input->height;
		S.global.refDist = 0.5f * std::sqrt(rw * rw + rh * rh);
	}

	if (!err && input && output) {
		PF_PixelFormat fmt = PF_PixelFormat_INVALID;
		AEFX_SuiteScoper<PF_WorldSuite2> ws(in_data, kPFWorldSuite, kPFWorldSuiteVersion2, out_data);
		ERR(ws->PF_GetPixelFormat(input, &fmt));
		switch (fmt) {
			case PF_PixelFormat_ARGB128: ProcessWorld<PF_PixelFloat>(input, output, maps, S); break;
			case PF_PixelFormat_ARGB64:  ProcessWorld<PF_Pixel16>(input, output, maps, S);    break;
			case PF_PixelFormat_ARGB32:  ProcessWorld<PF_Pixel8>(input, output, maps, S);     break;
			default: err = PF_Err_BAD_CALLBACK_PARAM; break;
		}
	}
	return err;
}

/* ---------------- registration & entry ---------------- */
extern "C" DllExport PF_Err PluginDataEntryFunction2(
	PF_PluginDataPtr inPtr, PF_PluginDataCB2 inPluginDataCallBackPtr,
	SPBasicSuite* inSPBasicSuitePtr, const char* inHostName, const char* inHostVersion) {
	PF_Err result = PF_Err_INVALID_CALLBACK;
	result = PF_REGISTER_EFFECT_EXT2(
		inPtr, inPluginDataCallBackPtr,
		NM_NAME, NM_MATCH_NAME, NM_CATEGORY,
		AE_RESERVED_INFO, "EffectMain", "https://github.com/cosmo88/Specularity");
	return result;
}

PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,
                  PF_ParamDef *params[], PF_LayerDef *output, void *extra) {
	PF_Err err = PF_Err_NONE;
	try {
		switch (cmd) {
			case PF_Cmd_ABOUT:            err = About(in_data, out_data, params, output); break;
			case PF_Cmd_GLOBAL_SETUP:     err = GlobalSetup(in_data, out_data, params, output); break;
			case PF_Cmd_PARAMS_SETUP:     err = ParamsSetup(in_data, out_data, params, output); break;
			case PF_Cmd_SMART_PRE_RENDER: err = PreRender(in_data, out_data, (PF_PreRenderExtra*)extra); break;
			case PF_Cmd_SMART_RENDER:     err = SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra); break;
		}
	} catch (PF_Err &thrown) { err = thrown; }
	return err;
}
