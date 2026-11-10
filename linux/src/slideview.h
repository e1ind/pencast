// One slide, letterboxed, with its videos, animations, ink and laser: the presenter's canvas, the projector
// picture, or the Beamer notes beside the slide.
#pragma once

#include "deck.h"

#include <QHash>
#include <QPixmap>
#include <QTimer>
#include <QWidget>

class QLabel;
class QSlider;
class QToolButton;
class QTouchEvent;

enum class Tool { Pointer, Laser, Pen, Highlighter, Eraser, Lasso };

struct ToolSettings {
    Tool tool = Tool::Pen;
    QColor penColor = Qt::red, highlighterColor = QColor(255, 220, 0);
    // Widths in PDF points (a 16:9 Beamer slide is ~454 pt wide), as on the iPad.
    double penWidth = 1.5, highlighterWidth = 10;
    enum PenStyle { Fountain, Ball } penStyle = Fountain;   // width follows pressure, or an even line
    double fine = 0.25, firm = 0.55;   // fountain pen pressure, see PressureCurve
    double eraserRadius = 10;          // whole-stroke eraser, screen pixels
    bool partialEraser = false;
    double eraserSize = 8;             // partial eraser diameter, screen pixels (6…360, as on the iPad)
};

class SlideView : public QWidget {
    Q_OBJECT
public:
    enum class Role { Presenter, Projector, Notes };
    SlideView(Deck *deck, Role role, ToolSettings *tools = nullptr, QWidget *parent = nullptr);
    void updateCursor();

    static void drawStroke(QPainter &p, const Stroke &s);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    bool event(QEvent *) override;
    void tabletEvent(QTabletEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    enum class Gesture { None, Ink, Erase, Laser, Lasso, Move, Click, Pan };
    std::optional<QRectF> box() const;   // part of the page this view shows
    QRectF region() const;               // part of that shown: the zoom, in slide points
    QRectF target() const;               // where it is drawn, letterboxed
    double scale() const;
    QPointF toSlide(QPointF widgetPos) const;
    QRectF toWidget(const QRectF &slideRect) const;
    bool layered() const;                // media on the slide: ink is drawn over it every frame
    QImage slideImage(int page, const QRectF &region, QSize pixels);
    void rebuildBase();
    void drawMedia(QPainter &p);
    void drawLaser(QPainter &p);
    void press(QPointF pos, std::optional<double> pressure, bool erase);
    void move(QPointF pos, std::optional<double> pressure, bool down);
    void release(QPointF pos);
    void click(QPointF pos);
    void selectionMenu(QPointF pos, bool paste);
    void touch(QTouchEvent *e);
    void cancelGesture();
    bool palm(QMouseEvent *e) const;
    double partialRadius() const { return m_tools ? m_tools->eraserSize / 2 / scale() : 0; }
    void showControls(const QString &id);
    void updateControls();

    Deck *m_deck;
    Role m_role;
    ToolSettings *m_tools;
    QPixmap m_base;   // slide (and finished ink when nothing plays); the rest is drawn over it each frame
    bool m_baseDirty = true;
    QHash<QString, QImage> m_cache;   // current and next slide, keyed by document, page, region and size
    Gesture m_gesture = Gesture::None;
    bool m_tabletDown = false;
    bool m_penSeen = false;   // once the pen is used, fingers only rest on the screen
    double m_lastWidth = 0;
    QPointF m_last, m_pressPos;
    QPolygonF m_lasso;
    std::optional<QPointF> m_hover;   // partial eraser ring
    bool m_moved = false;

    // Touchscreen: one finger writes (until a pen is used; then it taps, and drags a zoomed slide); two pinch.
    bool m_fingerDown = false, m_pinching = false, m_pinchMoved = false;
    double m_pinchDistance = 0;
    QPointF m_pinchCentre, m_pinchAnchor;   // widget, and the slide point that stays under the fingers
    QRectF m_pinchRegion;
    qint64 m_pinchStarted = 0;

    // Tap a video for its play/pause button and scrubber (presenter only); they hide after 3 s untouched.
    QWidget *m_controls = nullptr;
    QToolButton *m_playButton = nullptr;
    QSlider *m_scrubber = nullptr;
    QLabel *m_time = nullptr;
    QString m_controlsFor;
    QTimer m_controlsHide;
};
