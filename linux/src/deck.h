// The open presentation: document, current slide, ink, laser and projector state. Shared by every view;
// the Linux counterpart of Presenter.swift.
#pragma once

#include "core.h"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QTimer>

class QPdfDocument;

struct LaserPoint { QPointF p; qint64 t; };

class Deck : public QObject {
    Q_OBJECT
public:
    enum class Projector { Slides, Black, White, Frozen };
    // After the last lift, laser lines stay for `laserHoldMs`, then fade out over `laserFadeMs`.
    static constexpr qint64 laserHoldMs = 500, laserFadeMs = 700;

    explicit Deck(QObject *parent = nullptr);
    ~Deck() override;

    QString open(const QString &path);   // error message, empty on success
    void close();
    bool isOpen() const { return m_doc; }
    QPdfDocument *doc() const { return m_doc; }
    int generation() const { return m_generation; }   // bumps whenever the document object changes
    QString path() const { return m_path; }
    QString name() const;
    int page() const { return m_page; }
    int pageCount() const;
    void go(int page);

    QRectF slideBox(int page) const;
    std::optional<QRectF> notesBox(int page) const;
    bool hasNotes() const { return m_hasNotes; }

    bool presenting() const { return m_presenting; }
    void setPresenting(bool on);
    Projector projector() const { return m_projector; }
    const QImage &frozen() const { return m_frozen; }
    void setProjector(Projector p, const QImage &frozen = {});

    // Ink of the current Beamer frame, in points of the slide (origin: the slide's top-left).
    const QVector<Stroke> &ink() const;
    const std::optional<Stroke> &liveStroke() const { return m_live; }
    void beginStroke(const Stroke &s);
    void extendStroke(const InkPoint &p);
    void endStroke();
    void beginErase();
    void eraseAt(QPointF p, double radius);
    void undo();
    void redo();
    void clearSlide();
    bool canUndo() const;
    bool canRedo() const;

    const QVector<QVector<LaserPoint>> &laser() const { return m_laser; }
    std::optional<QPointF> laserDot() const { return m_laserDot; }
    bool laserPressing() const { return m_laserPressing; }
    qint64 laserLiftedAt() const { return m_laserLiftedAt; }
    void laserPress(QPointF p);
    void laserMove(QPointF p);   // pressed: extends the line; hovering: moves the dot
    void laserRelease();
    void laserLeave();

signals:
    void reloaded();   // a new document object: opened, recompiled or closed
    void changed();    // slide, presenting, ink, projector or undo state
    void drawn();      // live stroke or laser moved: repaint only

private:
    QString inkKey() const;
    QVector<Stroke> &inkRef();
    void pushUndo();
    void structure();
    void loadInk();
    void saveInk();
    void inkEdited();
    void poll();
    void reload();
    void laserTick();

    QPdfDocument *m_doc = nullptr;
    int m_generation = 0;
    QString m_path;
    int m_page = 0;
    bool m_hasNotes = false;
    bool m_presenting = false;
    Projector m_projector = Projector::Slides;
    QImage m_frozen;
    QVector<int> m_frameStart;
    QStringList m_keys;
    QHash<QString, QVector<Stroke>> m_ink;   // by slide key, so a recompile with new slides keeps notes
    QHash<QString, QVector<QVector<Stroke>>> m_undo, m_redo;
    std::optional<Stroke> m_live;
    bool m_eraseSnapshot = false;
    QVector<QVector<LaserPoint>> m_laser;
    std::optional<QPointF> m_laserDot;
    bool m_laserPressing = false;
    qint64 m_laserLiftedAt = 0;
    QTimer m_laserTimer, m_saveTimer, m_pollTimer;
    QPair<qint64, qint64> m_seen;   // modification time and size of the PDF
    bool m_fileChanged = false;
};
