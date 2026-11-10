#include "core.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonObject>
#include <QLineF>
#include <algorithm>
#include <cmath>

double PressureCurve::width(std::optional<double> force) const {
    if (!force) return size * (fine + (1 - fine) * 0.5);
    const double x = std::clamp(*force / std::max(firm, 0.05), 0.0, 1.0);
    return size * (fine + (1 - fine) * x * x * (3 - 2 * x));   // smoothstep: light touches stay fine
}

namespace Slide {

Split split(QSizeF page) {
    const double w = page.width(), h = page.height(), ratio = w / std::max(h, 1.0);
    if (ratio > 2.2) return {QRectF(0, 0, w / 2, h), QRectF(w / 2, 0, w / 2, h)};   // 2 × 4:3 … 2 × 16:9
    if (ratio < 0.95) return {QRectF(0, 0, w, h / 2), QRectF(0, h / 2, w, h / 2)};  // 16:9 over 16:9
    return {QRectF(QPointF(0, 0), page), std::nullopt};
}

QStringList inkKeys(const QStringList &pageTexts) {
    QHash<QString, int> seen;
    QStringList keys;
    for (const QString &text : pageTexts) {
        QString core;
        for (QChar c : text) if (!c.isDigit() && !c.isSpace()) core += c;
        const QString hash = QString::fromLatin1(
            QCryptographicHash::hash(core.toUtf8(), QCryptographicHash::Sha256).left(8).toHex());
        const int n = seen.value(hash);
        seen[hash] = n + 1;
        keys << hash + '#' + QString::number(n);
    }
    return keys;
}

QVector<int> frameStarts(const QStringList &labels) {
    QVector<int> starts;
    for (int i = 0; i < labels.size(); ++i) {
        int j = i;
        while (j > 0 && !labels[j].isEmpty() && labels[j] == labels[j - 1]) --j;
        starts << j;
    }
    return starts;
}

}

namespace Ink {

static double r2(double v) { return std::round(v * 100) / 100; }

// {"k": "pen" | "hl", "c": "#aarrggbb", "p": [x, y, w, x, y, w, …]}
QJsonArray toJson(const QVector<Stroke> &strokes) {
    QJsonArray out;
    for (const Stroke &s : strokes) {
        QJsonArray pts;
        for (const InkPoint &p : s.points) { pts << r2(p.p.x()) << r2(p.p.y()) << r2(p.w); }
        out << QJsonObject{{"k", s.kind == Stroke::Highlighter ? "hl" : "pen"},
                           {"c", s.color.name(QColor::HexArgb)}, {"p", pts}};
    }
    return out;
}

QVector<Stroke> fromJson(const QJsonArray &array) {
    QVector<Stroke> out;
    for (const auto &v : array) {
        const QJsonObject o = v.toObject();
        Stroke s;
        s.kind = o["k"].toString() == "hl" ? Stroke::Highlighter : Stroke::Pen;
        s.color = QColor(o["c"].toString());
        const QJsonArray pts = o["p"].toArray();
        for (int i = 0; i + 2 < pts.size(); i += 3)
            s.points << InkPoint{QPointF(pts[i].toDouble(), pts[i + 1].toDouble()), pts[i + 2].toDouble()};
        if (!s.points.isEmpty() && s.color.isValid()) out << s;
    }
    return out;
}

static double distanceToSegment(QPointF p, QPointF a, QPointF b) {
    const QPointF ab = b - a;
    const double len2 = QPointF::dotProduct(ab, ab);
    const double t = len2 > 0 ? std::clamp(QPointF::dotProduct(p - a, ab) / len2, 0.0, 1.0) : 0;
    const QPointF d = p - (a + t * ab);
    return std::hypot(d.x(), d.y());
}

bool hits(const Stroke &s, QPointF p, double radius) {
    for (int i = 0; i < s.points.size(); ++i) {
        const InkPoint &a = s.points[i], &b = s.points[std::min(i + 1, int(s.points.size()) - 1)];
        if (distanceToSegment(p, a.p, b.p) <= radius + std::max(a.w, b.w) / 2) return true;
    }
    return false;
}

std::optional<QVector<Stroke>> cut(const QVector<Stroke> &strokes, QPointF a, QPointF b, double radius) {
    const QRectF reach = QRectF(a, b).normalized().adjusted(-radius, -radius, radius, radius);
    QVector<Stroke> out;
    bool changed = false;
    for (const Stroke &s : strokes) {
        const QRectF b0 = bounds({s}, {0});
        if (!b0.intersects(reach) || !hits(s, a, radius + QLineF(a, b).length())) { out << s; continue; }
        // Dense points (≤ 1 pt apart), split into the runs that lie outside the eraser.
        QVector<QVector<InkPoint>> runs(1);
        bool cutHere = false;
        auto visit = [&](const InkPoint &p) {
            if (distanceToSegment(p.p, a, b) <= radius + p.w / 2) {
                cutHere = true;
                if (!runs.last().isEmpty()) runs.append(QVector<InkPoint>());
            } else {
                runs.last() << p;
            }
        };
        visit(s.points.first());
        for (int i = 1; i < s.points.size(); ++i) {
            const InkPoint &p0 = s.points[i - 1], &p1 = s.points[i];
            const int steps = std::max(1, int(std::ceil(QLineF(p0.p, p1.p).length())));
            for (int k = 1; k <= steps; ++k) {
                const double t = double(k) / steps;
                visit({p0.p + (p1.p - p0.p) * t, p0.w + (p1.w - p0.w) * t});
            }
        }
        if (!cutHere) { out << s; continue; }
        changed = true;
        for (const auto &run : runs)
            if (run.size() > 1) out << Stroke{s.kind, s.color, run};
    }
    if (!changed) return std::nullopt;
    return out;
}

QVector<int> inside(const QVector<Stroke> &strokes, const QPolygonF &lasso) {
    QVector<int> out;
    if (lasso.size() < 3) return out;
    for (int i = 0; i < strokes.size(); ++i) {
        int in = 0;
        for (const InkPoint &p : strokes[i].points) in += lasso.containsPoint(p.p, Qt::OddEvenFill);
        if (!strokes[i].points.isEmpty() && in * 2 >= strokes[i].points.size()) out << i;
    }
    return out;
}

QRectF bounds(const QVector<Stroke> &strokes, const QVector<int> &indices) {
    QRectF r;
    for (int i : indices) {
        if (i < 0 || i >= strokes.size()) continue;
        for (const InkPoint &p : strokes[i].points)
            r |= QRectF(p.p - QPointF(p.w / 2, p.w / 2), QSizeF(p.w, p.w));
    }
    return r;
}

}
