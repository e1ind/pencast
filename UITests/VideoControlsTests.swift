import XCTest

/// Tap a video, then its play/pause button: the very first tap on the button must work.
final class VideoControlsTests: XCTestCase {
    func testFirstTapOnPlayPauseWorks() {
        let app = XCUIApplication()
        app.launch()
        // Open the bundled demo deck the way a user would, then go to the vortex slide (page 5).
        app.buttons["Test decks"].tap()
        app.buttons["PenCast_Demo"].tap()
        for _ in 0..<4 { app.buttons["Forward"].tap() }

        let slide = app.descendants(matching: .any)["slide"].firstMatch
        XCTAssertTrue(slide.waitForExistence(timeout: 10), "slide shown")
        sleep(2)                                                   // video loads
        // Tap the video: its centre sits at (0.31, 0.49) of the slide (see PenCast_Demo.tex, slide 5).
        slide.coordinate(withNormalizedOffset: CGVector(dx: 0.31, dy: 0.49)).tap()

        let button = app.buttons["playPause"]
        XCTAssertTrue(button.waitForExistence(timeout: 3), "controls appear")
        XCTAssertEqual(button.label, "Play")
        button.tap()
        XCTAssertTrue(waitFor(button, label: "Pause"), "first tap starts playback")
        button.tap()
        XCTAssertTrue(waitFor(button, label: "Play"), "second tap pauses")
    }

    /// Pinching in the middle of a slide must zoom into the middle, not drift somewhere else.
    func testPinchZoomsWhereYouPinch() {
        let app = XCUIApplication()
        app.launch()
        app.buttons["Test decks"].tap()
        app.buttons["PenCast_Demo"].tap()
        app.buttons["Forward"].tap()                                  // page 2: no media
        let slide = app.descendants(matching: .any)["slide"].firstMatch
        XCTAssertTrue(slide.waitForExistence(timeout: 10))
        XCTAssertEqual(slide.value as? String, "0.500 0.500 1.000")
        slide.pinch(withScale: 2.5, velocity: 1.5)
        let parts = (slide.value as? String ?? "").split(separator: " ").compactMap { Double($0) }
        XCTAssertEqual(parts.count, 3)
        XCTAssertEqual(parts[0], 0.5, accuracy: 0.03, "zoom centre x stays under the fingers")
        XCTAssertEqual(parts[1], 0.5, accuracy: 0.03, "zoom centre y stays under the fingers")
        XCTAssertLessThan(parts[2], 0.7, "actually zoomed in")
    }

    /// While zoomed, dragging one finger moves the zoomed view.
    func testOneFingerDragMovesZoomedView() {
        let app = XCUIApplication()
        app.launch()
        app.buttons["Test decks"].tap()
        app.buttons["PenCast_Demo"].tap()
        app.buttons["Forward"].tap()                                  // page 2: no media
        let slide = app.descendants(matching: .any)["slide"].firstMatch
        XCTAssertTrue(slide.waitForExistence(timeout: 10))
        slide.pinch(withScale: 2.5, velocity: 1.5)
        let before = zoomState(slide)
        XCTAssertLessThan(before[2], 0.7, "zoomed in first")
        // Drag the page to the left: the view moves right over the slide.
        slide.coordinate(withNormalizedOffset: CGVector(dx: 0.7, dy: 0.5))
            .press(forDuration: 0.05, thenDragTo: slide.coordinate(withNormalizedOffset: CGVector(dx: 0.4, dy: 0.5)))
        sleep(1)   // a released drag glides briefly before it settles
        let after = zoomState(slide)
        XCTAssertGreaterThan(after[0] - before[0], 0.05, "one-finger drag moved the zoomed view")
        XCTAssertEqual(after[2], before[2], accuracy: 0.001, "dragging doesn't change the zoom")
        XCTAssertEqual(app.staticTexts["2 / 18"].exists, true, "still on the same slide")
        // The ink canvas must sit over the same region the slide shows, or writing lands elsewhere.
        let ink = app.descendants(matching: .any)["ink"].firstMatch
        XCTAssertTrue(ink.exists)
        let inkOrigin = (ink.value as? String ?? "").split(separator: " ").compactMap { Double($0) }
        let slideH = after[2]   // zoom keeps the slide's aspect, so height fraction == width fraction
        XCTAssertEqual(inkOrigin[0], after[0] - after[2] / 2, accuracy: 0.01, "ink canvas x matches the zoomed view")
        XCTAssertEqual(inkOrigin[1], after[1] - slideH / 2, accuracy: 0.01, "ink canvas y matches the zoomed view")
    }

