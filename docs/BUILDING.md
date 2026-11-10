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

## Linux

The Linux app is a separate Qt 6 program in `linux/` (C++, CMake). It needs Qt 6.4 or later with the Qt PDF
module. On Rocky Linux 9 (and RHEL / AlmaLinux 9), Qt PDF comes from EPEL:

```sh
sudo dnf install epel-release
sudo dnf config-manager --set-enabled crb
sudo dnf install cmake gcc-c++ qt6-qtbase-devel qt6-qtpdf-devel
```

On Fedora: `sudo dnf install cmake gcc-c++ qt6-qtbase-devel qt6-qtpdf-devel`. On Ubuntu 24.04 / Debian 13:
`sudo apt install cmake g++ qt6-base-dev qt6-pdf-dev`. Then, from the repository root:

```sh
cmake -B build/linux -S linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux -j
ctest --test-dir build/linux          # ink logic, plus simulated pen, eraser and finger input
sudo cmake --install build/linux      # pencast command, app menu entry and icon (under /usr/local)
```

`pencast deck.pdf` opens a deck in the reading view; `pencast --present deck.pdf` starts presenting. Point your
LaTeX editor's PDF viewer at `pencast %f` and the open deck reloads after every compile. The same sources build on
a Mac for development (`brew install cmake qtbase qtwebengine`, then add `-DCMAKE_PREFIX_PATH=$(brew --prefix)`).

Ink is saved in `~/.local/share/PenCast/Ink/`. Not on Linux yet: embedded videos and `animate`, zoom, the lasso,
the partial eraser, and export.

## Making a release

```sh
./scripts/package-mac.sh     # → dist/PenCast-mac.dmg
```

The Linux packages are built by GitHub Actions (`.github/workflows/linux.yml`): publishing a release builds
and tests them on each distribution and attaches the `.rpm`, `.deb` and AppImage files to it. To try a build
without releasing, run the **Linux packages** workflow by hand from the Actions tab and download its artifacts.

The script builds a universal (Apple silicon + Intel) Release app, signs it ad hoc (no paid Apple Developer
account needed) and packs it in a disk image. Create a GitHub release for the new version tag, attach
`dist/PenCast-mac.dmg` and the release notes from [CHANGELOG.md](../CHANGELOG.md). The README's download link
points at `releases/latest/download/PenCast-mac.dmg`, which GitHub resolves to the newest release that is not
marked as a pre-release.

Bump the version in `project.yml` (`CFBundleShortVersionString`, and `CFBundleVersion` for every upload) and in
`linux/CMakeLists.txt` (`project(... VERSION ...)`), and run `xcodegen` before building.

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
