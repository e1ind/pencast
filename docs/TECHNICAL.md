# Technical notes

## Repository layout

```
PenCast/     The app (SwiftUI, UIKit, PDFKit, AVFoundation, PencilKit); one target for iPad and Mac Catalyst
Core/        Swift package PenCastCore: PDF media scanner (CGPDF) and slide geometry; builds on macOS
  Sources/pencast-scan/   CLI: list a deck's media, extract embedded videos, self-check
UITests/     XCUITest suite
Corpus/      Test decks and videos (./build.sh rebuilds them)
Demo/        Showcase deck PenCast_Demo.pdf (./build.sh renders its videos and compiles it)
scripts/     package-mac.sh: the Mac disk image for a release
docs/        This documentation and README media
project.yml  XcodeGen spec (PenCast.xcodeproj is generated from it)
```

```sh
cd Core
swift run pencast-scan path/to/talk.pdf                 # what PenCast finds in a deck
swift run pencast-scan path/to/talk.pdf --extract out   # write out its embedded videos
```

## How PDF media is found

- **media9:** a `/RichMedia` annotation. The MP4 is an embedded file in `RichMediaContent/Assets`; autoplay and
  loop are in the instance `FlashVars`. Extraction is byte-identical.
- **`\movie`:** a `/Movie` annotation; `/Movie/F` is the path relative to the PDF and `/A/Mode /Repeat` means
  loop. Autostart is a hidden widget with a page-open `/S /Movie` action.
- **Screen + Rendition** (standard PDF video, e.g. through `\pdfextension annot`): a `/Screen` annotation whose
  page-open `/Rendition` action plays a MediaClip with the MP4 embedded in `/D/EF/F`; `/P/BE/RC 0` means loop
  forever.
- **animate:** one widget per frame named `<anim>.<frame>`, the frame in its appearance stream; fps, loop and
  autoplay are read from the page-open JavaScript (no JavaScript is executed). PDFKit's `page.draw` skips these
  widgets while `PDFAnnotation.draw` renders them, so frames are drawn on demand.

Each embedded video is extracted once per deck version (cached by name and modification date) and shared by
every slide that uses it.

**Apple renderer bug with media9's play button:** media9 puts a `cm` operator inside a hidden optional-content
layer. Apple's renderer (PDFKit/Quartz, macOS and iOS) skips it, contrary to the PDF spec, and everything drawn
after the video shifts by about half the video's size. Decks should use `\usepackage[noplaybutton]{media9}`;
PenCast does not yet correct decks compiled without it.

## Ink

Ink is PencilKit drawings stored per Beamer frame (consecutive pages with the same page label share one),
keyed by a hash of the slide's text so it survives recompiling and reordering. Files live in Application
Support/Ink as `<deck>.ink.plist`.

- The fountain pen is captured with its own gesture recognizer (coalesced and predicted touches), drawn live as
  a union of round-capped segments, and stored as a PencilKit stroke. PencilKit draws a point of size *s* about
  2(*s* − 2) wide and nothing below 2, so widths are converted accordingly.
- The partial eraser cuts strokes geometrically; PencilKit's own pixel eraser is capped at about 80 slide points.
- PencilKit ignores a programmatic scroll offset for input, so the canvas is never scrolled: it holds the ink
  shifted to the visible region, and every change is shifted back before it is saved.

## Zoom

The settled zoom is rendered sharp. Pinches, drags and animations move a live region that is shown as a GPU
transform of the settled view over a whole-slide underlay (slide image plus an ink snapshot), so nothing
re-renders per frame and zooming out never uncovers empty space. Gestures overshoot the edges and limits with
rubber-band resistance and spring back; flicks glide with scroll-view deceleration. While PencilKit redraws ink
after a zoom, its stale picture is hidden and the ink snapshot stands in.

## External display

The iPad declares a non-interactive external-display scene in Info.plist (the SwiftUI app lifecycle does not
consult the app delegate for this role). That scene shows slide, media and ink only and shares the `AVPlayer`
with the iPad screen.

## Performance

Measured on a real 138-page lecture deck with 39 `animate` sequences (10 fps JPEG frames, up to 197 frames) and
8 embedded videos: open 84 ms, hiding 4,047 frame widgets 182 ms, one animation frame 3 ms, one full slide 6 ms,
notes export 0.3 s. Animation frames render on demand, so memory holds one frame per animation instead of
~240 MB for the largest. In a two-minute scripted session on the 100-slide stress deck (paging, writing,
zooming, undo) memory levelled off around 420 MB, mostly the capped slide-image cache.

## Test decks (`Corpus/`)

- `media_test.pdf`: media9 autoplay+loop (720p30), media9 click-to-play (1080p60), two videos on one slide,
  `\movie` linked file, `animate` loop+autoplay and play-once, `\pause` overlays, a heavy pgfplots slide,
  hyperlinks and a blank ink slide. The videos are test patterns with a frame counter; the 720p clip has a quiet
  1 kHz blip once per second for checking A/V sync.
- `stress_100.pdf`: 100 vector slides for memory and slide-change timing.

## Known gaps

- Tappable `animate` control buttons (tap the animation itself to play or pause).
- Correcting decks compiled without media9's `noplaybutton`.
- Streaming extraction for embedded videos over ~200 MB (currently decoded in memory once, then cached).
- On the projector, partial erasing appears when the Pencil lifts rather than live.
