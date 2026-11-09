# PenCast: iPad Presenter for Beamer PDFs

A project spec covering what must be built, in what order, and how to know it works.

---

## 1. Goal

An iPad app that opens a Beamer-generated PDF **as-is** and presents it with:

- Embedded videos and animations playing correctly (no conversion, no sidecar files, no special LaTeX macros)
- Apple Pencil any color pen, highlighter, and eraser drawn over slides and playing video
- A projector/external display showing the slide, video, and ink, while the iPad shows the same content plus the controls

The author workflow is: write Beamer, compile PDF, AirDrop or open from Files, present. No PowerPoint step.

## 2. Non-goals (v1)

- Editing slide content
- Authoring or compiling LaTeX on the iPad
- Playing Flash/interactive/3D content from `media9` (only the video itself)
- Executing arbitrary PDF JavaScript
- Android or desktop versions (desktop presenting is covered by existing tools such as pympress)

## 3. Requirements

### 3.1 Functional

| ID | Requirement | Priority |
|---|---|---|
| F1 | Open a PDF from Files, AirDrop, or share sheet; remember recent decks | Must |
| F2 | Navigate: swipe, tap edges, Bluetooth clicker/keyboard arrows, thumbnail jump | Must |
| F3 | Play `media9`-embedded video at the correct position and size on its slide | Must |
| F4 | Play `\movie` (`multimedia`) videos, resolving linked files when access is granted | Must |
| F5 | Play `animate` animations (frame sequences) with correct frame rate and loop behavior | Must |
| F6 | Video controls: play/pause, scrub, loop, mute; honor autoplay/loop flags from the PDF | Must |
| F7 | Red pen, highlighter, eraser, undo/redo, clear slide (Pencil draws, finger navigates) | Must |
| F8 | External display: projector shows slide + video + ink with no toolbar | Must |
| F9 | Ink persists per deck and per slide across launches | Must |
| F10 | Hyperref internal links work | Should |
| F11 | Laser pointer (fading ink), timer, blank/freeze screen | Should |
| F12 | Export annotated PDF | Should |
| F13 | Speaker notes on iPad only (Beamer notes pages) | Could |

### 3.2 Non-functional

- Pencil-to-ink latency on iPad is not noticeable
- Ink latency on projector vs. iPad under ~100 ms
- Slide change under ~200 ms for typical vector-heavy slides
- Video plays without dropped frames while drawing
- Stable memory over a 100-slide deck and a 45-minute session
- Missing or unsupported media shows a clear on-slide message, never a crash or silent blank

## 4. Key unknowns to resolve first

These decide whether the plan is viable. Resolve them before building features.

| # | Unknown | How to resolve |
|---|---|---|
| U1 | What PDFKit exposes for movie, screen, and RichMedia annotations | Spike: open a test PDF, dump annotations. If PDFKit hides them, use `CGPDFDocument` |
| U2 | Exact PDF structure of `media9` embeds (RichMedia annotation, embedded file streams) | Inspect real PDFs from your LaTeX setup with `qpdf --qdf` or `pdfdetach`/`mutool show` |
| U3 | Exact structure of `animate` output (frame storage, timing, loop/control settings in JavaScript) | Inspect a real PDF. This is the least certain part of the plan |
| U4 | External display behavior on your iPad model and iPadOS version (mirrored vs. extended, what an app may render on the second screen) | Spike: colored window on projector, shared player, one ink stroke, tested over wired HDMI and AirPlay |
| U5 | Codecs in your existing MP4s | Run `ffprobe` on them. AVPlayer handles H.264/HEVC + AAC; anything else needs a fallback |
| U6 | Whether your decks use embedded (`media9`) or linked (`\movie`) video | Check your real decks. Linked files need folder access from the app |

**Input needed from the author:** one or two real Beamer PDFs, ideally one with an `animate` slide, one with `media9`, and one with `\movie`.

## 5. Architecture

### 5.1 Modules

| Module | Responsibility | Apple tech |
|---|---|---|
| **Library** | Open decks, recents, security-scoped bookmarks for deck folders | `UIDocumentPicker`, Files |
| **Deck model** | Page list, page sizes, hyperlinks | `PDFKit` |
| **Media scanner** | Walk every page's annotations, emit media items (`richMedia`, `movie`, `animate`) with rect, source, and options | `CGPDFDocument` (fallback from PDFKit) |
| **Media loaders** | Extract embedded streams to a cache directory; resolve linked paths; decode `animate` frames | Core Graphics, FileManager |
| **Players** | `AVPlayer` for video; custom timer-driven frame player for `animate` | `AVFoundation`, `CADisplayLink` |
| **Slide renderer** | Draw current page at high resolution on both screens | `PDFPage` drawing into `CALayer`/`UIView` |
| **Ink layer** | Red pen, highlighter, eraser, undo/redo, per-slide drawings | `PencilKit` |
| **Display manager** | Detect external screen, create projector window, keep it in sync | `UIScene` external display role |
| **Presenter state** | Single source of truth: slide index, media state, tool, drawings | `@Observable` model |
| **Controls UI** | Toolbar, thumbnails, video controls, timer, blank/freeze | SwiftUI |
| **Persistence** | Ink per deck/slide, settings, optional annotated-PDF export | `Codable`, `PKDrawing.dataRepresentation()` |

