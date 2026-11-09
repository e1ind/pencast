# Building PenCast

Requirements: a Mac with Xcode (iPadOS 17 / macOS 14 SDK or later) and [XcodeGen](https://github.com/yonaskolb/XcodeGen).

```sh
brew install xcodegen
xcodegen                    # writes PenCast.xcodeproj from project.yml (don't edit the .xcodeproj by hand)
open PenCast.xcodeproj
```

In `project.yml`, set `DEVELOPMENT_TEAM` to your own team ID and `bundleIdPrefix` to your own prefix, then run
`xcodegen` again.

If `xcodebuild` reports that the active developer directory is the Command Line Tools, prefix commands with
`DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer` (or run `sudo xcode-select -s /Applications/Xcode.app`).

## iPad

1. Sign in to Xcode (Settings → Accounts).
2. Plug in the iPad and turn on Developer Mode (Settings → Privacy & Security).
3. Select the iPad as the run destination and press ▶.
4. On the iPad, trust the certificate: Settings → General → VPN & Device Management → Apple Development → Trust.

With a free Apple ID the app expires after 7 days; press ▶ again to renew. For performance checks use the
Release configuration (Edit Scheme → Run → Build Configuration). From the command line:

```sh
xcodebuild -project PenCast.xcodeproj -scheme PenCast -configuration Release \
  -destination 'platform=iOS,name=Your iPad' -derivedDataPath build/device \
  -allowProvisioningUpdates -allowProvisioningDeviceRegistration build
xcrun devicectl device install app --device 'Your iPad' build/device/Build/Products/Release-iphoneos/PenCast.app
```

## Mac

The same target builds for the Mac with Mac Catalyst: pick "My Mac (Mac Catalyst)" in Xcode, or

```sh
xcodebuild -project PenCast.xcodeproj -scheme PenCast -configuration Release \
  -destination 'platform=macOS,variant=Mac Catalyst' -derivedDataPath build/mac -allowProvisioningUpdates build
cp -R build/mac/Build/Products/Release-maccatalyst/PenCast.app /Applications/
```

## Making a release

```sh
./scripts/package-mac.sh     # → dist/PenCast-mac.dmg
```

The script builds a universal (Apple silicon + Intel) Release app, signs it ad hoc (no paid Apple Developer
account needed) and packs it in a disk image. Create a GitHub release for the new version tag, attach
`dist/PenCast-mac.dmg` and the release notes from [CHANGELOG.md](../CHANGELOG.md). The README's download link
points at `releases/latest/download/PenCast-mac.dmg`, which GitHub resolves to the newest release that is not
marked as a pre-release.

Bump the version in `project.yml` (`CFBundleShortVersionString`, and `CFBundleVersion` for every upload) and run
`xcodegen` before building.

With a paid Apple Developer account, sign with a Developer ID certificate and notarize (`xcrun notarytool`)
instead, and macOS opens the app without the "unidentified developer" step.

## Tests

```sh
xcodebuild test -project PenCast.xcodeproj -scheme PenCast -destination 'platform=iOS Simulator,name=iPad Pro 13-inch (M4)'
cd Core && swift run pencast-scan --check
```

The UI tests (`UITests/`) cover zoom accuracy and smoothness, ink and eraser placement while zoomed, undo/redo,
tool selection, video controls, the export dialogs, edge taps, and a two-minute stress session on the 100-slide
test deck (`StressTests`, the slowest; skip it with `-skip-testing:PenCastUITests/StressTests`).
`pencast-scan --check` verifies media detection, byte-identical video extraction and slide geometry against
`Corpus/media_test.pdf`.

Launch arguments useful while developing (Scheme → Run → Arguments): `-openDeck PenCast_Demo -openPage 5` opens
a bundled deck on a slide; `-openDeck /path/to/deck.pdf` opens any file.

Note that `Corpus/media_test.pdf` contains a quiet test tone; the demo deck's videos are silent.
