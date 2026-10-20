import Cocoa
import SwiftUI

// The prototype is a hand-built AppKit window around a SwiftUI tree. There is
// no @main: the application object and the window are constructed here, which
// is what gives us a custom title area and our own window controls.

final class VantaWindow: NSWindow {
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { true }
}

/// SwiftUI draws the chrome edge to edge; the hosting view must not inset it by
/// the titlebar safe area, and it must not claim window drags before tabs and
/// buttons see the press.
final class ContentHostingView<Content: View>: NSHostingView<Content> {
    override var safeAreaInsets: NSEdgeInsets { NSEdgeInsets() }
    override var mouseDownCanMoveWindow: Bool { false }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {

    let model = AppModel()
    private var window: VantaWindow!
    private var keyMonitor: Any?

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.appearance = NSAppearance(named: .darkAqua)
        buildMenu()

        let rect = NSRect(x: 0, y: 0, width: 1360, height: 860)
        window = VantaWindow(
            contentRect: rect,
            styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
            backing: .buffered,
            defer: false)
        window.title = "Vanta"
        window.titleVisibility = .hidden
        window.titlebarAppearsTransparent = true
        window.titlebarSeparatorStyle = .none
        window.backgroundColor = NSColor(hex: 0x09090B)
        window.minSize = NSSize(width: 1060, height: 680)
        window.isMovable = false
        window.delegate = self
        window.level = .normal

        // The three standard buttons are replaced by the monochrome controls in
        // the top-right of the custom menu bar.
        for type in [NSWindow.ButtonType.closeButton, .miniaturizeButton, .zoomButton] {
            window.standardWindowButton(type)?.isHidden = true
        }

        let host = ContentHostingView(rootView: RootView(model: model))
        window.contentView = host

        window.setFrameAutosaveName("VantaPrototypeWindow")
        if !window.setFrameUsingName("VantaPrototypeWindow") {
            window.center()
        }
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)

        model.closeWindow = { [weak self] in self?.window.close() }
        model.minimizeWindow = { [weak self] in self?.window.miniaturize(nil) }
        model.zoomWindow = { [weak self] in self?.window.zoom(nil) }

        installKeyMonitor()

