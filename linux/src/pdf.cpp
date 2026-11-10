#include "pdf.h"

#include <QtEndian>
#include <cmath>
#include <cstring>
#include <functional>

namespace Pdf {

Object Object::operator[](const QByteArray &key) const {
    if (!dict) return {};
    return dict->value(key);
}

Object Object::makeStream(const QByteArray &data, Dict d) {
    d["Filter"] = makeName("FlateDecode");
    Object o;
    o.type = Stream;
    o.bytes = qCompress(data).mid(4);   // qCompress = 4-byte length + a zlib stream, which is Flate
    o.dict = std::make_shared<Dict>(d);
    return o;
}

// MARK: Lexer

static bool isWhite(char c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
static bool isDelim(char c) { return c && std::strchr("()<>[]{}/%", c); }

namespace {
struct Lexer {
    const QByteArray &data;
    qint64 pos;
    const Document *doc;   // resolves an indirect stream /Length; null inside object streams

    char peek() const { return pos < data.size() ? data[pos] : 0; }

    void skip() {
        while (pos < data.size()) {
            const char c = data[pos];
            if (isWhite(c)) ++pos;
            else if (c == '%') { while (pos < data.size() && data[pos] != '\n' && data[pos] != '\r') ++pos; }
            else break;
        }
    }

    QByteArray token() {
        skip();
        const qint64 start = pos;
        while (pos < data.size() && !isWhite(data[pos]) && !isDelim(data[pos])) ++pos;
        return data.mid(start, pos - start);
    }

    static bool isInt(const QByteArray &t) {
        if (t.isEmpty()) return false;
        for (char c : t) if (c < '0' || c > '9') return false;
        return true;
    }

    Object parse(int depth = 0) {
        Object o;
        if (depth > 64) return o;
        skip();
        const char c = peek();
        if (c == '/') {
            ++pos;
            QByteArray n;
            while (pos < data.size() && !isWhite(data[pos]) && !isDelim(data[pos])) {
                if (data[pos] == '#' && pos + 2 < data.size()) {
                    n += char(data.mid(pos + 1, 2).toInt(nullptr, 16));
                    pos += 3;
                } else {
                    n += data[pos++];
                }
            }
            return Object::makeName(n);
        }
        if (c == '(') return Object::makeString(literal());
        if (c == '<') {
            if (pos + 1 < data.size() && data[pos + 1] == '<') {
                pos += 2;
                Dict d;
                for (;;) {
                    skip();
                    if (pos >= data.size()) break;
                    if (data[pos] == '>') { pos += 2; break; }
                    const Object key = parse(depth + 1);
                    if (key.type != Object::Name) { ++pos; continue; }   // garbage: step over it
                    d[key.bytes] = parse(depth + 1);
                }
                Object dict = Object::makeDict(d);
                return streamAfter(dict);
            }
            ++pos;
            QByteArray hex;
            while (pos < data.size() && data[pos] != '>') { if (!isWhite(data[pos])) hex += data[pos]; ++pos; }
            ++pos;
            if (hex.size() % 2) hex += '0';
            return Object::makeString(QByteArray::fromHex(hex));
        }
        if (c == '[') {
            ++pos;
            Array a;
            for (;;) {
                skip();
                if (pos >= data.size()) break;
                if (data[pos] == ']') { ++pos; break; }
                const qint64 before = pos;
                a << parse(depth + 1);
                if (pos == before) ++pos;
            }
            return Object::makeArray(a);
        }
        if (c == ']' || c == '>' || c == ')' || c == '{' || c == '}') { ++pos; return o; }
        const QByteArray t = token();
        if (t == "true" || t == "false") { o.type = Object::Bool; o.number = t == "true"; return o; }
        if (t == "null" || t.isEmpty()) return o;
        bool ok = false;
        const double v = t.toDouble(&ok);
        if (!ok) return o;   // a keyword (obj, endobj, R out of place): not a value
        if (isInt(t)) {   // "12 0 R" is a reference
            const qint64 save = pos;
            const QByteArray gen = token();
            if (isInt(gen) && token() == "R") {
                Object r = Object::makeRef(t.toInt());
                r.gen = gen.toInt();
                return r;
            }
            pos = save;
        }
        return Object::makeNumber(v);
    }

    QByteArray literal() {
        ++pos;   // (
        QByteArray s;
        int nest = 1;
        while (pos < data.size()) {
            char c = data[pos++];
            if (c == '(') ++nest;
            else if (c == ')' && --nest == 0) break;
            else if (c == '\\' && pos < data.size()) {
                c = data[pos++];
                switch (c) {
                case 'n': s += '\n'; continue;
                case 'r': s += '\r'; continue;
                case 't': s += '\t'; continue;
                case 'b': s += '\b'; continue;
                case 'f': s += '\f'; continue;
                case '\r': if (peek() == '\n') ++pos; continue;   // line continuation
                case '\n': continue;
                default:
                    if (c >= '0' && c <= '7') {
                        int v = c - '0';
                        for (int k = 0; k < 2 && peek() >= '0' && peek() <= '7'; ++k) v = v * 8 + (data[pos++] - '0');
                        s += char(v);
                        continue;
                    }
                    s += c;   // \( \) \\ and unknown escapes
                    continue;
                }
            }
            s += c;
        }
        return s;
    }

    /// A dictionary followed by `stream`: its raw data, by /Length when that checks out, else up to `endstream`.
    Object streamAfter(Object dict) {
        const qint64 save = pos;
        skip();
        if (data.mid(pos, 6) != "stream") { pos = save; return dict; }
        pos += 6;
        if (peek() == '\r') ++pos;
        if (peek() == '\n') ++pos;
        const qint64 start = pos;
        Object len = dict["Length"];
        if (len.type == Object::Ref && doc) len = doc->object(len.num);
        qint64 n = len.type == Object::Number ? qint64(len.number) : -1;
        auto endsAt = [&](qint64 end) {
            qint64 p = end;
            while (p < data.size() && isWhite(data[p])) ++p;
            return data.mid(p, 9) == "endstream";
        };
        if (n < 0 || start + n > data.size() || !endsAt(start + n)) {
            const qint64 end = data.indexOf("endstream", start);
            n = (end < 0 ? data.size() : end) - start;
            while (n > 0 && (data[start + n - 1] == '\n' || data[start + n - 1] == '\r')) --n;
        }
        dict.type = Object::Stream;
        dict.bytes = data.mid(start, n);
        pos = start + n;
        const qint64 end = data.indexOf("endstream", pos);
        if (end >= 0 && end - pos < 64) pos = end + 9;
        return dict;
    }
};
}

// MARK: Document

bool Document::load(const QString &path) {
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::ReadOnly) || m_file.size() < 16) return false;
    const uchar *map = m_file.map(0, m_file.size());
    m_data = map ? QByteArray::fromRawData(reinterpret_cast<const char *>(map), m_file.size()) : m_file.readAll();
    const qint64 sx = m_data.lastIndexOf("startxref");
    bool ok = false;
    if (sx >= 0) {
        Lexer lx{m_data, sx + 9, this};
        const qint64 offset = lx.token().toLongLong(&ok);
        ok = ok && readXref(offset, 0) && !m_trailer["Root"].isNull();
    }
    if (!ok) scanObjects();

