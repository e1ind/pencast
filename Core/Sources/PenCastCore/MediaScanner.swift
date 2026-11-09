import CoreGraphics
import CryptoKit
import Foundation

/// One playable thing on a slide, in PDF user-space coordinates.
public struct MediaItem: Hashable, Identifiable, Sendable {
    public enum Kind: Hashable, Sendable {
        /// Video stored inside the PDF: media9 RichMedia asset, or a Screen/Rendition MediaClip.
        case embeddedVideo(name: String)
        /// `\movie`, or media9 whose source is not embedded. Relative path or URL.
        case linkedVideo(path: String)
        /// animate: widgets named "<id>.<k>", one per frame.
        case animation(id: Int, frameCount: Int, fps: Double)
        case unsupported(reason: String)
    }

    public var id: String { "\(page)-\(annotIndex)" }
    public let page: Int          // 0-based
    public let annotIndex: Int    // index into the page's /Annots
    public let rect: CGRect       // PDF user space, normalized
    public let kind: Kind
    public var autoplay = false
    public var loop = false
}

/// Walks page annotations with CGPDF (PDFKit hides RichMedia/Movie internals).
/// Structures verified against media9 1.29, beamer multimedia and animate (TeX Live 2025).
public final class MediaScanner {
    public let document: CGPDFDocument

    public init?(url: URL) {
        guard let doc = CGPDFDocument(url as CFURL) else { return nil }
        document = doc
    }

    public func scan() -> [MediaItem] {
        guard document.numberOfPages > 0 else { return [] }
        return (0..<document.numberOfPages).flatMap(items(onPage:))
    }