        let env = ProcessInfo.processInfo.environment
        let autorun = env["VANTA_UI_AUTORUN"] == "1"
        if autorun {
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.8) {
                self.model.route = .editor
                if self.model.tabs.isEmpty { self.model.newScript() }
                self.model.activeTabID = self.model.tabs.first?.id
                self.model.run()
            }
        }

        // A dev-only switch used to capture the real window during UI work.
        if let snapshot = env["VANTA_UI_SNAPSHOT"] {
            if env["VANTA_UI_START"] == "editor" {
                model.route = .editor
                model.activeTabID = model.tabs.first?.id
            }
            if env["VANTA_UI_NEW"] == "1" {
                model.newScript()
            }
            if env["VANTA_UI_CONSOLE"] == "1" {
                model.route = .editor
                if model.tabs.isEmpty { model.newScript() }
                model.consoleOpen = true
            }
            if env["VANTA_UI_SETTINGS"] == "1" {
                model.openSettings()
            }
            if env["VANTA_UI_ABOUT"] == "1" {
                model.openAbout()
            }
            if env["VANTA_UI_PALETTE"] == "1" {
                model.openPalette()
            }
            if env["VANTA_UI_START"] == "search" {
                model.search.source = env["VANTA_UI_SEARCH"] == "scriptblox"
                    ? .scriptblox : .local
                model.search.query = env["VANTA_UI_QUERY"] ?? ""
                model.openSearch()
                model.search.reload()
                if env["VANTA_UI_SELECT_FIRST"] == "1" {
                    DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) {
                        if let first = self.model.search.results.first {
                            self.model.search.select(first)
                        }
                    }
                }
                if env["VANTA_UI_SAVE_FIRST"] == "1" {
                    DispatchQueue.main.asyncAfter(deadline: .now() + 4.0) {
                        if let selected = self.model.search.selected,
                           let source = self.model.search.sourceText {
                            self.model.saveSearchResultToLibrary(selected, source: source)
                        }
                    }
                }
            }
            let delay = env["VANTA_UI_START"] == "search" ? 10.0 : (autorun ? 4.2 : 1.4)
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
                self.writeSnapshot(to: snapshot)
                NSApp.terminate(nil)
            }
        }
    }

    private func writeSnapshot(to path: String) {
        guard let view = window.contentView,
              let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else {
            NSLog("VantaUI: snapshot failed (no view)")
            return
        }
        view.cacheDisplay(in: view.bounds, to: rep)
        guard let png = rep.representation(using: .png, properties: [:]) else {
            NSLog("VantaUI: snapshot failed (no png)")
            return
        }
        do {
            try png.write(to: URL(fileURLWithPath: path))
            NSLog("VantaUI: wrote snapshot %@", path)
        } catch {
            NSLog("VantaUI: snapshot write failed: %@", "\(error)")
        }
    }

    // MARK: - System menu (the one at the top of the screen)

    private func buildMenu() {
        let main = NSMenu()

        let appMenu = NSMenu(title: "Vanta")
        let about = NSMenuItem(title: "About Vanta", action: #selector(showAbout),
                               keyEquivalent: "")
        about.target = self
        appMenu.addItem(about)
        appMenu.addItem(.separator())
        let hide = NSMenuItem(title: "Hide Vanta",
                              action: #selector(NSApplication.hide(_:)),
                              keyEquivalent: "h")
        hide.target = nil
        appMenu.addItem(hide)
        let quit = NSMenuItem(title: "Quit Vanta",
                              action: #selector(NSApplication.terminate(_:)),
                              keyEquivalent: "q")
        quit.target = nil
        appMenu.addItem(quit)
        let appItem = NSMenuItem()
        appItem.submenu = appMenu
        main.addItem(appItem)

        let file = NSMenu(title: "File")
        file.addItem(item("New Script", "n", #selector(menuNewScript)))
        file.addItem(item("Open…", "o", #selector(menuOpen)))
        file.addItem(item("Search Scripts…", "k", #selector(menuSearch)))
        file.addItem(item("Save", "s", #selector(menuSave)))
        file.addItem(item("Save As…", "s", #selector(menuSaveAs), [.command, .shift]))
        file.addItem(.separator())
        file.addItem(item("Close Tab", "w", #selector(menuCloseTab)))
        file.addItem(item("Close Window", "w", #selector(menuCloseWindow), [.command, .shift]))
        let fileItem = NSMenuItem()
        fileItem.submenu = file
        main.addItem(fileItem)

        let edit = NSMenu(title: "Edit")
        edit.addItem(chain("Undo", "z", Selector(("undo:"))))
        edit.addItem(chain("Redo", "z", Selector(("redo:")), [.command, .shift]))
        edit.addItem(.separator())
        edit.addItem(chain("Cut", "x", #selector(NSText.cut(_:))))
        edit.addItem(chain("Copy", "c", #selector(NSText.copy(_:))))
        edit.addItem(chain("Paste", "v", #selector(NSText.paste(_:))))
        edit.addItem(chain("Select All", "a", #selector(NSText.selectAll(_:))))
        let editItem = NSMenuItem()
        editItem.submenu = edit
        main.addItem(editItem)

        let view = NSMenu(title: "View")
        view.addItem(item("Home", "1", #selector(menuHome)))
        view.addItem(item("Editor", "2", #selector(menuEditor)))
        view.addItem(.separator())
        view.addItem(item("Toggle Explorer", "b", #selector(menuToggleSidebar)))
        view.addItem(item("Focus Mode", "f", #selector(menuFocusMode), [.command, .shift]))
        view.addItem(item("Toggle Console", "j", #selector(menuToggleConsole)))
        view.addItem(.separator())
        view.addItem(item("Settings…", ",", #selector(menuSettings)))
        view.addItem(item("Command Palette…", "p", #selector(menuPalette),
                          [.command, .shift]))
        let viewItem = NSMenuItem()
        viewItem.submenu = view
        main.addItem(viewItem)

        let run = NSMenu(title: "Run")
        run.addItem(item("Run Script", "\r", #selector(menuRun)))
        run.addItem(item("Clear Console", "", #selector(menuClearConsole)))
        let runItem = NSMenuItem()
        runItem.submenu = run
        main.addItem(runItem)

        let help = NSMenu(title: "Help")
        let aboutHelp = NSMenuItem(title: "About Vanta", action: #selector(showAbout),
                                   keyEquivalent: "")
        aboutHelp.target = self
        help.addItem(aboutHelp)
        let helpItem = NSMenuItem()
        helpItem.submenu = help
        main.addItem(helpItem)

        NSApp.mainMenu = main
    }

    private func item(_ title: String, _ key: String, _ action: Selector,
                      _ mask: NSEvent.ModifierFlags = .command) -> NSMenuItem {
        let entry = NSMenuItem(title: title, action: action, keyEquivalent: key)
        entry.keyEquivalentModifierMask = mask
        entry.target = self
        return entry
    }

    private func chain(_ title: String, _ key: String, _ action: Selector,
                       _ mask: NSEvent.ModifierFlags = .command) -> NSMenuItem {
        let entry = NSMenuItem(title: title, action: action, keyEquivalent: key)
        entry.keyEquivalentModifierMask = mask
        entry.target = nil
        return entry
    }

    @objc private func showAbout() { model.openAbout() }
    @objc private func menuNewScript() { model.newScript() }
    @objc private func menuOpen() { model.openFromDisk() }
    @objc private func menuSearch() { model.openSearch() }
    @objc private func menuSave() { model.saveActive() }
    @objc private func menuSaveAs() { model.saveActiveAs() }
    @objc private func menuCloseTab() { model.closeActiveTab() }
    @objc private func menuCloseWindow() { model.closeWindowAction() }
    @objc private func menuHome() { model.showHome() }
    @objc private func menuEditor() { model.showEditor() }
    @objc private func menuToggleSidebar() { model.toggleSidebar() }
    @objc private func menuFocusMode() { model.toggleFocusMode() }
    @objc private func menuToggleConsole() { model.toggleConsole() }
    @objc private func menuSettings() { model.openSettings() }
    @objc private func menuPalette() { model.openPalette() }
    @objc private func menuRun() { model.run() }
    @objc private func menuClearConsole() { model.clearConsole() }

    func applicationWillTerminate(_ notification: Notification) {
        model.saveSession()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    func windowDidBecomeKey(_ notification: Notification) {
        model.closeMenu()
    }

    func applicationDidBecomeActive(_ notification: Notification) {
        model.reloadFiles()
        model.refreshConnection()
    }

    // MARK: - Keyboard

    private func installKeyMonitor() {
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
            guard let self else { return event }
            let flags = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
            let command = flags.contains(.command)
            let shift = flags.contains(.shift)

            if event.keyCode == 53 { // escape
                if self.model.showSettings {
                    self.model.closeSettings(); return nil
                }
                if self.model.showAbout {
                    self.model.closeAbout(); return nil
                }
                if self.model.showSearch {
                    self.model.closeSearch(); return nil
                }
                if self.model.showPalette {
                    self.model.closePalette(); return nil
                }
                return event
            }

            guard command else { return event }

            switch event.charactersIgnoringModifiers?.lowercased() {
            case "n":
                self.model.newScript(); return nil
            case "o":
                self.model.openFromDisk(); return nil
            case "s":
                if shift { self.model.saveActiveAs() } else { self.model.saveActive() }
                return nil
            case "w":
                if shift {
                    self.model.closeWindowAction()
                } else if self.model.route == .editor, !self.model.tabs.isEmpty {
                    self.model.closeActiveTab()
                } else {
                    self.model.closeWindowAction()
                }
                return nil
            case "b":
                self.model.toggleSidebar(); return nil
            case "j":
                self.model.toggleConsole(); return nil
            case "f":
                if shift { self.model.toggleFocusMode(); return nil }
                return event
            case "p":
                if shift { self.model.openPalette(); return nil }
                return event
            case "1":
                self.model.showHome(); return nil
            case "2":
                self.model.showEditor(); return nil
            case "k":
                self.model.openSearch(); return nil
            case "\r":
                self.model.run(); return nil
            default:
                return event
            }
        }
    }
}

// MARK: - Entry point

let application = NSApplication.shared
application.setActivationPolicy(.regular)
let delegate = AppDelegate()
application.delegate = delegate
application.run()