    /// Writing on a zoomed slide: the ink must land exactly under the finger (fountain pen and ball pen).
    func testInkLandsUnderFingerWhenZoomed() {
        for style in ["fountain", "ball"] {
            let app = XCUIApplication()
            app.launchArguments = ["-fingerInk", "YES", "-freshInk", "YES", "-penStyle", style]
            app.launch()
            app.buttons["Test decks"].tap()
            app.buttons["PenCast_Demo"].tap()
            app.buttons["Forward"].tap()                              // page 2: no media
            let slide = app.descendants(matching: .any)["slide"].firstMatch
            XCTAssertTrue(slide.waitForExistence(timeout: 10))
            slide.pinch(withScale: 2.5, velocity: 1.5)
            sleep(1)
            let z = zoomState(slide)
            slide.coordinate(withNormalizedOffset: CGVector(dx: 0.3, dy: 0.4))
                .press(forDuration: 0.05, thenDragTo: slide.coordinate(withNormalizedOffset: CGVector(dx: 0.6, dy: 0.6)))
            sleep(1)
            let shot = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
            shot.name = "inktest-\(style)"; shot.lifetime = .keepAlways; add(shot)
            let ink = app.descendants(matching: .any)["ink"].firstMatch
            let got = ink.label.split(separator: " ").dropFirst().compactMap { Double($0) }   // "n=K x y"
            XCTAssertEqual(got.count, 2, "\(style): a stroke was drawn (label: \(ink.label))")
            guard got.count == 2 else { continue }
            // Middle of the drag is 0.45, 0.5 of the visible view; convert to fractions of the slide.
            let expectX = z[0] - z[2] / 2 + 0.45 * z[2], expectY = z[1] - z[2] / 2 + 0.5 * z[2]
            XCTAssertEqual(got[0], expectX, accuracy: 0.02, "\(style): ink x under the finger")
            XCTAssertEqual(got[1], expectY, accuracy: 0.02, "\(style): ink y under the finger")
            app.terminate()
        }
    }

    /// One Undo removes the stroke just drawn, and Redo brings it back (PencilKit's late change reports
    /// used to be recorded as extra steps, so the first Undo did nothing).
    func testUndoRedoAfterOneStroke() {
        for style in ["fountain", "ball"] {
            let app = XCUIApplication()
            app.launchArguments = ["-fingerInk", "YES", "-freshInk", "YES", "-penStyle", style]
            app.launch()
            app.buttons["Test decks"].tap()
            app.buttons["PenCast_Demo"].tap()
            app.buttons["Forward"].tap()
            let slide = app.descendants(matching: .any)["slide"].firstMatch
            XCTAssertTrue(slide.waitForExistence(timeout: 10))
            slide.coordinate(withNormalizedOffset: CGVector(dx: 0.3, dy: 0.4))
                .press(forDuration: 0.05, thenDragTo: slide.coordinate(withNormalizedOffset: CGVector(dx: 0.6, dy: 0.6)))
            sleep(1)
            let undo = app.buttons["Undo"], redo = app.buttons["Redo"]
            XCTAssertTrue(undo.isEnabled, "\(style): stroke can be undone")
            undo.tap()
            sleep(1)
            XCTAssertFalse(undo.isEnabled, "\(style): one Undo removed the only stroke")
            XCTAssertTrue(redo.isEnabled, "\(style): Redo available after Undo")
            redo.tap()
            sleep(1)
            XCTAssertTrue(undo.isEnabled, "\(style): Redo restored the stroke")
            XCTAssertFalse(redo.isEnabled, "\(style): nothing left to redo")
            app.terminate()
        }
    }

    /// A tap anywhere on a tool button picks it, not only on the icon's lines.
    func testToolButtonsPickOnFirstTap() {
        let app = XCUIApplication()
        app.launch()
        app.buttons["Test decks"].tap()
        app.buttons["PenCast_Demo"].tap()
        for tool in ["laser", "pen", "highlighter", "eraser", "lasso", "pointer"] {
            let b = app.buttons[tool]
            b.coordinate(withNormalizedOffset: CGVector(dx: 0.12, dy: 0.15)).tap()   // near a corner
            XCTAssertTrue(b.isSelected, "\(tool) picked by one tap near its edge")
        }
    }

