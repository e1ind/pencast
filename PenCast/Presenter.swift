import AVFoundation
import PDFKit
import PencilKit
import PenCastCore
import SwiftUI

/// `pointer` = no ink: clicks/taps play media and follow links (the default on Mac, where the mouse draws).
/// `laser` = fading red dot + trail on both screens, never saved.
/// `lasso` = select handwriting to move it (PencilKit's lasso: drag the selection; tap it for cut/copy/delete).
enum InkTool: String, CaseIterable {
    case pointer, laser, pen, highlighter, eraser, lasso
    /// Tools that work on the ink canvas (rather than tapping/pointing at the slide).
    var inks: Bool { self == .pen || self == .highlighter || self == .eraser || self == .lasso }
}

/// Pen look, after GoodNotes: a round nib whose width follows pressure (fountain), or an even line (ball).
/// PencilKit's own "fountain pen" is an angled calligraphy nib, so it is not used.
enum PenStyle: String, CaseIterable { case fountain, ball }

/// Eraser: remove whole strokes, or rub out just the part you touch.
enum EraserMode: String, CaseIterable { case stroke, partial }

struct LaserPoint { var p: CGPoint; var t: CFTimeInterval }   // p in page points, y down

let isMac = ProcessInfo.processInfo.isMacCatalystApp

/// Stroke in progress on the iPad, mirrored live to the projector
/// (PencilKit only reports a stroke once the Pencil lifts).
struct LiveStroke { var points: [CGPoint]; var color: UIColor; var width: CGFloat; var marker: Bool }

struct MediaProblem { var text: String; var needsFolderAccess = false }

/// What the projector shows instead of the live slide.
enum ProjectorOverride: Equatable { case none, black, white, frozen(UIImage) }

/// Single source of truth for both screens (spec 5.2 #1).
@MainActor @Observable final class Presenter {
    static let shared = Presenter()

    private(set) var document: PDFDocument?
    private(set) var deckURL: URL?
    private var watchTask: Task<Void, Never>?
    private(set) var pageIndex = 0
    private(set) var media: [MediaItem] = []
    private(set) var videos: [String: VideoController] = [:]          // current page only
    private(set) var animations: [String: AnimationController] = [:]  // current page only
    private(set) var problems: [String: MediaProblem] = [:]
    private(set) var drawings: [Int: PKDrawing] = [:]
    private var undoStack: [Int: [PKDrawing]] = [:]
    private var redoStack: [Int: [PKDrawing]] = [:]
    var liveStroke: LiveStroke?
    private(set) var laser: [[LaserPoint]] = []   // lines on screen; one per press
    private(set) var laserDot: CGPoint?            // shown only while touching (or hovering on Mac)
    private(set) var laserPressing = false
    private(set) var laserLiftedAt = Date.distantPast   // when the touch ended; the fade-out starts here
    var laserTrail = UserDefaults.standard.object(forKey: "laserTrail") as? Bool ?? true {
        didSet { UserDefaults.standard.set(laserTrail, forKey: "laserTrail") }
    }
    // Laser dot diameter and trail width, in page points (a Beamer slide is ~454 pt wide).
    var laserDotSize = storedNumber("laserDotSize", default: 7) {
        didSet { UserDefaults.standard.set(laserDotSize, forKey: "laserDotSize") }
    }
    var laserTrailWidth = storedNumber("laserTrailWidth", default: 4) {
        didSet { UserDefaults.standard.set(laserTrailWidth, forKey: "laserTrailWidth") }
    }
    static let laserDotSizes: ClosedRange<CGFloat> = 3...18
    static let laserTrailWidths: ClosedRange<CGFloat> = 1...12
    /// Bumped whenever ink changes from outside the canvas (page change, undo, redo, clear, load).
    /// The canvas reloads its drawing only then, so a finished stroke costs no comparison or copy.
    private(set) var inkRevision = 0
    var projectorsShown = 0   // live-stroke mirroring only runs while a projector is showing
    var tool = isMac ? InkTool.pointer : .pen

    // Ink settings, remembered between launches. Widths are in PDF points (a 16:9 Beamer page is ~454 pt wide).
    var penStyle = PenStyle(rawValue: UserDefaults.standard.string(forKey: "penStyle") ?? "") ?? .fountain {
        didSet { UserDefaults.standard.set(penStyle.rawValue, forKey: "penStyle") }
    }
    var penWidth = storedNumber("penWidth", default: 1.5) {
        didSet { UserDefaults.standard.set(penWidth, forKey: "penWidth") }
    }
    var highlighterWidth = storedNumber("highlighterWidth", default: 10) {
        didSet { UserDefaults.standard.set(highlighterWidth, forKey: "highlighterWidth") }
    }
    var penColor = Color(storedKey: "penColor") ?? .red {
        didSet { penColor.store(key: "penColor") }
    }
    var highlighterColor = Color(storedKey: "highlighterColor") ?? .yellow {
        didSet { highlighterColor.store(key: "highlighterColor") }
    }
    var eraserMode = EraserMode(rawValue: UserDefaults.standard.string(forKey: "eraserMode") ?? "") ?? .stroke {
        didSet { UserDefaults.standard.set(eraserMode.rawValue, forKey: "eraserMode") }
    }
    var eraserWidth = storedNumber("eraserWidth", default: 8) {
        didSet { UserDefaults.standard.set(eraserWidth, forKey: "eraserWidth") }
    }
    /// Fountain pen pressure (see PressureCurve): thinnest line as a fraction of the size, and how much of
    /// the Pencil's full force gives the thickest line.
    var pressureFine = storedNumber("pressureFine", default: 0.25) {
        didSet { UserDefaults.standard.set(pressureFine, forKey: "pressureFine") }
    }
    var pressureFirm = storedNumber("pressureFirm", default: 0.55) {
        didSet { UserDefaults.standard.set(pressureFirm, forKey: "pressureFirm") }
    }
    var pressureCurve: PressureCurve { PressureCurve(size: penWidth, fine: pressureFine, firm: pressureFirm) }
    /// The fountain pen is drawn by PenCast (FountainRecognizer), not by PencilKit's drawing gesture.
    var fountainActive: Bool { tool == .pen && penStyle == .fountain }
    var partialEraserActive: Bool { tool == .eraser && eraserMode == .partial }
    static let penWidths: ClosedRange<CGFloat> = 0.5...8
    /// Partial eraser diameter in screen points: from a fine tip to clearing a large part of the slide.
    static let eraserWidths: ClosedRange<CGFloat> = 6...360
    static let highlighterWidths: ClosedRange<CGFloat> = 3...30
    var openError: String?
    var externalDisplayConnected = false

