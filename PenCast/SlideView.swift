import AVFoundation
import PDFKit
import PencilKit
import PenCastCore
import SwiftUI
import UniformTypeIdentifiers

/// Slide + media + ink in one fixed logical canvas, letterboxed to the screen.
/// Both the iPad (interactive) and the projector (passive) use this view. When zoomed, both show
/// the zoomed part, re-rendered sharp rather than enlarged.
struct SlideView: View {
    let presenter: Presenter
    let interactive: Bool

    var body: some View {
        GeometryReader { geo in
            if let doc = presenter.document, let page = doc.page(at: presenter.pageIndex) {
                let slide = presenter.slideBox(page)
                let zoom = interactive || presenter.projectorFollowsZoom ? presenter.zoom : nil
                let view = zoom ?? slide
                let scale = SlideGeometry.fitScale(page: view.size, in: geo.size)
                // Ink, laser and live strokes live in slide content coordinates (points from the slide's
                // top-left, y down); `origin` is where the visible part starts in them.
                let origin = CGPoint(x: view.minX - slide.minX, y: slide.maxY - view.maxY)
                // During a pinch/drag: map the committed view onto the live region with a GPU transform.
                let live = interactive || presenter.projectorFollowsZoom ? presenter.liveZoom : nil
                let liveTransform = live.map { l -> CGAffineTransform in
                    let k = view.width / l.width
                    return CGAffineTransform(scaleX: k, y: k)
                        .translatedBy(x: -(l.minX - view.minX) * scale, y: -(view.maxY - l.maxY) * scale)
                } ?? .identity
                // Z-order (spec 5.2 #3): slide, media, ink.
                let stack = ZStack(alignment: .topLeading) {
                    SlideImage(doc: doc, index: presenter.pageIndex, renderer: presenter.renderer,
                               zoom: zoom, notes: false, scale: scale)
                    // Keyed by source + position, not page: a video continuing onto the next overlay page
                    // keeps its player view, so it plays on without a flicker.
                    ForEach(presenter.pageMedia, id: \.continuity) { item in
                        let r = SlideGeometry.canvasRect(for: item.rect, pageBox: view, scale: scale)
                        MediaView(item: item, presenter: presenter, interactive: interactive)
                            .frame(width: r.width, height: r.height)
                            .offset(x: r.minX, y: r.minY)
                    }
                    // While the ink canvas redraws for a new zoom, the same ink from a snapshot, in place.
                    if interactive, presenter.inkSettling, let ink = presenter.inkSnapshot {
                        let r = SlideGeometry.canvasRect(for: slide, pageBox: view, scale: scale)
                        Image(uiImage: ink).resizable().frame(width: r.width, height: r.height).offset(x: r.minX, y: r.minY)
                            .allowsHitTesting(false)
                    }
                    InkCanvas(presenter: presenter, interactive: interactive, pageBox: slide, scale: scale, origin: origin,
                              controlsFrame: controlsFrame(view: view, scale: scale))
                    // The iPad draws the laser on GPU layers inside the ink canvas (same latency as ink);
                    // the projector mirrors it here.
                    if !interactive { LaserView(presenter: presenter, scale: scale, origin: origin) }
                    if !interactive { LiveStrokeView(presenter: presenter, scale: scale, origin: origin) }
                    if interactive {
                        // Above the ink canvas so the buttons get touches; iPad screen only.
                        ForEach(presenter.pageMedia, id: \.continuity) { item in
                            if presenter.controlsFor == item.id, let video = presenter.videos[item.id],
                               let f = controlsFrame(view: view, scale: scale) {
                                TouchPanel(content: VideoControls(video: video, presenter: presenter))
                                    .frame(width: f.width, height: f.height)
                                    .position(x: f.midX, y: f.midY)
                                    .transition(.opacity)
                            }
                        }
                    }
                }
                ZStack(alignment: .topLeading) {
                    // Whole slide and its ink behind the sharp view: fills whatever it doesn't cover, at the
                    // edges while zooming out and until a new sharp render arrives.
                    if zoom != nil || live != nil {
                        let fit = SlideGeometry.fitScale(page: slide.size, in: geo.size)
                        let shown = live ?? view, q = view.width * scale / shown.width, m = q / fit
                        ZStack(alignment: .topLeading) {
                            SlideImage(doc: doc, index: presenter.pageIndex, renderer: presenter.renderer,
                                       zoom: nil, notes: false, scale: fit)
                            if live != nil, let ink = presenter.inkSnapshot { Image(uiImage: ink).resizable() }
                        }
                        .frame(width: slide.width * fit, height: slide.height * fit)
                        .transformEffect(CGAffineTransform(translationX: (slide.minX - shown.minX) * q,
                                                           y: (shown.maxY - slide.maxY) * q).scaledBy(x: m, y: m))
                        .allowsHitTesting(false)     // picture only: touches belong to the ink canvas above
                        .accessibilityHidden(true)   // a scaled-up underlay would stretch the slide's frame off screen
                    }
                    stack.transformEffect(liveTransform)
                }
                .frame(width: view.width * scale, height: view.height * scale, alignment: .topLeading)
                .clipped()
                // The slide's own accessibility element, exactly the visible area (a container's frame would
                // grow with the zoomed layers inside it). Zoomed region's centre and width as fractions of
                // the slide in its value (UI tests read this).
                .overlay {
                    Color.clear
                        .allowsHitTesting(false)
                        .accessibilityElement()
                        .accessibilityIdentifier(interactive ? "slide" : "projector")
                        .accessibilityValue({ let v = live ?? view
                            return String(format: "%.3f %.3f %.3f", (v.midX - slide.minX) / slide.width,
                                          (slide.maxY - v.midY) / slide.height, v.width / slide.width) }())
                }
                .position(x: geo.size.width / 2, y: geo.size.height / 2)
            }
        }
        .background(Color.black)
    }

    /// Where the tap-to-show video bar sits (bottom of the video), in this view's coordinates.
    private func controlsFrame(view: CGRect, scale: CGFloat) -> CGRect? {
        guard interactive, let id = presenter.controlsFor, let item = presenter.pageMedia.first(where: { $0.id == id })
        else { return nil }
        let r = SlideGeometry.canvasRect(for: item.rect, pageBox: view, scale: scale)
        let w = max(r.width - 16, 240)
        return CGRect(x: r.midX - w / 2, y: r.maxY - 52, width: w, height: 44)
    }
}

/// Renders the slide (or notes) part of a page, or the zoomed region, at screen resolution.
/// Until a new image arrives the previous one stays, placed where its region falls in the current
/// view, so slide changes never flash and pinch-zoom stays steady.
struct SlideImage: View {
    let doc: PDFDocument
    let index: Int
    let renderer: SlideRenderer
    let zoom: CGRect?
    let notes: Bool
    let scale: CGFloat
    @Environment(\.displayScale) private var displayScale
    @State private var shown: (image: UIImage, region: CGRect)?

    private static func part(_ page: PDFPage, notes: Bool) -> CGRect {
        let split = SlideGeometry.split(page.bounds(for: .cropBox))
        return notes ? split.notes ?? split.slide : split.slide
    }

