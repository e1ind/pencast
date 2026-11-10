#include "media.h"

#include <QFileInfo>
#include <functional>
#include <QMap>
#include <QRegularExpression>
#include <QTransform>
#include <QUrl>

using Pdf::Object;

QString MediaItem::continuity() const {
    return QStringLiteral("%1|%2|%3|%4,%5,%6,%7").arg(kind).arg(source).arg(animation)
        .arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height());
}

namespace Media {

namespace {
// Structures verified against media9 1.29, beamer multimedia and animate (TeX Live 2025), as in Swift.
const QStringList videoExtensions = {"mp4", "m4v", "mov"};

bool isVideo(const QString &name) { return videoExtensions.contains(QFileInfo(name).suffix().toLower()); }

struct Scanner {
    const Pdf::Document &doc;

    Object dict(const Object &d, const QByteArray &k) const {
        const Object v = doc.get(d, k);
        return v.isDict() ? v : Object();
    }

    QVector<std::pair<QString, Object>> assets(const Object &content) const {
        QVector<std::pair<QString, Object>> out;
        const Object names = doc.get(dict(content, "Assets"), "Names");
        for (int i = 0; i + 1 < names.size(); i += 2) {
            const Object pair = Object::makeDict({{"n", names.at(i)}, {"s", names.at(i + 1)}});
            const auto n = doc.text(pair, "n");
            const Object spec = dict(pair, "s");
            if (n && spec.isDict()) out.append({*n, spec});
        }
        return out;
    }

    QHash<QString, QString> flashVars(const Object &content) const {
        const Object configs = doc.get(content, "Configurations");
        const Object c = doc.resolve(configs.at(0));
        const Object inst = doc.resolve(doc.get(c, "Instances").at(0));
        const auto fv = doc.text(dict(inst, "Params"), "FlashVars");
        return fv ? parseFlashVars(*fv) : QHash<QString, QString>();
    }

    MediaItem richMedia(const Object &a, MediaItem item) const {
        const Object content = dict(a, "RichMediaContent");
        if (content.isNull()) { item.source = "RichMedia without content"; return item; }
        const auto vars = flashVars(content);
        QStringList embedded;
        for (const auto &[n, spec] : assets(content)) if (!dict(spec, "EF").isNull()) embedded << n;
        const QString src = vars.value("source");
        if (!src.isEmpty() && embedded.contains(src)) {
            item.kind = MediaItem::EmbeddedVideo;
            item.source = src;
        } else if (!src.isEmpty() && (isVideo(src) || src.contains("://"))) {
            item.kind = MediaItem::LinkedVideo;
            item.source = src;
        } else if (const auto first = std::find_if(embedded.begin(), embedded.end(), isVideo); first != embedded.end()) {
            item.kind = MediaItem::EmbeddedVideo;
            item.source = *first;
        } else {
            item.source = "RichMedia without a playable video (Flash/3D content)";
        }
        item.autoplay = vars.value("autoPlay") == "true";
        item.loop = vars.value("loop") == "true";
        return item;
    }

    /// Media rendition (/S /MR) -> its clip; selector rendition (/S /SR) -> first playable choice.
    std::optional<std::pair<Object, Object>> mediaRendition(const Object &r, int depth = 0) const {
        if (depth > 8) return std::nullopt;
        if (doc.name(r, "S") == QStringLiteral("SR")) {
            const Object choices = doc.get(r, "R");
            for (int i = 0; i < choices.size(); ++i)
                if (const auto found = mediaRendition(doc.resolve(choices.at(i)), depth + 1)) return found;
            return std::nullopt;
        }
        const Object clip = dict(r, "C");
        if (clip.isNull()) return std::nullopt;
        return std::make_pair(clip, r);
    }

    struct Rendition { Object clip, params; bool autoplay; };

