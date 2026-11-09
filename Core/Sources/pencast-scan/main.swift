// Spike S1: list the media PenCast finds in a PDF.
//   swift run pencast-scan deck.pdf            dump items
//   swift run pencast-scan deck.pdf --extract dir   write embedded videos to dir
//   swift run pencast-scan --check             self-check against ../Corpus/media_test.pdf
import CoreGraphics
import Foundation
import PenCastCore

let args = Array(CommandLine.arguments.dropFirst())
let check = args.contains("--check")
let path = args.first { $0.hasSuffix(".pdf") } ?? "../Corpus/media_test.pdf"

guard let scanner = MediaScanner(url: URL(fileURLWithPath: path)) else {
    print("cannot open \(path)"); exit(1)
}
let items = scanner.scan()
for i in items {
    let r = i.rect
    print(String(format: "page %2d  annot %2d  [%.1f %.1f %.1f×%.1f]  ", i.page + 1, i.annotIndex, r.minX, r.minY, r.width, r.height)
          + "\(i.kind)  autoplay=\(i.autoplay) loop=\(i.loop)")
}
// --extract <dir>: write every embedded video out (once per name), e.g. to compare with the originals.
if let i = args.firstIndex(of: "--extract"), i + 1 < args.count {
    let dir = URL(fileURLWithPath: args[i + 1])
    var done = Set<String>()
    for item in items {
        guard case .embeddedVideo(let name) = item.kind, done.insert(name).inserted else { continue }
        try scanner.extractEmbeddedVideo(item, to: dir.appendingPathComponent(name))
        print("extracted \(name)")
    }
}
guard check else { exit(0) }

// Expectations for Corpus/media_test.pdf (pages are 0-based here, slide titles in the .tex).
func expect(_ cond: Bool, _ msg: String) { if !cond { print("FAIL: \(msg)"); exit(1) } }
let byPage = Dictionary(grouping: items, by: \.page)

expect(byPage[1]?.count == 1 && byPage[1]![0].kind == .embeddedVideo(name: "clip720p30.mp4")
       && byPage[1]![0].autoplay && byPage[1]![0].loop, "slide 1: media9 autoplay+loop")
expect(byPage[2]?.first?.kind == .embeddedVideo(name: "clip1080p60.mp4")
       && byPage[2]?.first?.autoplay == false && byPage[2]?.first?.loop == false, "slide 2: media9 click-to-play")
expect(byPage[3]?.count == 2, "slide 3: two videos")
expect(byPage[4]?.first?.kind == .linkedVideo(path: "clip720p30.mp4")
       && byPage[4]?.first?.autoplay == true && byPage[4]?.first?.loop == true, "slide 4: \\movie autostart+loop")
expect(byPage[5]?.first?.kind == .animation(id: 0, frameCount: 24, fps: 12)
       && byPage[5]?.first?.autoplay == true && byPage[5]?.first?.loop == true, "slide 5: animate loop+autoplay")
expect(byPage[6]?.first?.kind == .animation(id: 1, frameCount: 16, fps: 8)
       && byPage[6]?.first?.autoplay == false && byPage[6]?.first?.loop == false, "slide 5b: animate once")
expect(items.count == 7, "no stray items (got \(items.count))")

// 16:9 video rect sanity: media9 rect must match the poster aspect, inside the page.
let r = byPage[1]![0].rect
expect(abs(r.width / r.height - 16.0 / 9) < 0.01 && r.minY >= 0, "slide 1 rect is 16:9 and on-page")

// Extraction writes an MP4 (ISO BMFF 'ftyp' box at offset 4) of the original size.
let out = FileManager.default.temporaryDirectory.appendingPathComponent("pencast-check.mp4")
try scanner.extractEmbeddedVideo(byPage[1]![0], to: out)
let data = try Data(contentsOf: out)
let original = try Data(contentsOf: URL(fileURLWithPath: path).deletingLastPathComponent().appendingPathComponent("clip720p30.mp4"))
expect(data == original, "extracted video is byte-identical to the source")

// Geometry: a 16:9 page in a 4:3 screen letterboxes vertically; PDF y flips.
let page = CGRect(x: 0, y: 0, width: 160, height: 90)
let s = SlideGeometry.fitScale(page: page.size, in: CGSize(width: 1024, height: 768))
expect(s == 6.4, "fit scale 16:9 in 4:3")
expect(SlideGeometry.canvasRect(for: CGRect(x: 10, y: 0, width: 20, height: 10), pageBox: page, scale: 2)
       == CGRect(x: 20, y: 160, width: 40, height: 20), "PDF→canvas mapping flips y")
expect(SlideGeometry.fitScale(page: page.size, in: CGSize(width: 1280, height: 800)) == 8, "16:9 in 16:10")

// Notes on second screen: right half / bottom half are notes; normal slides are untouched.
let wide = SlideGeometry.split(CGRect(x: 0, y: 0, width: 907, height: 255))
expect(wide.slide == CGRect(x: 0, y: 0, width: 453.5, height: 255) && wide.notes?.minX == 453.5, "notes on the right")
let tall = SlideGeometry.split(CGRect(x: 0, y: 0, width: 453, height: 510))
expect(tall.slide == CGRect(x: 0, y: 255, width: 453, height: 255) && tall.notes?.minY == 0, "notes at the bottom")
expect(SlideGeometry.split(page).notes == nil, "plain 16:9 slide has no notes")

// Ink keys survive inserted slides and renumbering; repeats are told apart by order.
let before = SlideGeometry.inkKeys(pageTexts: ["Intro 1/3", "Results 2/3", "Results 3/3"])
let after = SlideGeometry.inkKeys(pageTexts: ["Intro 1/4", "New slide 2/4", "Results 3/4", "Results 4/4"])
expect(before[0] == after[0] && before[1] == after[2] && before[2] == after[3], "ink keys follow their slides")
expect(before[1] != before[2], "repeated text gets distinct keys")

print("check OK")