    var body: some View {
        let region = zoom ?? doc.page(at: index).map { Self.part($0, notes: notes) } ?? .zero
        let px = Int((region.width * scale * displayScale).rounded())
        let hit = zoom == nil ? renderer.cached(doc, index, region: region, width: px).map { ($0, region) } : nil
        ZStack(alignment: .topLeading) {
            if zoom == nil { Color.white }   // zoomed: the whole-slide underlay shows until this renders
            if let (image, at) = hit ?? shown {
                let r = SlideGeometry.canvasRect(for: at, pageBox: region, scale: scale)
                Image(uiImage: image).resizable().interpolation(.high)
                    .frame(width: r.width, height: r.height).offset(x: r.minX, y: r.minY)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .task(id: "\(ObjectIdentifier(doc).hashValue):\(index)@\(px)@\(region)") {
            guard px > 0 else { return }
            if let img = await renderer.render(doc, index, region: region, width: px, cacheable: zoom == nil) {
                shown = (img, region)
            }
            if zoom == nil {
                let notes = notes
                renderer.prefetch(doc, around: index, width: px) { Self.part($0, notes: notes) }
            }
        }
    }
}

struct MediaView: View {
    let item: MediaItem
    let presenter: Presenter
    let interactive: Bool
    @State private var pickingFolder = false

    var body: some View {
        if let video = presenter.videos[item.id] {
            PlayerView(player: video.player)
                .accessibilityElement(children: .ignore).accessibilityLabel("Video").accessibilityIdentifier("video")
        } else if let anim = presenter.animations[item.id] {
            if let img = anim.image { Image(decorative: img, scale: 1).resizable() } else { Color.clear }
        } else if let problem = presenter.problems[item.id] {
            // Spec: unsupported media is a visible message, never a silent blank.
            ZStack {
                Rectangle().fill(.black.opacity(0.75))
                VStack(spacing: 8) {
                    Label(problem.text, systemImage: "exclamationmark.triangle")
                        .font(.caption).foregroundStyle(.white).multilineTextAlignment(.center)
                    if interactive && problem.needsFolderAccess {
                        Button("Grant folder access…") { pickingFolder = true }.buttonStyle(.borderedProminent)
                    }
                }
                .padding(6)
            }
            .fileImporter(isPresented: $pickingFolder, allowedContentTypes: [.folder]) { result in
                if case .success(let url) = result { presenter.grantFolder(url) }
            }
        } else {
            Color.clear   // loading: the PDF's own poster shows through
        }
    }
}

struct PlayerView: UIViewRepresentable {
    let player: AVPlayer

    final class LayerView: UIView {
        override class var layerClass: AnyClass { AVPlayerLayer.self }
        var playerLayer: AVPlayerLayer { layer as! AVPlayerLayer }
    }

    func makeUIView(context: Context) -> LayerView {
        let v = LayerView()
        v.playerLayer.videoGravity = .resizeAspect
        v.isUserInteractionEnabled = false
        return v
    }
    func updateUIView(_ v: LayerView, context: Context) {
        if v.playerLayer.player !== player { v.playerLayer.player = player }
    }
}

/// PencilKit canvas whose content space is the PDF page (in points, y down).
/// It is zoomed to the screen scale rather than transformed, so ink stays sharp on both screens.
struct InkCanvas: UIViewRepresentable {
    let presenter: Presenter
    let interactive: Bool
    let pageBox: CGRect   // the slide part of the page, PDF coordinates
    let scale: CGFloat
    let origin: CGPoint   // visible part's top-left in slide content coordinates (non-zero when zoomed)
    var controlsFrame: CGRect? = nil   // video bar: taps there belong to its buttons, not the canvas

    func makeCoordinator() -> Coordinator { Coordinator(presenter: presenter) }

    func makeUIView(context: Context) -> PKCanvasView {
        let c = PKCanvasView()
        c.backgroundColor = .clear
        c.isOpaque = false
        c.drawingPolicy = isMac ? .anyInput : .pencilOnly   // spec 5.2 #5: fingers navigate; Mac draws with the mouse
        c.isScrollEnabled = false
        c.showsVerticalScrollIndicator = false
        c.showsHorizontalScrollIndicator = false
        c.contentInsetAdjustmentBehavior = .never
        c.overrideUserInterfaceStyle = .light   // PencilKit would otherwise draw black ink white in dark mode
        c.bouncesZoom = false
        c.delegate = context.coordinator
        c.isUserInteractionEnabled = interactive
        context.coordinator.canvas = c
        if interactive {
            c.drawingGestureRecognizer.addTarget(context.coordinator, action: #selector(Coordinator.pencilMoved(_:)))
            let fingers = [NSNumber(value: UITouch.TouchType.direct.rawValue)]
            let tap = UITapGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.tapped(_:)))
            if !isMac { tap.allowedTouchTypes = fingers }
            tap.delegate = context.coordinator
            c.addGestureRecognizer(tap)
            context.coordinator.tap = tap
            // Laser: captured exactly like ink (from the first touch, every Pencil sample), not with a pan
            // gesture, which waits for ~10 pt of movement before starting. Mac: the pointer also hovers.
            let laserCapture = FountainRecognizer(target: context.coordinator, action: #selector(Coordinator.laserCaptured(_:)))
            if !isMac { laserCapture.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.pencil.rawValue)] }
            context.coordinator.laserRecognizers = [laserCapture]
            context.coordinator.installLaserLayers(on: c)
            // Test hook: `-selfTestLaser YES` draws a laser line at 2 s and lifts at 4 s, via the same
            // calls a Pencil stroke makes.
            if UserDefaults.standard.bool(forKey: "selfTestLaser") {
                let co = context.coordinator, p = presenter
                DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
                    p.tool = .laser
                    p.laserMoved(along: (0...160).map { i in
                        let t = CGFloat(i) / 160
                        return CGPoint(x: 60 + t * 330, y: 120 + sin(t * 9) * 40)
                    })
                    co.renderLaser()
                }
                DispatchQueue.main.asyncAfter(deadline: .now() + 4) { co.testLift() }
            }
            // Fountain pen: our own pressure-driven nib, drawn live on a layer above the ink.
            let fountain = FountainRecognizer(target: context.coordinator, action: #selector(Coordinator.fountainMoved(_:)))
            // Test hook: `-fingerInk YES` lets a finger write (UI tests have no Pencil).
            let fingerInk = UserDefaults.standard.bool(forKey: "fingerInk")
            if fingerInk { c.drawingPolicy = .anyInput; context.coordinator.fingerInk = true }
            if !isMac && !fingerInk { fountain.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.pencil.rawValue)] }
            c.addGestureRecognizer(fountain)
            let eraser = FountainRecognizer(target: context.coordinator, action: #selector(Coordinator.eraserMoved(_:)))
            if !isMac && !fingerInk { eraser.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.pencil.rawValue)] }
            c.addGestureRecognizer(eraser)
            context.coordinator.eraser = eraser
            let ring = CAShapeLayer()
            ring.fillColor = UIColor.white.withAlphaComponent(0.25).cgColor
            ring.strokeColor = UIColor.gray.cgColor
            ring.lineWidth = 1.5
            ring.zPosition = 1001
            c.layer.addSublayer(ring)
            context.coordinator.eraserRing = ring
            let live = CAShapeLayer()
            live.fillRule = .nonZero
            live.zPosition = 1000
            c.layer.addSublayer(live)
            context.coordinator.fountain = fountain
            // Test hook: `-selfTestStroke YES` finishes one synthetic fountain stroke after launch.
            if UserDefaults.standard.bool(forKey: "selfTestStroke") {
                let co = context.coordinator
                DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
                    co.finish((0...120).map { i in
                        let t = CGFloat(i) / 120
                        return InkSample(p: CGPoint(x: 80 + t * 280, y: 140 + sin(t * 12) * 25), w: 0.5 + 4 * sin(t * .pi),
                                         t: Double(i) / 240, force: 0.5)
                    })
                    print("selfTestStroke: drawing has \(co.canvas?.drawing.strokes.count ?? -1) strokes")
                }
            }
            context.coordinator.liveLayer = live
            if isMac {   // the Mac pointer follows the mouse; on iPad the laser shows only while touching
                let hover = UIHoverGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.laserMoved(_:)))
                context.coordinator.laserRecognizers.append(hover)
            }
            context.coordinator.laserRecognizers.forEach(c.addGestureRecognizer)
            // Zoom: pinch (fingers, or trackpad on Mac), two-finger drag or trackpad scroll to move,
            // two-finger tap to reset.
            let co = context.coordinator
            let pinch = UIPinchGestureRecognizer(target: co, action: #selector(Coordinator.pinched(_:)))
            // While zoomed, one finger moves the page; the Pencil keeps writing. Pinches pan by themselves.
            let drag = UIPanGestureRecognizer(target: co, action: #selector(Coordinator.panned(_:)))
            drag.maximumNumberOfTouches = 1
            let scroll = UIPanGestureRecognizer(target: co, action: #selector(Coordinator.panned(_:)))
            scroll.allowedScrollTypesMask = .continuous
            scroll.allowedTouchTypes = []
            let reset = UITapGestureRecognizer(target: co, action: #selector(Coordinator.resetZoom))
            reset.numberOfTouchesRequired = 2
            for g in [pinch, drag, reset] where !isMac { g.allowedTouchTypes = fingers }
            co.panRecognizer = drag
            drag.delegate = co
            [pinch, drag, scroll, reset].forEach(c.addGestureRecognizer)
            // Finger swipes on iPad. Not on Mac: the mouse is a "direct" touch there and swipes would steal strokes.
            for dir in [UISwipeGestureRecognizer.Direction.left, .right] where !isMac {
                let swipe = UISwipeGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.swiped(_:)))
                swipe.direction = dir
                swipe.allowedTouchTypes = fingers
                c.addGestureRecognizer(swipe)
                context.coordinator.swipes.append(swipe)
            }
        }
        return c
    }

    func updateUIView(_ c: PKCanvasView, context: Context) {
        context.coordinator.pageBox = pageBox
        context.coordinator.controlsFrame = controlsFrame
        if interactive { context.coordinator.deferPencilKitTaps(c) }
        let scaleChanged = c.zoomScale != scale
        if scaleChanged {
            c.maximumZoomScale = max(scale, c.maximumZoomScale); c.minimumZoomScale = min(scale, c.minimumZoomScale)
            c.zoomScale = scale
            c.minimumZoomScale = scale; c.maximumZoomScale = scale
            if interactive { context.coordinator.renderLaser() }   // laser paths are in zoomed coordinates
        }
        // PencilKit ignores a programmatic scroll offset for touches, drawing and erasing (measured: ink
        // landed as if the zoomed view started at the slide's corner). So the canvas is never scrolled:
        // it holds the ink shifted so the visible region starts at its origin, and every change is
        // shifted back before it is saved.
        let co = context.coordinator
        let content = CGSize(width: pageBox.width * scale, height: pageBox.height * scale)
        if c.contentSize != content { c.contentSize = content }
        if c.contentOffset != .zero { c.contentOffset = .zero }
        let shiftChanged = co.shift != origin
        if shiftChanged {
            co.shift = origin
            if interactive { co.renderLaser() }
        }
        // For UI tests: where the visible ink region starts, as a fraction of the slide.
        c.accessibilityIdentifier = interactive ? "ink" : nil
        c.accessibilityValue = String(format: "%.3f %.3f", origin.x / pageBox.width, origin.y / pageBox.height)
        if interactive {
            let tool = presenter.pkTool
            if !co.sameTool(tool) { c.tool = tool }
            c.drawingGestureRecognizer.isEnabled = presenter.tool.inks && !presenter.fountainActive && !presenter.partialEraserActive
            co.fountain?.isEnabled = presenter.fountainActive
            co.eraser?.isEnabled = presenter.partialEraserActive
            co.fountain?.curve = presenter.pressureCurve
            co.fountain?.zoom = scale
            co.eraser?.zoom = scale
            co.penColor = UIColor(presenter.penColor).resolvedColor(with: UITraitCollection(userInterfaceStyle: .light))
            co.laserRecognizers.forEach { $0.isEnabled = presenter.tool == .laser }
            co.laserRecognizers.compactMap { $0 as? FountainRecognizer }.forEach { $0.zoom = scale }
            co.tap?.isEnabled = !isMac || !presenter.tool.inks   // on Mac a click with a pen is a dot
            let zoomed = presenter.zoom != nil
            co.panRecognizer?.isEnabled = zoomed && !isMac && !(co.fingerInk && presenter.tool.inks)   // Mac: the mouse draws; trackpad scroll moves
            co.swipes.forEach { $0.isEnabled = !zoomed && !co.fingerInk }
        }
        // The iPad canvas already holds its own strokes; it only reloads when ink changed elsewhere.
        // The projector follows every commit.
        // Its ink is hidden while it redraws at a new zoom (the stale picture would show misplaced); a
        // snapshot of the same ink stands in until `canvasViewDidFinishRendering`.
        if interactive && presenter.inkSettling && (shiftChanged || scaleChanged) { co.hideInkUntilRendered(c) }
        if interactive {
            if co.revision != presenter.inkRevision || shiftChanged {
                co.revision = presenter.inkRevision
                co.renderLaser()   // new frame: its laser lines were cleared
                co.applying = true
                c.drawing = co.toCanvas(presenter.currentDrawing)
                co.applying = false
                co.reportLastStroke(c)
            }
        } else {
            c.drawing = co.toCanvas(presenter.currentDrawing)
        }
    }

    @MainActor final class Coordinator: NSObject, PKCanvasViewDelegate, UIGestureRecognizerDelegate {
        let presenter: Presenter
        weak var canvas: PKCanvasView?
        var pageBox = CGRect.zero
        var applying = false
        var laserRecognizers: [UIGestureRecognizer] = []
        weak var panRecognizer: UIPanGestureRecognizer?
        var swipes: [UISwipeGestureRecognizer] = []
        var controlsFrame: CGRect?
        var fingerInk = false
        /// Visible region's top-left in slide content coordinates; the canvas holds ink shifted by it.
        var shift = CGPoint.zero
        func toCanvas(_ d: PKDrawing) -> PKDrawing {
            shift == .zero ? d : d.transformed(using: CGAffineTransform(translationX: -shift.x, y: -shift.y))
        }
        func toSlide(_ d: PKDrawing) -> PKDrawing {
            shift == .zero ? d : d.transformed(using: CGAffineTransform(translationX: shift.x, y: shift.y))
        }
        func toSlide(_ p: CGPoint) -> CGPoint { CGPoint(x: p.x + shift.x, y: p.y + shift.y) }
        weak var fountain: FountainRecognizer?
        weak var eraser: FountainRecognizer?
        var eraserRing: CAShapeLayer?
        private var erasedUpTo = 0, erasedAny = false
        var liveLayer: CAShapeLayer?
        var penColor = UIColor.red
        private var clearLivePending = false
        weak var tap: UITapGestureRecognizer?
        var revision = -1
        private var lastToolKey: String?

        /// Reassigning an equal tool is wasted work; compare the parts that matter.
        func sameTool(_ t: PKTool) -> Bool {
            let ink = t as? PKInkingTool
            let key = ink.map { "\($0.inkType.rawValue) \($0.color) \($0.width)" }
                ?? (t as? PKEraserTool).map { "eraser \($0.eraserType) \($0.width)" } ?? "\(type(of: t))"
            defer { lastToolKey = key }
            return key == lastToolKey
        }

        init(presenter: Presenter) { self.presenter = presenter }

        @objc func fountainMoved(_ g: FountainRecognizer) {
            guard let c = canvas, let live = liveLayer else { return }
            switch g.state {
            case .began, .changed:
                clearLivePending = false
                CATransaction.begin(); CATransaction.setDisableActions(true)
                live.fillColor = penColor.cgColor
                live.path = NibPath.path(g.samples + g.predicted, scale: c.zoomScale)
                CATransaction.commit()
                if presenter.projectorsShown > 0 {
                    presenter.liveStroke = LiveStroke(points: g.samples.map { toSlide($0.p) }, color: penColor,
                                                      width: g.samples.last?.w ?? 1, marker: false)
                }
            case .ended, .cancelled:
                // A cancelled stroke (the system took the touch) is still what you wrote: keep it.
                print("fountain stroke \(g.state == .ended ? "ended" : "cancelled"), \(g.samples.count) samples")
                if g.samples.isEmpty { live.path = nil; presenter.liveStroke = nil } else { finish(g.samples) }
            default:
                live.path = nil
                presenter.liveStroke = nil
            }
        }

        /// Partial eraser: cuts the ink under a circle as wide on screen as the eraser size, shown as a ring.
        @objc func eraserMoved(_ g: FountainRecognizer) {
            guard let c = canvas, let ring = eraserRing else { return }
            let r = presenter.eraserWidth / 2 / c.zoomScale   // drawing coordinates
            switch g.state {
            case .began, .changed:
                let pts = g.samples.map(\.p)
                guard let last = pts.last else { return }
                var d = c.drawing, changed = false
                for i in erasedUpTo..<pts.count {
                    if let e = PartialEraser.erase(d, from: pts[max(i - 1, 0)], to: pts[i], radius: r) { d = e; changed = true }
                }
                erasedUpTo = pts.count
                if changed { applying = true; c.drawing = d; applying = false; erasedAny = true }
                let z = c.zoomScale
                CATransaction.begin(); CATransaction.setDisableActions(true)
                ring.path = UIBezierPath(ovalIn: CGRect(x: (last.x - r) * z, y: (last.y - r) * z, width: 2 * r * z, height: 2 * r * z)).cgPath
                CATransaction.commit()
            default:
                ring.path = nil
                erasedUpTo = 0
                // One undo step for the whole erase.
                if erasedAny { erasedAny = false; presenter.commit(toSlide(c.drawing)); reportLastStroke(c) }
            }
        }

        /// Pen lifted: hand the stroke to PencilKit and keep the live copy until it is drawn.
        func finish(_ samples: [InkSample]) {
            guard let c = canvas, let live = liveLayer else { return }
            CATransaction.begin(); CATransaction.setDisableActions(true)
            live.path = NibPath.path(samples, scale: c.zoomScale)   // drop the predicted tail
            CATransaction.commit()
            var d = c.drawing
            d.strokes.append(NibPath.stroke(samples, color: penColor))
            applying = true   // commit once, explicitly, whether or not PencilKit reports the change
            c.drawing = d
            applying = false
            presenter.commit(toSlide(d))
            reportLastStroke(c)
            clearLivePending = true
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in self?.clearLive() }
        }

        func canvasViewDidFinishRendering(_ c: PKCanvasView) {
            clearLive()
            // PencilKit reports this as its tiles start to fade in; reveal once they are fully drawn
            // (measured: three frames of faint, patchy ink otherwise).
            let token = hideToken
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { [weak self, weak c] in
                if let self, let c, self.hideToken == token { self.showInk(c) }
            }
        }

        private var hideToken = 0
        /// PencilKit draws into its subviews; the canvas itself (and its touches, live stroke and laser
        /// layers) stays put. Hiding the canvas instead would also stop it receiving touches.
        func hideInkUntilRendered(_ c: PKCanvasView) {
            hideToken += 1
            let token = hideToken
            c.subviews.forEach { $0.alpha = 0 }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self, weak c] in
                if let self, let c, self.hideToken == token { self.showInk(c) }
            }
        }

        private func showInk(_ c: PKCanvasView) {
            c.subviews.forEach { $0.alpha = 1 }
            presenter.inkSettled()
        }

        private func clearLive() {
            guard clearLivePending else { return }
            clearLivePending = false
            CATransaction.begin(); CATransaction.setDisableActions(true)
            liveLayer?.path = nil
            CATransaction.commit()
        }

        /// For UI tests: "n=<stroke count> <x> <y>", the newest stroke's centre as a fraction of the slide.
        func reportLastStroke(_ c: PKCanvasView) {
            guard let stroke = c.drawing.strokes.last else { c.accessibilityLabel = "n=0"; return }
            // Points in drawing coordinates: PencilKit keeps strokes drawn while zoomed in local
            // coordinates plus a transform, so apply it.
            let pts = stroke.path.map { toSlide($0.location.applying(stroke.transform)) }
            let b = pts.reduce(CGRect.null) { $0.union(CGRect(origin: $1, size: .zero)) }
            c.accessibilityLabel = String(format: "n=%d %.3f %.3f", c.drawing.strokes.count, b.midX / pageBox.width, b.midY / pageBox.height)
        }

        func canvasViewDrawingDidChange(_ c: PKCanvasView) {
            if !applying { presenter.commit(toSlide(c.drawing)); reportLastStroke(c) }
        }

        /// Canvas point (screen) -> content point (page points, y down).
        private func contentPoint(_ g: UIGestureRecognizer) -> CGPoint? {
            guard let c = canvas else { return nil }
            let p = g.location(in: c)
            return toSlide(CGPoint(x: p.x / c.zoomScale, y: p.y / c.zoomScale))
        }

        @objc func pencilMoved(_ g: UIGestureRecognizer) {
            guard presenter.projectorsShown > 0, let p = contentPoint(g), let ink = canvas?.tool as? PKInkingTool else { return }
            switch g.state {
            case .began:
                presenter.liveStroke = LiveStroke(points: [p], color: ink.color, width: ink.width, marker: ink.inkType == .marker)
            case .changed:
                presenter.liveStroke?.points.append(p)
            case .cancelled, .failed:
                presenter.liveStroke = nil
            default: break   // .ended: cleared when the stroke is committed, so the projector never flickers
            }
        }

        @objc func tapped(_ g: UITapGestureRecognizer) {
            // On Mac a click with an ink tool is a dot, not a tap.
            guard !isMac || !presenter.tool.inks, let p = contentPoint(g) else { return }
            presenter.tap(at: CGPoint(x: pageBox.minX + p.x, y: pageBox.maxY - p.y))
        }

        // MARK: Laser on the iPad screen
        // Drawn like ink: GPU shape layers updated inside the touch callback, with predicted points, a GPU
        // shadow for the glow and a GPU fade. (Going through SwiftUI cost a frame or more per sample.)

        private var laserSent = 0
        private var laserFadeToken = 0
        private let laserBox = CALayer()                              // lines; this fades
        private let laserGlow = CAShapeLayer(), laserBody = CAShapeLayer(), laserCore = CAShapeLayer()
        private let dotBox = CALayer()                                // the dot; never fades, just hides
        private let dotGlow = CAShapeLayer(), dotBody = CAShapeLayer(), dotCore = CAShapeLayer()
        static let laserRed = UIColor(red: 1, green: 0.16, blue: 0.12, alpha: 1)
        static let laserLight = UIColor(red: 1, green: 0.86, blue: 0.83, alpha: 1)

        func installLaserLayers(on c: PKCanvasView) {
            for (box, glow, body, core) in [(laserBox, laserGlow, laserBody, laserCore), (dotBox, dotGlow, dotBody, dotCore)] {
                box.zPosition = 1001
                for l in [glow, body, core] {
                    l.lineCap = .round; l.lineJoin = .round
                    box.addSublayer(l)
                }
                glow.shadowColor = UIColor.red.cgColor; glow.shadowOpacity = 1; glow.shadowOffset = .zero
                c.layer.addSublayer(box)
            }
            for l in [laserGlow, laserBody, laserCore] { l.fillColor = nil }
            laserGlow.strokeColor = UIColor.red.withAlphaComponent(0.55).cgColor
            laserBody.strokeColor = Self.laserRed.cgColor
            laserCore.strokeColor = Self.laserLight.cgColor
            dotGlow.fillColor = UIColor.red.withAlphaComponent(0.55).cgColor
            dotBody.fillColor = Self.laserRed.cgColor
            dotCore.fillColor = Self.laserLight.cgColor
        }

        /// Redraw from the presenter's lines, plus `predicted` points on the line being drawn.
        func renderLaser(predicted: [CGPoint] = []) {
            guard let c = canvas else { return }
            let z = c.zoomScale, w = presenter.laserTrailWidth * z, r = presenter.laserDotSize * z / 2
            var lines = presenter.laser.map { $0.map(\.p) }
            if presenter.laserPressing, !lines.isEmpty { lines[lines.count - 1] += predicted }
            let path = CGMutablePath()
            for line in lines where line.count > 1 {
                path.addPath(LaserView.smooth(line.map { CGPoint(x: ($0.x - shift.x) * z, y: ($0.y - shift.y) * z) }).cgPath)
            }
            CATransaction.begin(); CATransaction.setDisableActions(true)
            for (l, k) in [(laserGlow, 1.5), (laserBody, 1), (laserCore, 0.38)] as [(CAShapeLayer, CGFloat)] {
                l.path = path; l.lineWidth = w * k
            }
            laserGlow.shadowRadius = w * 0.8
            if let d = presenter.laserDot.map({ predicted.last ?? $0 }) {
                let cx = (d.x - shift.x) * z, cy = (d.y - shift.y) * z
                let disc = { (k: CGFloat) in CGPath(ellipseIn: CGRect(x: cx - r * k, y: cy - r * k, width: 2 * r * k, height: 2 * r * k), transform: nil) }
                dotGlow.path = disc(1.35); dotBody.path = disc(1); dotCore.path = disc(0.42)
                dotGlow.shadowRadius = r * 0.7
            } else {
                dotGlow.path = nil; dotBody.path = nil; dotCore.path = nil
            }
            CATransaction.commit()
        }

        /// After the last lift: hold, then an eased GPU fade; then clear. A new press cancels it.
        private func fadeOutLaser() {
            laserFadeToken += 1
            guard !presenter.laser.isEmpty else { return }
            let token = laserFadeToken, total = Presenter.laserFade
            let a = CAKeyframeAnimation(keyPath: "opacity")
            a.values = [1, 1, 0]
            a.keyTimes = [0, NSNumber(value: Presenter.laserHold / total), 1]
            a.timingFunctions = [CAMediaTimingFunction(name: .linear), CAMediaTimingFunction(name: .easeInEaseOut)]
            a.duration = total
            a.fillMode = .forwards
            a.isRemovedOnCompletion = false
            laserBox.add(a, forKey: "fade")
            DispatchQueue.main.asyncAfter(deadline: .now() + total) { [weak self] in
                guard let self, token == laserFadeToken else { return }
                // Clear the layers themselves; redrawing from the presenter could race its own clear-out.
                CATransaction.begin(); CATransaction.setDisableActions(true)
                for l in [laserGlow, laserBody, laserCore] { l.path = nil }
                laserBox.removeAnimation(forKey: "fade")
                CATransaction.commit()
            }
        }

        @objc func laserCaptured(_ g: FountainRecognizer) {
            switch g.state {
            case .began, .changed:
                if g.state == .began {
                    laserSent = 0
                    laserFadeToken += 1                      // earlier lines come back and stay
                    laserBox.removeAnimation(forKey: "fade")
                }
                presenter.laserMoved(along: g.samples[min(laserSent, g.samples.count)...].map { toSlide($0.p) })
                laserSent = g.samples.count
                renderLaser(predicted: g.predicted.map { toSlide($0.p) })
            default:
                presenter.laserEnded()
                renderLaser()                                // drop predicted points, hide the dot
                fadeOutLaser()
            }
        }

        func testLift() { presenter.laserEnded(); renderLaser(); fadeOutLaser() }

        /// Mac pointer hovering: just the dot.
        @objc func laserMoved(_ g: UIGestureRecognizer) {
            switch g.state {
            case .began, .changed:
                if let p = contentPoint(g) { presenter.laserMoved(to: p, drawing: false) }
            default: presenter.laserEnded(drawing: false)
            }
            renderLaser()
        }

        /// A tap on the video bar is for its buttons only; without this the canvas also saw it and hid the bar.
        /// A tap PenCast acts on (edge, link, media) is PenCast's alone; one it ignores goes on to PencilKit.
        func gestureRecognizerShouldBegin(_ g: UIGestureRecognizer) -> Bool {
            guard g === tap else { return true }
            guard let p = contentPoint(g) else { return false }
            return presenter.tapAction(at: CGPoint(x: pageBox.minX + p.x, y: pageBox.maxY - p.y)) != nil
        }

        private var deferred = Set<ObjectIdentifier>()
        /// PencilKit's own taps open its Paste / Select menu (on a single finger tap since iPadOS 26). They wait
        /// for PenCast's tap to pass on a touch, so turning the slide no longer opens the menu on the next one.
        /// Walked on every update: PencilKit adds its selection views lazily.
        func deferPencilKitTaps(_ c: PKCanvasView) {
            guard let tap else { return }
            func walk(_ v: UIView) {
                for g in v.gestureRecognizers ?? [] where !deferred.contains(ObjectIdentifier(g))
                    && (g is UITapGestureRecognizer || "\(type(of: g))".contains("Tap")) {
                    g.require(toFail: tap)
                    deferred.insert(ObjectIdentifier(g))
                }
                v.subviews.forEach(walk)
            }
            c.subviews.forEach(walk)   // the canvas's own recognizers are PenCast's
        }

        func gestureRecognizer(_ g: UIGestureRecognizer, shouldReceive touch: UITouch) -> Bool {
            if g === panRecognizer {
                // Never move the page under a stroke being written (that is when a resting palm drags).
                // (A touch-size palm filter rejected real fingertips too.)
                let writing = [fountain?.state, eraser?.state, canvas?.drawingGestureRecognizer.state]
                    .contains { $0 == .began || $0 == .changed }
                return !writing
            }
            guard g === tap, let f = controlsFrame, let c = canvas else { return true }
            let p = touch.location(in: c)
            return !f.contains(CGPoint(x: p.x - c.contentOffset.x, y: p.y - c.contentOffset.y))
        }

        /// The visible region and its screen scale, from the presenter's current zoom. Pinch and pan must not
        /// use the canvas's own zoom/offset: SwiftUI applies each change a frame later, so mid-gesture the
        /// canvas lags the presenter, and the error compounds until the zoom drifts away from your fingers.
        private func visible() -> (region: CGRect, scale: CGFloat)? {
            guard let c = canvas, c.bounds.width > 0 else { return nil }
            let region = presenter.zoom ?? pageBox
            return (region, c.bounds.width / region.width)
        }

        // Pinch and drag are measured in window coordinates against the view's frame as it was when the
        // gesture began: during the gesture the canvas itself is moved by the live transform.
        private var pinchStart: (anchor: CGPoint, width: CGFloat, frame: CGRect)?

        @objc func pinched(_ g: UIPinchGestureRecognizer) {
            guard let c = canvas else { return }
            switch g.state {
            case .began:
                presenter.beginLiveZoom()
                let (frame, region) = visibleFrame(c)
                let loc = g.location(in: nil)
                let f = CGPoint(x: (loc.x - frame.minX) / frame.width, y: (loc.y - frame.minY) / frame.height)
                pinchStart = (CGPoint(x: region.minX + f.x * region.width, y: region.maxY - f.y * region.height),
                              region.width, frame)
            case .changed:
                // One finger lifted early: hold still rather than jump to the remaining finger.
                guard let start = pinchStart, g.numberOfTouches == 2 else { return }
                let loc = g.location(in: nil)
                let f = CGPoint(x: (loc.x - start.frame.minX) / start.frame.width, y: (loc.y - start.frame.minY) / start.frame.height)
                presenter.zoom(toWidth: start.width / g.scale, keeping: start.anchor, at: f)
            default:
                pinchStart = nil
                presenter.endLiveZoom()
            }
        }

        @objc func panned(_ g: UIPanGestureRecognizer) {
            guard let c = canvas else { return }
            switch g.state {
            case .began, .changed:
                if g.state == .began { presenter.beginLiveZoom() }
                let (frame, region) = visibleFrame(c)
                let s = frame.width / region.width   // screen points per PDF point
                let t = g.translation(in: nil)
                presenter.pan(dx: -t.x / s, dy: t.y / s)
                g.setTranslation(.zero, in: nil)
            case .ended:
                let (frame, region) = visibleFrame(c)
                let s = frame.width / region.width, v = g.velocity(in: nil)
                presenter.endLiveZoom(velocity: CGPoint(x: -v.x / s, y: v.y / s))
            default:
                presenter.endLiveZoom()
            }
        }

        @objc func resetZoom() { presenter.resetZoom() }

        /// Where the region now on screen sits in window coordinates, and that region. The canvas holds the
        /// settled view; during a gesture or animation it is transformed, so undo that from its frame.
        private func visibleFrame(_ c: PKCanvasView) -> (CGRect, CGRect) {
            let t = c.convert(c.bounds, to: nil)
            let settled = presenter.zoom ?? pageBox
            guard let live = presenter.liveZoom else { return (t, settled) }
            let q = t.width / settled.width
            return (CGRect(x: t.minX + (live.minX - settled.minX) * q, y: t.minY + (settled.maxY - live.maxY) * q,
                           width: live.width * q, height: live.height * q), live)
        }

        @objc func swiped(_ g: UISwipeGestureRecognizer) {
            guard presenter.zoom == nil else { return }   // while zoomed, fingers move the view
            g.direction == .left ? presenter.next() : presenter.previous()
        }
    }
}