    /// The media clip a Screen plays, its play parameters, and whether a page-open action starts it.
    std::optional<Rendition> rendition(const Object &a) const {
        const auto isRendition = [&](const Object &act) { return doc.name(act, "S") == QStringLiteral("Rendition"); };
        Object onOpen;
        const Object aa = dict(a, "AA");
        for (const char *key : {"PO", "PV"})
            if (const Object act = dict(aa, key); onOpen.isNull() && !act.isNull() && isRendition(act)) onOpen = act;
        Object action = onOpen;
        if (action.isNull()) if (const Object act = dict(a, "A"); !act.isNull() && isRendition(act)) action = act;
        if (action.isNull()) return std::nullopt;
        const auto found = mediaRendition(dict(action, "R"));
        if (!found) return std::nullopt;
        // /OP 0 = play (stop others), 4 = play (resume); a page-open play is autoplay.
        const int op = doc.integer(action, "OP");
        return Rendition{found->first, dict(found->second, "P"), !onOpen.isNull() && (op == 0 || op == 4)};
    }

    std::optional<MediaItem> screen(const Object &a, MediaItem item) const {
        const auto r = rendition(a);
        if (!r) return std::nullopt;
        const Object spec = dict(r->clip, "D");
        if (!spec.isNull() && !dict(spec, "EF").isNull()) {
            // Cached by this name, so it needs a file extension the player recognises.
            QString n = doc.text(r->clip, "N").value_or(doc.text(spec, "UF").value_or(doc.text(spec, "F").value_or("video")));
            if (QFileInfo(n).suffix().isEmpty()) n += doc.text(r->clip, "CT") == QStringLiteral("video/quicktime") ? ".mov" : ".mp4";
            item.kind = MediaItem::EmbeddedVideo;
            item.source = n;
        } else if (const auto path = doc.text(r->clip, "D") ? doc.text(r->clip, "D")
                                     : doc.text(spec, "UF") ? doc.text(spec, "UF") : doc.text(spec, "F")) {
            item.kind = MediaItem::LinkedVideo;
            item.source = *path;
        } else {
            return std::nullopt;
        }
        item.autoplay = r->autoplay;
        // MediaPlayParams /BE (or /MH) /RC: repeat count, 0 = forever (default 1).
        std::optional<double> rc;
        for (const char *key : {"BE", "MH"})
            if (!rc) rc = doc.number(dict(r->params, key), "RC");
        item.loop = rc == 0.0;
        return item;
    }

    std::optional<QString> movieFile(const Object &a) const {
        const Object movie = dict(a, "Movie");
        if (movie.isNull()) return std::nullopt;
        if (const auto f = doc.text(movie, "F")) return f;
        const Object spec = dict(movie, "F");
        if (spec.isNull()) return std::nullopt;
        if (const auto uf = doc.text(spec, "UF")) return uf;
        return doc.text(spec, "F");
    }

    bool movieLoops(const Object &a) const {
        const auto mode = doc.name(dict(a, "A"), "Mode");
        return mode == QStringLiteral("Repeat") || mode == QStringLiteral("Palindrome");
    }

