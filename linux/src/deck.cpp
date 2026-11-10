#include "deck.h"
#include "playback.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>

static qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }

static QPair<qint64, qint64> stat(const QString &path) {
    const QFileInfo f(path);   // a fresh QFileInfo each time: it caches
    return {f.lastModified().toMSecsSinceEpoch(), f.size()};
}

static QString inkFile(const QString &deck) {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Ink/" + deck + ".ink.json";
}

Deck::Deck(QObject *parent) : QObject(parent) {
    m_laserTimer.setInterval(16);
    connect(&m_laserTimer, &QTimer::timeout, this, &Deck::laserTick);
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(1000);
    connect(&m_saveTimer, &QTimer::timeout, this, &Deck::saveInk);
    // ponytail: polls once a second; a file watcher loses the file when LaTeX replaces it.
    m_pollTimer.setInterval(1000);
    connect(&m_pollTimer, &QTimer::timeout, this, &Deck::poll);
    m_followZoom = QSettings().value("projectorFollowsZoom", false).toBool();
}

Deck::~Deck() {
    close();
    for (const auto &t : std::as_const(m_extracting)) if (t) t->wait();
}

QString Deck::open(const QString &path) {
    close();
    auto *doc = new QPdfDocument(this);
    if (doc->load(path) != QPdfDocument::Error::None) {
        delete doc;
        return tr("Could not open %1 as a PDF.").arg(QFileInfo(path).fileName());
    }
    m_doc = doc;
    ++m_generation;
    m_path = QFileInfo(path).absoluteFilePath();
    m_page = 0;
    structure();
    scanMedia();
    loadInk();
    m_seen = stat(m_path);
    m_pollTimer.start();
    emit reloaded();
    emit changed();
    return {};
}

void Deck::close() {
    if (!m_doc) return;
    m_pollTimer.stop();
    saveInk();
    stopMedia();
    m_raw.reset();
    m_media.clear();
    m_zoom.reset();
    m_selection.clear();
    QPdfDocument *old = m_doc;
    m_doc = nullptr;
    ++m_generation;
    m_path.clear();
    m_ink.clear(); m_undo.clear(); m_redo.clear(); m_keys.clear(); m_frameStart.clear();
    m_live.reset(); m_laser.clear(); m_laserDot.reset();
    m_page = 0; m_hasNotes = false; m_presenting = false; m_projector = Projector::Slides; m_frozen = {};
    emit reloaded();
    emit changed();
    old->deleteLater();
}

QString Deck::name() const { return QFileInfo(m_path).completeBaseName(); }
int Deck::pageCount() const { return m_doc ? m_doc->pageCount() : 0; }

void Deck::go(int page) {
    if (page < 0 || page >= pageCount() || page == m_page) return;
    QVector<MediaItem> next;
    for (const MediaItem &m : m_media) if (m.page == page) next << m;
    const auto kept = stopMedia(next);
    m_page = page;
    m_live.reset();
    m_laser.clear();
    m_zoom.reset();
    m_selection.clear();
    if (m_presenting) {   // reading view: nothing plays while scrolling
        startMedia(kept);
        prepareVideos(page + 1);
    }
    emit changed();
}

QRectF Deck::slideBox(int page) const { return m_doc ? Slide::split(m_doc->pagePointSize(page)).slide : QRectF(); }
std::optional<QRectF> Deck::notesBox(int page) const {
    return m_doc ? Slide::split(m_doc->pagePointSize(page)).notes : std::nullopt;
}

void Deck::setPresenting(bool on) {
    if (on == m_presenting) return;
    m_presenting = on;
    m_projector = Projector::Slides;
    m_frozen = {};
    m_live.reset();
    m_laser.clear();
    m_zoom.reset();
    m_selection.clear();
    stopMedia();
    if (on) {
        startMedia();
        prepareVideos(m_page + 1);
    }
    emit changed();
}

void Deck::setProjector(Projector p, const QImage &frozen) {
    m_projector = p;
    m_frozen = frozen;
    emit changed();
}

// MARK: Ink

