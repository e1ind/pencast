#include "deck.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QSaveFile>
#include <QStandardPaths>

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
}

Deck::~Deck() { close(); }

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
    m_page = page;
    m_live.reset();
    m_laser.clear();
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
    m_redo[inkKey()].append(ink());
    inkRef() = stack.takeLast();
    inkEdited();
}

void Deck::redo() {
    auto &stack = m_redo[inkKey()];
    if (stack.isEmpty()) return;
    m_undo[inkKey()].append(ink());
    inkRef() = stack.takeLast();
    inkEdited();
}

void Deck::clearSlide() {
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
    m_doc = doc;
    ++m_generation;
    m_page = std::min(m_page, doc->pageCount() - 1);
    structure();
    m_undo.clear(); m_redo.clear();   // undo steps may belong to slides that no longer exist
    m_live.reset();
    emit reloaded();
    emit changed();
    old->deleteLater();
}

// MARK: Laser

void Deck::laserPress(QPointF p) {
    if (now() - m_laserLiftedAt > laserHoldMs + laserFadeMs) m_laser.clear();   // lines fade out together
    m_laser.append({{p, now()}});
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
