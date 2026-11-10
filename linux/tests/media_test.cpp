// The media scanner must find what the Swift one finds (`swift run pencast-scan Corpus/media_test.pdf`),
// extract videos byte for byte, render animation frames, and export ink into the PDF.
#include "media.h"

#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPdfDocument>
#include <QBuffer>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static QByteArray readFile(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

static bool reddish(QRgb c) { return qRed(c) > 180 && qGreen(c) < 90 && qBlue(c) < 90; }

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    const QString corpus = CORPUS;

    CHECK(Media::parseFlashVars("source=a%20b.mp4&autoPlay=true&x") ==
          (QHash<QString, QString>{{"source", "a b.mp4"}, {"autoPlay", "true"}}));
    CHECK(Media::animateFrame("3.17") == std::make_pair(3, 17));
    CHECK(!Media::animateFrame("anim.1") && !Media::animateFrame("1.2.3"));

    Pdf::Document doc;
    CHECK(doc.load(corpus + "/media_test.pdf"));
    QPdfDocument reference;
    reference.load(corpus + "/media_test.pdf");
    CHECK(doc.pageCount() == reference.pageCount() && doc.pageCount() > 7);
    QVector<MediaItem> all;
    for (int p = 0; p < doc.pageCount(); ++p) all += Media::scan(doc, p);
    for (const MediaItem &m : all)
        std::printf("page %2d  annot %2d  [%.1f %.1f %.1fx%.1f]  kind %d %s  frames %d fps %.0f  autoplay=%d loop=%d\n",
                    m.page + 1, m.annotIndex, m.rect.x(), m.rect.y(), m.rect.width(), m.rect.height(), m.kind,
                    qPrintable(m.source), m.frameCount, m.fps, m.autoplay, m.loop);

    // Swift: pages are 1-based in its listing, 0-based here.
    struct Expected { int page, annot; MediaItem::Kind kind; const char *source; int frames; double fps; bool autoplay, loop; };
    const Expected expected[] = {
        {1, 1, MediaItem::EmbeddedVideo, "clip720p30.mp4", 0, 0, true, true},
        {2, 1, MediaItem::EmbeddedVideo, "clip1080p60.mp4", 0, 0, false, false},
        {3, 1, MediaItem::EmbeddedVideo, "clip720p30.mp4", 0, 0, true, true},
        {3, 3, MediaItem::EmbeddedVideo, "clip1080p60.mp4", 0, 0, false, false},
        {4, 1, MediaItem::LinkedVideo, "clip720p30.mp4", 0, 0, true, true},
        {5, 1, MediaItem::Animation, "", 24, 12, true, true},
        {6, 1, MediaItem::Animation, "", 16, 8, false, false},
    };
    CHECK(all.size() == int(std::size(expected)));
    for (int i = 0; i < std::min<int>(all.size(), std::size(expected)); ++i) {
        const MediaItem &m = all[i];
        const Expected &e = expected[i];
        CHECK(m.page == e.page && m.annotIndex == e.annot && m.kind == e.kind && m.source == e.source);
        CHECK(m.frameCount == e.frames && qFuzzyCompare(1 + m.fps, 1 + e.fps) && m.autoplay == e.autoplay && m.loop == e.loop);
    }
    CHECK(std::abs(all.value(0).rect.x() - 64.9) < 0.1 && std::abs(all.value(0).rect.width() - 323.8) < 0.1);
    CHECK(all.value(0).continuity() != all.value(2).continuity());   // other spot: another video

    // Embedded videos come out byte for byte.
    const auto clip = Media::embeddedVideo(doc, all.value(0));
    CHECK(clip && *clip == readFile(corpus + "/clip720p30.mp4"));
    const auto clip2 = Media::embeddedVideo(doc, all.value(3));
    CHECK(clip2 && *clip2 == readFile(corpus + "/clip1080p60.mp4"));

    // Animation frames: one page each, the animation's size, and not blank.
    QByteArray frames = Media::animationFrames(doc, all.value(5));
    QBuffer buffer(&frames);
    buffer.open(QIODevice::ReadOnly);
    QPdfDocument anim;
    anim.load(&buffer);
    CHECK(anim.pageCount() == 24);
    CHECK(std::abs(anim.pagePointSize(0).width() - all.value(5).rect.width()) < 0.5);
    const QImage f0 = anim.render(0, QSize(200, 200)), f5 = anim.render(5, QSize(200, 200));
    CHECK(!f0.isNull() && f0 != f5);
    bool notWhite = false;
    for (int y = 0; y < f0.height(); y += 7) for (int x = 0; x < f0.width(); x += 7) notWhite |= (f0.pixel(x, y) & 0xffffff) != 0xffffff;
    CHECK(notWhite);
    if (qEnvironmentVariableIsSet("PENCAST_TEST_OUT")) {
        f0.save(qEnvironmentVariable("PENCAST_TEST_OUT") + "/frame0.png");
        f5.save(qEnvironmentVariable("PENCAST_TEST_OUT") + "/frame5.png");
    }

    // Export: the slide part of page 2 with a red line across it, as vector ink.
    const QRectF slide(QPointF(0, 0), doc.pageBox(1).size());
    Stroke red{Stroke::Pen, QColor(230, 30, 30), {{QPointF(20, 20), 3}, {QPointF(200, 20), 3}}};
    Stroke hl{Stroke::Highlighter, QColor(255, 235, 59), {{QPointF(20, 60), 10}, {QPointF(200, 60), 10}}};
    QByteArray out = Media::exportPdf(doc, {{1, slide, {red, hl}}, {5, slide, {}}});
    QBuffer outBuffer(&out);
    outBuffer.open(QIODevice::ReadOnly);
    QPdfDocument exported;
    exported.load(&outBuffer);
    CHECK(exported.pageCount() == 2);
    const QSizeF size = exported.pagePointSize(0);
    CHECK(std::abs(size.width() - slide.width()) < 0.5 && std::abs(size.height() - slide.height()) < 0.5);
    const QImage page = exported.render(0, size.toSize() * 2);
    CHECK(reddish(page.pixel(220, 40)));          // on the line
    CHECK(!reddish(page.pixel(220, 70)));
    // The title bar (blue) under the line and the poster survive: vector content copied, not dropped.
    CHECK(qBlue(page.pixel(600, 10)) > qRed(page.pixel(600, 10)));
    if (qEnvironmentVariableIsSet("PENCAST_TEST_OUT")) {
        page.save(qEnvironmentVariable("PENCAST_TEST_OUT") + "/export0.png");
        exported.render(1, size.toSize() * 2).save(qEnvironmentVariable("PENCAST_TEST_OUT") + "/export1.png");
        QFile f(qEnvironmentVariable("PENCAST_TEST_OUT") + "/export.pdf");
        if (f.open(QIODevice::WriteOnly)) f.write(out);
    }

    // Links: Beamer's navigation symbols and \hyperlink targets resolve to pages.
    {
        int internal = 0, toOther = 0;
        for (int p = 0; p < doc.pageCount(); ++p)
            for (const Media::Link &l : Media::links(doc, p)) {
                ++internal;
                CHECK(l.action != Media::Link::GoTo || (l.page >= 0 && l.page < doc.pageCount()));
                if (l.action == Media::Link::GoTo && l.page != p) ++toOther;
            }
        std::printf("links: %d, %d to another page\n", internal, toOther);
        CHECK(internal > 0 && toOther > 0);
    }

    // A second deck: the showcase, with media9 videos in subfolders and a 48-frame animation.
    Pdf::Document demo;
    if (demo.load(corpus + "/../Demo/PenCast_Demo.pdf")) {
        QVector<MediaItem> items;
        for (int p = 0; p < demo.pageCount(); ++p) items += Media::scan(demo, p);
        CHECK(items.size() == 4);
        CHECK(items.value(0).source == "videos/turing.mp4" && items.value(0).autoplay && items.value(0).loop);
        CHECK(items.value(3).kind == MediaItem::Animation && items.value(3).frameCount == 48 && items.value(3).fps == 24);
    }

    std::printf(failures ? "%d failure(s)\n" : "media: all checks passed\n", failures);
    return failures ? 1 : 0;
}
