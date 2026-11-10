#include "tooloptions.h"

#include <QColorDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSlider>
#include <QTabletEvent>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

// Size ranges, as on the iPad (Presenter.swift).
static constexpr double penMin = 0.5, penMax = 8, highlighterMin = 3, highlighterMax = 30, eraserMin = 6, eraserMax = 360;
static constexpr double dotMin = 3, dotMax = 18, trailMin = 1, trailMax = 12;
static constexpr double previewScale = 2.5;   // a full-screen slide is ~2.5 screen pixels per slide point

const QVector<QColor> penSwatches = {Qt::black, QColor(0, 102, 255), QColor(230, 30, 30), QColor(0, 140, 64),
                                     Qt::white, QColor(255, 149, 0), QColor(175, 82, 222)};
const QVector<QColor> highlighterSwatches = {QColor(255, 235, 59), QColor(140, 240, 120), QColor(255, 150, 200),
                                             QColor(120, 220, 255), QColor(255, 190, 90), QColor(200, 160, 255)};

static QLabel *caption(const QString &text) {
    auto *l = new QLabel(text);
    l->setWordWrap(true);
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() * 0.9);
    l->setFont(f);
    l->setEnabled(false);   // secondary text colour
    return l;
}

/// A slider over [lo, hi] for a double setting.
static QWidget *slider(double value, double lo, double hi, const QString &low, const QString &high,
                       std::function<void(double)> set, const QString &title = {}) {
    auto *box = new QWidget;
    auto *col = new QVBoxLayout(box);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(2);
    if (!title.isEmpty()) col->addWidget(new QLabel(title));
    auto *row = new QHBoxLayout;
    auto *s = new QSlider(Qt::Horizontal);
    s->setRange(0, 1000);
    s->setValue(int(std::round((value - lo) / (hi - lo) * 1000)));
    QObject::connect(s, &QSlider::valueChanged, s, [=](int v) { set(lo + (hi - lo) * v / 1000.0); });
    row->addWidget(caption(low));
    row->addWidget(s, 1);
    row->addWidget(caption(high));
    col->addLayout(row);
    return box;
}

/// Two-way choice, like the iPad's segmented pickers.
static QWidget *segmented(const QString &a, const QString &b, bool firstOn, std::function<void(bool first)> set) {
    auto *box = new QWidget;
    auto *row = new QHBoxLayout(box);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    for (const bool first : {true, false}) {
        auto *button = new QPushButton(first ? a : b);
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setChecked(first == firstOn);
        QObject::connect(button, &QPushButton::clicked, button, [=] { set(first); });
        row->addWidget(button, 1);
    }
    return box;
}

ToolOptions::ToolOptions(Tool tool, ToolSettings *tools, Deck *deck, std::function<void()> changed, QWidget *parent)
    : QFrame(parent, Qt::Popup), m_tool(tool), m_tools(tools), m_deck(deck), m_changed(std::move(changed)) {
    setFrameShape(QFrame::StyledPanel);
    setFixedWidth(340);
    rebuild();
}