    /// Pinching out past the whole slide springs back to it, and gestures keep working after a zoom.
    func testZoomOutPastWholeSlideSpringsBack() {
        let app = XCUIApplication()
        app.launchArguments = ["-openDeck", "PenCast_Demo", "-openPage", "1"]
        app.launch()
        let slide = app.descendants(matching: .any)["slide"].firstMatch
        XCTAssertTrue(slide.waitForExistence(timeout: 10))
        slide.pinch(withScale: 2.5, velocity: 1.5)
        sleep(1)
        XCTAssertLessThan(zoomState(slide)[2], 0.7, "zoomed in")
        slide.pinch(withScale: 2, velocity: 1.5)   // a second pinch still zooms
        sleep(1)
        XCTAssertLessThan(zoomState(slide)[2], 0.35, "second pinch zoomed further")
        slide.pinch(withScale: 0.1, velocity: -3)
        sleep(1)
        XCTAssertEqual(zoomState(slide)[2], 1, accuracy: 0.001, "settled on the whole slide")
        XCTAssertEqual(zoomState(slide)[0], 0.5, accuracy: 0.001, "centred")
    }

    /// The partial eraser cuts a gap as wide as its size on screen (PencilKit's own pixel eraser cut ~15
    /// points whatever the setting).
    func testLargePartialEraserCutsWideGap() {
        let app = XCUIApplication()
        app.launchArguments = ["-fingerInk", "YES", "-freshInk", "YES", "-penStyle", "ball", "-openDeck", "PenCast_Demo",
                               "-openPage", "1", "-eraserMode", "partial", "-eraserWidth", "300"]
        app.launch()
        let slide = app.descendants(matching: .any)["slide"].firstMatch
        XCTAssertTrue(slide.waitForExistence(timeout: 10))
        app.buttons["pen"].tap()
        slide.coordinate(withNormalizedOffset: CGVector(dx: 0.05, dy: 0.5))
            .press(forDuration: 0.05, thenDragTo: slide.coordinate(withNormalizedOffset: CGVector(dx: 0.95, dy: 0.5)))
        app.buttons["eraser"].tap()
        slide.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.2))
            .press(forDuration: 0.05, thenDragTo: slide.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.8)))
        sleep(1)
        let ink = app.descendants(matching: .any)["ink"].firstMatch
        XCTAssertTrue(ink.label.hasPrefix("n=2 "), "line cut in two (\(ink.label))")
        // The newest piece is the right one: it starts about 150 points (half the eraser) right of centre.
        let x = ink.label.split(separator: " ").dropFirst().compactMap { Double($0) }.first ?? 0
        let w = Double(slide.frame.width), rightStart = 2 * x * w - 0.95 * w   // centre*2 - end
        XCTAssertEqual(rightStart - 0.5 * w, 150, accuracy: 25, "gap edge half the eraser size from the cut")
    }

    /// An edge tap turns the slide and nothing else: PencilKit's Paste / Select menu must not pop up too.
    func testEdgeTapDoesNotOpenEditMenu() {
        for tool in ["pen", "highlighter", "lasso", "eraser"] {
            let app = XCUIApplication()
            app.launchArguments = ["-openDeck", "PenCast_Demo", "-openPage", "1"]
            app.launch()
            let slide = app.descendants(matching: .any)["slide"].firstMatch
            XCTAssertTrue(slide.waitForExistence(timeout: 10))
            if !app.buttons[tool].isSelected { app.buttons[tool].tap() }   // tapping the current tool opens its menu
            slide.coordinate(withNormalizedOffset: CGVector(dx: 0.92, dy: 0.5)).tap()
            sleep(1)
            XCTAssertTrue(app.staticTexts["3 / 18"].exists, "\(tool): edge tap turned the slide")
            let menu = ["Paste", "Select All", "Select", "Copy", "Insert Space"].filter { app.menuItems[$0].exists || app.buttons[$0].exists }
            XCTAssertEqual(menu, [], "\(tool): no edit menu after an edge tap")
            app.terminate()
        }
    }

    /// Zoomed in: the eraser must hit the stroke under the finger.
    func testEraserHitsUnderFingerWhenZoomed() {
        let app = XCUIApplication()
        app.launchArguments = ["-fingerInk", "YES", "-freshInk", "YES", "-penStyle", "fountain"]
        app.launch()
        app.buttons["Test decks"].tap()
        app.buttons["PenCast_Demo"].tap()
        app.buttons["Forward"].tap()
        let slide = app.descendants(matching: .any)["slide"].firstMatch
        slide.pinch(withScale: 2.5, velocity: 1.5)
        sleep(1)
        // A short stroke near the top of the view, clear of strokes other tests leave in the middle.
        slide.coordinate(withNormalizedOffset: CGVector(dx: 0.4, dy: 0.12))
            .press(forDuration: 0.05, thenDragTo: slide.coordinate(withNormalizedOffset: CGVector(dx: 0.6, dy: 0.12)))
        sleep(1)
        let ink = app.descendants(matching: .any)["ink"].firstMatch
        let count = { Int(ink.label.split(separator: " ").first?.dropFirst(2) ?? "") ?? -1 }
        let before = count()
        // (With finger ink on, one pinch finger also leaves a stroke; only the Pencil draws in the app.)
        XCTAssertGreaterThan(before, 0, "fountain stroke drawn (\(ink.label))")
        app.buttons["eraser"].tap()
        slide.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.05))
            .press(forDuration: 0.05, thenDragTo: slide.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.2)))
        sleep(1)
        XCTAssertEqual(count(), before - 1, "eraser removed the stroke under the finger (\(ink.label))")
    }

    /// Each export option must actually open something: a save dialog, the share sheet, the print panel.
    func testExportOptionsOpenSomething() {
        let app = XCUIApplication()
        app.launch()
        app.buttons["Test decks"].tap()
        app.buttons["PenCast_Demo"].tap()
        sleep(2)
        for (index, item) in ["Save As…", "Share…", "Print…"].enumerated() {
            app.buttons["Export"].tap()
            let option = app.buttons.matching(NSPredicate(format: "label == %@", item)).firstMatch
            XCTAssertTrue(option.waitForExistence(timeout: 3), "\(item) offered")
            option.tap()
            sleep(4)
            let shown = app.debugDescription
            let shot = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
            shot.name = "export\(index)"; shot.lifetime = .keepAlways; add(shot)
            // Something modal appeared: the slide toolbar's Export button is covered or gone.
            XCTAssertFalse(app.buttons["Export"].isHittable, "\(item) opened a dialog\n\(shown.prefix(0))")
            // Dismiss whatever appeared.
            for name in ["Cancel", "Close", "Done"] where app.buttons[name].exists { app.buttons[name].firstMatch.tap(); break }
            if app.buttons["Export"].isHittable == false { app.coordinate(withNormalizedOffset: CGVector(dx: 0.02, dy: 0.5)).tap() }
            sleep(1)
        }
    }

    private func zoomState(_ slide: XCUIElement) -> [Double] {
        (slide.value as? String ?? "").split(separator: " ").compactMap { Double($0) }
    }

    private func waitFor(_ e: XCUIElement, label: String) -> Bool {
        let done = expectation(for: NSPredicate(format: "label == %@", label), evaluatedWith: e)
        return XCTWaiter().wait(for: [done], timeout: 2) == .completed
    }
}


