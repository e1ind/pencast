# Using PenCast

## Opening presentations

Every presentation you add (the **+** button, or "Open in PenCast" from Files, AirDrop or Mail) appears on the
home screen with a thumbnail, its slide count and when you last presented it. Files stay where they are, so
recompiling a deck updates it in place. On iPad, PDFs placed in PenCast's own folder (Files → On My iPad →
PenCast, or Finder file sharing) appear automatically. Long-press or right-click a card to remove it.

On the Mac, an open presentation reloads by itself about a second after LaTeX rewrites the PDF, on the same
slide and with the timer still running, so you can keep it open beside your editor.

## Moving through slides

| | iPad | Mac |
|---|---|---|
| Next slide | Tap the right quarter, swipe left, or the › button | → ↓ Space Page Down |
| Previous slide | Tap the left quarter, swipe right, or the ‹ button | ← ↑ Page Up |
| Any slide | The slide grid button | The slide grid button |

Links in the PDF (hyperref, Beamer's navigation symbols, "jump to appendix" buttons) work as they do in a PDF reader.

## Writing

Pick a tool in the toolbar; tap it again for its settings.

- **Pen:** a fountain pen whose line follows Pencil pressure (set how hard you press for the thinnest and
  thickest line, and try it on the scratch pad), or a ball pen of even width. Sizes and colours are remembered.
- **Highlighter**, with its own colours.
- **Eraser:** whole strokes, or partial (anything under the circle, from a fine tip to a large area).
- **Lasso:** circle ink to select it, then drag it or tap it for cut, copy, delete and duplicate.
- **Laser:** a glowing trail that fades after you lift, or just a dot. It is never saved.
- **Undo / redo / clear** act on the current slide.

Ink belongs to the Beamer frame, so notes stay while `\pause` and `\only` content appears on later steps. Ink
is saved automatically and kept with the presentation.

On the Mac, the mouse or trackpad draws with the pen and highlighter; there is no pressure, so the fountain pen
draws at an even width.

## Zooming

Pinch to zoom in where you want to write, and drag with one finger to move around. Two-finger tap, or the
"whole slide" button, zooms back out. While zoomed, taps never change the slide. On the Mac: ⌘= ⌘− ⌘0, a
trackpad pinch, or two-finger scrolling. On Linux: pinch on a touchscreen (one finger moves
around once you have used a pen; a two-finger tap zooms out), a trackpad pinch, Ctrl+scroll, or Ctrl+= Ctrl+−
Ctrl+0; scroll, or drag with the pointer tool, to move around.

The projector either keeps showing the whole slide (the default) or follows your zoom; choose in the
**Present** menu (share icon).

## Videos and animations

Embedded videos and `animate` animations play in place, with their autoplay and loop settings. Tap a video to
show its play/pause button and scrubber; they fade after a few seconds and appear only on the iPad. Tap an
animation to pause or resume it. A video that continues across overlay steps keeps playing without a break.
On Linux, click with the pointer tool; the same click follows links. G opens the slide grid, S picks the lasso,
and Delete, Ctrl+X/C/V and Ctrl+D act on a lasso selection. Videos need an H.264 codec on Fedora, Rocky and RHEL
(see the README).

## Presenting on a projector

Connect a projector or TV with a cable, or choose **Screen Mirroring** in Control Center for AirPlay. The
external screen shows only the slide, its videos and your ink; the iPad keeps the toolbar, notes and timer.
The display icon in the toolbar turns green when a projector is connected.

From the **Present** menu (share icon) you can black out, white out or freeze the projector, show speaker notes
(Beamer's `show notes on second screen`), and choose whether the projector follows your zoom. The timer next to
the slide number has its own play/pause button.

On the Mac, choose **Open Projector Window**, drag the window to the projector and make it full screen (⌃⌘F).

## Sharing your notes

The share icon offers two exports, each with **Save As…**, **Share…** and **Print…**:

- **Notes:** one page per slide (its last, fully uncovered step) with your ink: a handout of what you wrote.
- **Every page:** every overlay step, as presented.

## Preparing decks

PenCast plays videos embedded with media9, `\movie` (linked files) and standard PDF Screen/Rendition
annotations, plus `animate` sequences. With media9, add the `noplaybutton` option:

```latex
\usepackage[noplaybutton]{media9}
```

Without it, Apple's PDF renderer (used by every app on iPad and Mac) shifts parts of the slide that come after
the video. Linked videos (`\movie`) must sit next to the PDF; PenCast asks for access to that folder the first
time.
