// One tool's settings, dropped down from its toolbar button: the Linux counterpart of ToolOptions in
// SlideView.swift (pen style, size, pressure with a scratch pad, colours; laser trail or dot; eraser mode).
#pragma once

#include "slideview.h"

#include <QFrame>
#include <functional>

/// Ink colours offered on the toolbar (the first four) and in the options (all).
extern const QVector<QColor> penSwatches, highlighterSwatches;

class ToolOptions : public QFrame {
    Q_OBJECT
public:
    /// `changed` runs after every edit (save settings, refresh the toolbar).
    ToolOptions(Tool tool, ToolSettings *tools, Deck *deck, std::function<void()> changed, QWidget *parent = nullptr);

private:
    void rebuild();

    Tool m_tool;
    ToolSettings *m_tools;
    Deck *m_deck;
    std::function<void()> m_changed;
};

/// Sample stroke at roughly the on-slide size, so a size or style change shows before you write.
class StrokePreview : public QWidget {
public:
    enum Style { Even, Fountain, Highlighter };
    StrokePreview(QColor color, double width, Style style, QWidget *parent = nullptr);
    QSize sizeHint() const override { return {280, 54}; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QColor m_color;
    double m_width;
    Style m_style;
};

/// Scratch pad for tuning the fountain pen's pressure: write on it with the pen tablet.
class PressurePad : public QWidget {
public:
    PressurePad(ToolSettings *tools, QWidget *parent = nullptr);
    void clear() { m_strokes.clear(); update(); }
    QSize sizeHint() const override { return {280, 130}; }

protected:
    void paintEvent(QPaintEvent *) override;
    void tabletEvent(QTabletEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;

private:
    void add(QPointF p, std::optional<double> pressure, bool start);
    ToolSettings *m_tools;
    QVector<Stroke> m_strokes;
};