    /// Zoomed part of the slide (PDF coordinates), shown on both screens. nil = whole slide.
    private(set) var zoom: CGRect?
    /// Region during a pinch or drag. Shown as a GPU transform of the committed view (no re-rendering of
    /// slide or ink per frame, which made zooming jittery), then committed sharp on release.
    private(set) var liveZoom: CGRect?
    /// Projector mirrors the zoom, or (default) keeps showing the whole slide: zoom is mostly for writing.
    var projectorFollowsZoom = UserDefaults.standard.bool(forKey: "projectorFollowsZoom") {
        didSet { UserDefaults.standard.set(projectorFollowsZoom, forKey: "projectorFollowsZoom") }
    }
    private(set) var projector = ProjectorOverride.none
    /// The video whose tap-to-show controls are visible (iPad screen only).
    private(set) var controlsFor: String?
    var showNotes = true
    /// Beamer `show notes on second screen`: pages hold slide + notes; notes stay on the iPad.
    private(set) var hasNotes = false
    // Talk timer: running since `timerStart`, plus time banked before the last pause.
    private(set) var timerStart: Date?
    private(set) var timerBanked: TimeInterval = 0

    // After the last lift, laser lines stay for `laserHold` s, then fade out over `laserFadeOut` s (GoodNotes-like).
    static let laserHold: CFTimeInterval = 0.5
    static let laserFadeOut: CFTimeInterval = 0.7
    static var laserFade: CFTimeInterval { laserHold + laserFadeOut }

    @ObservationIgnored let renderer = SlideRenderer()
    @ObservationIgnored private var scanner: MediaScanner?
    @ObservationIgnored private var generation = 0   // bumps on slide change; stale async loads are dropped
    @ObservationIgnored private var accessed: [URL] = []
    @ObservationIgnored private var saveTask: Task<Void, Never>?
    @ObservationIgnored private var laserClear: Task<Void, Never>?
    @ObservationIgnored private var controlsHide: Task<Void, Never>?
    @ObservationIgnored private var preparing: Set<String> = []   // videos being unpacked ahead of time
    @ObservationIgnored private var inkKeys: [String] = []          // per page, see SlideGeometry.inkKeys
    @ObservationIgnored private var orphanInk: [String: Data] = [:]  // ink of slides no longer in the deck

    var pageCount: Int { document?.pageCount ?? 0 }
    var pageMedia: [MediaItem] { media.filter { $0.page == pageIndex } }
    /// Ink belongs to a Beamer frame, not a page: every overlay step (\pause, \only, …) of a frame shows
    /// the same notes. Beamer gives all steps of a frame the same page label; ink is stored on the first.
    @ObservationIgnored private var frameStart: [Int] = []
    func inkPage(_ i: Int) -> Int { frameStart.indices.contains(i) ? frameStart[i] : i }
    private var inkIndex: Int { inkPage(pageIndex) }
    var currentDrawing: PKDrawing { drawings[inkIndex] ?? PKDrawing() }
    var canUndo: Bool { !(undoStack[inkIndex] ?? []).isEmpty }
    var canRedo: Bool { !(redoStack[inkIndex] ?? []).isEmpty }
    /// Video cache key (name + page count). Ink is keyed per slide content instead; see `inkKeys`.
    var deckID: String { "\(deckURL?.deletingPathExtension().lastPathComponent ?? "deck")-\(pageCount)p" }

    var deckName: String { deckURL?.deletingPathExtension().lastPathComponent ?? "deck" }

    /// The slide part of a page (the whole page unless it carries Beamer notes).
    func slideBox(_ page: PDFPage) -> CGRect { SlideGeometry.split(page.bounds(for: .cropBox)).slide }
    func notesBox(_ page: PDFPage) -> CGRect? { SlideGeometry.split(page.bounds(for: .cropBox)).notes }

    func timerElapsed(at date: Date) -> TimeInterval { timerBanked + (timerStart.map { date.timeIntervalSince($0) } ?? 0) }

    var pkTool: PKTool {
        switch tool {
        case .pen, .pointer, .laser:
            return PKInkingTool(penStyle == .fountain ? .pen : .monoline, color: UIColor(penColor), width: penWidth)
        case .highlighter: return PKInkingTool(.marker, color: UIColor(highlighterColor), width: highlighterWidth)
        case .eraser: return eraserMode == .stroke ? PKEraserTool(.vector) : PKEraserTool(.bitmap, width: eraserWidth)
        case .lasso: return PKLassoTool()
        }
    }

    // MARK: Deck

    func open(_ url: URL) {
        close()
        if url.startAccessingSecurityScopedResource() { accessed.append(url) }
        accessed += FolderAccess.restore(containing: url)
        guard let doc = PDFDocument(url: url) else {
            openError = "Could not open \(url.lastPathComponent) as a PDF."
            close(); return
        }
        openError = nil
        document = doc
        deckURL = url
        scanner = MediaScanner(url: url)
        media = scanner?.scan() ?? []
        // The slide is rendered without animate's frame widgets; the frame player draws them on top.
        for item in media { frameAnnotations(item).forEach { $0.shouldDisplay = false } }
        hasNotes = doc.page(at: 0).map { notesBox($0) != nil } ?? false
        let labels = (0..<doc.pageCount).map { doc.page(at: $0)?.label }
        frameStart = labels.indices.map { i in
            var j = i
            while j > 0, let l = labels[j], l == labels[j - 1] { j -= 1 }
            return j
        }
        inkKeys = SlideGeometry.inkKeys(pageTexts: (0..<doc.pageCount).map { i in
            guard let page = doc.page(at: i) else { return "" }
            return (hasNotes ? page.selection(for: slideBox(page))?.string : page.string) ?? ""
        })
        (drawings, orphanInk) = InkStore.load(deck: deckName, keys: inkKeys, legacyID: deckID)
        if UserDefaults.standard.bool(forKey: "freshInk") { drawings = [:] }   // test hook: start with no ink
        inkRevision += 1
        timerBanked = 0; timerStart = Date()
        // Drop videos cached from older compiles of this deck.
        let current = videoCacheDir.lastPathComponent
        for old in (try? FileManager.default.contentsOfDirectory(atPath: Self.cacheDir.path)) ?? []
        where old.hasPrefix("\(deckID)-") && old != current {
            try? FileManager.default.removeItem(at: Self.cacheDir.appendingPathComponent(old))
        }
        DeckLibrary.shared.opened(url, pageCount: doc.pageCount)
        UIApplication.shared.isIdleTimerDisabled = true
        go(to: 0)
        watch(url)
    }