### 5.2 Design decisions

1. **One state, two views.** iPad and projector both render from the same presenter state.
2. **Fixed logical canvas.** Slide, video rects, and ink share the PDF page coordinate system, scaled and letterboxed per screen.
3. **Z-order:** slide, then media, then ink (pen draws over video).
4. **Shared player.** One `AVPlayer` feeds an `AVPlayerLayer` on each screen, so the two cannot drift.
5. **Pencil-only drawing** (`drawingPolicy = .pencilOnly`); fingers navigate and tap media.
6. **Lazy media extraction.** Extract embedded video to the cache on first need, not on open, so big decks open fast.
7. **Graceful degradation.** Unsupported item becomes a visible placeholder with the reason.

### 5.3 Media support matrix

| Beamer method | PDF representation | App behavior | Risk |
|---|---|---|---|
| `media9` | RichMedia annotation with embedded file stream and a Flash player (ignored) | Extract the video stream, play with AVPlayer in the annotation rect | Moderate |
| `\movie` | Movie/Screen annotation referencing an external file path | Resolve path relative to the deck folder, play with AVPlayer | Easy, but needs folder access |
| `animate` | Frames stored in the PDF, driven by embedded JavaScript | Locate the frames, recover timing and loop settings, play with the custom frame player | Hardest; structure to be verified (U3) |

## 6. Work breakdown

### Phase 0: Feasibility spikes (do first)

- [ ] **S1 Annotation dump.** Command-line or debug screen that lists every annotation subtype, rect, and keys for each page of a test PDF (resolves U1, U2).
- [ ] **S2 PDF structure inspection** of `media9` and `animate` samples (resolves U2, U3).
- [ ] **S3 External display spike.** Throwaway app: colored view on projector, one `AVPlayer` on both screens, one `PKCanvasView` stroke mirrored (resolves U4).
- [ ] **S4 Codec check** of existing MP4s (resolves U5).

**Exit criteria:** you know what the PDF contains, how to read it, and that two-screen presenting works on your hardware. If S3 fails, redesign before continuing.

### Phase 1: Core viewer

- [ ] Project setup (SwiftUI app, minimum iPadOS target, test targets)
- [ ] Open PDF from Files, recents, bookmarks
- [ ] Slide rendering and navigation (touch, clicker, thumbnails)
- [ ] Presenter state model

### Phase 2: Media

- [ ] Media scanner producing typed items per page
- [ ] `media9` loader and AVPlayer overlay
- [ ] `\movie` loader with folder access flow
- [ ] Playback controls; autoplay/loop flags
- [ ] Media state across slide changes (stop, continue, or resume per setting)
- [ ] `animate` loader and frame player
- [ ] Placeholder UI for missing or unsupported media

### Phase 3: Ink

- [ ] PencilKit canvas with red pen, highlighter, eraser, undo/redo
- [ ] Per-slide drawing storage and clear-slide
- [ ] Ink persistence across launches
- [ ] Pencil-only policy; touch passes through to navigation and media

### Phase 4: Second screen

- [ ] Display manager: detect connect/disconnect
- [ ] Projector window rendering slide + media + ink from shared state
- [ ] Shared `AVPlayer` across both layers
- [ ] Ink mirroring from `canvasViewDrawingDidChange`
- [ ] Recovery when the display is unplugged and replugged mid-talk

### Phase 5: Polish (v1.1)

- [ ] Laser pointer, timer, blank/freeze
- [ ] Hyperref link navigation
- [ ] Zoom into a region
- [ ] Annotated-PDF export
- [ ] Speaker notes on iPad only

### Phase 6: Hardening

- [ ] Run the full test plan (section 7) on real hardware
- [ ] Fix performance and memory findings
- [ ] Dry run in a real conference room

## 7. Test plan

### 7.1 Test corpus

Build and keep a set of PDFs, compiled with the author's actual LaTeX setup:

| File | Contents |
|---|---|
| `media9_single.pdf` | One embedded MP4 |
| `media9_multi.pdf` | Several videos on one slide and across slides |
| `movie_linked.pdf` + video folder | `\movie` with relative paths |
| `animate_basic.pdf` | One `animate` sequence, looping |
| `animate_controls.pdf` | `animate` with play/pause/step settings |
| `mixed.pdf` | media9 + animate + hyperlinks on one slide |
| `overlays.pdf` | Beamer `\pause` overlays (multi-page slides) |
| `large_video.pdf` | 200 MB+ embedded video |
| `bad_codec.pdf` | Embedded video in an unsupported format |
| `real_deck_*.pdf` | The author's actual talks |

