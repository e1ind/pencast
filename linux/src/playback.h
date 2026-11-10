// Players for a slide's media, shared by the presenter and the projector so they never drift: the
// Linux counterparts of VideoController and AnimationController in Presenter.swift.
#pragma once

#include "media.h"

#include <QAudioOutput>
#include <QBuffer>
#include <QElapsedTimer>
#include <QImage>
#include <QMediaPlayer>
#include <QPdfDocument>
#include <QTimer>
#include <QVideoSink>

class Video : public QObject {
    Q_OBJECT
public:
    Video(const QUrl &url, bool autoplay, bool loop, QObject *parent = nullptr);
    ~Video() override;
    const QImage &frame() const { return m_frame; }   // the latest picture, null before the first
    bool playing() const { return m_player.playbackState() == QMediaPlayer::PlayingState; }
    qint64 position() const { return m_player.position(); }
    qint64 duration() const { return m_player.duration(); }
    QString error() const { return m_error; }
    void toggle();
    void seek(qint64 ms) { m_player.setPosition(ms); }
    void stop() { m_player.stop(); }

signals:
    void updated();   // new frame, position, state or error

private:
    QMediaPlayer m_player;
    QAudioOutput m_audio;
    QVideoSink m_sink;
    QImage m_frame;
    QString m_error;
};

/// animate frame player. The frame index comes from elapsed time, so timer jitter never changes the rate.
/// Frames render on demand from a small PDF holding just the frames, so a 200-frame animation holds one
/// image per screen size, not hundreds.
class Animation : public QObject {
    Q_OBJECT
public:
    Animation(const QByteArray &framesPdf, const MediaItem &item, QObject *parent = nullptr);
    bool valid() const { return m_doc.pageCount() == m_count && m_count > 0; }
    bool playing() const { return m_timer.isActive(); }
    void toggle() { playing() ? pause() : play(); }
    void play();
    void pause();
    /// The current frame at `pixels`, rendered when it changed.
    QImage frame(QSize pixels);

signals:
    void updated();

private:
    void tick();

    QByteArray m_data;
    QBuffer m_buffer;
    QPdfDocument m_doc;
    int m_count;
    double m_fps;
    bool m_loop;
    int m_index = 0, m_startIndex = 0;
    QElapsedTimer m_clock;
    QTimer m_timer;
    QHash<quint64, QPair<int, QImage>> m_rendered;   // by pixel size: (frame, image), one per screen
};
