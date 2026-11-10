// Drives the presenter's slide with synthetic pen, eraser and finger events, standing in for a 2-in-1.
// Runs without a screen (QT_QPA_PLATFORM=offscreen); ink goes to Qt's test location, not the real one.
#include "playback.h"
#include "slideview.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QTest>
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

static void mouse(QWidget *w, QPointF from, QPointF to, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    auto send = [&](QEvent::Type t, QPointF p, Qt::MouseButton b, Qt::MouseButtons bs) {
        QMouseEvent e(t, p, p, w->mapToGlobal(p), b, bs, mods);
        QApplication::sendEvent(w, &e);
    };
    send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 8; ++i) send(QEvent::MouseMove, from + (to - from) * i / 8, Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
}

/// Runs the event loop until `done` or `ms` pass.
template <typename F> static bool waitFor(F done, int ms) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
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

    // Laser, dot only: no line is kept. Ball pen: an even line whatever the pressure.
    tools.tool = Tool::Laser;
    deck.laserStyle.trail = false;
    stroke(&view, pen, {100, 100}, {300, 100}, {0.5, 0.5, 0.5});
    CHECK(deck.laser().isEmpty());
    deck.laserStyle.trail = true;
    tools.tool = Tool::Pen;
    tools.penStyle = ToolSettings::Ball;
    stroke(&view, pen, {100, 250}, {400, 250}, {0.1, 0.3, 0.6, 0.9});
    CHECK(!deck.ink().isEmpty() && deck.ink().last().points.first().w == tools.penWidth
          && deck.ink().last().points.last().w == tools.penWidth);
    tools.penStyle = ToolSettings::Fountain;
    deck.clearSlide();

    // Partial eraser: a cut through the middle leaves two pieces, one undo step.
    tools.tool = Tool::Pen;
    mouse(&view, {100, 380}, {500, 380});
    CHECK(deck.ink().size() == 1);
    tools.tool = Tool::Eraser;
    tools.partialEraser = true;
    tools.eraserSize = 30;
    mouse(&view, {300, 340}, {300, 420});
    CHECK(deck.ink().size() == 2);
    deck.undo();
    CHECK(deck.ink().size() == 1);
    tools.partialEraser = false;

    // Lasso: circle the stroke, drag it down, delete it.
    tools.tool = Tool::Lasso;
    view.updateCursor();
    {
        auto send = [&](QEvent::Type t, QPointF p, Qt::MouseButton b, Qt::MouseButtons bs) {
            QMouseEvent e(t, p, p, view.mapToGlobal(p), b, bs, Qt::NoModifier);
            QApplication::sendEvent(&view, &e);
        };
        const QPointF loop[] = {{60, 340}, {540, 340}, {540, 420}, {60, 420}, {60, 345}};
        send(QEvent::MouseButtonPress, loop[0], Qt::LeftButton, Qt::LeftButton);
        for (QPointF p : loop) send(QEvent::MouseMove, p, Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, loop[4], Qt::LeftButton, Qt::NoButton);
    }
    CHECK(deck.selection() == QVector<int>{0});
    const double before = deck.ink().first().points.first().p.y();
    mouse(&view, {300, 380}, {300, 330});
    CHECK(deck.ink().first().points.first().p.y() < before - 10);
    deck.duplicateSelection();
    CHECK(deck.ink().size() == 2 && deck.selection() == QVector<int>{1});
    deck.deleteSelection();
    CHECK(deck.ink().size() == 1);
    deck.undo();
    CHECK(deck.ink().size() == 2);
    deck.clearSlide();

    // Zoom: Ctrl+wheel zooms at the pointer, a pointer drag pans, the zoom stays inside the slide.
    tools.tool = Tool::Pointer;
    {
        QWheelEvent wheel({400, 225}, view.mapToGlobal(QPointF(400, 225)), {}, {0, 480}, Qt::NoButton, Qt::ControlModifier,
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(&view, &wheel);
    }
    CHECK(deck.zoom() && deck.zoom()->width() < deck.slideBox(deck.page()).width() / 1.5);
    const QRectF z = deck.zoom().value_or(QRectF());
    mouse(&view, {400, 225}, {300, 225});
    CHECK(deck.zoom() && deck.zoom()->x() > z.x());
    const int page = deck.page();
    mouse(&view, {790, 225}, {790, 225});   // an edge click while zoomed never turns the slide
    CHECK(deck.page() == page);
    deck.zoomBy(100, {0, 0});
    CHECK(std::abs(deck.zoom().value_or(QRectF()).width() - deck.slideBox(page).width() / Deck::maxZoom) < 0.01);
    deck.resetZoom();
    CHECK(!deck.zoom());

    // Media: a video plays in place under the ink, a click shows its controls; an animation advances.
    // (Only slides whose media has no sound: clip1080p60 is silent.)
    {
        Deck media;
        CHECK(media.open(CORPUS "/media_test.pdf").isEmpty());
        media.setPresenting(true);
        SlideView mv(&media, SlideView::Role::Presenter, &tools);
        mv.resize(800, 450);
        mv.show();

        // Touchscreen: one finger writes; a second finger turns it into a pinch (no stray ink), and the
        // slide point between the fingers stays under them; a quick two-finger tap shows the whole slide.
        {
            QPointingDevice *screen = QTest::createTouchDevice();
            auto at = [](QPointF p) { return p.toPoint(); };
            tools.tool = Tool::Pen;
            media.clearSlide();
            QTest::touchEvent(&mv, screen).press(0, at({200, 300}));
            QTest::touchEvent(&mv, screen).move(0, at({240, 300}));
            QTest::touchEvent(&mv, screen).release(0, at({280, 300}));
            CHECK(media.ink().size() == 1);
            const QRectF full(QPointF(0, 0), media.slideBox(media.page()).size());
            const double k = std::min(800 / full.width(), 450 / full.height());
            const QPointF origin((800 - full.width() * k) / 2, (450 - full.height() * k) / 2);
            const QPointF anchor = (QPointF(401, 225) - origin) / k;   // between the fingers when the second lands
            QTest::touchEvent(&mv, screen).press(0, at({350, 225}));
            QTest::touchEvent(&mv, screen).move(0, at({352, 225})).press(1, at({450, 225}));
            for (int i = 1; i <= 5; ++i)
                QTest::touchEvent(&mv, screen).move(0, at({350.0 - 20 * i, 225})).move(1, at({450.0 + 20 * i, 225}));
            QTest::touchEvent(&mv, screen).release(0, at({250, 225})).release(1, at({550, 225}));
            CHECK(media.ink().size() == 1);   // the first finger's start of a stroke was dropped
            CHECK(media.zoom() && std::abs(media.zoom()->width() - full.width() * 98 / 300) < 0.5);
            if (const auto z = media.zoom()) {   // the anchor is still at the fingers' centre
                const double s = std::min(800 / z->width(), 450 / z->height());
                CHECK(QLineF(origin + (anchor - z->topLeft()) * s, QPointF(400, 225)).length() < 1.5);
            }
            QTest::touchEvent(&mv, screen).press(0, at({300, 200})).press(1, at({400, 200}));
            QTest::touchEvent(&mv, screen).release(0, at({300, 200})).release(1, at({400, 200}));
            CHECK(!media.zoom());
            media.clearSlide();
            tools.tool = Tool::Pointer;
        }
        media.go(2);
        const auto items = media.pageMedia();
        CHECK(items.size() == 1);
        const QString id = items.value(0).id();
        CHECK(waitFor([&] { return media.video(id) != nullptr; }, 15000));
        if (Video *v = media.video(id)) {
            CHECK(!v->playing());   // not autoplay
            const QRectF r = media.mediaRect(items[0]);
            const double k = std::min(800 / media.slideBox(2).width(), 450 / media.slideBox(2).height());
            const QPointF centre = QPointF((800 - media.slideBox(2).width() * k) / 2, (450 - media.slideBox(2).height() * k) / 2)
                                   + r.center() * k;
            mouse(&mv, centre, centre);   // pointer click: the controls
            QWidget *controls = mv.findChild<QWidget *>("videoControls");
            CHECK(controls && controls->isVisible());
            v->toggle();
            // Decoding depends on the system's codecs (and macOS's backend needs a real event loop), so a
            // missing decoder is reported, not failed; the slide then shows the error over the poster.
            waitFor([&] { return (!v->frame().isNull() && v->position() > 600) || !v->error().isEmpty(); }, 10000);
            std::printf("video: playing %d, %lld of %lld ms, frame %dx%d%s\n", v->playing(), v->position(), v->duration(),
                        v->frame().width(), v->frame().height(), qPrintable(v->error().isEmpty() ? "" : ", " + v->error()));
            if (!v->error().isEmpty()) CHECK(media.problem(id) == v->error());
            const QImage shot = mv.grab().toImage();
            CHECK(!shot.isNull());
            if (qEnvironmentVariableIsSet("PENCAST_TEST_OUT")) shot.save(qEnvironmentVariable("PENCAST_TEST_OUT") + "/video.png");
        }
        media.go(5);
        const auto anim = media.pageMedia();
        CHECK(anim.size() == 1 && anim.value(0).kind == MediaItem::Animation);
        Animation *a = media.animation(anim.value(0).id());
        CHECK(a && a->playing());   // autoplay
        if (a) {
            const QImage first = a->frame({120, 120});
            CHECK(waitFor([&] { return a->frame({120, 120}) != first; }, 3000));
            if (qEnvironmentVariableIsSet("PENCAST_TEST_OUT")) mv.grab().save(qEnvironmentVariable("PENCAST_TEST_OUT") + "/anim.png");
        }
        // A pointer click on a \hyperlink button follows it.
        tools.tool = Tool::Pointer;
        for (int pg = 0; pg < media.pageCount(); ++pg) {
            for (const Media::Link &l : Media::links(*media.raw(), pg)) {
                if (l.action != Media::Link::GoTo || l.page == pg) continue;
                media.go(pg);
                const QRectF r = Media::onPage(l.rect, media.raw()->pageBox(pg)).translated(-media.slideBox(pg).topLeft());
                const double k = std::min(800 / media.slideBox(pg).width(), 450 / media.slideBox(pg).height());
                const QPointF at = QPointF((800 - media.slideBox(pg).width() * k) / 2, (450 - media.slideBox(pg).height() * k) / 2)
                                   + r.center() * k;
                mouse(&mv, at, at);
                CHECK(media.page() == l.page);
                pg = media.pageCount();
                break;
            }
        }
        media.setPresenting(false);
        CHECK(media.pageMedia().size() == 1 && !media.animation(anim.value(0).id()));   // stopped
    }

    std::printf(failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
