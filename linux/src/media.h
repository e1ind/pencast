// Videos and animations in a deck, and writing PDFs from it: the Linux counterpart of MediaScanner.swift
// (same rules, same results) and of the Swift export. No windows, no playback.
#pragma once

#include "core.h"
#include "pdf.h"

#include <QHash>

/// One playable thing on a slide.
struct MediaItem {
    enum Kind {
        EmbeddedVideo,   // stored inside the PDF: media9 RichMedia asset, or a Screen/Rendition MediaClip
        LinkedVideo,     // \movie, or media9 whose source is not embedded: path relative to the PDF, or URL
        Animation,       // animate: widgets named "<id>.<k>", one per frame
        Unsupported,
    };
    Kind kind = Unsupported;
    int page = 0;          // 0-based
    int annotIndex = 0;    // index into the page's /Annots
    QRectF rect;           // PDF user space (y up), normalized
    QString source;        // embedded file name, linked path or URL, or why it cannot play
    int animation = 0, frameCount = 0;
    double fps = 0;
    bool autoplay = false, loop = false;

    QString id() const { return QStringLiteral("%1-%2").arg(page).arg(annotIndex); }
    /// Same source at the same spot = the same video continuing across Beamer overlay pages.
    QString continuity() const;
};

namespace Media {
QVector<MediaItem> scan(const Pdf::Document &doc, int page);
QHash<QString, QString> parseFlashVars(const QString &s);
/// "3.17" -> (3, 17): animate names frame widgets "<animation>.<frame>".
std::optional<std::pair<int, int>> animateFrame(const QString &t);

/// A link on a page: where it goes, by page or by a named action (Beamer's navigation symbols).
struct Link {
    enum Action { GoTo, Next, Previous, First, Last };
    QRectF rect;   // PDF user space
    Action action = GoTo;
    int page = -1;  // GoTo target, 0-based
};
/// Internal links (hyperref, Beamer navigation); web links are left to the reading view.
QVector<Link> links(const Pdf::Document &doc, int page);

/// `rect` (PDF user space) in top-left coordinates of the page's visible box, as Qt's PDF module uses.
QRectF onPage(const QRectF &rect, const QRectF &pageBox);

/// The decoded bytes of an embedded video, byte-identical to the file that was embedded.
/// ponytail: the whole video is held in memory once while extracting; a 200 MB+ video spikes memory here.
std::optional<QByteArray> embeddedVideo(const Pdf::Document &doc, const MediaItem &item);

/// A PDF with one page per frame of an animation, each the size of the animation on the slide.
QByteArray animationFrames(const Pdf::Document &doc, const MediaItem &item);

struct ExportPage {
    int page;
    QRectF slide;          // the part of the page to keep, top-left coordinates (Slide::split)
    QVector<Stroke> ink;   // slide points, origin the slide's top-left
};
/// A flattened copy: each page's slide part (vector), its annotations' appearances (video posters, the
/// first animation frame), then the ink as vector strokes.
QByteArray exportPdf(const Pdf::Document &doc, const QVector<ExportPage> &pages);
}