    /// Reopens the deck whenever LaTeX rewrites it, so a deck left open while editing stays current.
    /// ponytail: polls once a second; a file-system event source loses the file when LaTeX replaces it.
    private func watch(_ url: URL) {
        watchTask = Task { [weak self] in
            var seen = Self.modified(url), changed = false
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(1))
                let now = Self.modified(url)
                if now != seen { seen = now; changed = true; continue }   // wait until LaTeX stops writing
                if changed, PDFDocument(url: url) != nil { changed = false; self?.reload(url) }
            }
        }
    }

    private func reload(_ url: URL) {
        let page = pageIndex, banked = timerElapsed(at: Date()), running = timerStart != nil
        open(url)
        timerBanked = banked; timerStart = running ? Date() : nil
        go(to: min(page, (document?.pageCount ?? 1) - 1))
    }

    /// Read fresh each time: URL resource values are cached per URL value and would never change.
    static func modified(_ url: URL) -> Date? {
        (try? FileManager.default.attributesOfItem(atPath: url.path))?[.modificationDate] as? Date
    }

    func close() {
        watchTask?.cancel(); watchTask = nil
        stopMedia()
        if document != nil { saveTask?.cancel(); InkStore.save(drawings, keys: inkKeys, orphans: orphanInk, deck: deckName) }
        accessed.forEach { $0.stopAccessingSecurityScopedResource() }
        accessed = []
        document = nil; deckURL = nil; scanner = nil
        media = []; drawings = [:]; undoStack = [:]; redoStack = [:]; pageIndex = 0; frameStart = []
        cancelZoom(); projector = .none; controlsFor = nil; hasNotes = false
        renderer.clear()
        UIApplication.shared.isIdleTimerDisabled = false
    }

    /// Called after the user grants access to the folder holding linked videos.
    func grantFolder(_ url: URL) {
        FolderAccess.add(url)
        if url.startAccessingSecurityScopedResource() { accessed.append(url) }
        go(to: pageIndex)
    }

    // MARK: Navigation

    func go(to i: Int) {
        guard (0..<pageCount).contains(i) else { return }
        let kept = stopMedia(keepingFor: media.filter { $0.page == i })
        if inkPage(i) != inkIndex { inkRevision += 1 }   // same frame: same ink, nothing to reload
        pageIndex = i
        liveStroke = nil
        laser = []
        laserDot = nil
        laserPressing = false
        cancelZoom()
        controlsFor = nil
        startMedia(reusing: kept)
        prepareVideos(onPage: i + 1)
    }
    func next() { go(to: pageIndex + 1) }
    func previous() { go(to: pageIndex - 1) }

    /// Finger tap at a point in PDF page space: media toggles, links jump, edges navigate.
    func tap(at p: CGPoint) { tapAction(at: p)?() }

    /// What a finger tap at a point in PDF page space does: media toggles, links jump, edges navigate.
    /// nil when it does nothing there, which leaves the tap to PencilKit (its Paste / Select menu).
    func tapAction(at p: CGPoint) -> (() -> Void)? {
        guard let doc = document, let page = doc.page(at: pageIndex) else { return nil }
        if let item = pageMedia.first(where: { $0.rect.contains(p) }) {
            if videos[item.id] != nil { return { self.toggleControls(item.id) } }
            if let a = animations[item.id] { return { a.toggle() } }
        }
        if controlsFor != nil { return { self.controlsFor = nil } }   // tap elsewhere just dismisses the controls
        if let link = page.annotation(at: p), zoom == nil {   // while zoomed, a stray finger never changes slide
            if let named = link.action as? PDFActionNamed {
                switch named.name {
                case .nextPage: return { self.next() }
                case .previousPage: return { self.previous() }
                case .firstPage: return { self.go(to: 0) }
                case .lastPage: return { self.go(to: self.pageCount - 1) }
                default: break
                }
            }
            if let dest = (link.action as? PDFActionGoTo)?.destination ?? link.destination,
               let target = dest.page {
                let i = doc.index(for: target)
                return { self.go(to: i) }
            }
        }
        guard zoom == nil else { return nil }   // while zoomed, edge taps would jump away by accident
        let box = slideBox(page)
        let x = (p.x - box.minX) / box.width
        return x < 0.25 ? { self.previous() } : x > 0.75 ? { self.next() } : nil
    }

    // MARK: Video controls (tap a video to show them; they hide after 3 s untouched)

    func toggleControls(_ id: String) {
        withAnimation(.easeInOut(duration: 0.25)) { controlsFor = controlsFor == id ? nil : id }
        keepControls()
    }

    func keepControls() {
        controlsHide?.cancel()
        guard controlsFor != nil else { return }
        controlsHide = Task {
            try? await Task.sleep(for: .seconds(3))
            if !Task.isCancelled { withAnimation(.easeInOut(duration: 0.4)) { controlsFor = nil } }
        }
    }

    // MARK: Zoom (both screens)
    //
    // `zoom` is the settled region, rendered sharp. Pinches, drags and animations move `liveZoom`, which is
    // shown as a GPU transform of the settled view over a whole-slide underlay, so nothing re-renders per
    // frame and zooming out never uncovers black. Like a scroll view, a gesture may overshoot the slide's
    // edges and the zoom limits with rubber-band resistance; on release the view springs back, or glides
    // on after a flick, and then settles.

    static let maxZoom: CGFloat = 8
    /// Unbanded region the gesture is steering; `liveZoom` is this with rubber-band resistance applied.
    private var rawZoom: CGRect?
    private var zoomAnimator: ZoomAnimator?
    /// Whole-slide ink, drawn on the underlay while zooming, and over the slide for the moment the ink
    /// canvas takes to redraw at a new zoom (it used to show its old picture misplaced: a visible jump).
    private(set) var inkSnapshot: UIImage?
    /// Set from a settled zoom change until the iPad's ink canvas has redrawn for it.
    private(set) var inkSettling = false
    private var settleTimeout: Task<Void, Never>?

    private var fullSlide: CGRect? { document?.page(at: pageIndex).map(slideBox) }

    /// A pinch or drag starts: take over from any running animation, from exactly where it is.
    func beginLiveZoom() {
        zoomAnimator?.stop(); zoomAnimator = nil
        guard let full = fullSlide else { return }
        if liveZoom == nil { liveZoom = zoom ?? full; snapshotInk() }
        rawZoom = liveZoom
    }

    /// Pinch: zoom to `width` (PDF points) with the slide point `anchor` shown at fraction `f` of the view
    /// (0…1 from the top-left). Computed from the pinch's start every time, so the point that was under
    /// your fingers stays exactly under them; no step-by-step drift.
    func zoom(toWidth width: CGFloat, keeping anchor: CGPoint, at f: CGPoint) {
        guard let full = fullSlide else { return }
        if rawZoom == nil { beginLiveZoom() }
        let minW = full.width / Self.maxZoom
        var w = width
        if w > full.width { w = full.width + Self.rubber(w - full.width, full.width) }
        else if w < minW { w = minW - Self.rubber(minW - w, minW) }
        let h = w * full.height / full.width
        rawZoom = CGRect(x: anchor.x - f.x * w, y: anchor.y + f.y * h - h, width: w, height: h)
        liveZoom = rubberBanded(rawZoom!, in: full)
    }

    /// Move the view by a distance in PDF points.
    func pan(dx: CGFloat, dy: CGFloat) {
        guard let full = fullSlide else { return }
        if rawZoom == nil { beginLiveZoom() }
        guard let raw = rawZoom else { return }
        rawZoom = raw.offsetBy(dx: dx, dy: dy)
        liveZoom = rubberBanded(rawZoom!, in: full)
    }

    /// Gesture over. `velocity` (PDF points/s) is a flick's speed: the view glides on as far as a scroll
    /// view would, easing out from that speed; otherwise it springs back inside the limits.
    func endLiveZoom(velocity v: CGPoint = .zero) {
        rawZoom = nil
        guard let live = liveZoom, let full = fullSlide else { return }
        let target = settledRegion(live.offsetBy(dx: v.x * 0.5, dy: v.y * 0.5), in: full)
        let speed = hypot(v.x, v.y), dist = hypot(target.midX - live.midX, target.midY - live.midY)
        animateZoom(to: target, duration: speed > 1 && dist > 0.5 ? min(max(3 * dist / speed, 0.2), 0.9) : 0.3)
    }

    func zoomStep(_ factor: CGFloat) {
        guard let full = fullSlide, factor > 0 else { return }
        let v = liveZoom ?? zoom ?? full
        let w = v.width / factor, h = w * full.height / full.width
        animateZoom(to: settledRegion(CGRect(x: v.midX - w / 2, y: v.midY - h / 2, width: w, height: h), in: full))
    }

    func resetZoom() { if let full = fullSlide { animateZoom(to: full) } }

    private func animateZoom(to target: CGRect, duration: Double = 0.3) {
        zoomAnimator?.stop(); zoomAnimator = nil
        guard let full = fullSlide else { return }
        if liveZoom == nil { liveZoom = zoom ?? full; snapshotInk() }
        guard let from = liveZoom else { return }
        if abs(from.minX - target.minX) + abs(from.minY - target.minY) + abs(from.width - target.width) < 0.05 {
            settleZoom(target, full: full); return
        }
        // Ease out; width moves geometrically so zooming feels even at every magnification.
        zoomAnimator = ZoomAnimator(duration: duration) { [weak self] t in
            guard let self else { return }
            let e = 1 - pow(1 - t, 3)
            let w = exp(log(from.width) + (log(target.width) - log(from.width)) * e)
            let cx = from.midX + (target.midX - from.midX) * e, cy = from.midY + (target.midY - from.midY) * e
            let h = w * full.height / full.width
            liveZoom = CGRect(x: cx - w / 2, y: cy - h / 2, width: w, height: h)
            if t >= 1 { zoomAnimator = nil; settleZoom(target, full: full) }
        }
    }

    private func settleZoom(_ r: CGRect, full: CGRect) {
        let new: CGRect? = r.width >= full.width * 0.98 ? nil : r
        if new != zoom {
            inkSettling = inkSnapshot != nil && !currentDrawing.strokes.isEmpty
            settleTimeout?.cancel()
            settleTimeout = Task { try? await Task.sleep(for: .milliseconds(500)); inkSettled() }
        }
        zoom = new
        liveZoom = nil
    }

    /// The ink canvas has redrawn for the settled zoom.
    func inkSettled() { if inkSettling { inkSettling = false } }

    private func cancelZoom() {
        zoomAnimator?.stop(); zoomAnimator = nil
        rawZoom = nil; zoom = nil; liveZoom = nil; inkSnapshot = nil; inkSettling = false
    }

    private func snapshotInk() {
        guard let full = fullSlide else { return }
        let ink = currentDrawing, page = pageIndex
        guard !ink.strokes.isEmpty else { inkSnapshot = nil; return }
        Task.detached(priority: .userInitiated) {
            // Rendered off the main thread so a pinch never waits for it (it is ready long before release).
            let img = lightInk { ink.image(from: CGRect(origin: .zero, size: full.size), scale: 2400 / full.width) }
            await MainActor.run { if self.pageIndex == page { self.inkSnapshot = img } }
        }
    }

    /// Inside the zoom limits and the slide.
    private func settledRegion(_ r: CGRect, in full: CGRect) -> CGRect {
        let w = min(max(r.width, full.width / Self.maxZoom), full.width)
        if w >= full.width * 0.98 { return full }
        let h = w * full.height / full.width
        return clamped(CGRect(x: r.midX - w / 2, y: r.midY - h / 2, width: w, height: h), in: full)
    }

    private func clamped(_ r: CGRect, in full: CGRect) -> CGRect {
        CGRect(x: min(max(r.minX, full.minX), full.maxX - r.width),
               y: min(max(r.minY, full.minY), full.maxY - r.height), width: r.width, height: r.height)
    }

    /// Past the slide's edges the view follows the fingers with resistance (a region wider than the slide
    /// stays centred on it).
    private func rubberBanded(_ r: CGRect, in full: CGRect) -> CGRect {
        func band(_ x: CGFloat, _ lo: CGFloat, _ hi: CGFloat, _ dim: CGFloat) -> CGFloat {
            x < lo ? lo - Self.rubber(lo - x, dim) : x > hi ? hi + Self.rubber(x - hi, dim) : x
        }
        let x = r.width <= full.width ? band(r.minX, full.minX, full.maxX - r.width, r.width) : full.midX - r.width / 2
        let y = r.height <= full.height ? band(r.minY, full.minY, full.maxY - r.height, r.height) : full.midY - r.height / 2
        return CGRect(x: x, y: y, width: r.width, height: r.height)
    }

    /// UIScrollView's rubber band: an overshoot of `d` shows as less, approaching `dim` but never reaching it.
    static func rubber(_ d: CGFloat, _ dim: CGFloat) -> CGFloat { (1 - 1 / (d * 0.55 / dim + 1)) * dim }

    // MARK: Projector overrides and timer

    func setProjector(_ o: ProjectorOverride) { projector = projector == o ? .none : o }

    /// Freeze: the projector keeps showing this slide (with its ink and zoom) while you navigate.
    func toggleFreeze() {
        if case .frozen = projector { projector = .none; return }
        guard let doc = document, let page = doc.page(at: pageIndex) else { return }
        let slide = slideBox(page), view = (projectorFollowsZoom ? zoom : nil) ?? slide
        let ink = currentDrawing, index = pageIndex
        Task {
            guard let base = await renderer.render(doc, index, region: view, width: 2560, cacheable: false) else { return }
            let inkRect = CGRect(x: view.minX - slide.minX, y: slide.maxY - view.maxY, width: view.width, height: view.height)
            let img = UIGraphicsImageRenderer(size: base.size).image { _ in
                base.draw(at: .zero)
                lightInk { ink.image(from: inkRect, scale: base.size.width / view.width) }
                    .draw(in: CGRect(origin: .zero, size: base.size))
            }
            projector = .frozen(img)
        }
    }

    func toggleTimer() {
        if let start = timerStart { timerBanked += Date().timeIntervalSince(start); timerStart = nil }
        else { timerStart = Date() }
    }
    func resetTimer() { timerBanked = 0; timerStart = timerStart == nil ? nil : Date() }

    // MARK: Export (F12)

    /// Flattened copy of the deck with ink, one page per slide (notes removed), for sharing lecture notes.
    /// Videos appear as their poster; animate slides show their first frame.
    /// `notesOnly`: one page per Beamer frame, its last overlay step (everything uncovered) with the
    /// frame's ink, i.e. lecture notes rather than the slide show.
    func exportAnnotated(notesOnly: Bool) async -> URL? {
        guard let doc = document else { return nil }
        var pages: [SlideRenderer.ExportPage] = []
        let lastSteps = (0..<doc.pageCount).filter { i in i + 1 == doc.pageCount || inkPage(i + 1) != inkPage(i) }
        for i in notesOnly ? lastSteps : Array(0..<doc.pageCount) {
            guard let page = doc.page(at: i) else { continue }
            let box = slideBox(page)
            let ink = drawings[inkPage(i)].flatMap { d in
                d.strokes.isEmpty ? nil : lightInk { d.image(from: CGRect(origin: .zero, size: box.size), scale: 4) }
            }
            let firstFrames = media.filter { $0.page == i }.compactMap { frameAnnotations($0).first }
            pages.append(.init(page: page, box: box, ink: ink, overlays: firstFrames))
        }
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("\(deckName) (\(notesOnly ? "notes" : "annotated")).pdf")
        return await renderer.export(pages, to: url) ? url : nil
    }

    // MARK: Media

    /// Stops this page's media, except videos that continue on the next page: Beamer overlays
    /// (\pause, \only) repeat the same video at the same place on consecutive pages, and it should keep
    /// playing rather than restart at every step. Returns those, keyed by `continuity(_:)`.
    @discardableResult
    private func stopMedia(keepingFor next: [MediaItem] = []) -> [String: VideoController] {
        generation += 1
        let continuing = Set(next.map(\.continuity))
        var kept: [String: VideoController] = [:]
        for item in pageMedia {
            guard let v = videos[item.id] else { continue }
            if continuing.contains(item.continuity) { kept[item.continuity] = v } else { v.stop() }
        }
        animations.values.forEach { $0.pause() }
        videos = [:]; animations = [:]; problems = [:]
        return kept
    }


    private func startMedia(reusing kept: [String: VideoController] = [:]) {
        for item in pageMedia {
            if let v = kept[item.continuity] { videos[item.id] = v; continue }
            switch item.kind {
            case .embeddedVideo(let name):
                // Lazy extraction (spec 5.2 #6): the first slide showing a video writes it; every later slide
                // using the same embedded file (same name) reuses it. Cleared when the deck file changes.
                let url = videoCacheDir.appendingPathComponent(name)
                let scanner = scanner
                loadVideo(item, url: url) {
                    if !FileManager.default.fileExists(atPath: url.path) { try scanner?.extractEmbeddedVideo(item, to: url) }
                }
            case .linkedVideo(let path):
                let url = path.contains("://") ? URL(string: path)
                    : deckURL?.deletingLastPathComponent().appendingPathComponent(path).standardizedFileURL
                guard let url else { problems[item.id] = MediaProblem(text: "Bad video path: \(path)"); continue }
                if url.isFileURL, !FileManager.default.isReadableFile(atPath: url.path) {
                    problems[item.id] = MediaProblem(text: "Linked video not accessible:\n\(path)", needsFolderAccess: true)
                } else {
                    loadVideo(item, url: url) {}
                }
            case .animation(_, let count, let fps):
                guard let page = document?.page(at: item.page) else { continue }
                let annots = frameAnnotations(item)
                guard annots.count == count else {
                    problems[item.id] = MediaProblem(text: "animate frames not found"); continue
                }
                // Frames render on demand (~3 ms each), so a 200-frame animation holds one image, not ~240 MB.
                let scale = Self.frameRenderWidth / page.bounds(for: .cropBox).width
                let renderer = renderer
                animations[item.id] = AnimationController(frameCount: count, fps: fps, loop: item.loop,
                                                          autoplay: item.autoplay) { i, done in
                    renderer.frame(page: page, annotation: annots[i], rect: item.rect, scale: scale, done: done)
                }
            case .unsupported(let reason):
                problems[item.id] = MediaProblem(text: reason)
            }
        }
    }

    /// Unpack the next slide's embedded videos ahead of time, so they start the moment you get there.
    private func prepareVideos(onPage p: Int) {
        guard let scanner else { return }
        for item in media where item.page == p {
            guard case .embeddedVideo(let name) = item.kind else { continue }
            let url = videoCacheDir.appendingPathComponent(name)
            guard !FileManager.default.fileExists(atPath: url.path), preparing.insert(name).inserted else { continue }
            Task.detached(priority: .utility) {
                try? scanner.extractEmbeddedVideo(item, to: url)
                await MainActor.run { _ = self.preparing.remove(name) }
            }
        }
    }

    private func loadVideo(_ item: MediaItem, url: URL, prepare: @escaping () throws -> Void) {
        let gen = generation
        Task { @MainActor in
            do {
                try await Task.detached(priority: .userInitiated) { try prepare() }.value
                guard try await AVURLAsset(url: url).load(.isPlayable) else {
                    throw CocoaError(.fileReadCorruptFile, userInfo: [NSLocalizedDescriptionKey: "unsupported format or codec"])
                }
                guard gen == generation else { return }
                videos[item.id] = VideoController(url: url, autoplay: item.autoplay, loop: item.loop)
            } catch {
                if gen == generation { problems[item.id] = MediaProblem(text: "Cannot play video: \(error.localizedDescription)") }
            }
        }
    }

    /// animate frame widgets "<id>.<k>" for an animation item, in frame order.
    private func frameAnnotations(_ item: MediaItem) -> [PDFAnnotation] {
        guard case .animation(let id, let n, _) = item.kind, let page = document?.page(at: item.page) else { return [] }
        let byName = Dictionary(page.annotations.compactMap { a in a.fieldName.map { ($0, a) } }, uniquingKeysWith: { a, _ in a })
        return (0..<n).compactMap { byName["\(id).\($0)"] }
    }

    /// Animation frames render as if the whole slide were this many pixels wide (sharp on 4K too).
    static let frameRenderWidth: CGFloat = 2752

    /// Per deck version: a recompiled PDF (new modification date) never plays stale cached videos.
    private var videoCacheDir: URL {
        let modified = deckURL.flatMap(Self.modified)
        return Self.cacheDir.appendingPathComponent("\(deckID)-\(Int(modified?.timeIntervalSince1970 ?? 0))")
    }

    static let cacheDir = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("Media", isDirectory: true)

    // MARK: Laser (F11)

    /// Laser moved. `drawing` = pressed (Pencil/finger down, mouse button held); `false` is the Mac pointer
    /// hovering, which only shows the dot. Each press starts a new line; earlier lines that are still on
    /// screen come back to full strength and stay, so several lines can be marked at once.
    func laserMoved(to p: CGPoint, drawing: Bool = true) { laserMoved(along: [p], drawing: drawing) }

    /// All points since the last call (the Pencil's full sample rate), so the line follows like ink.
    func laserMoved(along points: [CGPoint], drawing: Bool = true) {
        guard let p = points.last else { return }
        laserDot = p
        guard drawing else { return }
        if !laserPressing {
            laserPressing = true
            laserClear?.cancel()
            if laserTrail { laser.append([]) }
        }
        guard laserTrail, !laser.isEmpty else { return }
        let now = CACurrentMediaTime()
        laser[laser.count - 1] += points.map { LaserPoint(p: $0, t: now) }
        if laser.reduce(0, { $0 + $1.count }) > 6000 { laser.removeFirst() }   // keep it bounded
    }

    /// Lifted (or the Mac pointer left the slide): the dot goes at once; lines fade out together,
    /// unless another press starts before they are gone.
    func laserEnded(drawing: Bool = true) {
        laserDot = nil
        guard drawing, laserPressing else { return }
        laserPressing = false
        laserLiftedAt = Date()
        laserClear?.cancel()
        laserClear = Task {
            try? await Task.sleep(for: .seconds(Self.laserFade))
            if !Task.isCancelled { laser = [] }
        }
    }

    // MARK: Ink (per-slide undo/redo, spec 7.2)

    /// Called by the canvas after each stroke/erase (the canvas already shows `d`).
    func commit(_ d: PKDrawing) {
        // PencilKit may report a drawing we set ourselves (after a stroke, undo or redo) a moment later.
        // Recording it again would make the next Undo do nothing and wipe Redo.
        guard d != currentDrawing else { return }
        undoStack[inkIndex, default: []].append(currentDrawing)
        redoStack[inkIndex] = []
        controlsFor = nil
        drawings[inkIndex] = d
        liveStroke = nil
        saveInkSoon()
    }
    func undo() {
        guard let prev = undoStack[inkIndex]?.popLast() else { return }
        redoStack[inkIndex, default: []].append(currentDrawing)
        drawings[inkIndex] = prev
        inkRevision += 1
        saveInkSoon()
    }
    func redo() {
        guard let next = redoStack[inkIndex]?.popLast() else { return }
        undoStack[inkIndex, default: []].append(currentDrawing)
        drawings[inkIndex] = next
        inkRevision += 1
        saveInkSoon()
    }
    func clearSlide() { commit(PKDrawing()); inkRevision += 1 }

    private func saveInkSoon() {
        saveTask?.cancel()
        let snapshot = drawings, keys = inkKeys, orphans = orphanInk, name = deckName
        saveTask = Task {
            // Wait for a pause in writing, then encode + write off the main thread.
            try? await Task.sleep(for: .seconds(2))
            guard !Task.isCancelled else { return }
            await Task.detached(priority: .utility) { InkStore.save(snapshot, keys: keys, orphans: orphans, deck: name) }.value
        }
    }
}

