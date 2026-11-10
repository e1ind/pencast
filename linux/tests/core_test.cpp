// Checks the shared logic: run `ctest --test-dir build` (or ./build/core_test).
#include "core.h"

#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
static bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

int main() {
    // Pressure: fine at a light touch, full size from `firm` up, mid weight without pressure.
    const PressureCurve c{2, 0.25, 0.55};
    CHECK(near(c.width(0.0), 0.5));
    CHECK(near(c.width(0.55), 2));
    CHECK(near(c.width(1.0), 2));
    CHECK(near(c.width(std::nullopt), 2 * (0.25 + 0.75 * 0.5)));
    CHECK(c.width(0.2) < c.width(0.3));

    // Beamer notes: side by side, stacked, or none.
    auto wide = Slide::split(QSizeF(2 * 454, 255));
    CHECK(wide.slide == QRectF(0, 0, 454, 255) && wide.notes == QRectF(454, 0, 454, 255));
    auto tall = Slide::split(QSizeF(454, 2 * 255));
    CHECK(tall.slide == QRectF(0, 0, 454, 255) && tall.notes == QRectF(0, 255, 454, 255));
    CHECK(!Slide::split(QSizeF(454, 255)).notes);

    // Ink keys ignore frame numbers, and number repeats.
    auto keys = Slide::inkKeys({"Intro 1/20", "Intro 2/21", "Other"});
    CHECK(keys[0].endsWith("#0") && keys[1].endsWith("#1"));
    CHECK(keys[0].section('#', 0, 0) == keys[1].section('#', 0, 0));
    CHECK(keys[2].endsWith("#0") && keys[2] != keys[0]);

    // Overlay steps share their frame's first page; unlabelled pages stand alone.
    CHECK((Slide::frameStarts({"1", "2", "2", "2", "3"}) == QVector<int>{0, 1, 1, 1, 4}));
    CHECK((Slide::frameStarts({"", ""}) == QVector<int>{0, 1}));

    // Ink survives a save and load; the eraser hits near the line only.
    Stroke s{Stroke::Pen, QColor(255, 0, 0), {{QPointF(10, 10), 2}, {QPointF(50, 10), 3}}};
    Stroke h{Stroke::Highlighter, QColor(255, 220, 0, 100), {{QPointF(0, 40), 10}}};
    auto back = Ink::fromJson(Ink::toJson({s, h}));
    CHECK(back.size() == 2 && back[0].points.size() == 2 && back[1].kind == Stroke::Highlighter);
    CHECK(back[0].color == s.color && back[1].color.alpha() == 100 && near(back[0].points[1].w, 3));
    CHECK(Ink::hits(s, QPointF(30, 13), 2));
    CHECK(!Ink::hits(s, QPointF(30, 20), 2));
    CHECK(Ink::hits(h, QPointF(0, 46), 2));   // a single-point stroke is a dot

    // Partial eraser: a cut through the middle of a line leaves two pieces; a miss changes nothing.
    {
        const Stroke line{Stroke::Pen, Qt::black, {{QPointF(0, 0), 2}, {QPointF(100, 0), 2}}};
        const auto cut = Ink::cut({line}, QPointF(50, -20), QPointF(50, 20), 5);
        CHECK(cut && cut->size() == 2);
        if (cut && cut->size() == 2) {
            CHECK((*cut)[0].points.last().p.x() < 44 && (*cut)[1].points.first().p.x() > 56);
            CHECK(near((*cut)[0].points.first().p.x(), 0) && near((*cut)[1].points.last().p.x(), 100));
        }
        CHECK(!Ink::cut({line}, QPointF(50, 30), QPointF(60, 30), 5));
        const auto all = Ink::cut({line}, QPointF(-10, 0), QPointF(110, 0), 5);   // erased entirely
        CHECK(all && all->isEmpty());
    }

    // Lasso picks strokes mostly inside; bounds include the line width.
    {
        const Stroke in{Stroke::Pen, Qt::black, {{QPointF(10, 10), 2}, {QPointF(20, 10), 2}}};
        const Stroke out{Stroke::Pen, Qt::black, {{QPointF(80, 80), 2}, {QPointF(90, 80), 2}}};
        const QPolygonF lasso({QPointF(0, 0), QPointF(50, 0), QPointF(50, 50), QPointF(0, 50)});
        CHECK(Ink::inside({in, out}, lasso) == QVector<int>{0});
        CHECK(Ink::bounds({in, out}, {0}) == QRectF(9, 9, 12, 2));
    }

    std::printf(failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
