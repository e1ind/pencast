// Logic shared by the Linux app and its test: no windows, no PDF rendering.
// Mirrors the Swift app (FountainPen.swift, SlideGeometry in MediaScanner.swift).
#pragma once

#include <QColor>
#include <QJsonArray>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QStringList>
#include <QVector>
#include <optional>

/// Fountain pen pressure response: width grows from `fine × size` at the lightest touch to `size` when
/// pressing with `firm` of the pen's full force (or harder).
struct PressureCurve {
    double size, fine, firm;
    /// `force` 0…1; nullopt when the input has no pressure (mouse, finger), which draws a mid-weight line.
    double width(std::optional<double> force) const;
};

struct InkPoint { QPointF p; double w; };   // slide points, origin top-left; w = line width there

struct Stroke {
    enum Kind { Pen, Highlighter } kind = Pen;
    QColor color;
    QVector<InkPoint> points;
};

namespace Slide {
/// Beamer `show notes on second screen`: pages are two screens wide (notes on the right) or two screens
/// tall (notes at the bottom). Page coordinates have y down, as in Qt.
struct Split { QRectF slide; std::optional<QRectF> notes; };
Split split(QSizeF page);

/// Stable per-slide keys for ink, so notes follow their slide when slides are added or removed:
/// hash of the slide text without digits/whitespace + "#n" for the n-th slide with that text.
QStringList inkKeys(const QStringList &pageTexts);

/// First page of each page's Beamer frame: overlay steps (\pause, \only) share the page label, and ink.
QVector<int> frameStarts(const QStringList &labels);
}

namespace Ink {
QJsonArray toJson(const QVector<Stroke> &strokes);
QVector<Stroke> fromJson(const QJsonArray &array);
/// Whether an eraser of `radius` at `p` touches the stroke.
bool hits(const Stroke &s, QPointF p, double radius);
/// Partial eraser: `strokes` with everything within `radius` of the segment a→b cut away (strokes split
/// where it crosses them); nullopt if nothing was hit.
std::optional<QVector<Stroke>> cut(const QVector<Stroke> &strokes, QPointF a, QPointF b, double radius);
/// Lasso: the strokes with at least half their points inside `lasso`.
QVector<int> inside(const QVector<Stroke> &strokes, const QPolygonF &lasso);
/// Bounding box of `strokes[indices]`, line widths included.
QRectF bounds(const QVector<Stroke> &strokes, const QVector<int> &indices);
}
