import PencilKit
import SwiftUI
import UIKit
import UIKit.UIGestureRecognizerSubclass

/// Fountain pen pressure response: width grows from `fine × size` at the lightest touch to `size` when
/// pressing with `firm` of the Pencil's full force (or harder). PencilKit's own round pen barely reacts
/// to pressure, so the fountain pen is drawn here and saved as a PencilKit stroke with these widths.
struct PressureCurve {
    var size: CGFloat
    var fine: CGFloat   // thinnest width as a fraction of `size`
    var firm: CGFloat   // fraction of full Pencil force that gives the thickest line

    /// `force` 0…1; nil when the input has no pressure (mouse), which draws a mid-weight line.
    func width(_ force: CGFloat?) -> CGFloat {
        guard let force else { return size * (fine + (1 - fine) * 0.5) }
        let x = min(max(force / max(firm, 0.05), 0), 1)
        return size * (fine + (1 - fine) * x * x * (3 - 2 * x))   // smoothstep: light touches stay fine
    }
}

struct InkSample { var p: CGPoint; var w: CGFloat; var t: TimeInterval; var force: CGFloat }

/// Collects one stroke at full Pencil resolution (coalesced touches) plus predicted points for lower
/// perceived latency. Points are view coordinates divided by `zoom`, i.e. the canvas's content coordinates.
final class FountainRecognizer: UIGestureRecognizer {
    var curve = PressureCurve(size: 2, fine: 0.3, firm: 0.55)
    var zoom: CGFloat = 1
    private(set) var samples: [InkSample] = []
    private(set) var predicted: [InkSample] = []
    private var touch: UITouch?
    private var width: CGFloat?
    private var start: TimeInterval = 0

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent) {
        guard touch == nil, let t = touches.first else {
            touches.forEach { ignore($0, for: event) }
            return
        }
        touch = t
        start = t.timestamp
        add(t, event)
        state = .began
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent) {
        guard let t = touch, touches.contains(t) else { return }
        add(t, event)
        state = .changed
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent) {
        guard let t = touch, touches.contains(t) else { return }
        // No samples from the lift itself: as the tip leaves the glass its reported position jumps a few
        // points (usually up, with the Pencil's tilt), which drew a small tick at the end of every stroke.
        if samples.count < 2 { add(t, event) }   // a tap still makes a dot
        // Same for the last moves before the lift, which arrive with almost no pressure.
        while samples.count > 2, let last = samples.last, last.force < 0.02 { samples.removeLast() }
        predicted = []
        state = .ended
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent) { state = .cancelled }

    // Nothing else on the page may cancel a stroke in progress (a cancelled stroke would be lost).
    override func canBePrevented(by other: UIGestureRecognizer) -> Bool { false }

    override func reset() {
        super.reset()
        touch = nil; samples = []; predicted = []; width = nil
    }

    private func add(_ t: UITouch, _ e: UIEvent) {
        for c in e.coalescedTouches(for: t) ?? [t] { samples.append(sample(c, keep: true)) }
        predicted = (e.predictedTouches(for: t) ?? []).map { sample($0, keep: false) }
    }

    private func sample(_ t: UITouch, keep: Bool) -> InkSample {
        let force: CGFloat? = t.maximumPossibleForce > 0 ? t.force / t.maximumPossibleForce : nil
        var w = curve.width(force)
        // Ease width changes so pressure jitter never shows as a lumpy line.
        if let prev = width { w = prev + (w - prev) * 0.3 }
        if keep { width = w }
        let loc = t.preciseLocation(in: view)
        return InkSample(p: CGPoint(x: loc.x / zoom, y: loc.y / zoom), w: w, t: t.timestamp - start, force: force ?? 0.5)
    }
}

enum NibPath {
    /// Filled outline of a round nib moving through `samples`, at `scale` screen points per content point.
    /// One capsule per segment, all wound the same way, so the non-zero fill is their seamless union:
    /// no overlapping dabs and no gaps, at any opacity.
    static func path(_ s: [InkSample], scale: CGFloat) -> CGPath {
        let path = CGMutablePath()
        let pt = { (x: InkSample) in CGPoint(x: x.p.x * scale, y: x.p.y * scale) }
        let steps = 8
        for i in s.indices.dropFirst() {
            let a = pt(s[i - 1]), b = pt(s[i])
            let ra = s[i - 1].w * scale / 2, rb = s[i].w * scale / 2
            let dx = b.x - a.x, dy = b.y - a.y
            guard hypot(dx, dy) > 0.01 else { continue }
            let theta = atan2(dx, -dy)   // direction of the normal (-dy, dx)
            path.move(to: CGPoint(x: a.x + cos(theta) * ra, y: a.y + sin(theta) * ra))
            for k in 0...steps {   // front cap around b
                let ang = theta - .pi * CGFloat(k) / CGFloat(steps)
                path.addLine(to: CGPoint(x: b.x + cos(ang) * rb, y: b.y + sin(ang) * rb))
            }
            for k in 0...steps {   // back cap around a
                let ang = theta + .pi - .pi * CGFloat(k) / CGFloat(steps)
                path.addLine(to: CGPoint(x: a.x + cos(ang) * ra, y: a.y + sin(ang) * ra))
            }
            path.closeSubpath()
        }
        if path.isEmpty, let first = s.first {   // a tap: a dot
            let c = pt(first), r = first.w * scale / 2
            path.addEllipse(in: CGRect(x: c.x - r, y: c.y - r, width: 2 * r, height: 2 * r))
        }
        return path
    }