/// Red dot with a fading trail, drawn on both screens.
struct LaserView: View {
    let presenter: Presenter
    let scale: CGFloat
    let origin: CGPoint

    var body: some View {
        let lines = presenter.laser, dotAt = presenter.laserDot
        let pressing = presenter.laserPressing, lifted = presenter.laserLiftedAt
        let dot = presenter.laserDotSize, width = presenter.laserTrailWidth
        // The fade is driven by the timeline's date: the drawing must read it, or nothing redraws
        // between lifting and the lines being removed (they then vanished instead of fading).
        TimelineView(.animation(paused: lines.isEmpty || pressing)) { timeline in
            let x = min(max((timeline.date.timeIntervalSince(lifted) - Presenter.laserHold) / Presenter.laserFadeOut, 0), 1)
            let alpha = pressing ? 1 : 1 - x * x * (3 - 2 * x)   // hold, then an eased fade
            Canvas { ctx, _ in
                let pt = { (p: CGPoint) in CGPoint(x: (p.x - origin.x) * scale, y: (p.y - origin.y) * scale) }
                let w = width * scale
                // GoodNotes-style neon line: soft blurred red glow, solid red body, bright core. Each layer is
                // one stroked, smoothed path, so overlaps never darken into beads.
                if alpha > 0 {
                    for line in lines where line.count > 1 {
                        let path = Self.smooth(line.map { pt($0.p) })
                        var glow = ctx
                        glow.addFilter(.blur(radius: w * 0.9))
                        glow.stroke(path, with: .color(.red.opacity(0.85 * alpha)), style: Self.round(w * 2))
                        ctx.stroke(path, with: .color(Color(red: 1, green: 0.16, blue: 0.12).opacity(alpha)), style: Self.round(w))
                        ctx.stroke(path, with: .color(Color(red: 1, green: 0.85, blue: 0.82).opacity(0.95 * alpha)), style: Self.round(w * 0.38))
                    }
                }
                // The dot exists only while touching: it vanishes the moment you lift.
                if let at = dotAt {
                    let c = pt(at), r = dot * scale / 2
                    let disc = { (k: CGFloat) in Path(ellipseIn: CGRect(x: c.x - r * k, y: c.y - r * k, width: 2 * r * k, height: 2 * r * k)) }
                    var glow = ctx
                    glow.addFilter(.blur(radius: r * 0.8))
                    glow.fill(disc(1.5), with: .color(.red.opacity(0.85)))
                    ctx.fill(disc(1), with: .color(Color(red: 1, green: 0.16, blue: 0.12)))
                    ctx.fill(disc(0.42), with: .color(Color(red: 1, green: 0.88, blue: 0.85)))
                }
            }
        }
        .allowsHitTesting(false)
    }

