# Changelog

## 0.1.1 (beta 2)

PenCast for Linux, with everything the Mac presenter does, and a reading view for checking a deck while you
write it.

- Decks open in a scrolling reading view for checking them while writing; **Present** switches to the presenter
  at that page (and opens the projector window on the Mac).
- Mac: the window can shrink to share the screen with a LaTeX editor; it grows to fit the presenter's toolbar
  when presenting, and the toolbar scrolls instead of squeezing its icons.
- **Linux (beta):** packages for Rocky / RHEL / AlmaLinux 9 and 10, Fedora, Ubuntu 24.04 and 26.04 and Debian 13,
  and an AppImage for other distributions. Reading view with reload on recompile, presenter with the projector
  on the second screen, laser, pressure-sensitive pen, highlighter, eraser (a tablet pen's eraser end and barrel
  button too), palm rejection, undo, talk timer and speaker notes, with the iPad's toolbar.
- **Linux:** embedded videos (media9, `\movie`, Screen/Rendition) and `animate` animations play in place on the
  presenter and the projector, with their autoplay and loop settings, under your ink. Click a video with the
  pointer for its play/pause button and scrubber; click an animation to pause or resume it. A video that continues
  across overlay steps keeps playing.
- **Linux:** zoom: pinch on a touchscreen or trackpad (a two-finger tap zooms out), Ctrl+scroll, or
  Ctrl+= / Ctrl+− / Ctrl+0; scroll or drag to move around. **Projector Follows Zoom** is in the projector menu.
  Once a pen is used, one finger taps and moves the zoomed slide instead of writing, as on the iPad.
- **Linux:** lasso (S): circle ink, drag it, click it for cut, copy, delete and duplicate; Delete and the clipboard
  keys work too. Partial eraser (tap the eraser again for its mode and size).
- **Linux:** export with ink (share icon): notes (one page per slide, fully uncovered) or every page, as vector PDF.
- **Linux:** links in the slide work with the pointer (Beamer's navigation symbols, `\hyperlink` buttons); the
  slide grid (G) jumps to any slide.
- **Linux:** tap the selected tool again for its settings, as on the iPad: fountain or ball pen, a size slider,
  pressure response with a scratch pad, more colours and a custom colour; the laser as a trail or just a dot, with
  its sizes; the partial eraser's size.

## 0.1.0 (beta 1)

The first public beta: PenCast for Mac as a download, and for iPad when built from source.

**Presenting**
- Beamer PDFs exactly as compiled: hyperlinks, navigation symbols and overlay steps work as in a PDF reader.
- Embedded videos (media9, `\movie`, PDF Screen/Rendition) and `animate` sequences play in place with their
  autoplay and loop settings; tap a video for play/pause and a scrubber.
- Projector or TV over HDMI or AirPlay shows the clean slide while the iPad keeps its controls; black, white and
  freeze; speaker notes; a talk timer with play/pause.
- On the Mac, a projector window, and decks that reload themselves when LaTeX recompiles them.

**Writing**
- Pressure-sensitive fountain pen with adjustable response, ball pen, highlighter, lasso, laser pointer.
- Partial eraser from a fine tip to a large circle, and a whole-stroke eraser.
- Ink stays across a frame's `\pause` steps and is saved with the deck.
- Smooth pinch zoom with one-finger panning, rubber-band edges and gliding flicks; the projector shows the
  whole slide or follows the zoom.

**Sharing**
- Export your notes (one page per slide, with ink) or every page, then save, share or print.

**Known limitations**
- The Mac app is not yet notarized: macOS asks you to allow it once (System Settings → Privacy & Security →
  Open Anyway).
- iPad builds need Xcode until PenCast is on the App Store.
- Decks using media9 should be compiled with `\usepackage[noplaybutton]{media9}`.
