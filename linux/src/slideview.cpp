#include "slideview.h"

#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPdfDocument>
#include <QPdfDocumentRenderOptions>
#include <QTabletEvent>
#include <QTimer>

// Laser dot diameter and trail width, in slide points, as on the iPad.
static constexpr double laserDotSize = 7, laserTrailWidth = 4;

SlideView::SlideView(Deck *deck, Role role, ToolSettings *tools, QWidget *parent)
    : QWidget(parent), m_deck(deck), m_role(role), m_tools(tools) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(160, 90);
    if (role == Role::Presenter) {
        setMouseTracking(true);                  // laser dot follows a hovering mouse
        setAttribute(Qt::WA_TabletTracking);     // … or a hovering pen
        setFocusPolicy(Qt::StrongFocus);
    }
    connect(deck, &Deck::reloaded, this, [this] { m_cache.clear(); m_baseDirty = true; update(); });
    connect(deck, &Deck::changed, this, [this] { m_baseDirty = true; update(); });
    connect(deck, &Deck::drawn, this, qOverload<>(&QWidget::update));
    updateCursor();
}

void SlideView::updateCursor() {
    if (m_role != Role::Presenter || !m_tools) return;
    switch (m_tools->tool) {
    case Tool::Pointer: setCursor(Qt::ArrowCursor); break;
    case Tool::Laser: setCursor(Qt::BlankCursor); break;   // the laser dot is the cursor
    default: setCursor(Qt::CrossCursor); break;
    }
    if (m_tools->tool != Tool::Laser) m_deck->laserLeave();
}

// MARK: Geometry

std::optional<QRectF> SlideView::box() const {
    if (!m_deck->isOpen()) return std::nullopt;
    return m_role == Role::Notes ? m_deck->notesBox(m_deck->page()) : m_deck->slideBox(m_deck->page());
}

double SlideView::scale() const {
    const auto b = box();
    if (!b || b->isEmpty()) return 1;
    return std::min(width() / b->width(), height() / b->height());
}

QRectF SlideView::target() const {
    const auto b = box();
    if (!b) return {};
    const QSizeF s = b->size() * scale();
    return QRectF(QPointF((width() - s.width()) / 2, (height() - s.height()) / 2), s);
}

QPointF SlideView::toSlide(QPointF pos) const { return (pos - target().topLeft()) / scale(); }

// MARK: Drawing

/// The part of `page` this view shows, rendered at `pixels`. Cached with the next slide, which is
/// rendered right after a paint so turning the slide shows it at once.
QImage SlideView::slideImage(int page, QSize pixels) {
    QPdfDocument *doc = m_deck->doc();
    if (!doc || page < 0 || page >= doc->pageCount() || pixels.isEmpty()) return {};
    const QString key = QStringLiteral("%1/%2/%3x%4").arg(m_deck->generation()).arg(page).arg(pixels.width()).arg(pixels.height());
    if (auto it = m_cache.constFind(key); it != m_cache.constEnd()) return *it;
    const QSizeF pageSize = doc->pagePointSize(page);
    const auto split = Slide::split(pageSize);
    const QRectF part = m_role == Role::Notes && split.notes ? *split.notes : split.slide;
    QPdfDocumentRenderOptions options;
    QImage img;
    if (part.size() == pageSize) {
        img = doc->render(page, pixels, options);
    } else {   // slide or notes half of a Beamer notes page
        const double k = pixels.width() / part.width();
        options.setScaledSize((pageSize * k).toSize());
        options.setScaledClipRect(QRectF(part.topLeft() * k, part.size() * k).toRect());
        img = doc->render(page, pixels, options);
    }
    if (m_cache.size() >= 4) m_cache.clear();   // ponytail: keeps the current and next slide
    m_cache.insert(key, img);
    return img;
}