// MARK: - Players

/// One AVPlayer per video, shared by the iPad and projector layers so they cannot drift (spec 5.2 #4).
@Observable final class VideoController {
    let player: AVPlayer
    var isPlaying = false
    var time: Double = 0
    var duration: Double = 0
    var loops: Bool
    var muted = false { didSet { player.isMuted = muted } }
    @ObservationIgnored private var timeObserver: Any?
    @ObservationIgnored private var endObserver: NSObjectProtocol?

    init(url: URL, autoplay: Bool, loop: Bool) {
        player = AVPlayer(url: url)
        loops = loop
        player.actionAtItemEnd = .pause
        timeObserver = player.addPeriodicTimeObserver(forInterval: CMTime(value: 1, timescale: 10), queue: .main) { [weak self] t in
            guard let self else { return }
            time = t.seconds
            isPlaying = player.rate != 0
            if let d = player.currentItem?.duration.seconds, d.isFinite { duration = d }
        }
        endObserver = NotificationCenter.default.addObserver(forName: .AVPlayerItemDidPlayToEndTime,
                                                             object: player.currentItem, queue: .main) { [weak self] _ in
            guard let self, loops else { return }
            player.seek(to: .zero)
            player.play()
        }
        if autoplay { player.play() }
    }

    func toggle() {
        if player.rate != 0 { player.pause() }
        else {
            if duration > 0, time >= duration - 0.05 { player.seek(to: .zero) }
            player.play()
        }
        isPlaying = player.rate != 0
    }

