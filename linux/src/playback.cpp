#include "playback.h"

#include <QVideoFrame>

Video::Video(const QUrl &url, bool autoplay, bool loop, QObject *parent) : QObject(parent) {
    m_player.setAudioOutput(&m_audio);
    m_player.setVideoSink(&m_sink);
    m_player.setLoops(loop ? QMediaPlayer::Infinite : 1);
    connect(&m_sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &f) {
        if (!f.isValid()) return;
        m_frame = f.toImage();
        emit updated();
    });
    connect(&m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &text) {
        // Usually a missing H.264 decoder (stock Fedora, Rocky and RHEL): the README says how to add one.
        m_error = tr("Cannot play video: %1\nInstall your system's H.264 codec (see PenCast's README), or use the AppImage.").arg(text);
        emit updated();
    });
    connect(&m_player, &QMediaPlayer::playbackStateChanged, this, &Video::updated);
    connect(&m_player, &QMediaPlayer::positionChanged, this, &Video::updated);
    if (!m_player.isAvailable()) {   // no Qt Multimedia backend installed: say so over the poster
        m_error = tr("Video playback is not available: install Qt Multimedia (libqt6multimedia6 or qt6-qtmultimedia).");
        return;
    }
    m_player.setSource(url);
    if (autoplay) m_player.play();
    else m_player.pause();   // loads the first frame, the poster until it plays
}

Video::~Video() { m_player.stop(); }

void Video::toggle() {
    if (playing()) { m_player.pause(); return; }
    if (duration() > 0 && position() >= duration() - 50) m_player.setPosition(0);
    m_player.play();
}

Animation::Animation(const QByteArray &framesPdf, const MediaItem &item, QObject *parent)
    : QObject(parent), m_data(framesPdf), m_count(item.frameCount), m_fps(std::max(item.fps, 0.1)), m_loop(item.loop) {
    m_buffer.setData(m_data);
    m_buffer.open(QIODevice::ReadOnly);
    m_doc.load(&m_buffer);
    m_timer.setInterval(int(500 / m_fps));
    connect(&m_timer, &QTimer::timeout, this, &Animation::tick);
    if (item.autoplay) play();
}

void Animation::play() {
    if (!m_loop && m_index == m_count - 1) m_index = 0;
    m_startIndex = m_index;
    m_clock.start();
    m_timer.start();
    emit updated();
}

void Animation::pause() {
    m_timer.stop();
    emit updated();
}

void Animation::tick() {
    const int n = m_startIndex + int(m_clock.elapsed() / 1000.0 * m_fps);
    int i = n;
    if (m_loop) i = n % m_count;
    else if (n >= m_count - 1) { i = m_count - 1; m_timer.stop(); }
    if (i == m_index && m_timer.isActive()) return;
    m_index = i;
    emit updated();
}

QImage Animation::frame(QSize pixels) {
    if (!valid() || pixels.isEmpty()) return {};
    const quint64 key = quint64(pixels.width()) << 32 | quint64(pixels.height());
    auto &slot = m_rendered[key];
    if (slot.first != m_index || slot.second.isNull()) {
        // ponytail: renders on the GUI thread (a few ms per frame); move to a worker if large frames stutter
        slot = {m_index, m_doc.render(m_index, pixels)};
        if (m_rendered.size() > 3) {   // screens come and go: keep the current ones
            const auto keep = slot;
            m_rendered.clear();
            m_rendered.insert(key, keep);
            return keep.second;
        }
    }
    return slot.second;
}