    static func round(_ w: CGFloat) -> StrokeStyle { StrokeStyle(lineWidth: w, lineCap: .round, lineJoin: .round) }

    /// Quadratic curves through the midpoints: a smooth line from raw samples, ending on the last point.
    static func smooth(_ p: [CGPoint]) -> Path {
        Path { path in
            path.move(to: p[0])
            for i in 1..<p.count - 1 {
                path.addQuadCurve(to: CGPoint(x: (p[i].x + p[i + 1].x) / 2, y: (p[i].y + p[i + 1].y) / 2), control: p[i])
            }
            path.addLine(to: p[p.count - 1])
        }
    }
}

/// Separate view so per-point updates only redraw this path, not the whole slide.
struct LiveStrokeView: View {
    let presenter: Presenter
    let scale: CGFloat
    let origin: CGPoint

    var body: some View {
        if let stroke = presenter.liveStroke { path(stroke) }
    }

    private func path(_ stroke: LiveStroke) -> some View {
        Path { p in p.addLines(stroke.points.map { CGPoint(x: ($0.x - origin.x) * scale, y: ($0.y - origin.y) * scale) }) }
            .stroke(Color(stroke.color).opacity(stroke.marker ? 0.5 : 1),
                    style: StrokeStyle(lineWidth: stroke.width * scale, lineCap: .round, lineJoin: .round))
            .allowsHitTesting(false)
    }
}