    func seek(to seconds: Double) {
        player.seek(to: CMTime(seconds: seconds, preferredTimescale: 600), toleranceBefore: .zero, toleranceAfter: .zero)
    }

    func stop() {
        player.pause()
        if let timeObserver { player.removeTimeObserver(timeObserver) }
        if let endObserver { NotificationCenter.default.removeObserver(endObserver) }
        timeObserver = nil; endObserver = nil
    }
}

/// animate frame player. Frame index comes from elapsed time, so timer jitter never changes the rate.
@Observable final class AnimationController {
    let frameCount: Int
    let fps: Double
    let loops: Bool
    private(set) var index = 0
    private(set) var image: CGImage?
    var isPlaying: Bool { timer != nil }
    private var timer: Timer?
    @ObservationIgnored private var startTime: CFTimeInterval = 0
    @ObservationIgnored private var startIndex = 0
    @ObservationIgnored private var rendering = false
    @ObservationIgnored private let render: (Int, @escaping (CGImage?) -> Void) -> Void

    /// `render(i, done)` draws frame i off the main thread and calls `done` on the main thread.
    init(frameCount: Int, fps: Double, loop: Bool, autoplay: Bool,
         render: @escaping (Int, @escaping (CGImage?) -> Void) -> Void) {
        self.frameCount = frameCount; self.fps = max(fps, 0.1); loops = loop; self.render = render
        show(0)
        if autoplay { play() }
    }

