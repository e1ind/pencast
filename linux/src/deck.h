// The open presentation: document, current slide, ink, laser and projector state. Shared by every view;
// the Linux counterpart of Presenter.swift.
#pragma once

#include "core.h"
#include "media.h"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QTimer>

class QPdfDocument;
class QThread;
class Video;
class Animation;

struct LaserPoint { QPointF p; qint64 t; };

/// Laser look, shared by presenter and projector. Sizes in slide points (a Beamer slide is ~454 pt wide).
struct LaserStyle {
    bool trail = true;   // a glowing line that fades after you lift; false: just the dot
    double dot = 7, width = 4;
};

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
    void cancelStroke() { m_live.reset(); emit drawn(); }   // a second finger landed: it was a pinch
    void extendStroke(const InkPoint &p);
    void endStroke();
    void beginErase();
    void eraseAt(QPointF p, double radius);
    void eraseAlong(QPointF a, QPointF b, double radius);   // partial eraser, from a to b
    void undo();
    void redo();
    void clearSlide();
    bool canUndo() const;
    bool canRedo() const;
    /// Ink of any page's frame (export).
    QVector<Stroke> inkOf(int page) const;
    /// The last overlay step of each Beamer frame: everything uncovered (notes export).
    QVector<int> lastSteps() const;
    const Pdf::Document *raw() const { return m_raw.get(); }

    // Lasso selection (indices into ink()). Moving, cutting and deleting are undoable.
    const QVector<int> &selection() const { return m_selection; }
    QRectF selectionBounds() const { return Ink::bounds(ink(), m_selection); }
    void select(const QPolygonF &lasso);
    void clearSelection();
    void beginMove();
    void moveSelection(QPointF delta);
    void endMove();
    void deleteSelection();
    void copySelection();
    void cutSelection() { copySelection(); deleteSelection(); }
    void duplicateSelection();
    bool canPaste() const { return !m_clipboard.isEmpty(); }
    void paste(QPointF at);

    // Zoom: the part of the slide shown (slide points, origin top-left); nullopt = the whole slide.
    static constexpr double maxZoom = 8;
    std::optional<QRectF> zoom() const { return m_zoom; }
    void zoomBy(double factor, QPointF anchor);   // `anchor` (slide points) stays where it is
    void pan(QPointF delta);                       // slide points
    void zoomTo(const QRectF &r) { setZoom(r); }   // kept inside the slide and the zoom limits
    void resetZoom();
    bool projectorFollowsZoom() const { return m_followZoom; }
    void setProjectorFollowsZoom(bool on);

    // Media of the current page, playing while presenting.
    QVector<MediaItem> pageMedia() const;
    QRectF mediaRect(const MediaItem &item) const;   // slide points, origin the slide's top-left
    Video *video(const QString &id) const { return m_videos.value(id); }
    Animation *animation(const QString &id) const { return m_animations.value(id); }
    QString problem(const QString &id) const;
    /// The link under `p` (slide points) on the current page, if any.
    std::optional<Media::Link> linkAt(QPointF p) const;
    void follow(const Media::Link &link);
    const QVector<QVector<LaserPoint>> &laser() const { return m_laser; }
    std::optional<QPointF> laserDot() const { return m_laserDot; }
    bool laserPressing() const { return m_laserPressing; }
    qint64 laserLiftedAt() const { return m_laserLiftedAt; }
    LaserStyle laserStyle;
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
    void setZoom(std::optional<QRectF> r);
    QHash<QString, Video *> stopMedia(const QVector<MediaItem> &next = {});
    void startMedia(const QHash<QString, Video *> &kept = {});
    void prepareVideos(int page);
    QString videoFile(const MediaItem &item) const;
    void extract(const MediaItem &item, const QString &file);
    void loadVideo(const MediaItem &item, const QUrl &url);
    void scanMedia();

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
    std::unique_ptr<Pdf::Document> m_raw;
    QVector<MediaItem> m_media;
    QHash<QString, Video *> m_videos;
    QHash<QString, Animation *> m_animations;
    QHash<QString, QString> m_problems;
    QHash<QString, QPointer<QThread>> m_extracting;   // by cache file
    std::optional<QRectF> m_zoom;
    bool m_followZoom = false;
    QVector<int> m_selection;
    QVector<Stroke> m_clipboard;
};
