// Drives the presenter's slide with synthetic pen, eraser and finger events, standing in for a 2-in-1.
// Runs without a screen (QT_QPA_PLATFORM=offscreen); ink goes to Qt's test location, not the real one.
#include "slideview.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QStandardPaths>
#include <QTabletEvent>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static const QPointingDevice *touch;   // devices are QObjects: made after the QApplication

static void tablet(QWidget *w, QEvent::Type type, const QPointingDevice &dev, QPointF p, double pressure,
                   Qt::MouseButtons buttons = Qt::LeftButton) {
    const Qt::MouseButton button = type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton;
    QTabletEvent e(type, &dev, p, w->mapToGlobal(p), pressure, 0, 0, 0, 0, 0, Qt::NoModifier, button, buttons);
    QApplication::sendEvent(w, &e);
}

static void stroke(QWidget *w, const QPointingDevice &dev, QPointF from, QPointF to, QVector<double> pressures,
                   Qt::MouseButtons buttons = Qt::LeftButton) {
    tablet(w, QEvent::TabletPress, dev, from, pressures.first(), buttons);
    for (int i = 1; i < pressures.size(); ++i)
        tablet(w, QEvent::TabletMove, dev, from + (to - from) * i / (pressures.size() - 1), pressures[i], buttons);
    tablet(w, QEvent::TabletRelease, dev, to, 0, Qt::NoButton);
}

static void finger(QWidget *w, QPointF from, QPointF to) {
    auto send = [&](QEvent::Type t, QPointF p, Qt::MouseButton b, Qt::MouseButtons bs) {
        QMouseEvent e(t, p, p, w->mapToGlobal(p), b, bs, Qt::NoModifier, touch);
        QApplication::sendEvent(w, &e);
    };
    send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 4; ++i) send(QEvent::MouseMove, from + (to - from) * i / 4, Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    const QPointingDevice pen("pen", 1, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
                              QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 3);
    const QPointingDevice eraser("eraser", 2, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Eraser,
                                 QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 3);
    const QPointingDevice touch("touch", 3, QInputDevice::DeviceType::TouchScreen, QPointingDevice::PointerType::Finger,
                                QInputDevice::Capability::Position, 10, 0);
    ::touch = &touch;

    Deck deck;
    CHECK(deck.open(DECK_PDF).isEmpty());
    deck.setPresenting(true);
    deck.clearSlide();
    ToolSettings tools;
    SlideView view(&deck, SlideView::Role::Presenter, &tools);
    view.resize(800, 450);
    view.show();

    // Before any pen: a finger draws a mid-weight line.
    finger(&view, {100, 100}, {300, 120});
    CHECK(deck.ink().size() == 1);

    // Pen: pressure sets the width; the near-zero tail as the tip lifts is dropped.
    stroke(&view, pen, {100, 200}, {400, 200}, {0.1, 0.1, 0.4, 0.9, 0.9, 0.9, 0.01});
    CHECK(deck.ink().size() == 2);
    const Stroke &s = deck.ink().last();
    CHECK(s.kind == Stroke::Pen);
    CHECK(s.points.size() == 6);
    CHECK(s.points.first().w < s.points.last().w);

    // After the pen: a resting palm draws nothing.
    finger(&view, {100, 300}, {300, 320});
    CHECK(deck.ink().size() == 2);

    // The eraser end and the barrel button erase, whatever tool is selected.
    stroke(&view, eraser, {200, 195}, {200, 205}, {0.5, 0.5});
    CHECK(deck.ink().size() == 1);
    stroke(&view, pen, {150, 100}, {250, 125}, {0.5, 0.5}, Qt::LeftButton | Qt::RightButton);
    CHECK(deck.ink().isEmpty());
    CHECK(tools.tool == Tool::Pen);

    // Hovering pen with the laser: the dot follows without drawing.
    tools.tool = Tool::Laser;
    tablet(&view, QEvent::TabletMove, pen, {300, 300}, 0, Qt::NoButton);
    CHECK(deck.laserDot().has_value() && deck.laser().isEmpty());

    deck.undo();   // undoes the barrel-button erase
    CHECK(deck.ink().size() == 1);
    deck.clearSlide();

    std::printf(failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