    func toggle() { isPlaying ? pause() : play() }

    func play() {
        if !loops, index == frameCount - 1 { show(0) }
        startTime = CACurrentMediaTime(); startIndex = index
        let t = Timer(timeInterval: 1 / fps / 2, repeats: true) { [weak self] _ in self?.tick() }
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    func pause() { timer?.invalidate(); timer = nil }

    private func tick() {
        let n = startIndex + Int((CACurrentMediaTime() - startTime) * fps)
        if loops { show(n % frameCount) }
        else if n >= frameCount - 1 { show(frameCount - 1); pause() }
        else { show(n) }
    }

    /// One render in flight at a time; if the renderer falls behind, intermediate frames are skipped
    /// but timing stays correct.
    private func show(_ i: Int) {
        index = i
        guard !rendering else { return }
        rendering = true
        render(i) { [weak self] img in
            guard let self else { return }
            rendering = false
            if let img { image = img }
            if index != i { show(index) }
        }
    }
}

// MARK: - Rendering

/// Renders slides off the main thread. PDFKit is not thread-safe, so every draw
/// (and every annotation visibility toggle) runs on one serial queue.
final class SlideRenderer {
    struct ExportPage { let page: PDFPage; let box: CGRect; let ink: UIImage?; let overlays: [PDFAnnotation] }