    // Pages, in order, with inherited attributes.
    std::function<void(const Object &, Dict, int)> walk = [&](const Object &ref, Dict inherited, int depth) {
        const Object node = resolve(ref);
        if (!node.isDict() || depth > 32) return;
        for (const char *key : {"Resources", "MediaBox", "CropBox", "Rotate"})
            if (node.dict->contains(key)) inherited[key] = node[key];
        const Object kids = get(node, "Kids");
        if (kids.type == Object::ArrayType) {
            for (const Object &k : *kids.array) walk(k, inherited, depth + 1);
            return;
        }
        Dict d = *node.dict;
        for (auto it = inherited.cbegin(); it != inherited.cend(); ++it) if (!d.contains(it.key())) d[it.key()] = *it;
        m_pages << Object::makeDict(d);
        m_pageNums << (ref.type == Object::Ref ? ref.num : 0);
    };
    walk(get(m_trailer, "Root")["Pages"], {}, 0);
    return !m_pages.isEmpty();
}

bool Document::readXref(qint64 offset, int depth) {
    if (offset <= 0 || offset >= m_data.size() || depth > 64) return false;
    Lexer lx{m_data, offset, this};
    const qint64 start = lx.pos;
    if (lx.token() == "xref") {
        for (;;) {
            const qint64 save = lx.pos;
            const QByteArray first = lx.token();
            if (first == "trailer") break;
            bool ok1, ok2;
            const int from = first.toInt(&ok1), count = lx.token().toInt(&ok2);
            if (!ok1 || !ok2) { lx.pos = save; return false; }
            for (int i = 0; i < count; ++i) {
                const qint64 off = lx.token().toLongLong();
                lx.token();   // generation
                const QByteArray type = lx.token();
                if (type == "n" && !m_offsets.contains(from + i) && !m_compressed.contains(from + i))
                    m_offsets[from + i] = off;
                else if (type == "f") m_offsets.insert(from + i, -1);   // freed: newer sections win
            }
        }
        const Object trailer = lx.parse();
        if (m_trailer.isNull()) m_trailer = trailer;
        if (const Object stm = trailer["XRefStm"]; stm.type == Object::Number) readXref(qint64(stm.number), depth + 1);
        if (const Object prev = trailer["Prev"]; prev.type == Object::Number) readXref(qint64(prev.number), depth + 1);
        return true;
    }
    // A cross-reference stream (PDF 1.5): "n g obj << /Type /XRef … >> stream".
    lx.pos = start;
    lx.token(); lx.token();
    if (lx.token() != "obj") return false;
    const Object xs = lx.parse();
    if (xs.type != Object::Stream || xs["Type"].bytes != "XRef") return false;
    if (m_trailer.isNull()) m_trailer = xs;
    const auto data = decode(xs);
    const Object w = xs["W"];
    if (!data || w.size() != 3) return false;
    const int w0 = int(w.at(0).number), w1 = int(w.at(1).number), w2 = int(w.at(2).number), row = w0 + w1 + w2;
    Object index = xs["Index"];
    if (index.size() < 2) index = Object::makeArray({Object::makeNumber(0), xs["Size"]});
    auto field = [&](qint64 at, int width, qint64 dflt) {
        if (!width) return dflt;
        qint64 v = 0;
        for (int k = 0; k < width; ++k) v = (v << 8) | uchar(data->at(at + k));
        return v;
    };
    qint64 p = 0;
    for (int s = 0; s + 1 < index.size(); s += 2) {
        const int from = int(index.at(s).number), count = int(index.at(s + 1).number);
        for (int i = 0; i < count && p + row <= data->size(); ++i, p += row) {
            const int num = from + i;
            if (m_offsets.contains(num) || m_compressed.contains(num)) continue;
            const qint64 type = field(p, w0, 1), a = field(p + w0, w1, 0), b = field(p + w0 + w1, w2, 0);
            if (type == 1) m_offsets[num] = a;
            else if (type == 2) m_compressed[num] = {int(a), int(b)};
            else if (type == 0) m_offsets[num] = -1;
        }
    }
    if (const Object prev = xs["Prev"]; prev.type == Object::Number) readXref(qint64(prev.number), depth + 1);
    return true;
}

