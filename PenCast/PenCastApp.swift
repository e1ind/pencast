import AVFoundation
import SwiftUI
import UIKit

@main
struct PenCastApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate

    var body: some Scene {
        WindowGroup {
            RootView(presenter: .shared)
                .onOpenURL { Presenter.shared.open($0) }   // Files "Open in", AirDrop, share sheet
                .onAppear {
                    setMinimumWindowSize(presenting: Presenter.shared.presenting)
                    // Test hook: `-openDeck media_test [-openPage 5]` opens a bundled deck (or an absolute path).
                    let d = UserDefaults.standard
                    if let name = d.string(forKey: "openDeck"), Presenter.shared.document == nil,
                       let url = name.hasPrefix("/") ? URL(fileURLWithPath: name)
                                                     : Bundle.main.url(forResource: name, withExtension: "pdf") {
                        Presenter.shared.open(url)
                        Presenter.shared.go(to: d.integer(forKey: "openPage"))
                        Presenter.shared.present()
                        let z = d.double(forKey: "selfTestZoom")   // test hook: `-selfTestZoom 3` zooms in
                        if z > 1 { Presenter.shared.zoomStep(z) }
                        // Test hook: `-selfTestExport notes|all` writes the export and prints where.
                        if let mode = d.string(forKey: "selfTestExport") {
                            Task {
                                let t = Date()
                                let url = await Presenter.shared.exportAnnotated(notesOnly: mode == "notes")
                                print("selfTestExport: \(url?.path ?? "failed") in \(String(format: "%.1f", -t.timeIntervalSinceNow)) s")
                            }
                        }
                    }
                }
        }
        .commands { SlideCommands() }
        WindowGroup("Projector", id: "projector") {
            ProjectorView(presenter: .shared, inWindow: true)
        }
    }
}

/// Slide menu. Menu shortcuts work whatever has focus (Mac menu bar; iPad hardware keyboard).
/// Bluetooth clickers send arrows, Page Up/Down or space.
struct SlideCommands: Commands {
    var body: some Commands {
        CommandMenu("Slide") {
            Button("Next Slide") { Presenter.shared.next() }.keyboardShortcut(.rightArrow, modifiers: [])
            Button("Previous Slide") { Presenter.shared.previous() }.keyboardShortcut(.leftArrow, modifiers: [])
            Divider()
            Button("Next Slide (↓)") { Presenter.shared.next() }.keyboardShortcut(.downArrow, modifiers: [])
            Button("Previous Slide (↑)") { Presenter.shared.previous() }.keyboardShortcut(.upArrow, modifiers: [])
            Button("Next Slide (Page Down)") { Presenter.shared.next() }.keyboardShortcut(.pageDown, modifiers: [])
            Button("Previous Slide (Page Up)") { Presenter.shared.previous() }.keyboardShortcut(.pageUp, modifiers: [])
            Button("Next Slide (Space)") { Presenter.shared.next() }.keyboardShortcut(.space, modifiers: [])
            Divider()
            Button("Zoom In") { Presenter.shared.zoomStep(1.25) }.keyboardShortcut("=", modifiers: .command)
            Button("Zoom Out") { Presenter.shared.zoomStep(0.8) }.keyboardShortcut("-", modifiers: .command)
            Button("Whole Slide") { Presenter.shared.resetZoom() }.keyboardShortcut("0", modifiers: .command)
            Divider()
            Button("Black Screen") { Presenter.shared.setProjector(.black) }.keyboardShortcut("b", modifiers: [])
            Button("White Screen") { Presenter.shared.setProjector(.white) }.keyboardShortcut("w", modifiers: [])
            Button("Freeze Projector") { Presenter.shared.toggleFreeze() }.keyboardShortcut("f", modifiers: [])
            Button("Speaker Notes") { Presenter.shared.showNotes.toggle() }.keyboardShortcut("n", modifiers: [])
            Divider()
            Button("Laser Pointer") { Presenter.shared.tool = .laser }.keyboardShortcut("l", modifiers: [])
            Button("Pointer") { Presenter.shared.tool = .pointer }.keyboardShortcut("p", modifiers: [])
        }
    }
}

/// Mac: the presenter toolbar needs about 1100 points, so the window can't be narrower while presenting.
/// The library and reading view can shrink to share the screen with a LaTeX editor.
@MainActor func setMinimumWindowSize(presenting: Bool) {
    #if targetEnvironment(macCatalyst)
    for case let s as UIWindowScene in UIApplication.shared.connectedScenes where s.session.role == .windowApplication {
        s.sizeRestrictions?.minimumSize = presenting ? CGSize(width: 1180, height: 760) : CGSize(width: 420, height: 320)
    }
    #endif
}

