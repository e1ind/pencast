#include "slideview.h"
#include "playback.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPdfDocument>
#include <QPdfDocumentRenderOptions>
#include <QSlider>
#include <QStyle>
#include <QTabletEvent>
#include <QToolButton>


SlideView::SlideView(Deck *deck, Role role, ToolSettings *tools, QWidget *parent)
    : QWidget(parent), m_deck(deck), m_role(role), m_tools(tools) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(160, 90);
    if (role == Role::Presenter) {
        setMouseTracking(true);                  // laser dot follows a hovering mouse
        setAttribute(Qt::WA_TabletTracking);     // … or a hovering pen
        setAttribute(Qt::WA_AcceptTouchEvents);  // fingers: write, tap, and pinch to zoom
        setFocusPolicy(Qt::StrongFocus);

        m_controls = new QWidget(this);
        m_controls->setObjectName("videoControls");
        m_controls->setAttribute(Qt::WA_StyledBackground);
        m_controls->setStyleSheet("#videoControls { background: rgba(30, 30, 30, 200); border-radius: 10px; }"
                                  "QLabel { color: white; } QToolButton { border: none; }");
        auto *row = new QHBoxLayout(m_controls);
        row->setContentsMargins(8, 4, 10, 4);
        m_playButton = new QToolButton;
        m_playButton->setIconSize(QSize(22, 22));
        m_playButton->setFocusPolicy(Qt::NoFocus);
        m_scrubber = new QSlider(Qt::Horizontal);
        m_scrubber->setFocusPolicy(Qt::NoFocus);
        m_time = new QLabel;
        row->addWidget(m_playButton);
        row->addWidget(m_scrubber, 1);
        row->addWidget(m_time);
        m_controls->hide();
        connect(m_playButton, &QToolButton::clicked, this, [this] {
            if (Video *v = m_deck->video(m_controlsFor)) v->toggle();
            m_controlsHide.start();
        });
        connect(m_scrubber, &QSlider::sliderMoved, this, [this](int ms) {
            if (Video *v = m_deck->video(m_controlsFor)) v->seek(ms);
            m_controlsHide.start();
        });
        m_controlsHide.setSingleShot(true);
        m_controlsHide.setInterval(3000);
        connect(&m_controlsHide, &QTimer::timeout, this, [this] {
            if (m_scrubber->isSliderDown()) { m_controlsHide.start(); return; }
            m_controlsFor.clear();
            m_controls->hide();
        });
    }
    connect(deck, &Deck::reloaded, this, [this] { m_cache.clear(); m_baseDirty = true; update(); });
    connect(deck, &Deck::changed, this, [this] {
        m_baseDirty = true;
        if (m_controls && !m_deck->video(m_controlsFor)) { m_controlsFor.clear(); m_controls->hide(); }
        updateControls();
        update();
    });
    connect(deck, &Deck::drawn, this, [this] { updateControls(); update(); });
    updateCursor();
}

void SlideView::updateCursor() {
    if (m_role != Role::Presenter || !m_tools) return;
    switch (m_tools->tool) {
    case Tool::Pointer: setCursor(Qt::ArrowCursor); break;
    case Tool::Laser: setCursor(Qt::BlankCursor); break;   // the laser dot is the cursor
    case Tool::Eraser: setCursor(m_tools->partialEraser ? Qt::BlankCursor : Qt::CrossCursor); break;   // the ring is
    default: setCursor(Qt::CrossCursor); break;
    }
    if (m_tools->tool != Tool::Laser) m_deck->laserLeave();
    if (m_tools->tool != Tool::Lasso) m_deck->clearSelection();
    update();
}

// MARK: Geometry

std::optional<QRectF> SlideView::box() const {
    if (!m_deck->isOpen()) return std::nullopt;
    return m_role == Role::Notes ? m_deck->notesBox(m_deck->page()) : m_deck->slideBox(m_deck->page());
}

QRectF SlideView::region() const {
    const auto b = box();
    if (!b) return {};
    const QRectF full(QPointF(0, 0), b->size());
    if (m_role == Role::Notes || (m_role == Role::Projector && !m_deck->projectorFollowsZoom())) return full;
    return m_deck->zoom().value_or(full);
}