void Document::scanObjects() {
    m_offsets.clear();
    m_compressed.clear();
    for (qint64 at = m_data.indexOf("obj"); at >= 0; at = m_data.indexOf("obj", at + 3)) {
        // "<num> <gen> obj" at the start of a line
        qint64 p = at - 1;
        while (p >= 0 && isWhite(m_data[p])) --p;
        while (p >= 0 && m_data[p] >= '0' && m_data[p] <= '9') --p;   // generation
        while (p >= 0 && isWhite(m_data[p])) --p;
        const qint64 numEnd = p + 1;
        while (p >= 0 && m_data[p] >= '0' && m_data[p] <= '9') --p;
        if (numEnd == p + 1 || (p >= 0 && !isWhite(m_data[p]))) continue;
        m_offsets[m_data.mid(p + 1, numEnd - p - 1).toInt()] = p + 1;   // later copies win, as in an update
    }
    const QList<int> found = m_offsets.keys();
    for (int num : found) {
        const Object o = object(num);
        if (o.type == Object::Stream && o["Type"].bytes == "ObjStm") fromObjectStream(num, 0);   // registers its objects
        if (o.isDict() && o["Type"].bytes == "Catalog") m_trailer = Object::makeDict({{"Root", Object::makeRef(num)}});
    }
}

Object Document::parseAt(qint64 offset, int expectedNum) const {
    Lexer lx{m_data, offset, this};
    if (lx.token().toInt() != expectedNum) return {};
    lx.token();
    if (lx.token() != "obj") return {};
    return lx.parse();
}

Object Document::fromObjectStream(int stream, int index) const {
    if (!m_objectStreams.contains(stream)) {
        QVector<Object> objects;
        m_objectStreams[stream] = objects;   // guards against a stream that refers to itself
        const Object s = object(stream);
        const auto data = decode(s);
        if (data) {
            const int n = integer(s, "N"), first = integer(s, "First");
            Lexer head{*data, 0, nullptr};
            QVector<QPair<int, qint64>> entries;
            for (int i = 0; i < n; ++i) {
                const int num = head.token().toInt();
                entries << qMakePair(num, head.token().toLongLong());
            }
            for (const auto &[num, off] : entries) {
                Lexer lx{*data, first + off, nullptr};
                objects << lx.parse();
                if (!m_offsets.contains(num) && !m_compressed.contains(num))
                    const_cast<Document *>(this)->m_compressed[num] = {stream, int(objects.size() - 1)};
            }
        }
        m_objectStreams[stream] = objects;
    }
    return m_objectStreams[stream].value(index);
}