struct RootView: View {
    let presenter: Presenter
    var body: some View {
        Group {
            if presenter.document == nil { HomeView(presenter: presenter) }
            else if presenter.presenting { PresenterView(presenter: presenter) }
            else { ReaderView(presenter: presenter) }
        }
        .onChange(of: presenter.presenting) { _, presenting in setMinimumWindowSize(presenting: presenting) }
    }
}

// MARK: - External display (spec 5.1 Display manager)

final class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ app: UIApplication, didFinishLaunchingWithOptions _: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        // Video sound plays even with the silent switch on.
        try? AVAudioSession.sharedInstance().setCategory(.playback, mode: .moviePlayback)
        return true
    }

    func application(_ app: UIApplication, configurationForConnecting session: UISceneSession,
                     options: UIScene.ConnectionOptions) -> UISceneConfiguration {
        print("PenCast scene connecting, role: \(session.role.rawValue)")
        if session.role == .windowExternalDisplayNonInteractive {
            let config = UISceneConfiguration(name: "Projector", sessionRole: session.role)
            config.delegateClass = ProjectorSceneDelegate.self
            return config
        }
        let config = UISceneConfiguration(name: nil, sessionRole: session.role)
        return config   // main role: SwiftUI supplies its own delegate
    }
}

/// Projector window: slide + media + ink from the shared presenter, no controls.
/// Unplug/replug simply tears down and recreates this scene; all state lives in Presenter.
@MainActor final class ProjectorSceneDelegate: NSObject, UIWindowSceneDelegate {
    var window: UIWindow?

    func scene(_ scene: UIScene, willConnectTo _: UISceneSession, options _: UIScene.ConnectionOptions) {
        guard let scene = scene as? UIWindowScene else { return }
        print("PenCast projector scene connected: \(scene.screen.bounds.size)")
        let window = UIWindow(windowScene: scene)
        window.rootViewController = UIHostingController(rootView: ProjectorView(presenter: .shared))
        window.isHidden = false
        self.window = window
        Presenter.shared.externalDisplayConnected = true
    }

    func sceneDidDisconnect(_: UIScene) {
        window = nil
        Presenter.shared.externalDisplayConnected = false
    }
}

struct ProjectorView: View {
    let presenter: Presenter
    /// A projector *window* (Mac, iPad Stage Manager) rather than an external display: tapping it shows a
    /// way out, otherwise on iPad it fills the screen with no controls.
    var inWindow = false
    @Environment(\.dismissWindow) private var dismissWindow
    @State private var showClose = false

    var body: some View {
        ZStack {
            if presenter.document != nil && presenter.presenting { SlideView(presenter: presenter, interactive: false) } else { Color.black }
            switch presenter.projector {
            case .none: EmptyView()
            case .black: Color.black
            case .white: Color.white
            case .frozen(let image): ZStack { Color.black; Image(uiImage: image).resizable().scaledToFit() }
            }
        }
        .ignoresSafeArea()
        .overlay(alignment: .top) {
            if inWindow && showClose {
                Button { dismissWindow(id: "projector") } label: {
                    Label("Close Projector Window", systemImage: "xmark.circle.fill")
                        .font(.headline).padding(.horizontal, 18).padding(.vertical, 10)
                        .background(.regularMaterial, in: Capsule())
                }
                .buttonStyle(.plain).padding(.top, 24).transition(.opacity)
            }
        }
        .contentShape(Rectangle())
        .onTapGesture {
            guard inWindow else { return }
            withAnimation { showClose = true }
            Task { try? await Task.sleep(for: .seconds(4)); withAnimation { showClose = false } }
        }
        .onAppear { presenter.projectorsShown += 1 }
        .onDisappear { presenter.projectorsShown -= 1 }
    }
}

/// Folders the user granted for `\movie` linked videos, as security-scoped bookmarks.
enum FolderAccess {
    private static let key = "folders"

    static func add(_ folder: URL) {
        let ok = folder.startAccessingSecurityScopedResource()
        defer { if ok { folder.stopAccessingSecurityScopedResource() } }
        guard let data = try? folder.bookmarkData() else { return }
        var all = UserDefaults.standard.array(forKey: key) as? [Data] ?? []
        all.append(data)
        UserDefaults.standard.set(all, forKey: key)
    }

    /// Starts access on every granted folder that contains `deck`; caller stops it on close.
    static func restore(containing deck: URL) -> [URL] {
        let dir = deck.deletingLastPathComponent().standardizedFileURL.path
        return (UserDefaults.standard.array(forKey: key) as? [Data] ?? []).compactMap { data in
            var stale = false
            guard let url = try? URL(resolvingBookmarkData: data, bookmarkDataIsStale: &stale),
                  dir.hasPrefix(url.standardizedFileURL.path),
                  url.startAccessingSecurityScopedResource() else { return nil }
            return url
        }
    }
}