/// A long session on the 100-slide stress deck: fast paging, writing, zooming, dragging, undo. The app must
/// stay up, on the right slide, with every page's ink where it was left.
final class StressTests: XCTestCase {
    func testLongSessionOnStressDeck() {
        let app = XCUIApplication()
        app.launchArguments = ["-fingerInk", "YES", "-freshInk", "YES", "-penStyle", "fountain",
                               "-openDeck", "stress_100", "-openPage", "0"]
        app.launch()
        let slide = app.descendants(matching: .any)["slide"].firstMatch
        XCTAssertTrue(slide.waitForExistence(timeout: 10))
        let ink = app.descendants(matching: .any)["ink"].firstMatch
        let forward = app.buttons["Forward"], back = app.buttons["Back"]
        for i in 1...60 {
            forward.tap()
            if i % 3 == 0 {
                app.buttons["pen"].tap()
                slide.coordinate(withNormalizedOffset: CGVector(dx: 0.2, dy: 0.3 + Double(i % 5) * 0.1))
                    .press(forDuration: 0.02, thenDragTo: slide.coordinate(withNormalizedOffset: CGVector(dx: 0.7, dy: 0.35 + Double(i % 5) * 0.1)))
                app.buttons["pointer"].tap()
            }
            if i % 5 == 0 {
                slide.pinch(withScale: 3, velocity: 4)
                slide.coordinate(withNormalizedOffset: CGVector(dx: 0.7, dy: 0.5))
                    .press(forDuration: 0.02, thenDragTo: slide.coordinate(withNormalizedOffset: CGVector(dx: 0.3, dy: 0.6)))
                slide.pinch(withScale: 0.2, velocity: -6)
            }
            if i % 7 == 0, app.buttons["Undo"].isEnabled { app.buttons["Undo"].tap() }
            XCTAssertEqual(app.state, .runningForeground, "still running at step \(i)")
        }
        XCTAssertTrue(app.staticTexts["61 / 100"].exists, "on the slide the taps should have reached")
        for _ in 0..<57 { back.tap() }
        XCTAssertTrue(app.staticTexts["4 / 100"].exists)
        // Slide 4 (step 3) got a stroke and was never undone (undo ran on steps 7, 14, …).
        XCTAssertTrue(ink.label.hasPrefix("n=1"), "slide 4 kept its ink (\(ink.label))")
        XCTAssertEqual(app.state, .runningForeground)
    }
}