Object Document::object(int num) const {
    if (auto it = m_cache.constFind(num); it != m_cache.constEnd()) return *it;
    m_cache.insert(num, Object());   // a reference cycle resolves to null instead of recursing forever
    Object o;
    if (const qint64 off = m_offsets.value(num, -1); off >= 0) o = parseAt(off, num);
    else if (m_compressed.contains(num)) o = fromObjectStream(m_compressed[num].first, m_compressed[num].second);
    m_cache.insert(num, o);
    return o;
}

QRectF Document::pageBox(int i) const {
    const Object p = page(i);
    const QRectF media = rect(p, "MediaBox").value_or(QRectF(0, 0, 612, 792));
    const auto crop = rect(p, "CropBox");
    return crop ? crop->intersected(media) : media;
}

// MARK: Decoding

/// PNG predictors (10–15), as used by cross-reference streams.
static QByteArray unpredict(const QByteArray &in, int colors, int bpc, int columns) {
    const int bpp = std::max(1, colors * bpc / 8), rowLen = (colors * bpc * columns + 7) / 8;
    QByteArray out;
    QByteArray prev(rowLen, 0);
    for (qint64 p = 0; p + rowLen < in.size() + 0 && p + 1 + rowLen <= in.size(); p += rowLen + 1) {
        const int filter = uchar(in[p]);
        QByteArray row = in.mid(p + 1, rowLen);
        for (int i = 0; i < rowLen; ++i) {
            const int a = i >= bpp ? uchar(row[i - bpp]) : 0, b = uchar(prev[i]), c = i >= bpp ? uchar(prev[i - bpp]) : 0;
            int x = uchar(row[i]);
            switch (filter) {
            case 1: x += a; break;
            case 2: x += b; break;
            case 3: x += (a + b) / 2; break;
            case 4: {
                const int pa = std::abs(b - c), pb = std::abs(a - c), pc = std::abs(a + b - 2 * c);
                x += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                break;
            }
            default: break;
            }
            row[i] = char(x);
        }
        out += row;
        prev = row;
    }
    return out;
}

std::optional<QByteArray> Document::decode(const Object &stream) const {
    if (stream.type != Object::Stream) return std::nullopt;
    Object filters = get(stream, "Filter"), parms = get(stream, "DecodeParms");
    if (filters.type == Object::Name) filters = Object::makeArray({filters});
    if (parms.type == Object::DictType) parms = Object::makeArray({parms});
    QByteArray data = stream.bytes;
    for (int i = 0; i < filters.size(); ++i) {
        const QByteArray f = resolve(filters.at(i)).bytes;
        if (f != "FlateDecode" && f != "Fl") return std::nullopt;
        // qUncompress wants a 4-byte size hint first; it grows the buffer when the hint is short.
        QByteArray framed(4, 0);
        qToBigEndian<quint32>(quint32(std::min<qint64>(qint64(data.size()) * 3 + 1024, 0x7fffffff)), framed.data());
        framed += data;
        data = qUncompress(framed);
        if (data.isEmpty() && !stream.bytes.isEmpty()) return std::nullopt;
        const Object p = resolve(parms.at(i));
        const int predictor = p.isDict() ? integer(p, "Predictor") : 0;
        if (predictor >= 10) {
            data = unpredict(data, std::max(1, p["Colors"].isNull() ? 1 : integer(p, "Colors")),
                             p["BitsPerComponent"].isNull() ? 8 : integer(p, "BitsPerComponent"),
                             std::max(1, p["Columns"].isNull() ? 1 : integer(p, "Columns")));
        }
    }
    return data;
}

// MARK: Typed lookups

std::optional<QString> Document::name(const Object &d, const QByteArray &key) const {
    const Object v = get(d, key);
    if (v.type != Object::Name) return std::nullopt;
    return QString::fromLatin1(v.bytes);
}

std::optional<QString> Document::text(const Object &d, const QByteArray &key) const {
    const Object v = get(d, key);
    if (v.type == Object::String) {
        const QByteArray &b = v.bytes;
        if (b.startsWith("\xFE\xFF")) {
            QString s;
            for (int i = 2; i + 1 < b.size(); i += 2) s += QChar(ushort(uchar(b[i]) << 8 | uchar(b[i + 1])));
            return s;
        }
        if (b.startsWith("\xEF\xBB\xBF")) return QString::fromUtf8(b.mid(3));
        return QString::fromLatin1(b);   // ponytail: PDFDocEncoding read as Latin-1; differs only in rare symbols
    }
    if (v.type == Object::Stream) {
        if (const auto data = decode(v)) return QString::fromLatin1(*data);
    }
    return std::nullopt;
}