QString Deck::inkKey() const {
    const int start = m_frameStart.value(m_page, m_page);
    return m_keys.value(start, QString::number(start));
}

const QVector<Stroke> &Deck::ink() const {
    static const QVector<Stroke> none;
    const auto it = m_ink.constFind(inkKey());
    return it == m_ink.constEnd() ? none : *it;
}

QVector<Stroke> &Deck::inkRef() { return m_ink[inkKey()]; }

void Deck::pushUndo() {
    m_undo[inkKey()].append(ink());
    m_redo[inkKey()].clear();
}

void Deck::beginStroke(const Stroke &s) { m_live = s; emit drawn(); }

void Deck::extendStroke(const InkPoint &p) {
    if (!m_live) return;
    m_live->points.append(p);
    emit drawn();
}

void Deck::endStroke() {
    if (!m_live) return;
    pushUndo();
    inkRef().append(*m_live);
    m_live.reset();
    inkEdited();
}

void Deck::beginErase() { m_eraseSnapshot = false; }

void Deck::eraseAt(QPointF p, double radius) {
    const QVector<Stroke> &strokes = ink();
    const bool any = std::any_of(strokes.begin(), strokes.end(), [&](const Stroke &s) { return Ink::hits(s, p, radius); });
    if (!any) return;
    if (!m_eraseSnapshot) { pushUndo(); m_eraseSnapshot = true; }   // one undo step per eraser drag
    inkRef().removeIf([&](const Stroke &s) { return Ink::hits(s, p, radius); });
    inkEdited();
}

void Deck::undo() {
    auto &stack = m_undo[inkKey()];
    if (stack.isEmpty()) return;
    m_selection.clear();
    m_redo[inkKey()].append(ink());
    inkRef() = stack.takeLast();
    inkEdited();
}

void Deck::redo() {
    auto &stack = m_redo[inkKey()];
    if (stack.isEmpty()) return;
    m_selection.clear();
    m_undo[inkKey()].append(ink());
    inkRef() = stack.takeLast();
    inkEdited();
}

void Deck::clearSlide() {
    m_selection.clear();
    if (ink().isEmpty()) return;
    pushUndo();
    inkRef().clear();
    inkEdited();
}

bool Deck::canUndo() const { return !m_undo.value(inkKey()).isEmpty(); }
bool Deck::canRedo() const { return !m_redo.value(inkKey()).isEmpty(); }

void Deck::inkEdited() {
    m_saveTimer.start();
    emit changed();
}

void Deck::loadInk() {
    QFile f(inkFile(name()));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject all = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = all.begin(); it != all.end(); ++it) m_ink[it.key()] = Ink::fromJson(it.value().toArray());
}

void Deck::saveInk() {
    if (!m_doc) return;
    m_saveTimer.stop();
    QJsonObject all;
    for (auto it = m_ink.cbegin(); it != m_ink.cend(); ++it)
        if (!it->isEmpty()) all[it.key()] = Ink::toJson(*it);
    QDir().mkpath(QFileInfo(inkFile(name())).path());
    QSaveFile f(inkFile(name()));   // atomic: a crash mid-write never loses the old notes
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(all).toJson(QJsonDocument::Compact)) < 0 || !f.commit())
        qWarning("ink save failed: %s", qPrintable(f.errorString()));
}

// MARK: Structure and reload

void Deck::structure() {
    QStringList labels, texts;
    m_hasNotes = notesBox(0).has_value();
    for (int i = 0; i < m_doc->pageCount(); ++i) {
        labels << m_doc->pageLabel(i);
        const QRectF box = slideBox(i);
        texts << (m_hasNotes ? m_doc->getSelection(i, box.topLeft(), box.bottomRight()).text()
                             : m_doc->getAllText(i).text());
    }
    m_frameStart = Slide::frameStarts(labels);
    m_keys = Slide::inkKeys(texts);
}

void Deck::poll() {
    const auto seen = stat(m_path);
    if (seen != m_seen) { m_seen = seen; m_fileChanged = true; return; }   // wait until LaTeX stops writing
    if (m_fileChanged) { m_fileChanged = false; reload(); }
}

