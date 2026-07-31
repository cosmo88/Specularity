/*
	Specularity.h — native Universal (arm64 + x86_64) After Effects effect.
	Normal-pass / PBR relighting for After Effects on Apple Silicon.
	Clean-room, original code. Inspired by Stefan Minning's *Normality* (© 2009
	Stefan Minning); not derived from its source.
*/
#pragma once
#ifndef SPECULARITY_H
#define SPECULARITY_H

#define PF_DEEP_COLOR_AWARE 1

#include "AEConfig.h"
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_EffectCBSuites.h"
#include "String_Utils.h"
#include "AE_GeneralPlug.h"
#include "Smart_Utils.h"
#include "AEGP_SuiteHandler.h"
#include "AEFX_SuiteHelper.h"
#include "AEFX_SuiteHandlerTemplate.h"

#ifdef AE_OS_WIN
	#include <Windows.h>
#endif

/* Versioning */
#define	MAJOR_VERSION	4
#define	MINOR_VERSION	0
#define	BUG_VERSION		0
#define	STAGE_VERSION	PF_Stage_DEVELOP
#define	BUILD_VERSION	1

#define NM_NAME			"Specularity"
#define NM_MATCH_NAME	"com.cosmo88.ae.Specularity"
#define NM_CATEGORY		"Stylize"
#define NM_DESCRIPTION	"Relights normal / PBR passes (e.g. Beeble exports) inside After Effects. Expects a normal map as input."

/* ---- Popup choice strings ---- */
#define NM_DISPLAY_CHOICES \
	"All|Adjusted Normals|Original Normals|Depth|Diffuse|Specular|Incidence|Rim|Toon|Gradient|Matcap|Reflection|Refraction|Bump|Off|" \
	"Source RGB|BaseColor|Roughness|Metallic|Specular Map|Alpha"
#define NM_BLEND_CHOICES \
	"Normal|Average|Darken|Multiply|Color Burn|Inverse Color Burn|Subtract|Darker|Lighten|Add|" \
	"Screen|Color Dodge|Inverse Color Dodge|Lighter|Divide|Overlay|Soft Light|Hard Light|Reflect|Glow|" \
	"Difference|Exclusion|Grain Merge|Exponential (A^B)|Exponential (B^A)|Hue|Saturation|Color|Luminosity|Phoenix"
#define NM_FALLOFF_CHOICES  "Constant|Linear|Quadratic|Cubic"
#define NM_TILE_CHOICES     "None|Edge|Repeat|Mirror"
#define NM_ENVMODE_CHOICES  "Spherical|Panorama"
#define NM_DCHAN_CHOICES    "None|Red|Green|Blue|Luminance"
#define NM_BLEND_COUNT      30
#define NM_DISPLAY_COUNT    21

/* ---- Parameter indices (MUST match ParamsSetup order) ---- */
// NOTE: this layout is APPEND-ONLY from here on. AE identifies stored params by
// position, and group headers count as params — so removing/reordering/merging
// breaks already-saved projects ("missing data in file"). Only add new params at
// the very end. (This ordering was set 2026-07-22 after the effect was removed from
// the user's project and re-applied fresh, so the one-time reorg was safe.)
enum {
	NM_INPUT = 0,
	NM_DISPLAY,

	// ===== Passes (top group) — every layer input in one spot =====
	// Normals + Depth + the Beeble PBR passes. Each pass is selectable (its own
	// layer picker) and toggleable (Use checkbox).
	NM_PBR_TOPIC,
	// Normals
	NM_NORMAL_MAP,
	NM_INVERT_X, NM_INVERT_Y, NM_INVERT_Z,
	// Depth
	NM_DEPTH_MAP,
	NM_DEPTH_INVERT, NM_DEPTH_CHANNEL, NM_DEPTH_GAMMA, NM_DEPTH_MULT,
	// Beeble PBR passes
	NM_PBR_SOURCE_USE,    NM_PBR_SOURCE_MAP,
	NM_PBR_BASECOLOR_USE, NM_PBR_BASECOLOR_MAP,
	NM_PBR_ROUGH_USE,     NM_PBR_ROUGH_MAP,  NM_PBR_ROUGH_INVERT,
	NM_PBR_METAL_USE,     NM_PBR_METAL_MAP,  NM_PBR_METAL_INVERT,
	NM_PBR_SPEC_USE,      NM_PBR_SPEC_MAP,
	NM_PBR_ALPHA_USE,     NM_PBR_ALPHA_MAP,  NM_PBR_ALPHA_INVERT,
	NM_PBR_TOPIC_END,

