# Specularity

**Native Apple-Silicon normal / PBR relighter for Adobe After Effects.**

Specularity relights a **normal-map pass** (plus optional depth, alpha, and full
[Beeble](https://beeble.ai) PBR passes — BaseColor, Roughness, Metallic, Specular) directly
inside After Effects, driven by the comp's own 3D lights or by in-effect light controls.
It ships as a **Universal (arm64 + x86_64)** plugin, so it loads on Apple-Silicon AE 2024+
without the "Intel-based application" warning.

> **Inspired by [Stefan Minning's *Normality*]([https://www.minning.de](https://3dcg.net/#projects)).** Specularity is a
> clean-room, original implementation written from scratch — it reproduces a similar feature
> set and workflow but shares **no source code** with the original. All shading math is a
> fresh implementation of standard, publicly documented computer-graphics techniques.
> *Normality* © 2009 Stefan Minning; huge thanks for the tool that inspired this one.

---

## Features

- **Relighting from a normal pass** — Lambert diffuse, Blinn-Phong specular, incidence
  (Fresnel-like facing), rim light, toon/cel banding, and a normal-direction gradient.
- **Comp 3D lights** — reads the composition's own Point / Spot / Parallel / Ambient lights
  and transforms them into the layer's space (position, scale, rotation, anchor aware) for
  true per-pixel point-light direction and falloff.
- **2.5D depth** — an optional depth pass gives each pixel a real position so point lights
  localize on the subject and fall off with distance.
- **Beeble PBR passes** — selectable + toggleable layer inputs for **Source, BaseColor,
  Roughness, Metallic, Specular, Alpha** (and Normal + Depth), matching a Beeble export.
  BaseColor drives albedo, Metallic tints/attenuates, Roughness drives spec spread, the
  Specular map scales spec level, Alpha drives coverage.
- **Map-based passes** — matcap, environment reflection (spherical/panorama, Fresnel),
  refraction (IOR), and bump.
- **21 display modes** — preview any intermediate pass (normals, depth, diffuse, specular,
  each Beeble pass, …) or the full composite.
- **16-bit and 32-bit float**, multi-threaded SmartFX rendering.

## Requirements

- macOS 11+ on **Apple Silicon** (also runs on Intel — the binary is Universal).
- After Effects 2024 or newer.

## Install

1. Download `Specularity.plugin.zip` from the [latest release](../../releases/latest) and unzip it.
2. Copy the plugin into AE's MediaCore plug-ins folder (requires admin):

   ```bash
   sudo cp -R Specularity.plugin "/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/"
   ```

3. Restart After Effects. The effect appears under **Effect ▸ Stylize ▸ Specularity**.

## Quick start (Beeble workflow)

1. Apply **Specularity** to your normal-map layer (or any layer, then point **Normal Map**
   at the normal pass in the **PBR Passes (Beeble)** group).
2. In **PBR Passes (Beeble)**, tick **Use BaseColor** and pick your BaseColor pass; add
   Roughness / Metallic / Specular / Depth / Alpha the same way.
3. Under **Lights**, enable **Use Comp 3D Lights** and add a light to your comp — or use the
   in-effect Azimuth / Elevation controls.
4. For distance-based falloff, set **Falloff Mode** to *Quadratic* or *Linear*. For strong
   point-light localization, place the light **close** to the subject and connect a **Depth**
   pass (set **Depth Channel**).

## Build from source

Requires Xcode command-line tools and the **Adobe After Effects SDK** (not redistributed
here — download it from the [Adobe Developer Console](https://developer.adobe.com/)).

```bash
# Point AE_SDK at the SDK root (must contain Examples/Headers/AE_Effect.h),
# or drop it in ./vendor/AfterEffectsSDK
AE_SDK=/path/to/AfterEffectsSDK ./build.sh          # builds build/Specularity.plugin (Universal)
AE_SDK=/path/to/AfterEffectsSDK ./build.sh install  # build + copy into AE MediaCore (sudo)
```

Offline unit tests (no AE / no SDK needed) live in `tests/` and cover the shading engine:

```bash
clang++ -std=c++17 -O2 -Isrc/common tests/test_core.cpp -o /tmp/tc && /tmp/tc
clang++ -std=c++17 -O2 -Isrc/common tests/test_maps.cpp -o /tmp/tm && /tmp/tm
```

## Project layout

```
src/common/     portable, SDK-free shading engine (shared by the plugin and unit tests)
src/ae/         After Effects glue (params, SmartFX render, comp-light reading)
tests/          offline unit tests + a dlopen load harness + a render preview
build.sh        builds the Universal .plugin (Rez PiPL + ad-hoc codesign)
SPEC.md         parameter tree and shading conventions
verify.jsx      in-AE smoke test (File ▸ Scripts ▸ Run Script File…)
```

## Credits

- **Inspired by** Stefan Minning's *Normality* (© 2009 Stefan Minning).
- PBR pass conventions follow [Beeble](https://beeble.ai) exports.
- Clean-room implementation — see [`NOTICE`](NOTICE).

## License

[MIT](LICENSE).
