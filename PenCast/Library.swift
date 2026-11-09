import CoreGraphics
import PDFKit
import PenCastCore
import SwiftUI
import UniformTypeIdentifiers

/// A presentation on the home screen. The file stays where it is (bookmarked), so a recompiled
/// deck shows up updated; nothing is copied.
struct Deck: Identifiable, Codable, Hashable {
    var id = UUID()
    var bookmark: Data
    var path: String
    var title: String
    var pageCount: Int?
    var added = Date()
    var lastOpened: Date?
}

/// Every presentation the user has added or opened, persisted in Application Support/library.json.
@MainActor @Observable final class DeckLibrary {
    static let shared = DeckLibrary()
    private(set) var decks: [Deck] = []

    private static let file = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("library.json")

    /// Most recently used first.
    var sorted: [Deck] { decks.sorted { ($0.lastOpened ?? $0.added) > ($1.lastOpened ?? $1.added) } }

    init() {
        if let data = try? Data(contentsOf: Self.file),
           let saved = try? JSONDecoder().decode([Deck].self, from: data) { decks = saved }
    }

    /// Adds PDFs picked in Files, dropped in the app's Documents folder, or AirDropped.
    func add(_ urls: [URL]) {
        for url in urls { _ = entry(for: url) }
        save()
    }

    /// Called whenever a deck is presented: adds it if new and marks it recently used.
    func opened(_ url: URL, pageCount: Int) {
        guard !url.path.hasPrefix(Bundle.main.bundlePath), let i = entry(for: url) else { return }
        decks[i].lastOpened = Date()
        decks[i].pageCount = pageCount
        save()
    }

    func remove(_ deck: Deck) {
        decks.removeAll { $0.id == deck.id }
        save()
    }

    /// Resolves the bookmark, refreshing it if the file moved. nil = file is gone.
    func url(for deck: Deck) -> URL? {
        var stale = false
        guard let url = try? URL(resolvingBookmarkData: deck.bookmark, bookmarkDataIsStale: &stale),
              FileManager.default.fileExists(atPath: url.path) else { return nil }
        if stale, let i = decks.firstIndex(of: deck), let fresh = Self.bookmark(url) {
            decks[i].bookmark = fresh; decks[i].path = url.path; save()
        }
        return url
    }

    /// iPad: PDFs put in the app's own folder (Finder file sharing, Files "On My iPad", AirDrop inbox)
    /// appear automatically. Not on Mac, where Documents is the user's whole ~/Documents.
    func scanAppFolder() {
        guard !isMac else { return }
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let pdfs = [docs, docs.appendingPathComponent("Inbox")].flatMap { dir in
            ((try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? [])
                .filter { $0.pathExtension.lowercased() == "pdf" }
        }
        let known = Set(decks.map(\.path))
        let new = pdfs.filter { !known.contains($0.standardizedFileURL.path) }
        if !new.isEmpty { add(new) }
    }

    private func entry(for url: URL) -> Int? {
        let path = url.standardizedFileURL.path
        if let i = decks.firstIndex(where: { $0.path == path }) { return i }
        guard let bookmark = Self.bookmark(url) else { return nil }
        decks.append(Deck(bookmark: bookmark, path: path, title: url.deletingPathExtension().lastPathComponent))
        return decks.count - 1
    }

    private static func bookmark(_ url: URL) -> Data? {
        let ok = url.startAccessingSecurityScopedResource()
        defer { if ok { url.stopAccessingSecurityScopedResource() } }
        return try? url.bookmarkData()
    }

    private func save() {
        try? FileManager.default.createDirectory(at: Self.file.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? JSONEncoder().encode(decks).write(to: Self.file, options: .atomic)
    }
}

// MARK: - Home screen

struct HomeView: View {
    let presenter: Presenter
    let library = DeckLibrary.shared
    @State private var importing = false
    @State private var search = ""
    @State private var showingAbout = false

    private var shown: [Deck] {
        search.isEmpty ? library.sorted : library.sorted.filter { $0.title.localizedCaseInsensitiveContains(search) }
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                if let error = presenter.openError {
                    Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red).padding()
                }
                if library.decks.isEmpty {
                    ContentUnavailableView {
                        Label("No presentations yet", systemImage: "rectangle.stack")
                    } description: {
                        Text("Add Beamer PDFs from Files, or open one from AirDrop or another app.")
                    } actions: {
                        Button("Add Presentations…") { importing = true }.buttonStyle(.borderedProminent)
                    }
                    .padding(.top, 80)
                } else {
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: 250, maximum: 360), spacing: 28)], spacing: 32) {
                        ForEach(shown) { deck in
                            DeckCard(deck: deck)
                                .onTapGesture { open(deck) }
                                .contextMenu {
                                    Button { open(deck) } label: { Label("Present", systemImage: "play.rectangle") }
                                    Button(role: .destructive) { library.remove(deck) } label: {
                                        Label("Remove from Library", systemImage: "trash")
                                    }
                                }
                        }
                    }
                    .padding(28)
                }
                samples
            }
            .navigationTitle("Presentations")
            .searchable(text: $search, prompt: "Search presentations")
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button { showingAbout = true } label: { Label("About PenCast", systemImage: "info.circle") }
                }
                ToolbarItem(placement: .primaryAction) {
                    Button { importing = true } label: { Label("Add Presentations", systemImage: "plus") }
                }
            }
            .sheet(isPresented: $showingAbout) { AboutView() }
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.pdf], allowsMultipleSelection: true) { result in
            if case .success(let urls) = result { library.add(urls) }
        }
        .onAppear { library.scanAppFolder() }
    }

    private func open(_ deck: Deck) {
        guard let url = library.url(for: deck) else {
            presenter.openError = "\(deck.title) was moved or deleted. Remove it and add it again."
            return
        }
        presenter.open(url)
    }

    /// Bundled test decks for checking playback and performance.
    private var samples: some View {
        DisclosureGroup("Test decks") {
            HStack(spacing: 16) {
                ForEach(["PenCast_Demo", "media_test", "stress_100"], id: \.self) { name in
                    if let url = Bundle.main.url(forResource: name, withExtension: "pdf") {
                        Button(name) { presenter.open(url) }.buttonStyle(.bordered)
                    }
                }
                Spacer()
            }
            .padding(.top, 8)
        }
        .padding(.horizontal, 28).padding(.bottom, 28)
        .foregroundStyle(.secondary)
    }
}