// MARK: - iPad presenter screen

/// What an opened deck shows first: every page in one scrolling column, for checking a deck while writing
/// it (it reloads on each LaTeX compile). Present switches to the presenter at the page being read.
struct ReaderView: View {
    let presenter: Presenter
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 4) {
                Button { presenter.close() } label: { Image(systemName: "xmark.circle").barTarget() }
                    .buttonStyle(.borderless).font(.title2).accessibilityLabel("Close Presentation")
                Text(presenter.deckName).font(.headline).lineLimit(1)
                Text("\(presenter.pageIndex + 1) / \(presenter.pageCount)").monospacedDigit().fixedSize()
                    .foregroundStyle(.secondary).padding(.horizontal, 8)
                Spacer()
                Button {
                    // Mac: the projector is a window; drag it to the other screen and make it full screen.
                    if isMac && presenter.projectorsShown == 0 { openWindow(id: "projector") }
                    presenter.present()
                } label: { Label("Present", systemImage: "play.rectangle.fill") }
                    .buttonStyle(.borderedProminent).tint(.accentColor).fixedSize()   // the title shrinks first, never this.keyboardShortcut(.return, modifiers: .command)
            }
            .padding(.horizontal, 10).padding(.vertical, 4)
            .background(.bar)
            if let doc = presenter.document {
                ReaderPDF(document: doc, page: presenter.pageIndex) { presenter.go(to: $0) }
            }
        }
    }
}

