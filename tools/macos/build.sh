#!/bin/sh
# Builds LLCast.app for macOS 14+ on a Mac (Xcode command line tools, CMake), as a
# universal binary (arm64 and x86_64), adapted from VibeOffice's desktop/macos/build.sh:
#   tools/macos/build.sh              LLCast.app, signed ad hoc; the keychain isn't touched
#   tools/macos/build.sh --test       also runs LLCast --selftest (encode, UDP loopback, decode)
#   tools/macos/build.sh --build-dmg  LLCast.app and LLCast.dmg signed with a Developer ID
#                                     Application identity (hardened runtime), notarized and
#                                     stapled. Needs ASC_KEY_ID, ASC_ISSUER_ID and ASC_KEY_PATH
#                                     (an App Store Connect API key); MACOS_DEVELOPER_ID picks
#                                     the identity. CI's release builds use this.
# macOS ties an ad-hoc app's Screen Recording grant to that exact build.
# Output: build/macos/dist/
set -eu
cd "$(dirname "$0")/../.."
TEST=no DMG_BUILD=no
for arg in "$@"; do
  case $arg in
    --test) TEST=yes ;;
    --build-dmg) DMG_BUILD=yes ;;
    *) echo "usage: tools/macos/build.sh [--test] [--build-dmg]" >&2; exit 2 ;;
  esac
done
if [ "$DMG_BUILD" = yes ]; then
  : "${ASC_KEY_PATH:?--build-dmg notarizes: set ASC_KEY_PATH (the .p8 key), ASC_KEY_ID and ASC_ISSUER_ID}"
  : "${ASC_KEY_ID:?}" "${ASC_ISSUER_ID:?}"
fi
BUILD=build/macos
DIST=$BUILD/dist

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
cmake --build "$BUILD" --target LLCast -j "$(sysctl -n hw.ncpu)"
rm -rf "$DIST"
mkdir -p "$DIST"
ditto "$BUILD/LLCast.app" "$DIST/LLCast.app"
APP="$DIST/LLCast.app"
plutil -lint "$APP/Contents/Info.plist" > /dev/null
lipo -info "$APP/Contents/MacOS/LLCast"

# The first identity in the keychains whose name starts with the prefix.
identity() {
  security find-identity -v -p codesigning | sed -n 's/^ *[0-9]*) [0-9A-F]* "\(.*\)"$/\1/p' |
    while read -r name; do
      case $name in "$1"*) echo "$name"; exit 0 ;; esac
    done | head -1
}
# A failed signature must not leave an app behind: the linker's own signature
# names no bundle and seals no Info.plist, so macOS can't keep its grants.
sign() {
  if ! codesign --force "$@" "$APP"; then
    rm -rf "$APP"
    echo "codesign failed, so LLCast.app was removed" >&2
    exit 1
  fi
}

if [ "$DMG_BUILD" = no ]; then
  BUNDLE_ID=$(plutil -extract CFBundleIdentifier raw "$APP/Contents/Info.plist")
  echo "Signed ad hoc. macOS ties its Screen Recording grant to this exact build; after a" >&2
  echo "rebuild: tccutil reset ScreenCapture $BUNDLE_ID, then allow it again." >&2
  sign --sign -
else
  DEVELOPER_ID=${MACOS_DEVELOPER_ID:-$(identity 'Developer ID Application: ')}
  [ -n "$DEVELOPER_ID" ] || { echo "--build-dmg: no Developer ID Application identity in the keychain" >&2; exit 1; }
  # Hardened runtime; no entitlements are needed outside the sandbox.
  sign --timestamp --options runtime --sign "$DEVELOPER_ID"
  codesign --verify --strict --verbose=2 "$APP"
fi

if [ "$TEST" = yes ]; then
  status=0
  "$APP/Contents/MacOS/LLCast" --selftest || status=$?
  if [ $status -eq 77 ]; then
    echo "Self-test skipped: no hardware HEVC here" >&2
  elif [ $status -ne 0 ]; then
    exit $status
  fi
fi

if [ "$DMG_BUILD" = yes ]; then
  DMG="$DIST/LLCast.dmg"
  rm -rf "$BUILD/dmgbuild"
  python3 -m venv "$BUILD/dmgbuild"
  "$BUILD/dmgbuild/bin/pip" install --quiet --disable-pip-version-check --require-hashes --no-deps \
    --only-binary :all: -r mac/dmg-requirements.txt
  # hdiutil sometimes finds the new image busy on CI machines: try again.
  n=0
  until "$BUILD/dmgbuild/bin/dmgbuild" -s mac/dmg-settings.py -D app="$APP" LLCast "$DMG"; do
    n=$((n + 1))
    [ $n -lt 5 ] || exit 1
    sleep 5
  done
  codesign --force --timestamp --sign "$DEVELOPER_ID" "$DMG"
  out=$(xcrun notarytool submit "$DMG" --key "$ASC_KEY_PATH" --key-id "$ASC_KEY_ID" --issuer "$ASC_ISSUER_ID" \
    --wait --output-format json) || true
  echo "$out"
  status=$(echo "$out" | plutil -extract status raw -o - - 2> /dev/null || echo "no answer")
  if [ "$status" != Accepted ]; then
    echo "Notarization: $status" >&2
    xcrun notarytool log "$(echo "$out" | plutil -extract id raw -o - -)" \
      --key "$ASC_KEY_PATH" --key-id "$ASC_KEY_ID" --issuer "$ASC_ISSUER_ID" >&2 || true
    exit 1
  fi
  xcrun stapler staple "$DMG"
  spctl --assess --type open --context context:primary-signature --verbose=2 "$DMG"
fi
ls -l "$DIST"