/// Reopens the deck after LaTeX rewrites it; a half-written or broken file keeps the old one showing.
void Deck::reload() {
    auto *doc = new QPdfDocument(this);
    if (doc->load(m_path) != QPdfDocument::Error::None || doc->pageCount() == 0) { delete doc; return; }
    QPdfDocument *old = m_doc;
    stopMedia();
    m_doc = doc;
    ++m_generation;
    m_page = std::min(m_page, doc->pageCount() - 1);
    m_selection.clear();
    m_zoom.reset();
    structure();
    scanMedia();
    if (m_presenting) startMedia();
    m_undo.clear(); m_redo.clear();   // undo steps may belong to slides that no longer exist
    m_live.reset();
    emit reloaded();
    emit changed();
    old->deleteLater();
}

// MARK: Laser

void Deck::laserPress(QPointF p) {
    if (now() - m_laserLiftedAt > laserHoldMs + laserFadeMs) m_laser.clear();   // lines fade out together
    if (laserStyle.trail) m_laser.append({{p, now()}});
    m_laserPressing = true;
    m_laserDot = p;
    m_laserTimer.start();
    emit drawn();
}

void Deck::laserMove(QPointF p) {
    m_laserDot = p;
    if (m_laserPressing && !m_laser.isEmpty()) m_laser.last().append({p, now()});
    emit drawn();
}

void Deck::laserRelease() {
    m_laserPressing = false;
    m_laserLiftedAt = now();
    emit drawn();
}

void Deck::laserLeave() {
    m_laserDot.reset();
    emit drawn();
}

void Deck::laserTick() {
    if (!m_laserPressing && now() - m_laserLiftedAt > laserHoldMs + laserFadeMs) {
        m_laser.clear();
        m_laserTimer.stop();
    }
    emit drawn();
}

// MARK: Partial eraser and lasso

void Deck::eraseAlong(QPointF a, QPointF b, double radius) {
    const auto cut = Ink::cut(ink(), a, b, radius);
    if (!cut) return;
    if (!m_eraseSnapshot) { pushUndo(); m_eraseSnapshot = true; }
    m_selection.clear();
    inkRef() = *cut;
    inkEdited();
}

void Deck::select(const QPolygonF &lasso) {
    m_selection = Ink::inside(ink(), lasso);
    emit changed();
}

void Deck::clearSelection() {
    if (m_selection.isEmpty()) return;
    m_selection.clear();
    emit changed();
}

void Deck::beginMove() { if (!m_selection.isEmpty()) pushUndo(); }

void Deck::moveSelection(QPointF delta) {
    QVector<Stroke> &strokes = inkRef();
    for (int i : std::as_const(m_selection))
        if (i < strokes.size()) for (InkPoint &p : strokes[i].points) p.p += delta;
    emit changed();
}

void Deck::endMove() { if (!m_selection.isEmpty()) inkEdited(); }

void Deck::deleteSelection() {
    if (m_selection.isEmpty()) return;
    pushUndo();
    QVector<Stroke> &strokes = inkRef();
    QVector<int> sorted = m_selection;
    std::sort(sorted.rbegin(), sorted.rend());
    for (int i : sorted) if (i < strokes.size()) strokes.removeAt(i);
    m_selection.clear();
    inkEdited();
}

void Deck::copySelection() {
    m_clipboard.clear();
    for (int i : std::as_const(m_selection)) if (i < ink().size()) m_clipboard << ink()[i];
}

void Deck::duplicateSelection() {
    copySelection();
    if (m_clipboard.isEmpty()) return;
    paste(selectionBounds().center() + QPointF(12, 12));
}

void Deck::paste(QPointF at) {
    if (m_clipboard.isEmpty()) return;
    pushUndo();
    const QPointF delta = at - Ink::bounds(m_clipboard, [&] { QVector<int> all; for (int i = 0; i < m_clipboard.size(); ++i) all << i; return all; }()).center();
    QVector<Stroke> &strokes = inkRef();
    m_selection.clear();
    for (Stroke s : std::as_const(m_clipboard)) {
        for (InkPoint &p : s.points) p.p += delta;
        m_selection << strokes.size();
        strokes << s;
    }
    inkEdited();
}