void SlideView::drawStroke(QPainter &p, const Stroke &s) {
    p.save();
    if (s.kind == Stroke::Highlighter) {
        // Multiply: the text under the highlighter stays dark, and overlaps don't build up.
        p.setCompositionMode(QPainter::CompositionMode_Multiply);
        QPainterPath path(s.points.first().p);
        for (const InkPoint &pt : s.points) path.lineTo(pt.p);
        p.setPen(QPen(s.color, s.points.first().w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
    } else if (s.points.size() == 1) {
        const InkPoint &pt = s.points.first();
        p.setPen(Qt::NoPen);
        p.setBrush(s.color);
        p.drawEllipse(pt.p, pt.w / 2, pt.w / 2);
    } else {
        // Opaque, so round-capped segments of varying width join without seams.
        for (int i = 1; i < s.points.size(); ++i) {
            const InkPoint &a = s.points[i - 1], &b = s.points[i];
            p.setPen(QPen(s.color, (a.w + b.w) / 2, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(a.p, b.p);
        }
    }
    p.restore();
}

void SlideView::rebuildBase() {
    const qreal dpr = devicePixelRatioF();
    m_base = QPixmap(size() * dpr);
    m_base.setDevicePixelRatio(dpr);
    m_base.fill(Qt::black);
    m_baseDirty = false;
    const QRectF t = target();
    if (t.isEmpty()) return;
    QPainter p(&m_base);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QImage img = slideImage(m_deck->page(), (t.size() * dpr).toSize());
    p.drawImage(t, img);
    if (m_role == Role::Notes) return;
    p.translate(t.topLeft());
    p.scale(scale(), scale());
    for (const Stroke &s : m_deck->ink()) drawStroke(p, s);
}

void SlideView::drawLaser(QPainter &p) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    double alpha = 1;
    if (!m_deck->laserPressing()) {
        const qint64 since = now - m_deck->laserLiftedAt() - Deck::laserHoldMs;
        alpha = std::clamp(1.0 - double(since) / Deck::laserFadeMs, 0.0, 1.0);
    }
    for (const auto &line : m_deck->laser()) {
        if (line.isEmpty() || alpha <= 0) continue;
        QPainterPath path(line.first().p);
        for (const LaserPoint &pt : line) path.lineTo(pt.p);
        p.setPen(QPen(QColor(255, 30, 30, int(70 * alpha)), laserTrailWidth * 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);   // glow
        p.setPen(QPen(QColor(255, 40, 30, int(255 * alpha)), laserTrailWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
    }
    if (const auto dot = m_deck->laserDot()) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 30, 30, 70));
        p.drawEllipse(*dot, laserDotSize, laserDotSize);
        p.setBrush(QColor(255, 40, 30));
        p.drawEllipse(*dot, laserDotSize / 2, laserDotSize / 2);
    }
}

void SlideView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    if (m_role == Role::Projector) {   // the audience sees slides only while presenting
        const Deck::Projector mode = m_deck->presenting() ? m_deck->projector() : Deck::Projector::Black;
        if (mode == Deck::Projector::Black) { p.fillRect(rect(), Qt::black); return; }
        if (mode == Deck::Projector::White) { p.fillRect(rect(), Qt::white); return; }
        if (mode == Deck::Projector::Frozen) {
            p.fillRect(rect(), Qt::black);
            const QImage &img = m_deck->frozen();
            const QSizeF s = img.deviceIndependentSize().scaled(size(), Qt::KeepAspectRatio);
            p.drawImage(QRectF(QPointF((width() - s.width()) / 2, (height() - s.height()) / 2), s), img);
            return;
        }
    }
    if (m_baseDirty || m_base.deviceIndependentSize() != QSizeF(size())) rebuildBase();
    p.drawPixmap(0, 0, m_base);
    if (m_role == Role::Notes || !m_deck->isOpen()) return;
    const QRectF t = target();
    p.setRenderHint(QPainter::Antialiasing);
    p.setClipRect(t);
    p.translate(t.topLeft());
    p.scale(scale(), scale());
    if (const auto &live = m_deck->liveStroke(); live && !live->points.isEmpty()) drawStroke(p, *live);
    drawLaser(p);
    if (m_deck->page() + 1 < m_deck->pageCount()) {
        const int next = m_deck->page() + 1;
        const QSize pixels = (t.size() * devicePixelRatioF()).toSize();
        QTimer::singleShot(0, this, [this, next, pixels] { slideImage(next, pixels); });
    }
}

void SlideView::resizeEvent(QResizeEvent *) { m_baseDirty = true; }

// MARK: Input
// A tablet pen (Wacom, a 2-in-1's stylus) arrives as tablet events with pressure; its eraser end or barrel
// button erases. A mouse or finger arrives as mouse events and draws a mid-weight line.

void SlideView::press(QPointF pos, std::optional<double> pressure, bool erase) {
    if (!m_deck->isOpen() || !m_tools) return;
    const QPointF pt = toSlide(pos);
    if (erase || m_tools->tool == Tool::Eraser) {
        m_gesture = Gesture::Erase;
        m_deck->beginErase();
        m_deck->eraseAt(pt, m_tools->eraserRadius / scale());
        return;
    }
    switch (m_tools->tool) {
    case Tool::Pen:
    case Tool::Highlighter: {
        const bool pen = m_tools->tool == Tool::Pen;
        m_lastWidth = pen ? PressureCurve{m_tools->penWidth, m_tools->fine, m_tools->firm}.width(pressure)
                          : m_tools->highlighterWidth;
        Stroke s{pen ? Stroke::Pen : Stroke::Highlighter, pen ? m_tools->penColor : m_tools->highlighterColor, {{pt, m_lastWidth}}};
        m_gesture = Gesture::Ink;
        m_deck->beginStroke(s);
        break;
    }
    case Tool::Laser:
        m_gesture = Gesture::Laser;
        m_deck->laserPress(pt);
        break;
    case Tool::Pointer:   // click near an edge to turn the slide, like a tap on the iPad
        if (pos.x() < width() * 0.15) m_deck->go(m_deck->page() - 1);
        else if (pos.x() > width() * 0.85) m_deck->go(m_deck->page() + 1);
        break;
    default: break;
    }
}

void SlideView::move(QPointF pos, std::optional<double> pressure, bool down) {
    if (!m_deck->isOpen() || !m_tools) return;
    const QPointF pt = toSlide(pos);
    if (!down || m_gesture == Gesture::None) {
        if (m_tools->tool == Tool::Laser) m_deck->laserMove(pt);   // hovering: the dot follows
        return;
    }
    switch (m_gesture) {
    case Gesture::Ink: {
        const auto &live = m_deck->liveStroke();
        if (!live) return;
        // As the tip leaves the screen it reports almost no pressure and a jumpy position: skip those.
        if (pressure && *pressure < 0.02) return;
        if (QLineF(live->points.last().p, pt).length() * scale() < 0.5) return;   // sub-pixel moves
        double w = m_lastWidth;
        if (live->kind == Stroke::Pen) {
            const double target = PressureCurve{m_tools->penWidth, m_tools->fine, m_tools->firm}.width(pressure);
            w = m_lastWidth + (target - m_lastWidth) * 0.3;   // ease, so pressure jitter never looks lumpy
        }
        m_lastWidth = w;
        m_deck->extendStroke({pt, w});
        break;
    }
    case Gesture::Erase: m_deck->eraseAt(pt, m_tools->eraserRadius / scale()); break;
    case Gesture::Laser: m_deck->laserMove(pt); break;
    default: break;
    }
}

void SlideView::release() {
    switch (m_gesture) {
    case Gesture::Ink: m_deck->endStroke(); break;
    case Gesture::Laser: m_deck->laserRelease(); break;
    default: break;
    }
    m_gesture = Gesture::None;
}

void SlideView::tabletEvent(QTabletEvent *e) {
    if (m_role != Role::Presenter) { e->ignore(); return; }
    e->accept();   // no synthesized mouse events for the pen
    m_penSeen = true;
    const bool hasPressure = e->pointingDevice()->capabilities().testFlag(QInputDevice::Capability::Pressure);
    const std::optional<double> pressure = hasPressure ? std::optional<double>(e->pressure()) : std::nullopt;
    const bool erase = e->pointerType() == QPointingDevice::PointerType::Eraser
                       || (e->buttons() & (Qt::RightButton | Qt::MiddleButton));   // barrel button
    switch (e->type()) {
    case QEvent::TabletPress:
        m_tabletDown = true;
        press(e->position(), pressure, erase);
        break;
    case QEvent::TabletMove: move(e->position(), pressure, m_tabletDown); break;
    case QEvent::TabletRelease:
        m_tabletDown = false;
        release();
        break;
    default: break;
    }
}

/// Once the pen has been used, a finger on the slide is the hand resting on it (as on the iPad: only the
/// pen draws). A mouse still works, except while the pen is down.
bool SlideView::palm(QMouseEvent *e) const {
    const bool finger = e->pointingDevice() && e->pointingDevice()->type() == QInputDevice::DeviceType::TouchScreen;
    return m_tabletDown || (finger && m_penSeen);
}

void SlideView::mousePressEvent(QMouseEvent *e) {
    if (m_role != Role::Presenter) { e->ignore(); return; }
    if (palm(e) || e->button() != Qt::LeftButton) return;
    press(e->position(), std::nullopt, false);
}

void SlideView::mouseMoveEvent(QMouseEvent *e) {
    if (m_role != Role::Presenter) { e->ignore(); return; }
    if (palm(e)) return;
    move(e->position(), std::nullopt, e->buttons() & Qt::LeftButton);
}

void SlideView::mouseReleaseEvent(QMouseEvent *e) {
    if (m_role != Role::Presenter) { e->ignore(); return; }
    if (palm(e) || e->button() != Qt::LeftButton) return;
    release();
}

void SlideView::leaveEvent(QEvent *) {
    if (m_role == Role::Presenter && m_gesture == Gesture::None) m_deck->laserLeave();
}