void ToolOptions::rebuild() {
    for (QWidget *w : findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) w->deleteLater();
    delete layout();
    auto *col = new QVBoxLayout(this);
    col->setContentsMargins(14, 14, 14, 14);
    col->setSpacing(12);
    // A change that alters which controls are shown rebuilds the popup after the click that caused it.
    const auto again = [this] { m_changed(); QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection); };
    const auto colours = [&](QColor *value, const QVector<QColor> &swatches) {
        auto *row = new QHBoxLayout;
        row->setSpacing(6);
        for (const QColor &c : swatches) {
            auto *b = new QToolButton;
            b->setFixedSize(28, 28);
            const bool current = c == *value;
            b->setStyleSheet(QStringLiteral("QToolButton { background: %1; border-radius: 13px; border: 2.5px solid %2; }")
                                 .arg(c.name(), current ? palette().color(QPalette::WindowText).name() : "rgba(128,128,128,90)"));
            connect(b, &QToolButton::clicked, this, [=] { *value = c; again(); });
            row->addWidget(b);
        }
        auto *custom = new QToolButton;
        custom->setText(tr("…"));
        custom->setToolTip(tr("Custom colour"));
        connect(custom, &QToolButton::clicked, this, [=] {
            const QColor c = QColorDialog::getColor(*value, window(), tr("Ink Colour"));
            if (c.isValid()) { *value = c; again(); }
        });
        row->addWidget(custom);
        row->addStretch();
        col->addLayout(row);
    };

    switch (m_tool) {
    case Tool::Pen: {
        const bool fountain = m_tools->penStyle == ToolSettings::Fountain;
        col->addWidget(segmented(tr("Fountain pen"), tr("Ball pen"), fountain, [=](bool f) {
            m_tools->penStyle = f ? ToolSettings::Fountain : ToolSettings::Ball;
            again();
        }));
        col->addWidget(caption(fountain ? tr("Round nib; the line gets thicker as you press harder.")
                                        : tr("Round nib; an even line whatever the pressure.")));
        col->addWidget(slider(m_tools->penWidth, penMin, penMax, "•", "●", [=](double v) { m_tools->penWidth = v; m_changed(); }));
        if (fountain) {
            col->addWidget(slider(m_tools->firm, 0.15, 1, tr("Light"), tr("Hard"), [=](double v) { m_tools->firm = v; m_changed(); },
                                  tr("Press for the thickest line")));
            col->addWidget(slider(m_tools->fine, 0.05, 1, tr("Hairline"), tr("Even"), [=](double v) { m_tools->fine = v; m_changed(); },
                                  tr("Thinnest line")));
            auto *pad = new PressurePad(m_tools);
            auto *clear = new QPushButton(tr("Clear"), pad);
            clear->setFlat(true);
            connect(clear, &QPushButton::clicked, pad, &PressurePad::clear);
            clear->move(220, 4);
            col->addWidget(pad);
        } else {
            col->addWidget(new StrokePreview(m_tools->penColor, m_tools->penWidth, StrokePreview::Even));
        }
        colours(&m_tools->penColor, penSwatches);
        break;
    }
    case Tool::Highlighter:
        col->addWidget(slider(m_tools->highlighterWidth, highlighterMin, highlighterMax, "•", "●",
                              [=](double v) { m_tools->highlighterWidth = v; m_changed(); }));
        col->addWidget(new StrokePreview(m_tools->highlighterColor, m_tools->highlighterWidth, StrokePreview::Highlighter));
        colours(&m_tools->highlighterColor, highlighterSwatches);
        break;
    case Tool::Laser: {
        LaserStyle &laser = m_deck->laserStyle;
        col->addWidget(segmented(tr("Trail"), tr("Dot only"), laser.trail, [=, &laser](bool trail) { laser.trail = trail; again(); }));
        col->addWidget(caption(laser.trail ? tr("A glowing line follows the pen and fades after you lift. Press again before "
                                                "it fades to keep it and add another.")
                                            : tr("Just a glowing dot while you press, or while the mouse hovers.")));
        col->addWidget(slider(laser.dot, dotMin, dotMax, tr("Small"), tr("Large"), [=, &laser](double v) { laser.dot = v; m_changed(); },
                              tr("Dot size")));
        if (laser.trail)
            col->addWidget(slider(laser.width, trailMin, trailMax, tr("Thin"), tr("Wide"),
                                  [=, &laser](double v) { laser.width = v; m_changed(); }, tr("Trail width")));
        break;
    }
    case Tool::Eraser:
        col->addWidget(segmented(tr("Whole stroke"), tr("Partial"), !m_tools->partialEraser, [=](bool whole) {
            m_tools->partialEraser = !whole;
            again();
        }));
        if (m_tools->partialEraser)
            col->addWidget(slider(m_tools->eraserSize, eraserMin, eraserMax, "•", "●", [=](double v) { m_tools->eraserSize = v; m_changed(); }));
        break;
    default: break;
    }
    adjustSize();
}

// MARK: Previews

StrokePreview::StrokePreview(QColor color, double width, Style style, QWidget *parent)
    : QWidget(parent), m_color(color), m_width(width), m_style(style) {
    setMinimumHeight(54);
}

void StrokePreview::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const int n = 60;
    QPolygonF pts;
    for (int i = 0; i <= n; ++i) {
        const double x = double(i) / n;
        pts << QPointF(14 + x * (width() - 28), height() / 2.0 + std::sin(x * M_PI * 2) * height() * 0.22);
    }
    const double w = m_width * previewScale;
    QColor c = m_color;
    if (m_style == Highlighter) c.setAlphaF(0.4);
    p.setPen(QPen(c, w, Qt::SolidLine, m_style == Highlighter ? Qt::FlatCap : Qt::RoundCap, Qt::RoundJoin));
    p.drawPolyline(pts);
}

PressurePad::PressurePad(ToolSettings *tools, QWidget *parent) : QWidget(parent), m_tools(tools) {
    setMinimumHeight(130);
    setAttribute(Qt::WA_StaticContents);
}

void PressurePad::add(QPointF p, std::optional<double> pressure, bool start) {
    if (pressure && *pressure < 0.02 && !start) return;
    const double w = PressureCurve{m_tools->penWidth, m_tools->fine, m_tools->firm}.width(pressure) * previewScale;
    if (start || m_strokes.isEmpty()) m_strokes << Stroke{Stroke::Pen, m_tools->penColor, {}};
    m_strokes.last().points << InkPoint{p, w};
    update();
}

void PressurePad::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(128, 128, 128, 90), 1));
    p.setBrush(Qt::white);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    p.setPen(Qt::gray);
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    p.setFont(f);
    p.drawText(rect().adjusted(8, 8, -8, -8), Qt::AlignBottom | Qt::AlignLeft, tr("Try it here"));
    for (const Stroke &s : m_strokes) if (!s.points.isEmpty()) SlideView::drawStroke(p, s);
}

void PressurePad::tabletEvent(QTabletEvent *e) {
    e->accept();
    const bool hasPressure = e->pointingDevice()->capabilities().testFlag(QInputDevice::Capability::Pressure);
    const auto pressure = hasPressure ? std::optional<double>(e->pressure()) : std::nullopt;
    if (e->type() == QEvent::TabletPress) add(e->position(), pressure, true);
    else if (e->type() == QEvent::TabletMove && e->buttons()) add(e->position(), pressure, false);
}

void PressurePad::mousePressEvent(QMouseEvent *e) { add(e->position(), std::nullopt, true); }
void PressurePad::mouseMoveEvent(QMouseEvent *e) { if (e->buttons()) add(e->position(), std::nullopt, false); }