QVector<Stroke> Deck::inkOf(int page) const {
    const int start = m_frameStart.value(page, page);
    return m_ink.value(m_keys.value(start, QString::number(start)));
}

QVector<int> Deck::lastSteps() const {
    QVector<int> out;
    for (int i = 0; i < pageCount(); ++i)
        if (i + 1 == pageCount() || m_frameStart.value(i + 1) != m_frameStart.value(i)) out << i;
    return out;
}

// MARK: Zoom

void Deck::setZoom(std::optional<QRectF> r) {
    const QRectF full(QPointF(0, 0), slideBox(m_page).size());
    if (r && !full.isEmpty()) {
        const double w = std::clamp(r->width(), full.width() / maxZoom, full.width());
        if (w >= full.width() * 0.98) {
            r.reset();
        } else {
            const double h = w * full.height() / full.width();
            const QPointF c = r->center();
            r = QRectF(std::clamp(c.x() - w / 2, 0.0, full.width() - w), std::clamp(c.y() - h / 2, 0.0, full.height() - h), w, h);
        }
    } else {
        r.reset();
    }
    if (r == m_zoom) return;
    m_zoom = r;
    emit changed();
}

void Deck::zoomBy(double factor, QPointF anchor) {
    if (factor <= 0) return;
    const QRectF full(QPointF(0, 0), slideBox(m_page).size());
    const QRectF v = m_zoom.value_or(full);
    const double w = std::clamp(v.width() / factor, full.width() / maxZoom, full.width()), k = w / v.width();
    setZoom(QRectF(anchor - (anchor - v.topLeft()) * k, v.size() * k));
}

void Deck::pan(QPointF delta) { if (m_zoom) setZoom(m_zoom->translated(delta)); }
void Deck::resetZoom() { setZoom(std::nullopt); }

void Deck::setProjectorFollowsZoom(bool on) {
    m_followZoom = on;
    QSettings().setValue("projectorFollowsZoom", on);
    emit changed();
}

// MARK: Links

std::optional<Media::Link> Deck::linkAt(QPointF p) const {
    if (!m_raw) return std::nullopt;
    for (const Media::Link &l : Media::links(*m_raw, m_page)) {
        const QRectF r = Media::onPage(l.rect, m_raw->pageBox(m_page)).translated(-slideBox(m_page).topLeft());
        if (r.contains(p)) return l;
    }
    return std::nullopt;
}

void Deck::follow(const Media::Link &link) {
    switch (link.action) {
    case Media::Link::GoTo: go(link.page); break;
    case Media::Link::Next: go(m_page + 1); break;
    case Media::Link::Previous: go(m_page - 1); break;
    case Media::Link::First: go(0); break;
    case Media::Link::Last: go(pageCount() - 1); break;
    }
}

// MARK: Media

void Deck::scanMedia() {
    m_media.clear();
    m_raw = std::make_unique<Pdf::Document>();
    if (!m_raw->load(m_path)) return;
    for (int p = 0; p < m_raw->pageCount(); ++p) m_media += Media::scan(*m_raw, p);
}

QVector<MediaItem> Deck::pageMedia() const {
    QVector<MediaItem> out;
    for (const MediaItem &m : m_media) if (m.page == m_page) out << m;
    return out;
}

QRectF Deck::mediaRect(const MediaItem &item) const {
    if (!m_raw) return {};
    return Media::onPage(item.rect, m_raw->pageBox(item.page)).translated(-slideBox(item.page).topLeft());
}

QString Deck::problem(const QString &id) const {
    if (const Video *v = m_videos.value(id); v && !v->error().isEmpty()) return v->error();
    return m_problems.value(id);
}