std::optional<double> Document::number(const Object &d, const QByteArray &key) const {
    const Object v = get(d, key);
    if (v.type != Object::Number) return std::nullopt;
    return v.number;
}

std::optional<QRectF> Document::rect(const Object &d, const QByteArray &key) const {
    const Object r = get(d, key);
    if (r.size() != 4) return std::nullopt;
    double n[4];
    for (int i = 0; i < 4; ++i) {
        const Object v = resolve(r.at(i));
        if (v.type != Object::Number) return std::nullopt;
        n[i] = v.number;
    }
    return QRectF(std::min(n[0], n[2]), std::min(n[1], n[3]), std::abs(n[2] - n[0]), std::abs(n[3] - n[1]));
}

// MARK: Writer

Object Writer::copy(const Document &src, const Object &o) {
    switch (o.type) {
    case Object::Ref: {
        auto &map = m_copied[&src];
        if (auto it = map.constFind(o.num); it != map.constEnd()) return Object::makeRef(*it);
        const int n = reserve();
        map.insert(o.num, n);   // before copying, so cycles end here
        set(n, copy(src, src.object(o.num)));
        return Object::makeRef(n);
    }
    case Object::ArrayType: {
        Array a;
        for (const Object &v : *o.array) a << copy(src, v);
        return Object::makeArray(a);
    }
    case Object::DictType:
    case Object::Stream: {
        Dict d;
        for (auto it = o.dict->cbegin(); it != o.dict->cend(); ++it)
            if (it.key() != "Parent") d[it.key()] = copy(src, *it);
        Object c = o;
        c.dict = std::make_shared<Dict>(d);
        return c;
    }
    default: return o;
    }
}

QByteArray formatNumber(double v) {
    if (v == std::floor(v) && std::abs(v) < 1e15) return QByteArray::number(qint64(v));
    QByteArray s = QByteArray::number(v, 'f', 4);
    while (s.endsWith('0')) s.chop(1);
    if (s.endsWith('.')) s.chop(1);
    return s == "-0" ? "0" : s;
}

static QByteArray serializeName(const QByteArray &n) {
    QByteArray out = "/";
    for (char c : n) {
        const uchar u = uchar(c);
        if (u < 33 || u > 126 || isDelim(c) || c == '#') out += '#' + QByteArray::number(u, 16).rightJustified(2, '0');
        else out += c;
    }
    return out;
}

QByteArray serialize(const Object &o) {
    switch (o.type) {
    case Object::Null: return "null";
    case Object::Bool: return o.number ? "true" : "false";
    case Object::Number: return formatNumber(o.number);
    case Object::String: return '<' + o.bytes.toHex() + '>';
    case Object::Name: return serializeName(o.bytes);
    case Object::Ref: return QByteArray::number(o.num) + " 0 R";
    case Object::ArrayType: {
        QByteArray s = "[";
        for (int i = 0; i < o.size(); ++i) s += (i ? " " : "") + serialize(o.at(i));
        return s + ']';
    }
    case Object::DictType:
    case Object::Stream: {
        QByteArray s = "<<";
        for (auto it = o.dict->cbegin(); it != o.dict->cend(); ++it)
            if (o.type != Object::Stream || it.key() != "Length") s += serializeName(it.key()) + ' ' + serialize(*it);
        if (o.type == Object::Stream) s += "/Length " + QByteArray::number(o.bytes.size());
        s += ">>";
        if (o.type == Object::Stream) s += "\nstream\n" + o.bytes + "\nendstream";
        return s;
    }
    }
    return "null";
}

QByteArray Writer::finish(int root) const {
    QByteArray out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    QVector<qint64> offsets;
    for (int i = 0; i < m_objects.size(); ++i) {
        offsets << out.size();
        out += QByteArray::number(i + 1) + " 0 obj\n" + serialize(m_objects[i]) + "\nendobj\n";
    }
    const qint64 xref = out.size();
    out += "xref\n0 " + QByteArray::number(m_objects.size() + 1) + "\n0000000000 65535 f \n";
    for (qint64 off : offsets) out += QByteArray::number(off).rightJustified(10, '0') + " 00000 n \n";
    out += "trailer\n<</Size " + QByteArray::number(m_objects.size() + 1) + "/Root " + QByteArray::number(root)
           + " 0 R>>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return out;
}

}