### 7.2 Unit tests (XCTest)

- Scanner returns correct page, rect, source type, and options for every corpus file
- Coordinate mapping from PDF rect to canvas to screen for 16:9, 4:3, 16:10
- Embedded stream extraction writes a playable file; repeat extraction reuses cache
- Linked path resolution after the deck folder is moved
- `animate` frame order, count, frame rate, and loop flag match the source
- Ink save/load round trip is lossless; undo/redo stacks are per slide
- Missing or unsupported media yields a placeholder state, not an exception

### 7.3 UI/integration tests (XCUITest)

- Open deck, advance through all slides, go back, jump via thumbnails
- Tap a video: plays; change slide mid-video; return: state is correct
- `animate` slide plays and loops as configured
- Draw, change slide, return: ink retained; clear affects only that slide
- Connect/disconnect external display mid-presentation: recovery without relaunch
- Backgrounding, screen lock, and notifications mid-video

### 7.4 Performance tests (real hardware, Instruments)

| Metric | Target | Tool |
|---|---|---|
| Pencil-to-ink latency (iPad) | Not noticeable | Visual check, Core Animation |
| Ink latency, projector vs. iPad | < ~100 ms | Slow-motion video of both screens |
| Dropped frames while drawing over video | None at 30/60 fps | Core Animation FPS |
| Slide change | < ~200 ms | Signposts |
| Deck open | < ~2 s typical | Signposts |
| Memory over 100 slides and repeated navigation | No growth | Allocations, Leaks |
| 4K / multi-minute video on both outputs | Smooth, no thermal throttling | Instruments |
| Large embedded video extraction | Acceptable delay, no memory spike | Time Profiler |

### 7.5 Hardware matrix

- The author's iPad model(s) and iPadOS versions, plus the oldest supported
- USB-C to HDMI, USB-C to DisplayPort, AirPlay
- Projectors at 1080p, 4K, and 4:3
- Apple Pencil 1, 2, and Pro, if available

### 7.6 Soak and real-world tests

- 45-minute simulated talk, 30+ slides, 10+ media items, no restarts
- Failure drills: unplug projector mid-video, low power mode, clicker disconnect
- Conference-room dry run before first real use

## 8. Acceptance criteria for v1

- [ ] Every real deck opens and all `media9`, `\movie`, and `animate` content appears in the right place
- [ ] A full dry-run presentation needs no PowerPoint step and no manual fixes
- [ ] Ink and video are identical on iPad and projector
- [ ] No crashes in the 45-minute soak test
- [ ] Unsupported content shows a clear message

## 9. Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| `animate` structure is hard to decode or relies on PDF JavaScript logic | Some animations won't play | Inspect samples early (S2). Fallback: ask for a specific `animate` option set, or show first frame with a message |
| PDFKit doesn't expose RichMedia/movie annotations | More low-level parsing | Plan for `CGPDFDocument` from the start |
| External display rules differ by iPadOS version | Core feature may behave differently than designed | Spike S3 before everything else; test mirrored and extended modes |
| Unsupported codecs | Video won't play | Check codecs (S4); consider re-encoding or a software decoder only if needed |
| Linked `\movie` files inaccessible in sandbox | Videos missing | Folder-access flow; recommend `media9` for self-contained decks |
| Large embedded videos cause memory or slow open | Poor performance | Lazy extraction to disk, stream instead of loading into memory |
| Edge cases across Beamer themes and packages | Rendering differences | Growing test corpus from real decks |

## 10. Distribution

- **Personal use:** build with Xcode and install on your iPad. A free Apple ID works with weekly re-signing; a $99/year developer account removes that
- **Sharing with colleagues:** TestFlight (requires the paid account)
- **Requirements:** a Mac with Xcode. Code can be written elsewhere, but it can't be built or run on iOS without one

## 11. Suggested repository layout

```
PenCast/
  App/                  SwiftUI app entry, scenes
  Deck/                 PDF loading, page model
  Media/
    Scanner/            annotation scanning (CGPDFDocument)
    Loaders/            media9, movie, animate
    Players/            AVPlayer wrapper, frame player
  Render/               slide + overlay rendering
  Ink/                  PencilKit canvas, storage
  Display/              external display manager, projector window
  State/                presenter state
  UI/                   toolbar, thumbnails, controls
  Persistence/
  Tests/
    Unit/
    UI/
    Corpus/             test PDFs and media
```

## 12. First actions

1. Provide sample Beamer PDFs (one per embed type you use) and your video codec info
2. Run spikes S1 and S2 on those PDFs to confirm what each embed looks like
3. Run spike S3 on your iPad and projector
4. Review results, then finalize Phase 1 scope
