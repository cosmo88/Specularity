#!/usr/bin/env bash
# build.sh — build Specularity as a Universal (arm64 + x86_64) After Effects plugin.
#
# Prerequisites:
#   - Xcode / command line tools (clang++, Rez)
#   - Adobe After Effects SDK. Point AE_SDK at its root, or drop it in ./vendor/AfterEffectsSDK
#     so that "$AE_SDK/Examples/Headers/AE_Effect.h" exists.
#
# Usage:
#   AE_SDK=/path/to/AfterEffectsSDK ./build.sh          # build only
#   AE_SDK=/path/to/AfterEffectsSDK ./build.sh install  # build + copy into AE MediaCore plugins
set -euo pipefail
cd "$(dirname "$0")"

PLUGIN_NAME="Specularity"
OUT_BUNDLE="build/${PLUGIN_NAME}.plugin"

# ---- locate the AE SDK ----
if [[ -z "${AE_SDK:-}" ]]; then
  for cand in "vendor/AfterEffectsSDK" "vendor/AfterEffects_SDK" vendor/*SDK* ; do
    if [[ -f "$cand/Examples/Headers/AE_Effect.h" ]]; then AE_SDK="$cand"; break; fi
  done
fi
if [[ -z "${AE_SDK:-}" || ! -f "$AE_SDK/Examples/Headers/AE_Effect.h" ]]; then
  echo "ERROR: After Effects SDK not found."
  echo "  Set AE_SDK to the SDK root (must contain Examples/Headers/AE_Effect.h),"
  echo "  or unzip it into ./vendor/AfterEffectsSDK"
  exit 1
fi
echo "Using AE SDK: $AE_SDK"

HEADERS="$AE_SDK/Examples/Headers"
UTIL="$AE_SDK/Examples/Util"
RESOURCES_SDK="$AE_SDK/Examples/Resources"

INCLUDES=(-I"$HEADERS" -I"$HEADERS/SP" -I"$UTIL" -I"src/common" -I"src/ae")

ARCHES="-arch arm64 -arch x86_64"
CXXFLAGS="-std=c++17 -O2 -fvisibility=hidden -DMAC_ENV=1 -Wno-deprecated-declarations"

mkdir -p build "$OUT_BUNDLE/Contents/MacOS" "$OUT_BUNDLE/Contents/Resources"

# ---- compile sources ----
# AEFX_SuiteScoper is header-only. We do need Smart_Utils.cpp for UnionLRect().
SRCS=( src/ae/Specularity.cpp )
[[ -f "$UTIL/Smart_Utils.cpp" ]] && SRCS+=("$UTIL/Smart_Utils.cpp")

echo "Compiling: ${SRCS[*]}"
clang++ $ARCHES $CXXFLAGS "${INCLUDES[@]}" \
  -bundle \
  "${SRCS[@]}" \
  -framework CoreFoundation -framework Cocoa \
  -o "$OUT_BUNDLE/Contents/MacOS/${PLUGIN_NAME}"

# ---- PiPL resource (compiled with Rez) ----
# The PiPL declares the plugin to AE. Compiled from src/ae/Specularity_PiPL.r
REZ_INCLUDES=(-i "$HEADERS" -i "$RESOURCES_SDK" -i "$UTIL")
if command -v Rez >/dev/null 2>&1; then
  echo "Building PiPL resource with Rez..."
  Rez "${REZ_INCLUDES[@]}" \
      -o "$OUT_BUNDLE/Contents/Resources/${PLUGIN_NAME}.rsrc" \
      -useDF \
      src/ae/Specularity_PiPL.r
else
  echo "WARNING: Rez not found; PiPL not built."
fi

# ---- bundle metadata ----
cp src/ae/Info.plist "$OUT_BUNDLE/Contents/Info.plist"
printf 'eFKTFXTC' > "$OUT_BUNDLE/Contents/PkgInfo"

# ---- ad-hoc codesign (required for AE to load on Apple Silicon) ----
codesign --force --deep --sign - "$OUT_BUNDLE" 2>/dev/null || echo "note: codesign skipped/failed (may still load)"

echo ""
echo "Built: $OUT_BUNDLE"
lipo -info "$OUT_BUNDLE/Contents/MacOS/${PLUGIN_NAME}" || true

# ---- optional install ----
if [[ "${1:-}" == "install" ]]; then
  DEST="/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore"
  echo "Installing to: $DEST"
  sudo mkdir -p "$DEST"
  sudo rm -rf "$DEST/${PLUGIN_NAME}.plugin"
  sudo cp -R "$OUT_BUNDLE" "$DEST/"
  echo "Installed. Restart After Effects."
fi