    private let queue = DispatchQueue(label: "pencast.render", qos: .userInitiated)
    private let cache = NSCache<NSString, UIImage>()
    private let lock = NSLock()
    private var latest: [Int: Int] = [:]   // screen width -> newest uncached request

    init() { cache.totalCostLimit = 200 << 20 }   // ~12 full-screen slides

    /// Keyed by document too: a reloaded deck never shows a stale page, even from a prefetch still in flight.
    private static func key(_ doc: PDFDocument, _ index: Int, _ region: CGRect, _ width: Int) -> NSString {
        "\(ObjectIdentifier(doc).hashValue):\(index)@\(width)@\(region)" as NSString
    }

    func cached(_ doc: PDFDocument, _ index: Int, region: CGRect, width: Int) -> UIImage? { cache.object(forKey: Self.key(doc, index, region, width)) }

    /// `region` (PDF coordinates) of page `index`, `width` pixels wide. Zoomed views pass
    /// `cacheable: false`: not cached, and a request superseded by a newer one for the same screen
    /// width is skipped, so pinching never builds a backlog of renders.
    func render(_ doc: PDFDocument, _ index: Int, region: CGRect, width: Int, cacheable: Bool = true) async -> UIImage? {
        if cacheable, let img = cached(doc, index, region: region, width: width) { return img }
        var token = 0
        if !cacheable { lock.withLock { token = (latest[width] ?? 0) + 1; latest[width] = token } }
        return await withCheckedContinuation { c in
            queue.async {
                if !cacheable, self.lock.withLock({ self.latest[width] }) != token { c.resume(returning: nil); return }
                c.resume(returning: self.renderNow(doc, index, region: region, width: width, cacheable: cacheable))
            }
        }
    }

    /// Pre-render neighbours so the next slide change is a cache hit (spec: < 200 ms).
    func prefetch(_ doc: PDFDocument, around index: Int, width: Int, region: @escaping (PDFPage) -> CGRect) {
        for i in [index + 1, index - 1] where (0..<doc.pageCount).contains(i) {
            queue.async {
                guard let page = doc.page(at: i) else { return }
                _ = self.renderNow(doc, i, region: region(page), width: width, cacheable: true)
            }
        }
    }

    /// One animate frame as a transparent image, drawn from the frame widget's appearance.
    /// (page.draw skips these widgets even with shouldDisplay = true; annotation.draw does not.)
    func frame(page: PDFPage, annotation a: PDFAnnotation, rect: CGRect, scale: CGFloat,
               done: @escaping (CGImage?) -> Void) {
        queue.async {
            let box = page.bounds(for: .cropBox)
            a.shouldDisplay = true
            let img = Self.draw(region: rect.offsetBy(dx: -box.minX, dy: -box.minY), scale: scale, opaque: false) {
                a.draw(with: .cropBox, in: $0)
            }.cgImage
            a.shouldDisplay = false
            DispatchQueue.main.async { done(img) }
        }
    }