    public func items(onPage page: Int) -> [MediaItem] {
        guard let annots = annotations(page) else { return [] }
        var items: [MediaItem] = []
        var frames: [Int: (first: Int, rect: CGRect, count: Int)] = [:]
        var scripts: [String] = []
        var autostartMovies: Set<String> = []
        var movieTitles: [Int: String] = [:]   // items index -> /T

        for i in 0..<CGPDFArrayGetCount(annots) {
            var a: CGPDFDictionaryRef?
            guard CGPDFArrayGetDictionary(annots, i, &a), let a,
                  let subtype = name(a, "Subtype"), let rect = rect(a) else { continue }
            // Page-open actions: animate's JavaScript (on a widget or a hidden Screen),
            // and \movie[autostart]'s hidden widget that plays the movie.
            if let aa = dict(a, "AA") {
                for key in ["PO", "PV"] {
                    guard let act = dict(aa, key) else { continue }
                    if let js = text(act, "JS") { scripts.append(js) }
                    if name(act, "S") == "Movie", name(act, "Operation") ?? "Play" == "Play",
                       let t = text(act, "T") {
                        autostartMovies.insert(t)
                    }
                }
            }
            let hidden = int(a, "F") & 2 != 0
            switch subtype {
            case "RichMedia":
                items.append(richMedia(a, page: page, index: i, rect: rect))
            case "Movie":
                var item = MediaItem(page: page, annotIndex: i, rect: rect,
                                     kind: movieFile(a).map { .linkedVideo(path: $0) }
                                         ?? .unsupported(reason: "Movie annotation without a file"))
                item.loop = movieLoops(a)
                movieTitles[items.count] = text(a, "T")
                items.append(item)
            case "Screen":
                // animate also uses a hidden, media-less Screen as a JavaScript host: skip those.
                if let item = screen(a, page: page, index: i, rect: rect) { items.append(item) }
                else if !hidden {
                    items.append(MediaItem(page: page, annotIndex: i, rect: rect,
                                           kind: .unsupported(reason: "Screen annotation without a playable video")))
                }
            case "Widget":
                if let t = text(a, "T"), let (anim, _) = Self.animateFrame(t) {
                    if let f = frames[anim] { frames[anim] = (f.first, f.rect, f.count + 1) }
                    else { frames[anim] = (i, rect, 1) }
                }
            default: break
            }
        }

        for (i, t) in movieTitles where autostartMovies.contains(t) { items[i].autoplay = true }

        let js = scripts.joined(separator: "\n")
        for (anim, f) in frames.sorted(by: { $0.key < $1.key }) {
            let p = "a\(anim)_"
            // ponytail: one global fps per animation; animate's per-frame rate changes (\newframe[fps]) are ignored
            let fps = firstMatch(#"a\#(anim)_fps=([0-9.]+)"#, in: js).flatMap(Double.init) ?? 12
            var item = MediaItem(page: page, annotIndex: f.first, rect: f.rect,
                                 kind: .animation(id: anim, frameCount: f.count, fps: fps))
            // `loop`: gotoNext wraps to frame 0 instead of stopping at the last frame
            if let r = js.range(of: "\(p)gotoNext=function"),
               let end = js.range(of: "\(p)gotoPrev", range: r.upperBound..<js.endIndex) {
                item.loop = js[r.upperBound..<end.lowerBound].contains("\(p)seekFrame(0)")
            }
            // `autoplay`: the page-open script ends by starting playback
            item.autoplay = js.contains("if(\(p)playsRight){\(p)playFwd();}else{\(p)playBwd();}")
            items.append(item)
        }
        return items
    }

    /// Decodes an embedded video (media9 or Screen/Rendition) to `url`.
    /// ponytail: CGPDFStreamCopyData holds the whole decoded video in memory once; a 200 MB+ video
    /// spikes memory here. Upgrade path: locate the raw stream offset and inflate in chunks.
    public func extractEmbeddedVideo(_ item: MediaItem, to url: URL) throws {
        guard case .embeddedVideo(let assetName) = item.kind,
              let annots = annotations(item.page) else { throw ScanError.notEmbedded }
        var a: CGPDFDictionaryRef?
        guard CGPDFArrayGetDictionary(annots, item.annotIndex, &a), let a,
              let spec = dict(a, "RichMediaContent").flatMap({ assets($0).first(where: { $0.name == assetName })?.spec })
                  ?? rendition(a).flatMap({ dict($0.clip, "D") }),
              let ef = dict(spec, "EF"), let stream = stream(ef, "F") else { throw ScanError.notEmbedded }
        var format = CGPDFDataFormat.raw
        guard let data = CGPDFStreamCopyData(stream, &format), format == .raw else { throw ScanError.undecodable }
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try (data as Data).write(to: url, options: .atomic)
    }

    public enum ScanError: Error { case notEmbedded, undecodable }

    // MARK: - media9

    private static let videoExtensions: Set<String> = ["mp4", "m4v", "mov"]

    private func richMedia(_ a: CGPDFDictionaryRef, page: Int, index: Int, rect: CGRect) -> MediaItem {
        guard let content = dict(a, "RichMediaContent") else {
            return MediaItem(page: page, annotIndex: index, rect: rect, kind: .unsupported(reason: "RichMedia without content"))
        }
        let vars = flashVars(content)
        let embedded = assets(content).filter { dict($0.spec, "EF") != nil }
        let isVideo = { (n: String) in Self.videoExtensions.contains((n as NSString).pathExtension.lowercased()) }
        let kind: MediaItem.Kind
        if let src = vars["source"], embedded.contains(where: { $0.name == src }) {
            kind = .embeddedVideo(name: src)
        } else if let src = vars["source"], isVideo(src) || src.contains("://") {
            kind = .linkedVideo(path: src)
        } else if let first = embedded.first(where: { isVideo($0.name) }) {
            kind = .embeddedVideo(name: first.name)
        } else {
            kind = .unsupported(reason: "RichMedia without a playable video (Flash/3D content)")
        }
        var item = MediaItem(page: page, annotIndex: index, rect: rect, kind: kind)
        item.autoplay = vars["autoPlay"] == "true"
        item.loop = vars["loop"] == "true"
        return item
    }

    private func assets(_ content: CGPDFDictionaryRef) -> [(name: String, spec: CGPDFDictionaryRef)] {
        guard let names = dict(content, "Assets").flatMap({ array($0, "Names") }) else { return [] }
        var out: [(String, CGPDFDictionaryRef)] = []
        var i = 0
        while i + 1 < CGPDFArrayGetCount(names) {
            var s: CGPDFStringRef?, d: CGPDFDictionaryRef?
            if CGPDFArrayGetString(names, i, &s), let s, let n = CGPDFStringCopyTextString(s),
               CGPDFArrayGetDictionary(names, i + 1, &d), let d {
                out.append((n as String, d))
            }
            i += 2
        }
        return out
    }

    private func flashVars(_ content: CGPDFDictionaryRef) -> [String: String] {
        var c: CGPDFDictionaryRef?, inst: CGPDFDictionaryRef?
        guard let configs = array(content, "Configurations"), CGPDFArrayGetDictionary(configs, 0, &c), let c,
              let instances = array(c, "Instances"), CGPDFArrayGetDictionary(instances, 0, &inst), let inst,
              let fv = dict(inst, "Params").flatMap({ text($0, "FlashVars") }) else { return [:] }
        return Self.parseFlashVars(fv)
    }

    static func parseFlashVars(_ s: String) -> [String: String] {
        var out: [String: String] = [:]
        for pair in s.split(separator: "&") {
            let kv = pair.split(separator: "=", maxSplits: 1).map(String.init)
            guard kv.count == 2 else { continue }
            out[kv[0]] = kv[1].removingPercentEncoding ?? kv[1]
        }
        return out
    }

    // MARK: - Screen + Rendition (the standard PDF way; e.g. LuaTeX \pdfextension annot, Acrobat)

    private func screen(_ a: CGPDFDictionaryRef, page: Int, index: Int, rect: CGRect) -> MediaItem? {
        guard let (clip, params, autoplay) = rendition(a) else { return nil }
        let kind: MediaItem.Kind
        if let spec = dict(clip, "D"), dict(spec, "EF") != nil {
            // Cached by this name, so it needs a file extension AVPlayer recognises.
            var n = text(clip, "N") ?? text(spec, "UF") ?? text(spec, "F") ?? "video"
            if (n as NSString).pathExtension.isEmpty { n += text(clip, "CT") == "video/quicktime" ? ".mov" : ".mp4" }
            kind = .embeddedVideo(name: n)
        } else if let path = text(clip, "D") ?? dict(clip, "D").flatMap({ text($0, "UF") ?? text($0, "F") }) {
            kind = .linkedVideo(path: path)
        } else {
            return nil
        }
        var item = MediaItem(page: page, annotIndex: index, rect: rect, kind: kind)
        item.autoplay = autoplay
        // MediaPlayParams /BE (or /MH) /RC: repeat count, 0 = forever (default 1).
        let rc = [params.flatMap { dict($0, "BE") }, params.flatMap { dict($0, "MH") }]
            .compactMap { $0.flatMap { number($0, "RC") } }.first
        item.loop = rc == 0
        return item
    }

    /// The media clip a Screen plays, its play parameters, and whether a page-open action starts it.
    private func rendition(_ a: CGPDFDictionaryRef) -> (clip: CGPDFDictionaryRef, params: CGPDFDictionaryRef?, autoplay: Bool)? {
        let isRendition = { (act: CGPDFDictionaryRef) in name(act, "S") == "Rendition" }
        let onOpen = dict(a, "AA").flatMap { aa in ["PO", "PV"].compactMap { dict(aa, $0) }.first(where: isRendition) }
        let action = onOpen ?? dict(a, "A").flatMap { isRendition($0) ? $0 : nil }
        guard let action, let r = dict(action, "R"), let (clip, rend) = mediaRendition(r) else { return nil }
        // /OP 0 = play (stop others), 4 = play (resume); a page-open play is autoplay.
        let op = int(action, "OP")
        return (clip, dict(rend, "P"), onOpen != nil && (op == 0 || op == 4))
    }

    /// Media rendition (/S /MR) -> its clip; selector rendition (/S /SR) -> first playable choice.
    private func mediaRendition(_ r: CGPDFDictionaryRef) -> (CGPDFDictionaryRef, CGPDFDictionaryRef)? {
        if name(r, "S") == "SR", let choices = array(r, "R") {
            for i in 0..<CGPDFArrayGetCount(choices) {
                var c: CGPDFDictionaryRef?
                if CGPDFArrayGetDictionary(choices, i, &c), let c, let found = mediaRendition(c) { return found }
            }
            return nil
        }
        guard let clip = dict(r, "C") else { return nil }
        return (clip, r)
    }

    // MARK: - \movie and animate

    private func movieFile(_ a: CGPDFDictionaryRef) -> String? {
        guard let movie = dict(a, "Movie") else { return nil }
        if let f = text(movie, "F") { return f }
        guard let spec = dict(movie, "F") else { return nil }
        return text(spec, "UF") ?? text(spec, "F")
    }

    private func movieLoops(_ a: CGPDFDictionaryRef) -> Bool {
        guard let act = dict(a, "A") else { return false }
        return ["Repeat", "Palindrome"].contains(name(act, "Mode") ?? "")
    }

    /// "3.17" -> (3, 17): animate names frame widgets "<animation>.<frame>".
    static func animateFrame(_ t: String) -> (Int, Int)? {
        let parts = t.split(separator: ".")
        guard parts.count == 2, let a = Int(parts[0]), let f = Int(parts[1]) else { return nil }
        return (a, f)
    }

    // MARK: - CGPDF plumbing

    private func annotations(_ page: Int) -> CGPDFArrayRef? {
        guard let p = document.page(at: page + 1)?.dictionary else { return nil }
        return array(p, "Annots")
    }
}

// CGPDF*Ref are all OpaquePointer, so these are free functions rather than extensions.
private func dict(_ d: CGPDFDictionaryRef, _ k: String) -> CGPDFDictionaryRef? {
    var v: CGPDFDictionaryRef?; return CGPDFDictionaryGetDictionary(d, k, &v) ? v : nil
}
private func array(_ d: CGPDFDictionaryRef, _ k: String) -> CGPDFArrayRef? {
    var v: CGPDFArrayRef?; return CGPDFDictionaryGetArray(d, k, &v) ? v : nil
}
private func stream(_ d: CGPDFDictionaryRef, _ k: String) -> CGPDFStreamRef? {
    var v: CGPDFStreamRef?; return CGPDFDictionaryGetStream(d, k, &v) ? v : nil
}
private func name(_ d: CGPDFDictionaryRef, _ k: String) -> String? {
    var v: UnsafePointer<CChar>?
    guard CGPDFDictionaryGetName(d, k, &v), let v else { return nil }
    return String(cString: v)
}
/// A text string, or a stream decoded as Latin-1 (animate stores long JavaScript in streams).
private func text(_ d: CGPDFDictionaryRef, _ k: String) -> String? {
    var s: CGPDFStringRef?
    if CGPDFDictionaryGetString(d, k, &s), let s, let t = CGPDFStringCopyTextString(s) { return t as String }
    var format = CGPDFDataFormat.raw
    guard let st = stream(d, k), let data = CGPDFStreamCopyData(st, &format) else { return nil }
    return String(data: data as Data, encoding: .isoLatin1)
}
private func int(_ d: CGPDFDictionaryRef, _ k: String) -> Int {
    var v: CGPDFInteger = 0; return CGPDFDictionaryGetInteger(d, k, &v) ? v : 0
}
private func number(_ d: CGPDFDictionaryRef, _ k: String) -> CGFloat? {
    var v: CGPDFReal = 0; return CGPDFDictionaryGetNumber(d, k, &v) ? v : nil
}
private func rect(_ d: CGPDFDictionaryRef) -> CGRect? {
    guard let r = array(d, "Rect"), CGPDFArrayGetCount(r) == 4 else { return nil }
    var n = [CGPDFReal](repeating: 0, count: 4)
    for i in 0..<4 where !CGPDFArrayGetNumber(r, i, &n[i]) { return nil }
    return CGRect(x: min(n[0], n[2]), y: min(n[1], n[3]), width: abs(n[2] - n[0]), height: abs(n[3] - n[1]))
}
private func firstMatch(_ pattern: String, in s: String) -> String? {
    guard let re = try? NSRegularExpression(pattern: pattern),
          let m = re.firstMatch(in: s, range: NSRange(s.startIndex..., in: s)),
          let r = Range(m.range(at: 1), in: s) else { return nil }
    return String(s[r])
}

// MARK: - Geometry (spec 5.2 #2: one logical canvas, letterboxed per screen)

public enum SlideGeometry {
    /// Largest scale at which `page` fits inside `container`.
    public static func fitScale(page: CGSize, in container: CGSize) -> CGFloat {
        guard page.width > 0, page.height > 0 else { return 1 }
        return min(container.width / page.width, container.height / page.height)
    }