double SlideView::scale() const {
    const QRectF r = region();
    if (r.isEmpty()) return 1;
    return std::min(width() / r.width(), height() / r.height());
}

QRectF SlideView::target() const {
    const QRectF r = region();
    if (r.isEmpty()) return {};
    const QSizeF s = r.size() * scale();
    return QRectF(QPointF((width() - s.width()) / 2, (height() - s.height()) / 2), s);
}

QPointF SlideView::toSlide(QPointF pos) const { return region().topLeft() + (pos - target().topLeft()) / scale(); }

QRectF SlideView::toWidget(const QRectF &r) const {
    return QRectF(target().topLeft() + (r.topLeft() - region().topLeft()) * scale(), r.size() * scale());
}

bool SlideView::layered() const {
    return m_role != Role::Notes && m_deck->presenting() && !m_deck->pageMedia().isEmpty();
}

// MARK: Drawing

/// `region` (slide points) of the part of `page` this view shows, rendered at `pixels`. Cached with the
/// next slide, which is rendered right after a paint so turning the slide shows it at once.
QImage SlideView::slideImage(int page, const QRectF &region, QSize pixels) {
    QPdfDocument *doc = m_deck->doc();
    if (!doc || page < 0 || page >= doc->pageCount() || pixels.isEmpty() || region.isEmpty()) return {};
    const QString key = QStringLiteral("%1/%2/%3x%4/%5,%6,%7").arg(m_deck->generation()).arg(page).arg(pixels.width())
                            .arg(pixels.height()).arg(region.x(), 0, 'f', 2).arg(region.y(), 0, 'f', 2).arg(region.width(), 0, 'f', 2);
    if (auto it = m_cache.constFind(key); it != m_cache.constEnd()) return *it;
    const QSizeF pageSize = doc->pagePointSize(page);
    const auto split = Slide::split(pageSize);
    const QRectF part = m_role == Role::Notes && split.notes ? *split.notes : split.slide;
    const QRectF shown(part.topLeft() + region.topLeft(), region.size());
    QPdfDocumentRenderOptions options;
    QImage img;
    if (shown.size() == pageSize) {
        img = doc->render(page, pixels, options);
    } else {   // slide or notes half of a Beamer notes page, or a zoomed part
        const double k = pixels.width() / shown.width();
        options.setScaledSize((pageSize * k).toSize());
        options.setScaledClipRect(QRectF(shown.topLeft() * k, shown.size() * k).toRect());
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
    p.drawImage(t, slideImage(m_deck->page(), region(), (t.size() * dpr).toSize()));
    if (m_role == Role::Notes || layered()) return;
    p.setClipRect(t);
    p.translate(t.topLeft());
    p.scale(scale(), scale());
    p.translate(-region().topLeft());
    for (const Stroke &s : m_deck->ink()) drawStroke(p, s);
}

/// Videos and animations in place, under the ink; a problem is a visible message, never a silent blank.
void SlideView::drawMedia(QPainter &p) {
    const qreal dpr = devicePixelRatioF();
    for (const MediaItem &item : m_deck->pageMedia()) {
        const QRectF r = toWidget(m_deck->mediaRect(item));
        if (Video *v = m_deck->video(item.id()); v && !v->frame().isNull() && v->error().isEmpty()) {
            const QSizeF s = QSizeF(v->frame().size()).scaled(r.size(), Qt::KeepAspectRatio);
            p.drawImage(QRectF(r.center() - QPointF(s.width() / 2, s.height() / 2), s), v->frame());
        } else if (Animation *a = m_deck->animation(item.id())) {
            p.drawImage(r, a->frame((r.size() * dpr).toSize()));
        } else if (const QString problem = m_deck->problem(item.id()); !problem.isEmpty()) {
            p.fillRect(r, QColor(0, 0, 0, 190));
            p.setPen(Qt::white);
            QFont f = font();
            f.setPixelSize(std::clamp(int(r.height() / 9), 10, 15));
            p.setFont(f);
            p.drawText(r.adjusted(6, 6, -6, -6), Qt::AlignCenter | Qt::TextWordWrap, QStringLiteral("⚠ ") + problem);
        }   // loading: the PDF's own poster shows through
    }
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
        const double w = m_deck->laserStyle.width;
        p.setPen(QPen(QColor(255, 30, 30, int(70 * alpha)), w * 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);   // glow
        p.setPen(QPen(QColor(255, 40, 30, int(255 * alpha)), w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
    }
    if (const auto dot = m_deck->laserDot()) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 30, 30, 70));
        const double d = m_deck->laserStyle.dot;
        p.drawEllipse(*dot, d, d);
        p.setBrush(QColor(255, 40, 30));
        p.drawEllipse(*dot, d / 2, d / 2);
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
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setClipRect(t);
    const bool layers = layered();
    if (layers) drawMedia(p);
    p.translate(t.topLeft());
    p.scale(scale(), scale());
    p.translate(-region().topLeft());
    if (layers) for (const Stroke &s : m_deck->ink()) drawStroke(p, s);
    if (const auto &live = m_deck->liveStroke(); live && !live->points.isEmpty()) drawStroke(p, *live);
    if (m_role == Role::Presenter) {
        const double px = 1 / scale();   // one screen pixel, in slide points
        if (!m_deck->selection().isEmpty()) {
            p.setPen(QPen(QColor(10, 132, 255), 1.5 * px, Qt::DashLine));
            p.setBrush(QColor(10, 132, 255, 18));
            p.drawRoundedRect(m_deck->selectionBounds().adjusted(-6 * px, -6 * px, 6 * px, 6 * px), 6 * px, 6 * px);
        }
        if (m_lasso.size() > 1) {
            p.setPen(QPen(QColor(10, 132, 255), 1.5 * px, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawPolyline(m_lasso);
        }
        if (m_hover && m_tools && m_tools->tool == Tool::Eraser && m_tools->partialEraser) {
            const double r = partialRadius();
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(255, 255, 255, 220), 2.5 * px));
            p.drawEllipse(*m_hover, r, r);
            p.setPen(QPen(QColor(0, 0, 0, 160), 1 * px));
            p.drawEllipse(*m_hover, r, r);
        }
    }
    drawLaser(p);
    if (m_deck->page() + 1 < m_deck->pageCount()) {
        const int next = m_deck->page() + 1;
        const QSize pixels = (QSizeF(m_deck->slideBox(next).size()).scaled(size(), Qt::KeepAspectRatio) * devicePixelRatioF()).toSize();
        const QRectF full(QPointF(0, 0), m_role == Role::Notes ? m_deck->notesBox(next).value_or(QRectF()).size()
                                                               : m_deck->slideBox(next).size());
        QTimer::singleShot(0, this, [this, next, full, pixels] { slideImage(next, full, pixels); });
    }
}

void SlideView::resizeEvent(QResizeEvent *) {
    m_baseDirty = true;
    updateControls();
}

// MARK: Video controls

void SlideView::showControls(const QString &id) {
    m_controlsFor = m_controlsFor == id ? QString() : id;
    m_controlsHide.start();
    updateControls();
}

void SlideView::updateControls() {
    if (!m_controls) return;
    Video *v = m_deck->video(m_controlsFor);
    const MediaItem *item = nullptr;
    const auto media = m_deck->pageMedia();
    for (const MediaItem &m : media) if (m.id() == m_controlsFor) item = &m;
    if (!v || !item) { m_controls->hide(); return; }
    const QRectF r = toWidget(m_deck->mediaRect(*item));
    const double w = std::max(r.width() - 16, 240.0);
    m_controls->setGeometry(QRectF(r.center().x() - w / 2, std::min(r.bottom(), double(height())) - 52, w, 44).toRect());
    m_playButton->setIcon(style()->standardIcon(v->playing() ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    if (!m_scrubber->isSliderDown()) {
        m_scrubber->setRange(0, int(std::max<qint64>(v->duration(), 0)));
        m_scrubber->setValue(int(v->position()));
    }
    const auto clock = [](qint64 ms) { return QString::asprintf("%lld:%02lld", ms / 60000, ms / 1000 % 60); };
    m_time->setText(clock(v->position()) + " / " + clock(v->duration()));
    m_controls->show();
    m_controls->raise();
}

// MARK: Input
// A tablet pen (Wacom, a 2-in-1's stylus) arrives as tablet events with pressure; its eraser end or barrel
// button erases. A mouse or finger arrives as mouse events and draws a mid-weight line.

void SlideView::press(QPointF pos, std::optional<double> pressure, bool erase) {
    if (!m_deck->isOpen() || !m_tools) return;
    const QPointF pt = toSlide(pos);
    m_pressPos = pos;
    m_last = pt;
    m_moved = false;
    if (erase || m_tools->tool == Tool::Eraser) {
        m_gesture = Gesture::Erase;
        m_deck->beginErase();
        if (m_tools->partialEraser) m_deck->eraseAlong(pt, pt, partialRadius());
        else m_deck->eraseAt(pt, m_tools->eraserRadius / scale());
        return;
    }
    switch (m_tools->tool) {
    case Tool::Pen:
    case Tool::Highlighter: {
        const bool pen = m_tools->tool == Tool::Pen;
        m_lastWidth = !pen ? m_tools->highlighterWidth
                    : m_tools->penStyle == ToolSettings::Ball ? m_tools->penWidth
                    : PressureCurve{m_tools->penWidth, m_tools->fine, m_tools->firm}.width(pressure);
        Stroke s{pen ? Stroke::Pen : Stroke::Highlighter, pen ? m_tools->penColor : m_tools->highlighterColor, {{pt, m_lastWidth}}};
        m_gesture = Gesture::Ink;
        m_deck->beginStroke(s);
        break;
    }
    case Tool::Laser:
        m_gesture = Gesture::Laser;
        m_deck->laserPress(pt);
        break;
    case Tool::Lasso: {
        const double slack = 8 / scale();
        if (!m_deck->selection().isEmpty() && m_deck->selectionBounds().adjusted(-slack, -slack, slack, slack).contains(pt)) {
            m_gesture = Gesture::Move;   // drag the selection; a tap on it opens its menu
            m_deck->beginMove();
        } else {
            m_deck->clearSelection();
            m_gesture = Gesture::Lasso;
            m_lasso = {pt};
        }
        break;
    }
    case Tool::Pointer: m_gesture = Gesture::Click; break;   // a click, or a drag that pans when zoomed
    default: break;
    }
}

void SlideView::move(QPointF pos, std::optional<double> pressure, bool down) {
    if (!m_deck->isOpen() || !m_tools) return;
    const QPointF pt = toSlide(pos);
    const bool partial = m_tools->tool == Tool::Eraser && m_tools->partialEraser;
    if (partial) { m_hover = pt; update(); }
    else if (m_hover) { m_hover.reset(); update(); }
    if (!down || m_gesture == Gesture::None) {
        if (m_tools->tool == Tool::Laser) m_deck->laserMove(pt);   // hovering: the dot follows
        if (m_tools->tool == Tool::Pointer)   // links show the hand, as in a PDF reader
            setCursor(!m_deck->zoom() && m_deck->linkAt(pt) ? Qt::PointingHandCursor : Qt::ArrowCursor);
        return;
    }
    if (QLineF(pos, m_pressPos).length() > 4) m_moved = true;
    switch (m_gesture) {
    case Gesture::Ink: {
        const auto &live = m_deck->liveStroke();
        if (!live) return;
        // As the tip leaves the screen it reports almost no pressure and a jumpy position: skip those.
        if (pressure && *pressure < 0.02) return;
        if (QLineF(live->points.last().p, pt).length() * scale() < 0.5) return;   // sub-pixel moves
        double w = m_lastWidth;
        if (live->kind == Stroke::Pen && m_tools->penStyle == ToolSettings::Fountain) {
            const double target = PressureCurve{m_tools->penWidth, m_tools->fine, m_tools->firm}.width(pressure);
            w = m_lastWidth + (target - m_lastWidth) * 0.3;   // ease, so pressure jitter never looks lumpy
        }
        m_lastWidth = w;
        m_deck->extendStroke({pt, w});
        break;
    }
    case Gesture::Erase:
        if (m_tools->partialEraser) m_deck->eraseAlong(m_last, pt, partialRadius());
        else m_deck->eraseAt(pt, m_tools->eraserRadius / scale());
        m_last = pt;
        break;
    case Gesture::Laser: m_deck->laserMove(pt); break;
    case Gesture::Lasso:
        m_lasso << pt;
        update();
        break;
    case Gesture::Move:
        m_deck->moveSelection(pt - m_last);
        m_last = pt;
        break;
    case Gesture::Click:
    case Gesture::Pan:
        if (!m_deck->zoom() || !m_moved) break;
        m_gesture = Gesture::Pan;
        m_deck->pan(m_last - pt);   // the slide follows the pointer
        break;   // m_last stays: the region moved under it instead
    default: break;
    }
}

void SlideView::release(QPointF pos) {
    switch (m_gesture) {
    case Gesture::Ink: m_deck->endStroke(); break;
    case Gesture::Laser: m_deck->laserRelease(); break;
    case Gesture::Lasso: {
        const bool tap = !m_moved;
        if (!tap) m_deck->select(m_lasso);
        m_lasso.clear();
        update();
        if (tap && m_deck->canPaste()) selectionMenu(pos, true);
        break;
    }
    case Gesture::Move:
        m_deck->endMove();
        if (!m_moved) selectionMenu(pos, false);
        break;
    case Gesture::Click: click(pos); break;
    default: break;
    }
    m_gesture = Gesture::None;
}

/// A click with the pointer: media toggles, edges navigate (never while zoomed: a stray click would jump away).
void SlideView::click(QPointF pos) {
    const QPointF pt = toSlide(pos);
    for (const MediaItem &item : m_deck->pageMedia()) {
        if (!m_deck->mediaRect(item).contains(pt)) continue;
        if (m_deck->video(item.id())) { showControls(item.id()); return; }
        if (Animation *a = m_deck->animation(item.id())) { a->toggle(); return; }
    }
    if (!m_controlsFor.isEmpty()) { m_controlsFor.clear(); m_controls->hide(); return; }   // just dismiss them
    if (m_deck->zoom()) return;
    if (const auto link = m_deck->linkAt(pt)) { m_deck->follow(*link); return; }
    if (pos.x() < width() * 0.15) m_deck->go(m_deck->page() - 1);
    else if (pos.x() > width() * 0.85) m_deck->go(m_deck->page() + 1);
}

void SlideView::selectionMenu(QPointF pos, bool paste) {
    QMenu menu;
    const QPointF at = toSlide(pos);
    if (paste) {
        menu.addAction(tr("Paste"), this, [this, at] { m_deck->paste(at); });
    } else {
        menu.addAction(tr("Cut"), m_deck, &Deck::cutSelection);
        menu.addAction(tr("Copy"), m_deck, &Deck::copySelection);
        menu.addAction(tr("Delete"), m_deck, &Deck::deleteSelection);
        menu.addAction(tr("Duplicate"), m_deck, &Deck::duplicateSelection);
    }
    menu.exec(mapToGlobal(pos.toPoint()));
}

// MARK: Touch

/// Undoes whatever one finger started when a second finger lands: the touch was a pinch, not ink.
void SlideView::cancelGesture() {
    switch (m_gesture) {
    case Gesture::Ink: m_deck->cancelStroke(); break;
    case Gesture::Laser: m_deck->laserRelease(); break;
    case Gesture::Lasso: m_lasso.clear(); update(); break;
    case Gesture::Move: m_deck->endMove(); break;
    default: break;   // ponytail: a partial erase already made stays; one undo brings it back
    }
    m_gesture = Gesture::None;
}

void SlideView::touch(QTouchEvent *e) {
    QVector<QPointF> down;
    for (const QEventPoint &p : e->points()) if (p.state() != QEventPoint::Released) down << p.position();
    if (e->type() == QEvent::TouchCancel) { cancelGesture(); m_fingerDown = m_pinching = false; return; }

    if (!m_pinching && down.size() >= 2) {   // second finger: a pinch from here on
        if (m_fingerDown) cancelGesture();
        m_fingerDown = false;
        m_pinching = true;
        m_pinchMoved = false;
        m_pinchStarted = QDateTime::currentMSecsSinceEpoch();
        m_pinchDistance = std::max(QLineF(down[0], down[1]).length(), 1.0);
        m_pinchCentre = (down[0] + down[1]) / 2;
        m_pinchRegion = region();
        m_pinchAnchor = toSlide(m_pinchCentre);
    }
    if (m_pinching) {
        if (down.size() >= 2) {
            // From the pinch's start every time, so the point under the fingers stays exactly under them.
            const double k = QLineF(down[0], down[1]).length() / m_pinchDistance;
            const QPointF centre = (down[0] + down[1]) / 2;
            if (std::abs(k - 1) > 0.04 || QLineF(centre, m_pinchCentre).length() > 8) m_pinchMoved = true;
            if (m_pinchMoved) {
                const double w = m_pinchRegion.width() / k, s = target().width() / w;
                const QSizeF size(w, w * m_pinchRegion.height() / m_pinchRegion.width());
                m_deck->zoomTo(QRectF(m_pinchAnchor - (centre - target().topLeft()) / s, size));
            }
        }
        if (down.isEmpty()) {   // all lifted; a quick two-finger tap shows the whole slide, as on the iPad
            if (!m_pinchMoved && QDateTime::currentMSecsSinceEpoch() - m_pinchStarted < 350) m_deck->resetZoom();
            m_pinching = false;
        }
        return;
    }

    if (m_tabletDown || e->points().isEmpty()) return;
    const QEventPoint &p = e->points().first();
    if (m_penSeen) {   // the pen writes; a finger taps (media, links, edges) or moves a zoomed slide
        switch (p.state()) {
        case QEventPoint::Pressed:
            m_gesture = Gesture::Click;
            m_pressPos = p.position();
            m_last = toSlide(p.position());
            m_moved = false;
            m_fingerDown = true;
            break;
        case QEventPoint::Updated: if (m_fingerDown) move(p.position(), std::nullopt, true); break;
        case QEventPoint::Released:
            if (m_fingerDown) release(p.position());
            m_fingerDown = false;
            break;
        default: break;
        }
        return;
    }
    switch (p.state()) {   // before any pen: a finger writes like a mouse
    case QEventPoint::Pressed:
        m_fingerDown = true;
        press(p.position(), std::nullopt, false);
        break;
    case QEventPoint::Updated: if (m_fingerDown) move(p.position(), std::nullopt, true); break;
    case QEventPoint::Released:
        if (m_fingerDown) release(p.position());
        m_fingerDown = false;
        break;
    default: break;
    }
}

bool SlideView::event(QEvent *e) {
    switch (e->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
        if (m_role != Role::Presenter) break;
        touch(static_cast<QTouchEvent *>(e));
        e->accept();   // no synthesized mouse events: this handles fingers itself
        return true;
    default: break;
    }
    // Trackpad pinch (Wayland and X11 via libinput).
    if (m_role == Role::Presenter && e->type() == QEvent::NativeGesture) {
        auto *g = static_cast<QNativeGestureEvent *>(e);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            m_deck->zoomBy(1 + g->value(), toSlide(g->position()));
            return true;
        }
        if (g->gestureType() == Qt::SmartZoomNativeGesture) { m_deck->resetZoom(); return true; }
    }
    return QWidget::event(e);
}

void SlideView::wheelEvent(QWheelEvent *e) {
    if (m_role != Role::Presenter || !m_deck->isOpen()) { e->ignore(); return; }
    if (e->modifiers() & Qt::ControlModifier) {   // Ctrl + wheel, or a pinch that arrives as one
        m_deck->zoomBy(std::pow(1.0015, e->angleDelta().y()), toSlide(e->position()));
    } else if (m_deck->zoom()) {   // two-finger scrolling moves around the zoomed slide
        const QPointF d = e->pixelDelta().isNull() ? QPointF(e->angleDelta()) / 2 : QPointF(e->pixelDelta());
        m_deck->pan(-d / scale());
    } else {
        e->ignore();
        return;
    }
    e->accept();
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
        release(e->position());
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
    release(e->position());
}

void SlideView::leaveEvent(QEvent *) {
    if (m_role != Role::Presenter) return;
    if (m_gesture == Gesture::None) m_deck->laserLeave();
    if (m_hover) { m_hover.reset(); update(); }
}
