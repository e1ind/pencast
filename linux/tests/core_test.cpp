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

    std::printf(failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