    /// Beamer `show notes on second screen`: pages are two screens wide (notes on the right) or two
    /// screens tall (notes at the bottom). Returns the slide part and, if present, the notes part.
    /// ponytail: left/top note placements are treated as plain slides.
    public static func split(_ box: CGRect) -> (slide: CGRect, notes: CGRect?) {
        let ratio = box.width / max(box.height, 1)
        if ratio > 2.2 {   // 2 × 4:3 = 2.67 … 2 × 16:9 = 3.56
            let w = box.width / 2
            return (CGRect(x: box.minX, y: box.minY, width: w, height: box.height),
                    CGRect(x: box.minX + w, y: box.minY, width: w, height: box.height))
        }
        if ratio < 0.95 {  // 16:9 over 16:9 = 0.89 (4:3 stacked = 0.67)
            let h = box.height / 2   // PDF y is up: the top half is the slide
            return (CGRect(x: box.minX, y: box.minY + h, width: box.width, height: h),
                    CGRect(x: box.minX, y: box.minY, width: box.width, height: h))
        }
        return (box, nil)
    }

    /// Stable per-slide keys for ink, so notes follow their slide when slides are added or removed.
    /// Key = hash of the slide text without digits/whitespace (frame numbers like "4/20" shift on every
    /// insert) + "#n" for the n-th slide with that same text (overlay steps, text-less slides).
    public static func inkKeys(pageTexts: [String]) -> [String] {
        var seen: [String: Int] = [:]
        return pageTexts.map { text in
            let core = text.filter { !$0.isNumber && !$0.isWhitespace }
            let hash = SHA256.hash(data: Data(core.utf8)).prefix(8).map { String(format: "%02x", $0) }.joined()
            let n = seen[hash, default: 0]
            seen[hash] = n + 1
            return "\(hash)#\(n)"
        }
    }

    /// PDF rect (origin bottom-left of `pageBox`) -> top-left-origin rect in a canvas drawn at `scale`.
    public static func canvasRect(for pdfRect: CGRect, pageBox: CGRect, scale: CGFloat) -> CGRect {
        CGRect(x: (pdfRect.minX - pageBox.minX) * scale,
               y: (pageBox.maxY - pdfRect.maxY) * scale,
               width: pdfRect.width * scale, height: pdfRect.height * scale)
    }
}
