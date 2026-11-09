#!/bin/sh
# Builds PenCast for Mac and packs it as dist/PenCast-mac.dmg, ready to attach to a GitHub release.
#
# Signed "to run locally" (ad hoc), which needs no Apple Developer account. macOS then blocks the first
# launch on other Macs; see "Download for Mac" in README.md for the one-time step. With a paid account,
# sign with a Developer ID and notarize instead, and the warning goes away.
set -e
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"

xcodebuild -project PenCast.xcodeproj -scheme PenCast -configuration Release \
  -destination 'platform=macOS,variant=Mac Catalyst' -derivedDataPath build/release \
  CODE_SIGN_IDENTITY=- CODE_SIGN_STYLE=Manual DEVELOPMENT_TEAM= PROVISIONING_PROFILE_SPECIFIER= \
  build | grep -E "error:|BUILD"

rm -rf build/dmg && mkdir -p build/dmg dist
cp -R build/release/Build/Products/Release-maccatalyst/PenCast.app build/dmg/
ln -s /Applications build/dmg/Applications
hdiutil create -volname PenCast -srcfolder build/dmg -ov -format UDZO dist/PenCast-mac.dmg >/dev/null
echo "dist/PenCast-mac.dmg ($(du -h dist/PenCast-mac.dmg | cut -f1))"