/// PDFKit's continuous scroller, kept in step with `Presenter.pageIndex` both ways
/// (scrolling sets the page; arrow keys and reloads move the scroller).
struct ReaderPDF: UIViewRepresentable {
    let document: PDFDocument
    let page: Int
    let onPage: (Int) -> Void

    func makeUIView(context: Context) -> PDFView {
        let v = PDFView()
        v.displayMode = .singlePageContinuous
        v.displayDirection = .vertical
        v.autoScales = true
        v.backgroundColor = .systemGray5
        context.coordinator.observer = NotificationCenter.default.addObserver(
            forName: .PDFViewPageChanged, object: v, queue: nil) { [coordinator = context.coordinator] _ in
            // Later, not during the view update; and read the page then: a reloaded document posts page 1 first.
            DispatchQueue.main.async { [weak v] in
                guard let v, let doc = v.document, let p = v.currentPage else { return }
                coordinator.onPage(doc.index(for: p))
            }
        }
        return v
    }

    func updateUIView(_ v: PDFView, context: Context) {
        context.coordinator.onPage = onPage
        // ponytail: a reload keeps the page, not the exact scroll offset within it.
        if v.document !== document { v.document = document }
        if let target = document.page(at: page), v.currentPage !== target { v.go(to: target) }
    }

    func makeCoordinator() -> Coordinator { Coordinator(onPage: onPage) }

    final class Coordinator {
        var onPage: (Int) -> Void
        var observer: NSObjectProtocol?
        init(onPage: @escaping (Int) -> Void) { self.onPage = onPage }
        deinit { observer.map(NotificationCenter.default.removeObserver) }
    }
}