    QVector<MediaItem> items(int page) const {
        const Object annots = doc.get(doc.page(page), "Annots");
        QVector<MediaItem> items;
        struct Frames { int first; QRectF rect; int count; };
        QMap<int, Frames> frames;
        QStringList scripts;
        QSet<QString> autostartMovies;
        QHash<int, QString> movieTitles;   // items index -> /T

        for (int i = 0; i < annots.size(); ++i) {
            const Object a = doc.resolve(annots.at(i));
            const auto subtype = doc.name(a, "Subtype");
            const auto rect = doc.rect(a);
            if (!a.isDict() || !subtype || !rect) continue;
            // Page-open actions: animate's JavaScript (on a widget or a hidden Screen),
            // and \movie[autostart]'s hidden widget that plays the movie.
            if (const Object aa = dict(a, "AA"); !aa.isNull()) {
                for (const char *key : {"PO", "PV"}) {
                    const Object act = dict(aa, key);
                    if (act.isNull()) continue;
                    if (const auto js = doc.text(act, "JS")) scripts << *js;
                    if (doc.name(act, "S") == QStringLiteral("Movie")
                        && doc.name(act, "Operation").value_or("Play") == QStringLiteral("Play"))
                        if (const auto t = doc.text(act, "T")) autostartMovies.insert(*t);
                }
            }
            const bool hidden = doc.integer(a, "F") & 2;
            MediaItem item;
            item.page = page;
            item.annotIndex = i;
            item.rect = *rect;
            if (*subtype == "RichMedia") {
                items << richMedia(a, item);
            } else if (*subtype == "Movie") {
                if (const auto f = movieFile(a)) { item.kind = MediaItem::LinkedVideo; item.source = *f; }
                else item.source = "Movie annotation without a file";
                item.loop = movieLoops(a);
                if (const auto t = doc.text(a, "T")) movieTitles[items.size()] = *t;
                items << item;
            } else if (*subtype == "Screen") {
                // animate also uses a hidden, media-less Screen as a JavaScript host: skip those.
                if (const auto s = screen(a, item)) items << *s;
                else if (!hidden) { item.source = "Screen annotation without a playable video"; items << item; }
            } else if (*subtype == "Widget") {
                if (const auto t = doc.text(a, "T"); t && animateFrame(*t)) {
                    const int anim = animateFrame(*t)->first;
                    if (frames.contains(anim)) ++frames[anim].count;
                    else frames.insert(anim, {i, *rect, 1});
                }
            }
        }

        for (auto it = movieTitles.cbegin(); it != movieTitles.cend(); ++it)
            if (autostartMovies.contains(*it)) items[it.key()].autoplay = true;

        const QString js = scripts.join('\n');
        for (auto it = frames.cbegin(); it != frames.cend(); ++it) {
            const QString p = QStringLiteral("a%1_").arg(it.key());
            MediaItem item;
            item.kind = MediaItem::Animation;
            item.page = page;
            item.annotIndex = it->first;
            item.rect = it->rect;
            item.animation = it.key();
            item.frameCount = it->count;
            // ponytail: one global fps per animation; animate's per-frame rate changes (\newframe[fps]) are ignored
            const auto m = QRegularExpression(p + "fps=([0-9.]+)").match(js);
            item.fps = m.hasMatch() ? m.captured(1).toDouble() : 12;
            // `loop`: gotoNext wraps to frame 0 instead of stopping at the last frame
            if (const qsizetype r = js.indexOf(p + "gotoNext=function"); r >= 0) {
                const qsizetype start = r + p.size() + 17, end = js.indexOf(p + "gotoPrev", start);
                if (end >= 0) item.loop = js.mid(start, end - start).contains(p + "seekFrame(0)");
            }
            // `autoplay`: the page-open script ends by starting playback
            item.autoplay = js.contains(QStringLiteral("if(%1playsRight){%1playFwd();}else{%1playBwd();}").arg(p));
            items << item;
        }
        return items;
    }
};

/// A form XObject's normal appearance (the stream itself, or the first state of a state dictionary),
/// unresolved so a copy keeps it shared.
Object appearance(const Pdf::Document &doc, const Object &annot) {
    const Object ap = doc.get(annot, "AP");
    Object n = ap["N"];
    if (doc.resolve(n).type == Object::DictType) {
        const Object states = doc.resolve(n);
        n = states.dict->isEmpty() ? Object() : states.dict->first();
    }
    return doc.resolve(n).type == Object::Stream ? n : Object();
}

/// "a b c d e f cm" that draws form `form` into `rect` (PDF spec 12.5.5: BBox through Matrix, then onto Rect).
QByteArray placement(const Pdf::Document &doc, const Object &form, const QRectF &rect, QPointF origin = {}) {
    const Object f = doc.resolve(form);
    const QRectF bbox = doc.rect(f, "BBox").value_or(QRectF(0, 0, 1, 1));
    const Object m = doc.get(f, "Matrix");
    QTransform t;
    if (m.size() == 6) {
        double v[6];
        for (int i = 0; i < 6; ++i) v[i] = doc.resolve(m.at(i)).number;
        t = QTransform(v[0], v[1], v[2], v[3], v[4], v[5]);
    }
    const QRectF box = t.mapRect(bbox);
    if (box.width() <= 0 || box.height() <= 0) return "1 0 0 1 0 0 cm";
    const double sx = rect.width() / box.width(), sy = rect.height() / box.height();
    using Pdf::formatNumber;
    return formatNumber(sx) + " 0 0 " + formatNumber(sy) + ' ' + formatNumber(rect.x() - origin.x() - box.x() * sx) + ' '
           + formatNumber(rect.y() - origin.y() - box.y() * sy) + " cm";
}

/// animate frame widgets "<id>.<k>" for an animation item, in frame order.
QVector<Object> frameWidgets(const Pdf::Document &doc, const MediaItem &item) {
    QHash<int, Object> byFrame;
    const Object annots = doc.get(doc.page(item.page), "Annots");
    for (int i = 0; i < annots.size(); ++i) {
        const Object a = doc.resolve(annots.at(i));
        const auto t = doc.text(a, "T");
        const auto f = t ? animateFrame(*t) : std::nullopt;
        if (f && f->first == item.animation && !byFrame.contains(f->second)) byFrame.insert(f->second, a);
    }
    QVector<Object> out;
    for (int k = 0; k < item.frameCount && byFrame.contains(k); ++k) out << byFrame[k];
    return out;
}

Object catalog(Pdf::Writer &w, int pagesNum, const Pdf::Array &kids) {
    w.set(pagesNum, Object::makeDict({{"Type", Object::makeName("Pages")}, {"Kids", Object::makeArray(kids)},
                                      {"Count", Object::makeNumber(kids.size())}}));
    return Object::makeDict({{"Type", Object::makeName("Catalog")}, {"Pages", Object::makeRef(pagesNum)}});
}

Object box(const QRectF &r) {
    return Object::makeArray({Object::makeNumber(r.left()), Object::makeNumber(r.top()),
                              Object::makeNumber(r.right()), Object::makeNumber(r.bottom())});
}
}

QVector<MediaItem> scan(const Pdf::Document &doc, int page) { return Scanner{doc}.items(page); }

QHash<QString, QString> parseFlashVars(const QString &s) {
    QHash<QString, QString> out;
    for (const QString &pair : s.split('&', Qt::SkipEmptyParts)) {
        const qsizetype eq = pair.indexOf('=');
        if (eq < 0) continue;
        out[pair.left(eq)] = QUrl::fromPercentEncoding(pair.mid(eq + 1).toUtf8());
    }
    return out;
}

std::optional<std::pair<int, int>> animateFrame(const QString &t) {
    const QStringList parts = t.split('.');
    bool ok1 = false, ok2 = false;
    if (parts.size() != 2) return std::nullopt;
    const int a = parts[0].toInt(&ok1), f = parts[1].toInt(&ok2);
    if (!ok1 || !ok2) return std::nullopt;
    return std::make_pair(a, f);
}

/// The page a destination points at: an explicit [page /XYZ …] array, or a name looked up in the catalog's
/// /Dests dictionary or /Names /Dests name tree.
static int destinationPage(const Pdf::Document &doc, Object dest, int depth = 0) {
    dest = doc.resolve(dest);
    if (depth > 4) return -1;
    if (dest.type == Object::Name || dest.type == Object::String) {
        const Object root = doc.get(doc.trailer(), "Root");
        Object found = doc.get(doc.get(root, "Dests"), dest.bytes);
        if (found.isNull()) {   // name tree: Kids down to Names [key value …]
            std::function<Object(const Object &, int)> search = [&](const Object &node, int level) -> Object {
                if (level > 32 || !node.isDict()) return {};
                const Object names = doc.get(node, "Names");
                for (int i = 0; i + 1 < names.size(); i += 2)
                    if (doc.resolve(names.at(i)).bytes == dest.bytes) return names.at(i + 1);
                const Object kids = doc.get(node, "Kids");
                for (int i = 0; i < kids.size(); ++i) {
                    const Object kid = doc.resolve(kids.at(i));
                    const Object limits = doc.get(kid, "Limits");
                    if (limits.size() == 2 && (dest.bytes < doc.resolve(limits.at(0)).bytes || doc.resolve(limits.at(1)).bytes < dest.bytes))
                        continue;
                    if (Object r = search(kid, level + 1); !r.isNull()) return r;
                }
                return {};
            };
            found = search(doc.get(doc.get(root, "Names"), "Dests"), 0);
        }
        found = doc.resolve(found);
        if (found.isDict()) found = found["D"];
        return destinationPage(doc, found, depth + 1);
    }
    if (dest.type != Object::ArrayType || dest.size() == 0) return -1;
    const Object target = dest.at(0);
    if (target.type == Object::Ref) {
        for (int i = 0; i < doc.pageCount(); ++i) if (doc.pageObject(i) == target.num) return i;
        return -1;
    }
    return target.type == Object::Number ? int(target.number) : -1;
}

QVector<Link> links(const Pdf::Document &doc, int page) {
    QVector<Link> out;
    const Object annots = doc.get(doc.page(page), "Annots");
    for (int i = 0; i < annots.size(); ++i) {
        const Object a = doc.resolve(annots.at(i));
        const auto rect = doc.rect(a);
        if (doc.name(a, "Subtype") != QStringLiteral("Link") || !rect) continue;
        Link link;
        link.rect = *rect;
        const Object action = doc.get(a, "A");
        const auto kind = doc.name(action, "S");
        if (kind == QStringLiteral("Named")) {
            const QString n = doc.name(action, "N").value_or(QString());
            if (n == "NextPage") link.action = Link::Next;
            else if (n == "PrevPage") link.action = Link::Previous;
            else if (n == "FirstPage") link.action = Link::First;
            else if (n == "LastPage") link.action = Link::Last;
            else continue;
        } else {
            const Object dest = kind == QStringLiteral("GoTo") ? action["D"] : action.isNull() ? a["Dest"] : Object();
            link.page = dest.isNull() ? -1 : destinationPage(doc, dest);
            if (link.page < 0) continue;
        }
        out << link;
    }
    return out;
}

QRectF onPage(const QRectF &r, const QRectF &pageBox) {
    return QRectF(r.x() - pageBox.x(), pageBox.y() + pageBox.height() - (r.y() + r.height()), r.width(), r.height());
}

std::optional<QByteArray> embeddedVideo(const Pdf::Document &doc, const MediaItem &item) {
    if (item.kind != MediaItem::EmbeddedVideo) return std::nullopt;
    const Scanner s{doc};
    const Object a = doc.resolve(doc.get(doc.page(item.page), "Annots").at(item.annotIndex));
    Object spec;
    if (const Object content = s.dict(a, "RichMediaContent"); !content.isNull()) {
        for (const auto &[n, sp] : s.assets(content)) if (n == item.source) { spec = sp; break; }
    } else if (const auto r = s.rendition(a)) {
        spec = s.dict(r->clip, "D");
    }
    const Object stream = doc.get(s.dict(spec, "EF"), "F");
    return doc.decode(stream);
}

QByteArray animationFrames(const Pdf::Document &doc, const MediaItem &item) {
    Pdf::Writer w;
    const int pages = w.reserve();
    Pdf::Array kids;
    for (const Object &widget : frameWidgets(doc, item)) {
        const Object form = appearance(doc, widget);
        const QRectF rect = doc.rect(widget).value_or(item.rect);
        QByteArray content = "q ";
        Pdf::Dict xobjects;
        if (!form.isNull()) {
            xobjects["F"] = w.copy(doc, form);
            content += placement(doc, form, rect, rect.topLeft()) + " /F Do";
        }
        content += " Q";
        kids << Object::makeRef(w.add(Object::makeDict({
            {"Type", Object::makeName("Page")}, {"Parent", Object::makeRef(pages)},
            {"MediaBox", box(QRectF(0, 0, rect.width(), rect.height()))},
            {"Resources", Object::makeDict({{"XObject", Object::makeDict(xobjects)}})},
            {"Contents", Object::makeRef(w.add(Object::makeStream(content)))}})));
    }
    return w.finish(w.add(catalog(w, pages, kids)));
}

// MARK: Export

static QByteArray rgb(const QColor &c) {
    using Pdf::formatNumber;
    return formatNumber(std::round(c.redF() * 1000) / 1000) + ' ' + formatNumber(std::round(c.greenF() * 1000) / 1000) + ' '
           + formatNumber(std::round(c.blueF() * 1000) / 1000);
}

QByteArray exportPdf(const Pdf::Document &doc, const QVector<ExportPage> &pages) {
    using Pdf::formatNumber;
    Pdf::Writer w;
    const int pagesNum = w.reserve();
    Pdf::Array kids;
    for (const ExportPage &ep : pages) {
        const Object page = doc.page(ep.page);
        if (page.isNull()) continue;
        const QRectF pageBox = doc.pageBox(ep.page);
        // The kept part in PDF space; ink (top-left, slide-relative) maps into it with y flipped.
        // ponytail: pages with /Rotate export unrotated; Beamer never sets it.
        const double left = pageBox.x() + ep.slide.x(), top = pageBox.y() + pageBox.height() - ep.slide.y();
        const QRectF kept(left, top - ep.slide.height(), ep.slide.width(), ep.slide.height());

        // The page's own content as a form, in its original coordinates.
        const QRectF media = doc.rect(page, "MediaBox").value_or(pageBox);
        Pdf::Dict form{{"Type", Object::makeName("XObject")}, {"Subtype", Object::makeName("Form")},
                       {"BBox", box(media)}, {"Resources", w.copy(doc, page["Resources"])}};
        const Object contents = doc.get(page, "Contents");
        Object formStream;
        if (contents.type == Object::Stream) {   // copied as is, still compressed
            formStream = w.copy(doc, contents);
            for (auto it = form.cbegin(); it != form.cend(); ++it) (*formStream.dict)[it.key()] = *it;
        } else {
            QByteArray joined;
            for (int i = 0; i < contents.size(); ++i)
                if (const auto part = doc.decode(doc.resolve(contents.at(i)))) joined += *part + '\n';
            formStream = Object::makeStream(joined, form);
        }
        Pdf::Dict xobjects{{"P", Object::makeRef(w.add(formStream))}};
        QByteArray content = "q /P Do Q\n";

        // Annotation appearances: video posters, buttons, the first frame of each animation.
        const Object annots = doc.get(page, "Annots");
        for (int i = 0; i < annots.size(); ++i) {
            const Object a = doc.resolve(annots.at(i));
            const auto t = doc.text(a, "T");
            const auto frame = t ? animateFrame(*t) : std::nullopt;
            const int flags = doc.integer(a, "F");
            if (frame ? frame->second != 0 : (flags & (2 | 32))) continue;   // hidden or no-view; later frames
            if (doc.name(a, "Subtype") == QStringLiteral("Popup")) continue;
            const Object ap = appearance(doc, a);
            const auto rect = doc.rect(a);
            if (ap.isNull() || !rect) continue;
            const QByteArray name = "A" + QByteArray::number(i);
            xobjects[name] = w.copy(doc, ap);
            content += "q " + placement(doc, ap, *rect) + " /" + name + " Do Q\n";
        }

        // Ink.
        Pdf::Dict states;
        content += "q 1 J 1 j\n";
        auto pt = [&](QPointF p) { return formatNumber(left + p.x()) + ' ' + formatNumber(top - p.y()); };
        for (int k = 0; k < ep.ink.size(); ++k) {
            const Stroke &s = ep.ink[k];
            if (s.points.isEmpty()) continue;
            const bool hl = s.kind == Stroke::Highlighter;
            content += "q ";
            if (hl || s.color.alphaF() < 1) {
                Pdf::Dict gs{{"Type", Object::makeName("ExtGState")}, {"CA", Object::makeNumber(s.color.alphaF())}};
                if (hl) gs["BM"] = Object::makeName("Multiply");   // as on screen: text under it stays dark
                const QByteArray name = "G" + QByteArray::number(k);
                states[name] = Object::makeDict(gs);
                content += '/' + name + " gs ";
            }
            content += rgb(s.color) + " RG\n";
            if (hl || s.points.size() == 1) {
                content += formatNumber(s.points.first().w) + " w " + pt(s.points.first().p) + " m";
                for (int j = s.points.size() == 1 ? 0 : 1; j < s.points.size(); ++j) content += ' ' + pt(s.points[j].p) + " l";
                content += " S\n";
            } else {   // segments of varying width, as SlideView draws them
                for (int j = 1; j < s.points.size(); ++j) {
                    const InkPoint &a = s.points[j - 1], &b = s.points[j];
                    content += formatNumber((a.w + b.w) / 2) + " w " + pt(a.p) + " m " + pt(b.p) + " l S\n";
                }
            }
            content += "Q\n";
        }
        content += "Q\n";

        Pdf::Dict resources{{"XObject", Object::makeDict(xobjects)}};
        if (!states.isEmpty()) resources["ExtGState"] = Object::makeDict(states);
        kids << Object::makeRef(w.add(Object::makeDict({
            {"Type", Object::makeName("Page")}, {"Parent", Object::makeRef(pagesNum)},
            {"MediaBox", box(kept)}, {"Resources", Object::makeDict(resources)},
            {"Contents", Object::makeRef(w.add(Object::makeStream(content)))}})));
    }
    return w.finish(w.add(catalog(w, pagesNum, kids)));
}

}
