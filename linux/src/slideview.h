// One slide, letterboxed, with its ink and laser: the presenter's canvas, the projector picture, or the
// Beamer notes beside the slide.
#pragma once

#include "deck.h"

#include <QHash>
#include <QPixmap>
#include <QWidget>

enum class Tool { Pointer, Laser, Pen, Highlighter, Eraser };

struct ToolSettings {
    Tool tool = Tool::Pen;
    QColor penColor = Qt::red, highlighterColor = QColor(255, 220, 0);
    // Widths in PDF points (a 16:9 Beamer slide is ~454 pt wide), as on the iPad.
    double penWidth = 1.5, highlighterWidth = 10;
    double fine = 0.25, firm = 0.55;   // fountain pen pressure, see PressureCurve
    double eraserRadius = 10;          // screen pixels
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
    void tabletEvent(QTabletEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    enum class Gesture { None, Ink, Erase, Laser };
    std::optional<QRectF> box() const;   // part of the page this view shows
    QRectF target() const;               // where it is drawn, letterboxed
    double scale() const;
    QPointF toSlide(QPointF widgetPos) const;
    QImage slideImage(int page, QSize pixels);
    void rebuildBase();
    void drawLaser(QPainter &p);
    void press(QPointF pos, std::optional<double> pressure, bool erase);
    void move(QPointF pos, std::optional<double> pressure, bool down);
    void release();
    bool palm(QMouseEvent *e) const;

    Deck *m_deck;
    Role m_role;
    ToolSettings *m_tools;
    QPixmap m_base;   // slide and finished ink; live ink and laser are drawn over it each frame
    bool m_baseDirty = true;
    QHash<QString, QImage> m_cache;   // current and next slide, keyed by document, page and size
    Gesture m_gesture = Gesture::None;
    bool m_tabletDown = false;
    bool m_penSeen = false;   // once the pen is used, fingers only rest on the screen
    double m_lastWidth = 0;
};