struct PresenterView: View {
    @Bindable var presenter: Presenter
    @State private var showThumbnails = false
    @State private var toolMenu: InkTool?   // tool whose settings are dropped down
    @State private var exporting = false
    @State private var exported: PDFFile?
    @State private var sharing: URL?
    @State private var showDisplays = false
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        VStack(spacing: 0) {
            // A window too narrow for the bar (tiled half screen, iPad split view) scrolls it instead of
            // squeezing the icons.
            ViewThatFits(in: .horizontal) {
                toolbar
                ScrollView(.horizontal, showsIndicators: false) { toolbar }.background(.bar)
            }
            GeometryReader { geo in
                HStack(spacing: 0) {
                    SlideView(presenter: presenter, interactive: true)
                    if presenter.hasNotes && presenter.showNotes {
                        NotesPanel(presenter: presenter).frame(width: geo.size.width * 0.4)
                    }
                }
            }
            .overlay(alignment: .top) { projectorBanner }
        }
        .background(Color.black)
        .fileExporter(isPresented: Binding(get: { exported != nil }, set: { if !$0 { exported = nil } }),
                      document: exported, contentType: .pdf,
                      defaultFilename: exported?.url.deletingPathExtension().lastPathComponent) { _ in exported = nil }
        .sheet(isPresented: $showDisplays) { DisplaysSheet(presenter: presenter) }
        .sheet(isPresented: Binding(get: { sharing != nil }, set: { if !$0 { sharing = nil } })) {
            if let sharing { ShareSheet(items: [sharing]) }
        }
        .sheet(isPresented: $showThumbnails) {
            ThumbnailGrid(presenter: presenter) { showThumbnails = false }
        }
        .statusBarHidden()
        .persistentSystemOverlays(.hidden)
    }

    static let penSwatches: [Color] = [.black, .blue, .red, Color(red: 0, green: 0.55, blue: 0.25)]
    static let highlighterSwatches: [Color] = [.yellow, .green, .pink, .cyan]

    /// One-tap colours for the pen, or for the highlighter while it is the active tool.
    private var swatches: some View {
        let highlighter = presenter.tool == .highlighter
        let current = highlighter ? presenter.highlighterColor : presenter.penColor
        return HStack(spacing: 0) {
            ForEach(Array((highlighter ? Self.highlighterSwatches : Self.penSwatches).enumerated()), id: \.offset) { _, color in
                Button {
                    if highlighter { presenter.highlighterColor = color }
                    else { presenter.penColor = color; presenter.tool = .pen }
                } label: {
                    Circle().fill(color).frame(width: 28, height: 28)
                        .overlay(Circle().stroke(Color.primary.opacity(UIColor(current) == UIColor(color) ? 0.9 : 0.15), lineWidth: 2.5))
                        .frame(width: 34, height: 44).contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
        }
    }

    enum ExportAction { case saveAs, share, print }

    /// Builds the annotated PDF, then saves it where you choose, shares it, or prints it.
    private func export(notesOnly: Bool, _ action: ExportAction) {
        exporting = true
        Task {
            defer { exporting = false }
            guard let url = await presenter.exportAnnotated(notesOnly: notesOnly) else { return }
            switch action {
            case .saveAs: exported = PDFFile(url: url)
            case .share: sharing = url
            case .print:
                let printer = UIPrintInteractionController.shared
                printer.printingItem = url
                let info = UIPrintInfo.printInfo()
                info.jobName = url.deletingPathExtension().lastPathComponent
                printer.printInfo = info
                printer.present(animated: true)
            }
        }
    }

    /// Share icon: presenting (AirPlay / displays, projector window, blank / freeze, zoom, notes) and
    /// exporting (notes or every page; save, share, print).
    private var exportMenu: some View {
        Menu {
            Section("Present") {
                Button { showDisplays = true } label: {
                    Label(presenter.externalDisplayConnected ? "Display Connected" : "AirPlay or Connect a Display…",
                          systemImage: "airplayvideo")
                }
                if UIApplication.shared.supportsMultipleScenes {
                    Button { openWindow(id: "projector") } label: { Label("Open Projector Window", systemImage: "macwindow.badge.plus") }
                }
                // These change only the projector's picture: greyed out until one is showing.
                Group {
                    Button { presenter.setProjector(.black) } label: { Label("Black Screen (B)", systemImage: "rectangle.fill") }
                    Button { presenter.setProjector(.white) } label: { Label("White Screen (W)", systemImage: "rectangle") }
                    Button { presenter.toggleFreeze() } label: { Label("Freeze Projector (F)", systemImage: "snowflake") }
                    if presenter.projector != .none {
                        Button { presenter.setProjector(.none) } label: { Label("Show Slides", systemImage: "play.rectangle") }
                    }
                    Toggle(isOn: $presenter.projectorFollowsZoom) { Label("Projector Follows Zoom", systemImage: "plus.magnifyingglass") }
                }
                .disabled(presenter.projectorsShown == 0)
                if presenter.projectorsShown == 0 {
                    Text("Connect a projector or AirPlay display to blank, freeze or zoom it.")
                }
                if presenter.hasNotes {
                    Toggle(isOn: $presenter.showNotes) { Label("Speaker Notes on iPad (N)", systemImage: "note.text") }
                }
            }
            Section("Export notes: one page per slide, with your ink") {
                Button { export(notesOnly: true, .saveAs) } label: { Label("Save As…", systemImage: "folder") }
                Button { export(notesOnly: true, .share) } label: { Label("Share…", systemImage: "square.and.arrow.up") }
                Button { export(notesOnly: true, .print) } label: { Label("Print…", systemImage: "printer") }
            }
            Section("Export every page, as presented") {
                Button { export(notesOnly: false, .saveAs) } label: { Label("Save As…", systemImage: "folder") }
                Button { export(notesOnly: false, .share) } label: { Label("Share…", systemImage: "square.and.arrow.up") }
                Button { export(notesOnly: false, .print) } label: { Label("Print…", systemImage: "printer") }
            }
        } label: {
            Group { if exporting { ProgressView() } else { Image(systemName: "square.and.arrow.up") } }.barTarget()
        }
        .menuIndicator(.hidden)   // Mac squeezed a dropdown arrow into the icon's 44 pt slot
        .help("Present and export")
        .accessibilityLabel("Export")
        .disabled(exporting)
    }

    /// Reminds the presenter that the audience is not seeing the live slide.
    @ViewBuilder private var projectorBanner: some View {
        let text: String? = switch presenter.projector {
        case .none: nil
        case .black: "Projector is black"
        case .white: "Projector is white"
        case .frozen: "Projector is frozen"
        }
        if let text {
            Button { presenter.setProjector(.none) } label: {
                Label("\(text) · tap to show slides", systemImage: "tv.slash").font(.callout.bold())
                    .padding(.horizontal, 14).padding(.vertical, 6)
                    .background(.orange, in: Capsule()).foregroundStyle(.white)
            }
            .buttonStyle(.plain).padding(.top, 8)
        }
    }

    /// GoodNotes-style: tap a tool to pick it, tap it again to drop down that tool's settings.
    private var toolButtons: some View {
        HStack(spacing: 2) {
            ForEach([(InkTool.pointer, "cursorarrow"), (.laser, "laser.burst"), (.pen, "pencil.tip"),
                     (.highlighter, "highlighter"), (.eraser, "eraser"), (.lasso, "lasso")], id: \.0) { tool, icon in
                Button {
                    if presenter.tool != tool { presenter.tool = tool }
                    else if tool != .pointer && tool != .lasso { toolMenu = tool }
                } label: {
                    Image(systemName: icon).frame(width: 50, height: 42)
                        .background(presenter.tool == tool ? Color.accentColor.opacity(0.25) : .clear,
                                    in: RoundedRectangle(cornerRadius: 8))
                        .contentShape(Rectangle())   // the whole button, not just the icon's lines
                }
                .accessibilityLabel("\(tool)")
                .accessibilityAddTraits(presenter.tool == tool ? .isSelected : [])
                .buttonStyle(.plain)
                .popover(isPresented: Binding(get: { toolMenu == tool }, set: { if !$0 { toolMenu = nil } }),
                         arrowEdge: .top) {
                    ToolOptions(presenter: presenter, tool: tool)
                }
            }
        }
        .padding(3)
        .background(Color.secondary.opacity(0.12), in: RoundedRectangle(cornerRadius: 9))
    }

    private var toolbar: some View {
        HStack(spacing: 4) {
            Button { presenter.stopPresenting() } label: { Image(systemName: "xmark.circle").barTarget() }
                .accessibilityLabel("Stop Presenting")
            Button { showThumbnails = true } label: { Image(systemName: "square.grid.3x2").barTarget() }
                .accessibilityLabel("Slides")
            Text("\(presenter.pageIndex + 1) / \(presenter.pageCount)").monospacedDigit().fixedSize()
                .padding(.horizontal, 4)
            TalkTimer(presenter: presenter)
            Spacer(minLength: 4)
            toolButtons
            swatches
            Button { presenter.undo() } label: { Image(systemName: "arrow.uturn.backward").barTarget() }
                .disabled(!presenter.canUndo).accessibilityLabel("Undo")
            Button { presenter.redo() } label: { Image(systemName: "arrow.uturn.forward").barTarget() }
                .disabled(!presenter.canRedo).accessibilityLabel("Redo")
            Button(role: .destructive) { presenter.clearSlide() } label: { Image(systemName: "trash").barTarget() }
                .accessibilityLabel("Clear Slide")
            Spacer(minLength: 4)
            if presenter.zoom != nil {
                Button { presenter.resetZoom() } label: { Image(systemName: "arrow.down.right.and.arrow.up.left").barTarget() }
                    .help("Show whole slide").accessibilityLabel("Show Whole Slide")
            }
            Button { showDisplays = true } label: {
                Image(systemName: presenter.projector != .none ? "tv.slash" : presenter.externalDisplayConnected ? "tv.fill" : "tv")
                    .foregroundStyle(presenter.projector != .none ? .orange : presenter.externalDisplayConnected ? .green : .secondary)
                    .barTarget()
            }
            .help("Display status")
            exportMenu
            Button { presenter.previous() } label: { Image(systemName: "chevron.left").barTarget() }
                .accessibilityLabel("Back")
            Button { presenter.next() } label: { Image(systemName: "chevron.right").barTarget() }
                .accessibilityLabel("Forward")
        }
        .font(.title2)
        .buttonStyle(.borderless).menuStyle(.button)   // no Mac bezels: they widened the bar past the window
        .padding(.horizontal, 10).padding(.vertical, 4)
        .background(.bar)
    }
}

/// One tool's settings, dropped down from its toolbar button.
struct ToolOptions: View {
    @Bindable var presenter: Presenter
    let tool: InkTool
    @State private var padClears = 0

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            switch tool {
            case .pen:
                Picker("Pen", selection: $presenter.penStyle) {
                    Text("Fountain pen").tag(PenStyle.fountain)
                    Text("Ball pen").tag(PenStyle.ball)
                }
                .pickerStyle(.segmented)
                Text(presenter.penStyle == .fountain ? "Round nib; the line gets thicker as you press harder."
                                                     : "Round nib; an even line whatever the pressure.")
                    .font(.caption).foregroundStyle(.secondary)
                size($presenter.penWidth, in: Presenter.penWidths)
                if presenter.penStyle == .fountain {
                    labelled("Press for the thickest line", low: "Light", high: "Hard") {
                        Slider(value: $presenter.pressureFirm, in: 0.15...1)
                    }
                    labelled("Thinnest line", low: "Hairline", high: "Even") {
                        Slider(value: $presenter.pressureFine, in: 0.05...1)
                    }
                    ZStack(alignment: .topTrailing) {
                        PressurePad(curve: presenter.pressureCurve,
                                    color: UIColor(presenter.penColor), clearToken: padClears)
                            .frame(height: 130)
                            .overlay(alignment: .bottomLeading) {
                                Text("Try it here").font(.caption2).foregroundStyle(.gray).padding(8)
                                    .allowsHitTesting(false)
                            }
                        Button("Clear") { padClears += 1 }.font(.caption).padding(8)
                    }
                } else {
                    StrokePreview(color: presenter.penColor, width: presenter.penWidth, style: .even)
                }
                colors($presenter.penColor, PresenterView.penSwatches + [.white, .orange, .purple])
            case .highlighter:
                size($presenter.highlighterWidth, in: Presenter.highlighterWidths)
                StrokePreview(color: presenter.highlighterColor, width: presenter.highlighterWidth, style: .highlighter)
                colors($presenter.highlighterColor, PresenterView.highlighterSwatches + [.orange, .purple])
            case .laser:
                Picker("Laser", selection: $presenter.laserTrail) {
                    Text("Trail").tag(true)
                    Text("Dot only").tag(false)
                }
                .pickerStyle(.segmented)
                Text(presenter.laserTrail ? "A glowing line follows the Pencil and fades after you lift. Press again before it fades to keep it and add another."
                                          : "Just a glowing dot while you touch the screen.")
                    .font(.caption).foregroundStyle(.secondary)
                labelled("Dot size", low: "Small", high: "Large") {
                    Slider(value: $presenter.laserDotSize, in: Presenter.laserDotSizes)
                }
                if presenter.laserTrail {
                    labelled("Trail width", low: "Thin", high: "Wide") {
                        Slider(value: $presenter.laserTrailWidth, in: Presenter.laserTrailWidths)
                    }
                }
            case .eraser:
                Picker("Eraser", selection: $presenter.eraserMode) {
                    Text("Whole stroke").tag(EraserMode.stroke)
                    Text("Partial").tag(EraserMode.partial)
                }
                .pickerStyle(.segmented)
                if presenter.eraserMode == .partial { size($presenter.eraserWidth, in: Presenter.eraserWidths) }
            case .pointer, .lasso:
                EmptyView()
            }
        }
        .padding()
        .frame(width: 320)
    }

    private func labelled<Content: View>(_ title: String, low: String, high: String,
                                         @ViewBuilder _ content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title).font(.subheadline)
            HStack { Text(low); content(); Text(high) }.font(.caption).foregroundStyle(.secondary)
        }
    }

    private func size(_ value: Binding<CGFloat>, in range: ClosedRange<CGFloat>) -> some View {
        HStack {
            Image(systemName: "circle.fill").font(.system(size: 6))
            Slider(value: value, in: range)
            Image(systemName: "circle.fill").font(.system(size: 16))
        }
        .foregroundStyle(.secondary)
    }

    private func colors(_ value: Binding<Color>, _ swatches: [Color]) -> some View {
        HStack(spacing: 10) {
            ForEach(Array(swatches.enumerated()), id: \.offset) { _, color in
                Button { value.wrappedValue = color } label: {
                    Circle().fill(color).frame(width: 26, height: 26)
                        .overlay(Circle().stroke(Color.primary.opacity(UIColor(value.wrappedValue) == UIColor(color) ? 0.9 : 0.2), lineWidth: 2.5))
                }
                .buttonStyle(.plain)
            }
            ColorPicker("Custom colour", selection: value).labelsHidden()
        }
    }
}

/// Sample stroke at roughly the on-slide size (a full-screen slide is ~2.5 screen points per page point).
/// Drawn as one continuous shape, like the real ink: no overlapping dabs.
struct StrokePreview: View {
    enum Style { case even, fountain, highlighter }
    let color: Color
    let width: CGFloat
    let style: Style

    var body: some View {
        Canvas { ctx, size in
            let n = 60
            let pts = (0...n).map { i -> CGPoint in
                let x = CGFloat(i) / CGFloat(n)
                return CGPoint(x: 14 + x * (size.width - 28), y: size.height / 2 + sin(x * .pi * 2) * size.height * 0.22)
            }
            let w = width * 2.5
            switch style {
            case .even:
                ctx.stroke(Path { $0.addLines(pts) }, with: .color(color),
                           style: StrokeStyle(lineWidth: w, lineCap: .round, lineJoin: .round))
            case .highlighter:
                ctx.stroke(Path { $0.addLines(pts) }, with: .color(color.opacity(0.4)),
                           style: StrokeStyle(lineWidth: w, lineCap: .butt, lineJoin: .round))
            case .fountain:
                // Outline of a round nib whose width swells with pressure mid-stroke.
                var left: [CGPoint] = [], right: [CGPoint] = []
                for i in 0...n {
                    let a = pts[max(i - 1, 0)], b = pts[min(i + 1, n)]
                    let len = max(hypot(b.x - a.x, b.y - a.y), 0.001)
                    let half = w * (0.35 + 0.65 * sin(CGFloat(i) / CGFloat(n) * .pi)) / 2
                    let nx = -(b.y - a.y) / len * half, ny = (b.x - a.x) / len * half
                    left.append(CGPoint(x: pts[i].x + nx, y: pts[i].y + ny))
                    right.append(CGPoint(x: pts[i].x - nx, y: pts[i].y - ny))
                }
                ctx.fill(Path { $0.addLines(left + right.reversed()); $0.closeSubpath() }, with: .color(color))
                for (p, r) in [(pts[0], w * 0.175), (pts[n], w * 0.175)] {
                    ctx.fill(Path(ellipseIn: CGRect(x: p.x - r, y: p.y - r, width: 2 * r, height: 2 * r)), with: .color(color))
                }
            }
        }
        .frame(height: 54)
    }
}