	// ===== Lights =====
	NM_LIGHT_TOPIC,
	NM_USE_COMP_LIGHTS,
	NM_LIGHT_AZIMUTH, NM_LIGHT_ELEVATION, NM_LIGHT_INTENSITY, NM_LIGHT_COLOR,
	NM_AMBIENT, NM_FALLOFF_MODE, NM_FALLOFF, NM_EXPOSURE, NM_GAMMA,
	NM_LIGHT_TOPIC_END,

	// ===== Shading =====
	NM_SHADE_TOPIC,
	NM_ENABLE_SHADING,
	// Diffuse
	NM_DIFF_COLOR, NM_DIFF_INCAND, NM_DIFF_BLEND,
	// Specular
	NM_SPEC_ENABLE, NM_SPEC_INTENSITY, NM_SPEC_SPREAD, NM_SPEC_COLOR, NM_SPEC_BLEND,
	// Incidence
	NM_INC_ENABLE, NM_INC_INTENSITY, NM_INC_FALLOFF, NM_INC_COLOR, NM_INC_BLEND,
	// Rim
	NM_RIM_ENABLE, NM_RIM_SIZE, NM_RIM_WIDTH, NM_RIM_ANGLE, NM_RIM_COLOR, NM_RIM_BLEND,
	// Toon
	NM_TOON_ENABLE, NM_TOON_SMOOTH, NM_TOON_SHADOW_INT, NM_TOON_SHADOW_WIDTH, NM_TOON_SHADOW_COLOR,
	NM_TOON_HILIGHT_INT, NM_TOON_BLEND,
	// Gradient
	NM_GRAD_ENABLE, NM_GRAD_TOP, NM_GRAD_BOT, NM_GRAD_BLEND,
	// Matcap
	NM_MATCAP_ENABLE, NM_MATCAP_MAP, NM_MATCAP_OFFX, NM_MATCAP_OFFY, NM_MATCAP_TILE, NM_MATCAP_BLEND,
	// Reflection
	NM_REFL_ENABLE, NM_REFL_MAP, NM_REFL_ENVMODE, NM_REFL_GAMMA, NM_REFL_TILE, NM_REFL_COLOR,
	NM_REFL_IOR, NM_REFL_FRESNEL, NM_REFL_FRESNEL_DEPTH, NM_REFL_BLEND,
	// Refraction
	NM_REFR_ENABLE, NM_REFR_MAP, NM_REFR_IOR, NM_REFR_GAMMA, NM_REFR_TILE, NM_REFR_SCALE,
	NM_REFR_OFFX, NM_REFR_OFFY, NM_REFR_COLOR, NM_REFR_FRESNEL, NM_REFR_FRESNEL_DEPTH, NM_REFR_BLEND,
	// Bump
	NM_BUMP_ENABLE, NM_BUMP_MAP, NM_BUMP_INTENSITY, NM_BUMP_TILE, NM_BUMP_SCALE,
	NM_SHADE_TOPIC_END,

	NM_NUM_PARAMS
};

/* Stable disk IDs (persisted; never reorder/reuse). 1:1 with the enum above. */
enum { DISK_BASE = 100 };  // param disk IDs = DISK_BASE + (param index)

extern "C" {
	DllExport PF_Err EffectMain(
		PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,
		PF_ParamDef *params[], PF_LayerDef *output, void *extra);
}

#endif // SPECULARITY_H