    /// Writes a flattened PDF: each page's slide part (vector), first animate frames, then the ink image.
    func export(_ pages: [ExportPage], to url: URL) async -> Bool {
        await withCheckedContinuation { c in
            queue.async {
                do {
                    try UIGraphicsPDFRenderer(bounds: CGRect(x: 0, y: 0, width: 1, height: 1)).writePDF(to: url) { ctx in
                        for p in pages {
                            let size = p.box.size, crop = p.page.bounds(for: .cropBox)
                            ctx.beginPage(withBounds: CGRect(origin: .zero, size: size), pageInfo: [:])
                            let cg = ctx.cgContext
                            cg.saveGState()
                            cg.translateBy(x: 0, y: size.height)
                            cg.scaleBy(x: 1, y: -1)
                            cg.translateBy(x: -(p.box.minX - crop.minX), y: -(p.box.minY - crop.minY))
                            p.page.draw(with: .cropBox, to: cg)
                            for a in p.overlays { a.shouldDisplay = true; a.draw(with: .cropBox, in: cg); a.shouldDisplay = false }
                            cg.restoreGState()
                            p.ink?.draw(in: CGRect(origin: .zero, size: size))
                        }
                    }
                    c.resume(returning: true)
                } catch {
                    print("export failed: \(error)")
                    c.resume(returning: false)
                }
            }
        }
    }

    func clear() { cache.removeAllObjects() }

    private func renderNow(_ doc: PDFDocument, _ index: Int, region: CGRect, width: Int, cacheable: Bool) -> UIImage? {
        if cacheable, let img = cached(doc, index, region: region, width: width) { return img }
        guard let page = doc.page(at: index), region.width > 0 else { return nil }
        let crop = page.bounds(for: .cropBox)
        let img = Self.draw(region: region.offsetBy(dx: -crop.minX, dy: -crop.minY),
                            scale: CGFloat(width) / region.width, opaque: true) { page.draw(with: .cropBox, to: $0) }
        if cacheable {
            cache.setObject(img, forKey: Self.key(doc, index, region, width), cost: Int(img.size.width * img.size.height * 4))
        }
        return img
    }

    /// `region` is in page space relative to the crop box origin (y up).
    static func draw(region: CGRect, scale: CGFloat, opaque: Bool, content: (CGContext) -> Void) -> UIImage {
        let size = CGSize(width: (region.width * scale).rounded(), height: (region.height * scale).rounded())
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        format.opaque = opaque
        return UIGraphicsImageRenderer(size: size, format: format).image { ctx in
            if opaque {
                UIColor.white.setFill()
                ctx.fill(CGRect(origin: .zero, size: size))
            }
            let cg = ctx.cgContext
            cg.translateBy(x: 0, y: size.height)
            cg.scaleBy(x: scale, y: -scale)
            cg.translateBy(x: -region.minX, y: -region.minY)
            content(cg)
        }
    }
}

/// PencilKit adapts ink to dark mode (black ink drawn white). Slides are light, so ink is always light-mode.
/// A number setting. `double(forKey:)` also reads values given as text (a launch argument); a cast to
/// CGFloat silently fell back to the default for those.
func storedNumber(_ key: String, default value: CGFloat) -> CGFloat {
    UserDefaults.standard.object(forKey: key) == nil ? value : CGFloat(UserDefaults.standard.double(forKey: key))
}

func lightInk<T>(_ body: () -> T) -> T {
    var out: T?
    UITraitCollection(userInterfaceStyle: .light).performAsCurrent { out = body() }
    return out!
}

// MARK: - Persistence

/// Ink per deck: Application Support/Ink/<deck name>.ink.plist, slide key -> PKDrawing data.
/// Keys come from slide content (SlideGeometry.inkKeys), so notes follow their slide when slides are
/// added or removed. Ink of slides that disappeared is kept, in case they come back.
enum InkStore {
    static let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("Ink", isDirectory: true)

    static func load(deck: String, keys: [String], legacyID: String) -> ([Int: PKDrawing], orphans: [String: Data]) {
        var raw = read("\(deck).ink.plist")
        if raw.isEmpty {   // earlier builds stored ink by page index under "<name>-<pages>p.plist"
            for (k, v) in read("\(legacyID).plist") { if let i = Int(k), keys.indices.contains(i) { raw[keys[i]] = v } }
        }
        var drawings: [Int: PKDrawing] = [:]
        for (i, k) in keys.enumerated() {
            if let d = raw.removeValue(forKey: k).flatMap({ try? PKDrawing(data: $0) }) { drawings[i] = d }
        }
        return (drawings, raw)
    }

    static func save(_ drawings: [Int: PKDrawing], keys: [String], orphans: [String: Data], deck: String) {
        var raw = orphans
        for (i, d) in drawings where keys.indices.contains(i) && !d.strokes.isEmpty { raw[keys[i]] = d.dataRepresentation() }
        do {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            try PropertyListEncoder().encode(raw).write(to: dir.appendingPathComponent("\(deck).ink.plist"), options: .atomic)
        } catch {
            print("ink save failed: \(error)")
        }
    }

    private static func read(_ name: String) -> [String: Data] {
        guard let data = try? Data(contentsOf: dir.appendingPathComponent(name)) else { return [:] }
        return (try? PropertyListDecoder().decode([String: Data].self, from: data)) ?? [:]
    }
}

extension Color {
    init?(storedKey key: String) {
        guard let data = UserDefaults.standard.data(forKey: key),
              let ui = try? NSKeyedUnarchiver.unarchivedObject(ofClass: UIColor.self, from: data) else { return nil }
        self.init(uiColor: ui)
    }
    func store(key: String) {
        let data = try? NSKeyedArchiver.archivedData(withRootObject: UIColor(self), requiringSecureCoding: true)
        UserDefaults.standard.set(data, forKey: key)
    }
}

extension MediaItem {
    /// Same source at the same spot = the same video continuing across Beamer overlay pages.
    /// Also the view identity, so a continuing video's player view is never torn down.
    var continuity: String { "\(kind)|\(rect)" }
}

/// Steps a zoom animation once per screen refresh (120 Hz on ProMotion iPads).
@MainActor final class ZoomAnimator: NSObject {
    private var link: CADisplayLink?
    private let start = CACurrentMediaTime(), duration: Double, step: (Double) -> Void

    init(duration: Double, step: @escaping (Double) -> Void) {
        self.duration = duration; self.step = step
        super.init()
        let link = CADisplayLink(target: self, selector: #selector(tick))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 120, preferred: 120)
        link.add(to: .main, forMode: .common)
        self.link = link
    }

    @objc private func tick() {
        let t = min((CACurrentMediaTime() - start) / duration, 1)
        if t >= 1 { stop() }
        step(t)
    }

    func stop() { link?.invalidate(); link = nil }
}
