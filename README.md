<p align="center">
  <img src="docs/media/banner.png" alt="PenCast: present Beamer PDFs with their videos, animations and your handwriting. For iPad and Mac.">
</p>

<p align="center">
  <a href="https://github.com/e1ind/pencast/releases/latest/download/PenCast-mac.dmg"><img src="https://img.shields.io/badge/Download_for_Mac-Beta-0A84FF?style=for-the-badge&logo=apple&logoColor=white" alt="Download for Mac"></a>
  <a href="https://ko-fi.com/gregoryjanes"><img src="https://img.shields.io/badge/Support_on_Ko--fi-FF5E5B?style=for-the-badge&logo=kofi&logoColor=white" alt="Support PenCast on Ko-fi"></a>
</p>

<p align="center">
  <a href="https://github.com/e1ind/pencast/releases"><img src="https://img.shields.io/github/v/release/e1ind/pencast?include_prereleases&label=version&style=flat-square" alt="Latest version"></a>
  <img src="https://img.shields.io/badge/platforms-iPadOS_17%2B_%7C_macOS_14%2B-555?style=flat-square" alt="Platforms: iPadOS 17+ and macOS 14+">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0-555?style=flat-square" alt="License: GPL-3.0"></a>
</p>

<p align="center">
  <a href="docs/media/pencast_demo.mp4"><img src="docs/media/pencast_preview.gif" width="720" alt="PenCast demo: writing over a playing video, the laser pointer, zoomed writing and the fountain pen"></a>
  <br>
  <a href="docs/media/pencast_demo.mp4"><b>▶ Watch the full demo</b></a> (4 min)
</p>

---

PenCast opens the PDF you compiled from LaTeX and presents it exactly as it is. Videos and animations play
right on the slide, you write over everything with Apple Pencil, and a projector shows the clean slide while
your iPad keeps the controls. No conversion, no PowerPoint step, no special macros.

## Features

- **Video and animation in place.** Embedded videos (media9, `\movie`, standard PDF video) and `animate`
  sequences play on the slide with their own autoplay and loop settings.
- **Write like on paper.** A pressure-sensitive fountain pen, ball pen, highlighter, lasso and eraser, with
  notes that stay on a slide while `\pause` reveals the rest.
- **Laser pointer.** A glowing trail that fades when you lift, or a simple dot.
- **Zoom in to write neatly.** Pinch and pan smoothly; the projector shows the whole slide or follows you.
- **Made for the lecture hall.** HDMI or AirPlay projector output, black/white/freeze, speaker notes and a talk
  timer.
- **Take your notes with you.** Export a handout with one page per slide and your ink, or every page.
- **Built for writing slides too.** On the Mac, an open deck reloads itself each time LaTeX recompiles it.

## Download

**Mac (beta):** [download PenCast for Mac](https://github.com/e1ind/pencast/releases/latest/download/PenCast-mac.dmg),
open the disk image and drag PenCast to Applications. Requires macOS 14 or later, Apple silicon or Intel.

> [!NOTE]
> The beta isn't notarized by Apple yet, so macOS blocks it the first time. Open it once, then go to
> **System Settings → Privacy & Security** and click **Open Anyway**. You only need to do this once.

**iPad:** coming to the App Store. Until then you can [build it from source](docs/BUILDING.md).

## Help bring PenCast to the App Store

PenCast is free and open source. Publishing it on the App Store, and notarizing the Mac app so it opens without
warnings, requires Apple's Developer Program at **$99 a year**. If PenCast is useful to you, a contribution
toward that goal makes the release possible.

<p align="center">
  <a href="https://ko-fi.com/gregoryjanes"><img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="Support PenCast on Ko-fi"></a>
</p>

## Roadmap

- **App Store release** for iPad and Mac.
- **Windows and Linux** versions.
- **PowerPoint and Keynote:** present their PDF exports with the original videos playing in place.
- **More sources:** any app that exports PDF (Google Slides, LibreOffice Impress) already works without video.

## Documentation

- [User guide](docs/GUIDE.md): presenting, writing tools, zoom, projectors, exporting, preparing decks
- [Building from source](docs/BUILDING.md): iPad and Mac builds, releases, tests
- [Technical notes](docs/TECHNICAL.md): how PDF media is found, ink, zoom, performance
- [Changelog](CHANGELOG.md)

## License

PenCast is free software under the [GNU General Public License v3.0](LICENSE).