    /// The finished stroke as PencilKit ink, so undo, eraser, saving and export treat it like any other.
    static func stroke(_ s: [InkSample], color: UIColor) -> PKStroke {
        var pts = s.map {
            PKStrokePoint(location: $0.p, timeOffset: $0.t, size: pointSize(forWidth: $0.w),
                          opacity: 1, force: $0.force, azimuth: 0, altitude: .pi / 2)
        }
        if pts.count == 1 { pts.append(pts[0]) }
        return PKStroke(ink: PKInk(.monoline, color: color), path: PKStrokePath(controlPoints: pts, creationDate: Date()))
    }
}

extension NibPath {
    /// PencilKit draws a point of size `s` as a line `2 × (s − 2)` points wide, and nothing at all
    /// below s = 2 (measured on macOS, identical at render scales 2×–16× and for .pen and .monoline).
    /// So a fine fountain line given its true width simply vanished. Invert the mapping.
    static func pointSize(forWidth w: CGFloat) -> CGSize {
        let s = max(w, 0.05) / 2 + 2
        return CGSize(width: s, height: s)
    }
}

/// Scratch area in the pen menu for trying the pressure settings.
struct PressurePad: UIViewRepresentable {
    let curve: PressureCurve
    let color: UIColor
    let clearToken: Int

    final class PadView: UIView {
        let done = CAShapeLayer(), live = CAShapeLayer()
        let recognizer = FountainRecognizer()
        private var strokes = CGMutablePath()
        var clearToken = 0
        static let scale: CGFloat = 2.5   // about a full-screen slide's zoom, so widths match the slide

        override init(frame: CGRect) {
            super.init(frame: frame)
            backgroundColor = .white
            layer.cornerRadius = 10
            layer.masksToBounds = true
            for l in [done, live] { l.fillRule = .nonZero; layer.addSublayer(l) }
            recognizer.zoom = Self.scale
            recognizer.addTarget(self, action: #selector(moved))
            addGestureRecognizer(recognizer)
        }
        required init?(coder: NSCoder) { fatalError() }

        @objc private func moved() {
            CATransaction.begin(); CATransaction.setDisableActions(true)
            switch recognizer.state {
            case .began, .changed:
                live.path = NibPath.path(recognizer.samples + recognizer.predicted, scale: Self.scale)
            case .ended:
                strokes.addPath(NibPath.path(recognizer.samples, scale: Self.scale))
                done.path = strokes
                live.path = nil
            default:
                live.path = nil
            }
            CATransaction.commit()
        }

        func clear() { strokes = CGMutablePath(); done.path = nil }
    }

    func makeUIView(context: Context) -> PadView { PadView() }

    func updateUIView(_ v: PadView, context: Context) {
        v.recognizer.curve = curve
        let c = color.resolvedColor(with: UITraitCollection(userInterfaceStyle: .light)).cgColor
        v.done.fillColor = c
        v.live.fillColor = c
        if v.clearToken != clearToken { v.clearToken = clearToken; v.clear() }
    }
}

/// Partial eraser: cuts away the ink inside a circle dragged along a path. PencilKit's own pixel eraser
/// ignores the width it is given (measured: a 15-point cut whatever the setting), so large erasers are
/// done here, on the strokes themselves.
enum PartialEraser {
    /// `d` with everything within `r` of the segment a→b removed (drawing coordinates); nil if nothing was hit.
    static func erase(_ d: PKDrawing, from a: CGPoint, to b: CGPoint, radius r: CGFloat) -> PKDrawing? {
        let reach = CGRect(x: min(a.x, b.x) - r, y: min(a.y, b.y) - r, width: abs(a.x - b.x) + 2 * r, height: abs(a.y - b.y) + 2 * r)
        var strokes: [PKStroke] = [], changed = false
        for s in d.strokes {
            guard s.renderBounds.intersects(reach) else { strokes.append(s); continue }
            // Dense points in drawing coordinates, split into the runs that lie outside the eraser.
            var runs: [[PKStrokePoint]] = [[]], cut = false
            for p in s.path.interpolatedPoints(by: .distance(1)) {
                let at = p.location.applying(s.transform)
                // PencilKit draws a point of size s about 2(s − 2) wide (see `NibPath.pointSize`).
                if distance(at, a, b) <= r + max(p.size.width - 2, 0.5) {
                    cut = true
                    if !runs[runs.count - 1].isEmpty { runs.append([]) }
                } else {
                    runs[runs.count - 1].append(PKStrokePoint(location: at, timeOffset: p.timeOffset, size: p.size,
                                                              opacity: p.opacity, force: p.force, azimuth: p.azimuth,
                                                              altitude: p.altitude))
                }
            }
            guard cut else { strokes.append(s); continue }
            changed = true
            let mask = s.mask.map { m -> UIBezierPath in let c = m.copy() as! UIBezierPath; c.apply(s.transform); return c }
            for run in runs where run.count > 1 {
                strokes.append(PKStroke(ink: s.ink, path: PKStrokePath(controlPoints: run, creationDate: s.path.creationDate),
                                        transform: .identity, mask: mask))
            }
        }
        return changed ? PKDrawing(strokes: strokes) : nil
    }

    /// Distance from `p` to the segment a→b.
    static func distance(_ p: CGPoint, _ a: CGPoint, _ b: CGPoint) -> CGFloat {
        let dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy
        let t = len2 > 0 ? min(max(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0), 1) : 0
        return hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy))
    }
}