struct DeckCard: View {
    let deck: Deck
    @State private var thumbnail: UIImage?
    @State private var missing = false

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ZStack {
                Rectangle().fill(Color.white)
                if let thumbnail { Image(uiImage: thumbnail).resizable().aspectRatio(contentMode: .fit) }
                if missing {
                    Label("File not found", systemImage: "questionmark.folder")
                        .font(.caption).padding(6).background(.thinMaterial, in: Capsule())
                }
            }
            .aspectRatio(16 / 9, contentMode: .fit)
            .clipShape(RoundedRectangle(cornerRadius: 10))
            .overlay(RoundedRectangle(cornerRadius: 10).stroke(.quaternary))
            .shadow(color: .black.opacity(0.15), radius: 6, y: 3)

            Text(deck.title).font(.headline).lineLimit(2)
            HStack(spacing: 4) {
                if let n = deck.pageCount { Text("\(n) slides ·") }
                if let date = deck.lastOpened { Text(date, format: .relative(presentation: .named)) } else { Text("Not opened yet") }
            }
            .font(.caption).foregroundStyle(.secondary)
        }
        .contentShape(Rectangle())
        .task(id: deck.id) {
            guard let url = DeckLibrary.shared.url(for: deck) else { missing = true; return }
            thumbnail = await Self.render(url)
        }
    }

    private static let cache = NSCache<NSString, UIImage>()

    /// First page via CGPDF (thread-safe), off the main thread; cached per file version.
    static func render(_ url: URL) async -> UIImage? {
        let modified = (try? url.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate
        let key = "\(url.path)@\(modified?.timeIntervalSince1970 ?? 0)" as NSString
        if let hit = cache.object(forKey: key) { return hit }
        let img = await Task.detached(priority: .utility) { () -> UIImage? in
            let ok = url.startAccessingSecurityScopedResource()
            defer { if ok { url.stopAccessingSecurityScopedResource() } }
            guard let page = CGPDFDocument(url as CFURL)?.page(at: 1) else { return nil }
            let box = SlideGeometry.split(page.getBoxRect(.cropBox)).slide   // without Beamer notes
            let size = CGSize(width: 640, height: (640 * box.height / box.width).rounded())
            return UIGraphicsImageRenderer(size: size).image { ctx in
                UIColor.white.setFill(); ctx.fill(CGRect(origin: .zero, size: size))
                let cg = ctx.cgContext
                cg.translateBy(x: 0, y: size.height); cg.scaleBy(x: 1, y: -1)
                cg.scaleBy(x: size.width / box.width, y: size.width / box.width)
                cg.translateBy(x: -box.minX, y: -box.minY)
                cg.drawPDFPage(page)
            }
        }.value
        if let img { cache.setObject(img, forKey: key) }
        return img
    }
}

/// The ⓘ page on the home screen: version, and where to find and support PenCast.
struct AboutView: View {
    @Environment(\.dismiss) private var dismiss
    private var version: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? ""
    }

    var body: some View {
        NavigationStack {
            List {
                Section {
                    VStack(spacing: 8) {
                        Image("AboutIcon").resizable().frame(width: 96, height: 96)
                        Text("PenCast").font(.title.bold())
                        Text("Version \(version) beta").foregroundStyle(.secondary)
                        Text("Present Beamer PDFs with their videos, animations and your handwriting.")
                            .multilineTextAlignment(.center).padding(.top, 4)
                    }
                    .frame(maxWidth: .infinity).padding(.vertical, 8)
                }
                // ponytail: App Store review rejects donation links in apps (guideline 3.1.1); drop this row there.
                Section("Support") {
                    Link(destination: URL(string: "https://ko-fi.com/gregoryjanes")!) {
                        Label("Support PenCast on Ko-fi", systemImage: "cup.and.saucer")
                    }
                }
                Section("GitHub") {
                    Link(destination: URL(string: "https://github.com/e1ind")!) {
                        Label("e1ind", systemImage: "chevron.left.forwardslash.chevron.right")
                    }
                    Link(destination: URL(string: "https://github.com/gregoryjanes")!) {
                        Label("gregoryjanes", systemImage: "person.crop.circle")
                    }
                }
            }
            .navigationTitle("About")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { Button("Done") { dismiss() } }
        }
    }
}