/// SwiftUI content in its own UIKit view. SwiftUI content merely drawn above the ink canvas still
/// lets the canvas receive the touches (a Pencil tap on a button became a stroke); a real view on top
/// takes them itself, so the first tap with finger or Pencil always reaches the button.
struct TouchPanel<Content: View>: UIViewControllerRepresentable {
    let content: Content

    func makeUIViewController(context: Context) -> UIHostingController<Content> {
        let host = UIHostingController(rootView: content)
        host.view.backgroundColor = .clear
        host.safeAreaRegions = []
        return host
    }

    func updateUIViewController(_ host: UIHostingController<Content>, context: Context) { host.rootView = content }
}

/// Tap-to-show bar over a video: play/pause, scrubber, time. Hides itself after 3 s untouched.
struct VideoControls: View {
    @Bindable var video: VideoController
    let presenter: Presenter
    @State private var scrub: Double?

    var body: some View {
        HStack(spacing: 10) {
            Button { video.toggle(); presenter.keepControls() } label: {
                Image(systemName: video.isPlaying ? "pause.fill" : "play.fill").font(.title3).frame(width: 30, height: 30)
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier("playPause")
            .accessibilityLabel(video.isPlaying ? "Pause" : "Play")
            Slider(value: Binding(get: { scrub ?? video.time },
                                  set: { scrub = $0; video.seek(to: $0); presenter.keepControls() }),
                   in: 0...max(video.duration, 0.1)) { editing in if !editing { scrub = nil } }
            Text(Self.clock(scrub ?? video.time) + " / " + Self.clock(video.duration))
                .font(.caption.monospacedDigit()).fixedSize()
        }
        .padding(.horizontal, 12).padding(.vertical, 6)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
        .environment(\.colorScheme, .dark)
    }

    static func clock(_ t: Double) -> String {
        let s = Int(t.isFinite ? max(t, 0) : 0)
        return String(format: "%d:%02d", s / 60, s % 60)
    }
}

/// Elapsed talk time; tap for pause/resume and reset.
struct TalkTimer: View {
    let presenter: Presenter

    var body: some View {
        HStack(spacing: 6) {
            Button { presenter.toggleTimer() } label: {
                Image(systemName: presenter.timerStart == nil ? "play.fill" : "pause.fill").barTarget()
            }
            .accessibilityLabel(presenter.timerStart == nil ? "Resume Timer" : "Pause Timer")
            clock
        }
    }

    private var clock: some View {
        TimelineView(.periodic(from: .now, by: 1)) { ctx in
            Menu {
                Button(presenter.timerStart == nil ? "Resume Timer" : "Pause Timer") { presenter.toggleTimer() }
                Button("Reset Timer") { presenter.resetTimer() }
            } label: {
                let t = Int(presenter.timerElapsed(at: ctx.date))
                Text(t >= 3600 ? String(format: "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60)
                               : String(format: "%d:%02d", t / 60, t % 60))
                    .monospacedDigit().fixedSize()
                    .foregroundStyle(presenter.timerStart == nil ? .orange : .primary)
            }
        }
    }
}

/// Beamer notes for the current slide (iPad screen only).
struct NotesPanel: View {
    let presenter: Presenter

    var body: some View {
        GeometryReader { geo in
            if let doc = presenter.document, let page = doc.page(at: presenter.pageIndex),
               let notes = presenter.notesBox(page) {
                let scale = SlideGeometry.fitScale(page: notes.size, in: geo.size)
                SlideImage(doc: doc, index: presenter.pageIndex, renderer: presenter.renderer,
                           zoom: nil, notes: true, scale: scale)
                    .frame(width: notes.width * scale, height: notes.height * scale)
                    .clipped()
                    .position(x: geo.size.width / 2, y: notes.height * scale / 2 + 8)
            }
        }
        .background(Color(white: 0.12))
    }
}

/// Connecting a projector: status, and how. Apple doesn't let apps start AirPlay screen mirroring, so this
/// shows the Control Center steps; a cable or a mirrored AirPlay display is picked up automatically.
struct DisplaysSheet: View {
    @Bindable var presenter: Presenter
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Label(presenter.externalDisplayConnected ? "Projector connected: it shows the slides, videos and ink, without the toolbar."
                                                             : "No projector connected yet.",
                          systemImage: presenter.externalDisplayConnected ? "checkmark.circle.fill" : "tv")
                        .foregroundStyle(presenter.externalDisplayConnected ? .green : .secondary)
                }
                Section("AirPlay (Apple TV, AirPlay TVs and Macs)") {
                    Text("1. Open Control Center (swipe down from the top-right corner).")
                    Text("2. Tap Screen Mirroring \(Image(systemName: "rectangle.on.rectangle")) and choose the display.")
                    Text("PenCast switches the display to presentation mode by itself.").foregroundStyle(.secondary)
                }
                Section("Cable") {
                    Text("Plug in a USB-C to HDMI or DisplayPort adapter. PenCast picks it up straight away; unplugging and replugging mid-talk is fine.")
                }
                Section("While presenting") {
                    Toggle("Projector Follows Zoom", isOn: $presenter.projectorFollowsZoom)
                    if presenter.hasNotes { Toggle("Speaker Notes on iPad", isOn: $presenter.showNotes) }
                    Picker("Projector shows", selection: Binding(
                        get: { switch presenter.projector { case .black: 1; case .white: 2; case .frozen: 3; default: 0 } },
                        set: { v in
                            switch v {
                            case 1: presenter.setProjector(.black)
                            case 2: presenter.setProjector(.white)
                            case 3: if case .frozen = presenter.projector {} else { presenter.toggleFreeze() }
                            default: presenter.setProjector(.none)
                            }
                        })) {
                        Text("Slides").tag(0); Text("Black").tag(1); Text("White").tag(2); Text("Frozen").tag(3)
                    }
                }
            }
            .navigationTitle("Projector")
            .toolbar { Button("Done") { dismiss() } }
        }
    }
}

/// The system share sheet (AirDrop, Mail, Messages, Save to Files, other apps).
struct ShareSheet: UIViewControllerRepresentable {
    let items: [Any]
    func makeUIViewController(context: Context) -> UIActivityViewController {
        UIActivityViewController(activityItems: items, applicationActivities: nil)
    }
    func updateUIViewController(_ vc: UIActivityViewController, context: Context) {}
}

/// The exported PDF, handed to the system save dialog.
struct PDFFile: FileDocument {
    static var readableContentTypes: [UTType] { [.pdf] }
    let url: URL
    init(url: URL) { self.url = url }
    init(configuration: ReadConfiguration) throws { throw CocoaError(.featureUnsupported) }
    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper { try FileWrapper(url: url) }
}

struct ThumbnailGrid: View {
    let presenter: Presenter
    let done: () -> Void

    var body: some View {
        NavigationStack {
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: 220))], spacing: 16) {
                        ForEach(0..<presenter.pageCount, id: \.self) { i in
                            Button { presenter.go(to: i); done() } label: {
                                VStack {
                                    Thumbnail(page: presenter.document?.page(at: i))
                                        .border(i == presenter.pageIndex ? Color.accentColor : .clear, width: 3)
                                    Text("\(i + 1)").font(.caption)
                                }
                            }
                            .id(i)
                        }
                    }
                    .padding()
                }
                .onAppear { proxy.scrollTo(presenter.pageIndex, anchor: .center) }
            }
            .navigationTitle("Slides")
            .toolbar { Button("Done", action: done) }
        }
    }
}

struct Thumbnail: View {
    let page: PDFPage?
    var body: some View {
        // ponytail: thumbnails render on the main thread as the grid scrolls; move to SlideRenderer if 100+ slide decks stutter
        if let page {
            let full = page.bounds(for: .cropBox), slide = SlideGeometry.split(full).slide
            let img = SlideRenderer.draw(region: slide.offsetBy(dx: -full.minX, dy: -full.minY),
                                         scale: 440 / slide.width, opaque: true) { page.draw(with: .cropBox, to: $0) }
            Image(uiImage: img).resizable().aspectRatio(contentMode: .fit)
        }
    }
}

extension View {
    /// Toolbar hit area: at least 44 points square however small the icon, so taps never fall between its lines.
    func barTarget() -> some View { frame(minWidth: 44, minHeight: 44).contentShape(Rectangle()) }
}
