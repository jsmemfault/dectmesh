#!/bin/sh
# mkapp.sh -- assemble achat.app: a self-contained macOS bundle of the libdraw
# GUI. Contents = achat-gui + the plan9port devdraw window server. Nothing else
# is needed: achat-gui statically links the plan9port libs (lib9pclient/libdraw/
# libthread/lib9), libdraw's built-in "*default*" font needs no font files, and
# bundle_setup() points $DEVDRAW at the bundled server. So it runs on a clean
# macOS with NO plan9port install.
#
# NOT a single static binary -- macOS has no static libSystem, and devdraw links
# Cocoa dynamically -- but a self-contained double-clickable .app, which is the
# real distributable. Signing + notarization (to run on OTHER people's Macs
# without a Gatekeeper override) is a separate step needing your Apple Developer
# identity; see the note printed at the end.
set -e
: "${PLAN9:=$HOME/src/plan9port}"
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

sh build.sh                                  # fresh achat-gui
[ -x "$PLAN9/bin/devdraw" ] || { echo "no devdraw at $PLAN9/bin/devdraw"; exit 1; }

APP=achat.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp achat-gui           "$APP/Contents/MacOS/achat-gui"
cp "$PLAN9/bin/devdraw" "$APP/Contents/MacOS/devdraw"

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>achat</string>
  <key>CFBundleDisplayName</key><string>Aether Chat</string>
  <key>CFBundleIdentifier</key><string>net.aether.achat</string>
  <key>CFBundleVersion</key><string>1.0</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleSignature</key><string>????</string>
  <key>CFBundleExecutable</key><string>achat-gui</string>
  <key>LSMinimumSystemVersion</key><string>10.13</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST

printf 'APPL????' > "$APP/Contents/PkgInfo"

echo "built $here/$APP"
echo
echo "run:   open $APP            (auto-detects /dev/cu.usbmodem*03)"
echo "debug: ./$APP/Contents/MacOS/achat-gui   (console errors visible)"
echo
echo "unsigned -- to distribute to other Macs:"
echo "  codesign --deep --force --options runtime --sign 'Developer ID Application: NAME' $APP"
echo "  then notarize (xcrun notarytool submit) + staple (xcrun stapler staple $APP)"
