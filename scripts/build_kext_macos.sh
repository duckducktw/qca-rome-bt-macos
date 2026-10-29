#!/usr/bin/env bash
# Build QCARomePatchRAM.kext (x86_64) on macOS with Xcode + MacKernelSDK.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK_ROOT="${MAC_KERNEL_SDK:-$ROOT/MacKernelSDK}"
OUT="${1:-$ROOT/build/macos}"
OBJ="$OUT/obj"
BUNDLE="$OUT/QCARomePatchRAM.kext"
BIN="$BUNDLE/Contents/MacOS/QCARomePatchRAM"
GEN="$ROOT/build/gen"
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$ROOT/kext/QCARomePatchRAM/Info.plist")"
BUNDLE_ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$ROOT/kext/QCARomePatchRAM/Info.plist")"

[[ "$(uname -s)" == Darwin ]] || { echo "error: requires macOS/Xcode" >&2; exit 2; }
[[ -f "$SDK_ROOT/Library/x86_64/libkmod.a" ]] || { echo "error: MacKernelSDK missing at $SDK_ROOT" >&2; exit 2; }
[[ -f "$GEN/qca_rome_firmware.h" ]] || python3 "$ROOT/scripts/gen_firmware_header.py" "$GEN/qca_rome_firmware.h"

SDK="$(xcrun --sdk macosx --show-sdk-path)"
CLANG="$(xcrun --find clang)"
CLANGXX="$(xcrun --find clang++)"
rm -rf "$OUT"
mkdir -p "$OBJ" "$BUNDLE/Contents/MacOS"

COMMON=(
  -arch x86_64 -isysroot "$SDK" -mmacosx-version-min=11.0
  -mkernel -fno-builtin -fno-common -fno-stack-protector -fno-pic
  -fno-exceptions -fno-rtti -DKERNEL -DKERNEL_PRIVATE
  -Wall -Wextra -Werror
  -I"$ROOT/build-support" -I"$SDK_ROOT/Headers"
  -I"$ROOT/src/core" -I"$GEN" -I"$ROOT/kext/QCARomePatchRAM"
)

"$CLANGXX" "${COMMON[@]}" -std=gnu++17 -fapple-kext -nostdinc++ \
  -c "$ROOT/kext/QCARomePatchRAM/QCARomePatchRAM.cpp" -o "$OBJ/QCARomePatchRAM.o"
"$CLANG" "${COMMON[@]}" -std=gnu11 -c "$ROOT/src/core/qca_rome.c" -o "$OBJ/qca_rome.o"
"$CLANG" "${COMMON[@]}" -std=gnu11 \
  -DQCAROME_BUNDLE_ID="\"$BUNDLE_ID\"" -DQCAROME_VERSION="\"$VERSION\"" \
  -c "$ROOT/kext/QCARomePatchRAM/kmod_info.c" -o "$OBJ/kmod_info.o"

"$CLANGXX" -arch x86_64 -isysroot "$SDK" -mmacosx-version-min=11.0 \
  -nostdlib -Wl,-kext -Wl,-fatal_warnings -L"$SDK_ROOT/Library/x86_64" \
  "$OBJ"/*.o -lkmod -o "$BIN"

cp "$ROOT/kext/QCARomePatchRAM/Info.plist" "$BUNDLE/Contents/Info.plist"
chmod -R u+rwX,go+rX "$BUNDLE"

file "$BIN"
plutil -lint "$BUNDLE/Contents/Info.plist"
echo "undefined symbols:"; nm -u "$BIN" | sed 's/^/  /'
shasum -a 256 "$BIN"
echo "built: $BUNDLE"
