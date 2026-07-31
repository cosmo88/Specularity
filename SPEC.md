# Normality — Native (Apple Silicon) Reimplementation Spec

> Clean-room specification. The parameter tree below was recovered from **observable
> metadata** (UI label strings) in the shipping Normality 3.2 x86_64 binary. No
> proprietary code was decompiled or copied. All shading math is a fresh
> implementation from standard, publicly documented computer-graphics techniques.
>
> Original plugin: *Normality 3.2* © 2009 Stefan Minning (free/donationware).
> Goal: a native **Universal (arm64 + x86_64)** After Effects plugin that reproduces
> the same feature set and loads without the "Intel-based Mac" compatibility warning.

## What the plugin does

Takes an **RGB-encoded normal map** (optionally + depth, matcap, environment maps) as
input and relights it inside After Effects using the comp's **3D lights** and/or
in-effect light controls. Output can be the final shaded result or any intermediate
pass (normals, depth, diffuse, specular, …).

- Description string: *"Simulates light effects via normal passes."*
- Input hint: *"Expects a normal map as input."*
- Category: (AE effect) — match: **Normality**
- Works in **16-bit and 32-bit (float)** color; SmartFX pipeline.

## Parameter tree (in original order)

### Display
- **Display** — popup (output/preview mode):
  `Off | Adjusted Normals | Original Normals | Depth | Diffuse | Specular | Incidence | Rim | Toon | Gradient | Matcap | Reflection | Refraction | Bump | All`
  (`All` = full composite of every enabled pass; `Off` = passthrough)

### Normals (group)
- **Normal Map** — layer param (the surface-normal source; default = this layer)
- **Alpha Map** — layer param (optional coverage/mask)
- **Invert X** — popup `Red | Green | Blue | Full | None` (which channel drives / inverts X)
- **Invert Y** — popup `Red | Green | Blue | Full | None`
- **Invert Z** — popup `Red | Green | Blue | Full | None`
- **Pre Blur** — slider (normal-map blur before shading)

### Depth (group)
- **Depth Map** — layer param
- **Invert** — checkbox
- **Channel** — popup `None | Red | Green | Blue | Luminance`
- **Gamma** — slider
- **Multiplier** — slider

### Lights (group)
- **Render** — popup `None | All | Light 1 | Light 2 | Light 3 | Light 4 | Light 5 | Light *`
  (which comp 3D lights are used; `Light *` = a named/wildcard convention)
- **Enable invisible lights** — checkbox ("Check if invisible lights should be used in calculations.")
- **Falloff Mode** — popup `Constant | Linear | Quadratic | Cubic`
- **Falloff** — slider
- **Exposure** — slider
- **Gamma** — slider

### Shading (group) — `Enable Shading` (checkbox)

Each sub-pass has an **Enable** checkbox, a **Color**, a **Blend** transfer mode
(30 modes, see below), and pass-specific controls:

- **Diffuse**: Color, Ambient, Incandescence, Blend
- **Specular**: Enable, Specular (intensity), Spread, Color, Blend  → Blinn-Phong
- **Incidence**: Enable, Incidence (intensity), Falloff, Color, Blend  → facing-ratio / fresnel
- **Rim Light**: Enable, Size, Width, Color, Angle, Blend
- **Toon**: Enable, Smoothing, Shadow(Intensity, Width, Color), Highlight(Intensity, Width), Blend → cel banding
- **Gradient**: Enable, Color, Blend  (normal-direction gradient / sky-ground)
- **Matcap**: Matcap Map (layer), Offset, Mode `None|Edge|Repeat|Mirror`, Blur, Blend
- **Reflection**: Enable, Environment Map (layer), Environment Mode `Spherical|Panorama`,
  Gamma, Tile Mode `None|Edge|Repeat|Mirror`, Blur, Color, Inclination, Azimuth, Seam,
  Index of Refraction, Fresnel, Fresnel Depth, Blend
- **Refraction**: Enable, Refraction Map (layer), Index of Refraction, Gamma,
  Tile Mode, Blur, Offset, Scale, Color, Fresnel, Fresnel Depth, Blend
- **Bump**: Enable, Bump/Normal Map (layer), Intensity, Tile Mode, Filter `Off|Low|Medium|High`,
  Offset, Scale, Blend/Transfer Mode

### PBR Passes (Beeble) — extension group

Feeds a **Beeble** export (as seen in the Nuke addon's `loaders.py`: `Source, Depth,
Alpha, Normal, BaseColor, Roughness, Specular, Metallic`) into the relighter. Normal +
Depth already exist above; this group adds the rest as **selectable** (per-pass layer
picker) + **toggleable** (per-pass `Use` checkbox) inputs:

- **Source RGB** — beauty/original render (preview passthrough via Display ▸ Source RGB)
- **BaseColor** — per-pixel albedo; multiplies the diffuse tint
- **Roughness** (+ Invert) — grayscale; drives specular spread (rough→broad, smooth→sharp)
- **Metallic** (+ Invert) — grayscale; attenuates diffuse `(1-m)` and tints specular toward BaseColor
- **Specular** map — grayscale; scales specular intensity (F0 level)
- **Alpha** (+ Invert) — coverage matte; drives the output alpha channel

Scalar passes (Roughness/Metallic/Specular/Alpha) are read as **luminance**. Each toggle
only takes effect when its layer is connected. Six new Display modes preview the raw
passes: `Source RGB | BaseColor | Roughness | Metallic | Specular Map | Alpha` (appended
after `Off` so existing popup indices don't shift).

### Blend / Transfer modes (30) — used by every pass' "Blend" popup
```
Normal | Average | Darken | Multiply | Color Burn | Inverse Color Burn |
Subtract | Darker | Lighten | Add | Screen | Color Dodge | Inverse Color Dodge |
Lighter | Divide | Overlay | Soft Light | Hard Light | Reflect | Glow |
Difference | Exclusion | Grain Merge | Exponential (A^B) | Exponential (B^A) |
Hue | Saturation | Color | Luminosity | Phoenix
```

## Scene integration (AEGP)
The original uses these AE suites (confirmed in binary):
`AEGP Layer / Light / Camera / Render / Render Options / Render Queue / Text Layer /
Layer Mask` suites, `PF Color Settings`, `PF ColorParam`, and `DRAWBOT Draw/Supplier/
Surface/Path` (custom UI drawing). Native port will use the current-SDK equivalents.

## Decoding conventions (implementation choices)
- Normal map: `n = normalize(2*rgb - 1)`, tangent-space; Y-up. Invert popups remap/flip axes.
- Depth: sample chosen channel, apply gamma + multiplier, optional invert → view-space Z.
- View vector: orthographic `(0,0,1)` unless a comp camera is read.
- Diffuse: Lambert `max(0, N·L)`; Ambient adds constant; Incandescence = self-emission.
- Specular: Blinn-Phong `pow(max(0,N·H), spread)` scaled by Specular intensity.
- Incidence: `pow(1 - max(0,N·V), falloff)` (fresnel-like facing term).
- Rim: incidence gated by light direction + Angle/Width/Size.
- Falloff modes: Constant=1, Linear=1/d, Quadratic=1/d², Cubic=1/d³ (distance attenuation).
- Exposure/Gamma: `out = pow(color * 2^exposure, 1/gamma)`.
