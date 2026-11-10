// Just enough of PDF to read what Qt's PDF module hides (annotations, embedded files, appearance
// streams) and to write new PDFs from pieces of an old one. The Linux counterpart of CGPDF in
// MediaScanner.swift. No encryption; pdfTeX, LuaTeX and XeTeX output only needs Flate.
#pragma once

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QMap>
#include <QRectF>
#include <QString>
#include <QVector>
#include <memory>
#include <optional>

namespace Pdf {

struct Object;
using Dict = QMap<QByteArray, Object>;
using Array = QVector<Object>;

struct Object {
    enum Type { Null, Bool, Number, String, Name, ArrayType, DictType, Ref, Stream };
    Type type = Null;
    double number = 0;                   // Number; Bool as 0/1
    QByteArray bytes;                    // String, Name, and a Stream's raw (still encoded) data
    int num = 0, gen = 0;                // Ref
    std::shared_ptr<Array> array;        // ArrayType
    std::shared_ptr<Dict> dict;          // DictType, and a Stream's dictionary

    bool isNull() const { return type == Null; }
    bool isDict() const { return type == DictType || type == Stream; }   // a stream is its dictionary too
    /// Dictionary entry, unresolved; Null when absent or not a dictionary.
    Object operator[](const QByteArray &key) const;
    int size() const { return array ? array->size() : 0; }
    Object at(int i) const { return array && i >= 0 && i < array->size() ? array->at(i) : Object(); }

    static Object makeNumber(double v) { Object o; o.type = Number; o.number = v; return o; }
    static Object makeName(const QByteArray &n) { Object o; o.type = Name; o.bytes = n; return o; }
    static Object makeString(const QByteArray &s) { Object o; o.type = String; o.bytes = s; return o; }
    static Object makeRef(int num) { Object o; o.type = Ref; o.num = num; return o; }
    static Object makeArray(const Array &a = {}) { Object o; o.type = ArrayType; o.array = std::make_shared<Array>(a); return o; }
    static Object makeDict(const Dict &d = {}) { Object o; o.type = DictType; o.dict = std::make_shared<Dict>(d); return o; }
    /// A Flate-compressed stream holding `data`.
    static Object makeStream(const QByteArray &data, Dict d = {});
};

class Document {
public:
    bool load(const QString &path);
    int pageCount() const { return m_pages.size(); }
    /// The page dictionary, with Resources, MediaBox, CropBox and Rotate inherited from the page tree.
    Object page(int i) const { return m_pages.value(i); }
    int pageObject(int i) const { return m_pageNums.value(i); }
    /// The visible page area (CropBox within MediaBox), PDF user space (y up).
    QRectF pageBox(int i) const;

    Object trailer() const { return m_trailer; }
    Object object(int num) const;
    Object resolve(const Object &o) const { return o.type == Object::Ref ? object(o.num) : o; }
    Object get(const Object &dict, const QByteArray &key) const { return resolve(dict[key]); }

    /// A stream's data with its filters undone; nullopt for filters other than Flate (images: DCT, JPX…).
    std::optional<QByteArray> decode(const Object &stream) const;

    // Typed lookups on a dictionary entry (resolving references), as in MediaScanner.swift.
    std::optional<QString> name(const Object &d, const QByteArray &key) const;
    /// A text string, or a stream decoded as Latin-1 (animate stores long JavaScript in streams).
    std::optional<QString> text(const Object &d, const QByteArray &key) const;
    std::optional<double> number(const Object &d, const QByteArray &key) const;
    int integer(const Object &d, const QByteArray &key) const { return int(number(d, key).value_or(0)); }
    std::optional<QRectF> rect(const Object &d, const QByteArray &key = "Rect") const;

private:
    bool readXref(qint64 offset, int depth);
    void scanObjects();   // damaged or missing xref: find "n g obj" by scanning the file
    Object parseAt(qint64 offset, int expectedNum) const;
    Object fromObjectStream(int stream, int index) const;

    QFile m_file;
    QByteArray m_data;   // the whole file, memory-mapped
    QHash<int, qint64> m_offsets;
    QHash<int, QPair<int, int>> m_compressed;   // object -> (object stream, index)
    Object m_trailer;
    QVector<Object> m_pages;
    QVector<int> m_pageNums;
    mutable QHash<int, Object> m_cache;
    mutable QHash<int, QVector<Object>> m_objectStreams;
};

/// Builds a new PDF. Objects copied from a Document keep their sharing: an image used by many pages is
/// written once.
class Writer {
public:
    int reserve() { m_objects.append(Object()); return m_objects.size(); }
    int add(const Object &o) { const int n = reserve(); m_objects[n - 1] = o; return n; }
    void set(int num, const Object &o) { m_objects[num - 1] = o; }
    /// Deep copy from `src`; references are followed and renumbered, /Parent links are dropped so a copy
    /// never pulls in the source's page tree.
    Object copy(const Document &src, const Object &o);
    /// The file, with catalog `root`.
    QByteArray finish(int root) const;

private:
    QVector<Object> m_objects;
    QHash<const Document *, QHash<int, int>> m_copied;
};

QByteArray serialize(const Object &o);
QByteArray formatNumber(double v);

}