/// Stops this page's media, except videos that continue on the next page: Beamer overlays (\pause,
/// \only) repeat the same video at the same place on consecutive pages, and it should keep playing
/// rather than restart at every step. Returns those, keyed by continuity.
QHash<QString, Video *> Deck::stopMedia(const QVector<MediaItem> &next) {
    QSet<QString> continuing;
    for (const MediaItem &m : next) continuing << m.continuity();
    QHash<QString, Video *> kept;
    for (const MediaItem &item : pageMedia()) {
        Video *v = m_videos.value(item.id());
        if (!v) continue;
        if (continuing.contains(item.continuity())) kept[item.continuity()] = v;
        else v->deleteLater();
    }
    qDeleteAll(m_animations);
    m_videos.clear();
    m_animations.clear();
    m_problems.clear();
    return kept;
}

void Deck::startMedia(const QHash<QString, Video *> &kept) {
    if (!m_raw) return;
    for (const MediaItem &item : pageMedia()) {
        if (Video *v = kept.value(item.continuity())) { m_videos[item.id()] = v; continue; }
        switch (item.kind) {
        case MediaItem::EmbeddedVideo: {
            // Extracted once per deck version: every later slide using the same file reuses it.
            const QString file = videoFile(item);
            if (QFileInfo::exists(file)) loadVideo(item, QUrl::fromLocalFile(file));
            else extract(item, file);
            break;
        }
        case MediaItem::LinkedVideo: {
            const QUrl url = item.source.contains("://") ? QUrl(item.source)
                             : QUrl::fromLocalFile(QFileInfo(m_path).dir().absoluteFilePath(item.source));
            if (url.isLocalFile() && !QFileInfo(url.toLocalFile()).isReadable())
                m_problems[item.id()] = tr("Linked video not found:\n%1").arg(item.source);
            else loadVideo(item, url);
            break;
        }
        case MediaItem::Animation: {
            auto *a = new Animation(Media::animationFrames(*m_raw, item), item, this);
            if (!a->valid()) { delete a; m_problems[item.id()] = tr("animate frames not found"); break; }
            connect(a, &Animation::updated, this, &Deck::drawn);
            m_animations[item.id()] = a;
            break;
        }
        case MediaItem::Unsupported: m_problems[item.id()] = item.source; break;
        }
    }
}

void Deck::loadVideo(const MediaItem &item, const QUrl &url) {
    auto *v = new Video(url, item.autoplay, item.loop, this);
    connect(v, &Video::updated, this, &Deck::drawn);
    m_videos[item.id()] = v;
}

/// Per deck version: a recompiled PDF never plays stale cached videos.
QString Deck::videoFile(const MediaItem &item) const {
    const QString deck = QString::fromLatin1(QCryptographicHash::hash(m_path.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/Media/" + deck + '-'
           + QString::number(m_seen.first / 1000) + '/' + item.source;
}

/// Unpacks a video off the GUI thread from its own copy of the PDF; the slide shows its poster meanwhile.
void Deck::extract(const MediaItem &item, const QString &file) {
    if (!m_extracting.value(file)) {
        QThread *t = QThread::create([path = m_path, item, file] {
            Pdf::Document doc;
            if (!doc.load(path)) return;
            const auto data = Media::embeddedVideo(doc, item);
            if (!data) return;
            QDir().mkpath(QFileInfo(file).path());
            QSaveFile f(file);
            if (f.open(QIODevice::WriteOnly) && f.write(*data) == data->size()) f.commit();
        });
        m_extracting[file] = t;
        connect(t, &QThread::finished, this, [this, t, file] {
            m_extracting.remove(file);
            t->deleteLater();
            for (const MediaItem &m : pageMedia()) {
                if (m.kind != MediaItem::EmbeddedVideo || videoFile(m) != file || m_videos.contains(m.id()) || !m_presenting) continue;
                if (QFileInfo::exists(file)) loadVideo(m, QUrl::fromLocalFile(file));
                else m_problems[m.id()] = tr("Could not unpack the video %1 from the PDF.").arg(m.source);
            }
            emit drawn();
        });
        t->start();
    }
}

/// Unpack the next slide's embedded videos ahead of time, so they start the moment you get there.
void Deck::prepareVideos(int page) {
    for (const MediaItem &m : std::as_const(m_media)) {
        if (m.page != page || m.kind != MediaItem::EmbeddedVideo) continue;
        const QString file = videoFile(m);
        if (!QFileInfo::exists(file)) extract(m, file);
    }
}
