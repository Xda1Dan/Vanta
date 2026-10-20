import Foundation
import AppKit
import UniformTypeIdentifiers
import SwiftUI

enum Route: Equatable {
    case home
    case editor
}

enum ExplorerFilter: String {
    case scripts
    case all

    var label: String {
        self == .scripts ? "Scripts only" : "All files"
    }
}

extension FileNode {
    var url: URL { URL(fileURLWithPath: path) }

    var isScript: Bool {
        let lower = name.lowercased()
        return lower.hasSuffix(".lua") || lower.hasSuffix(".luau")
    }
}

enum ConnectionState: String, CaseIterable {
    case attached, waiting, offline, reinstall, failed

    var label: String {
        switch self {
        case .attached:  return "ATTACHED"
        case .waiting:   return "WAITING"
        case .offline:   return "OFFLINE"
        case .reinstall: return "REINSTALL"
        case .failed:    return "FAILED"
        }
    }

    /// Fallback text for when the server has not reported a detail of its own.
    var detail: String {
        switch self {
        case .attached:  return "Executor environment is ready"
        case .waiting:   return "Roblox is running; executor is not ready"
        case .offline:   return "No client on 127.0.0.1:5553"
        case .reinstall: return "Roblox is not the pinned build"
        case .failed:    return "The executor stood down"
        }
    }

    var color: Color {
        switch self {
        case .attached:  return V.accent
        case .waiting:   return V.warning
        case .offline:   return V.faint
        case .reinstall: return V.danger
        case .failed:    return V.danger
        }
    }

    /// A script sent now either runs on the next heartbeat or is queued for the
    /// next game. Reinstall and failed will never run it, and offline has no
    /// server to take it.
    var canExecute: Bool {
        switch self {
        case .attached, .waiting: return true
        case .offline, .reinstall, .failed: return false
        }
    }
}

enum ConsoleLevel: String {
    case boot, link, exec, print, warn, error

    var color: Color {
        switch self {
        case .boot:   return V.accent
        case .link:   return V.info
        case .exec:   return V.accent
        case .print:  return V.muted
        case .warn:   return V.warning
        case .error:  return V.danger
        }
    }

    var nsColor: NSColor {
        switch self {
        case .boot, .exec: return V.nsAccent
        case .link:        return NSColor(hex: 0x8AB4F8)
        case .print:       return V.nsMuted
        case .warn:        return NSColor(hex: 0xF5B85D)
        case .error:       return NSColor(hex: 0xF27575)
        }
    }
}

struct ConsoleEntry: Identifiable {
    let id = UUID()
    let time: String
    let level: ConsoleLevel
    let message: String

}

enum EditorLanguage {
    case luau, plain

    var label: String {
        switch self {
        case .luau:  return "Luau"
        case .plain: return "Text"
        }
    }
}

struct EditorTab: Identifiable, Equatable {
    let id: UUID
    var name: String
    var path: String?
    var url: URL?
    var text: String
    var isDirty: Bool
    var language: EditorLanguage

    init(id: UUID = UUID(), name: String, path: String? = nil, url: URL? = nil,
         text: String, isDirty: Bool = false, language: EditorLanguage = .luau) {
        self.id = id
        self.name = name
        self.path = path
        self.url = url
        self.text = text
        self.isDirty = isDirty
        self.language = language
    }

    static func == (a: EditorTab, b: EditorTab) -> Bool {
        a.id == b.id && a.name == b.name && a.path == b.path && a.text == b.text
    }
}

struct FileNode: Identifiable, Hashable {
    let id: String
    let name: String
    let path: String
    let isDirectory: Bool
    var children: [FileNode] = []
    var autoexecOrder: Int? = nil

    static func file(_ path: String, _ name: String? = nil, order: Int? = nil) -> FileNode {
        FileNode(id: path, name: name ?? (path as NSString).lastPathComponent,
                 path: path, isDirectory: false, children: [], autoexecOrder: order)
    }

    static func folder(_ path: String, _ name: String, _ children: [FileNode]) -> FileNode {
        FileNode(id: path, name: name, path: path, isDirectory: true, children: children)
    }

    static func == (a: FileNode, b: FileNode) -> Bool { a.id == b.id }
    func hash(into hasher: inout Hasher) { hasher.combine(id) }
}

enum SampleContent {
    static let starter = """
    -- Vanta
    -- A blank script. Press ⌘↩ to run.
    """

    static func language(for name: String) -> EditorLanguage {
        name.hasSuffix(".luau") || name.hasSuffix(".lua") ? .luau : .plain
    }
}

struct Toast: Identifiable {
    let id = UUID()
    let message: String
    let icon: String
}

enum RunPhase {
    case sent, running, executed, failed
}

struct RunStatus {
    let name: String
    var phase: RunPhase
    let at: Date
    var duration: TimeInterval?
    var error: String?
}

private struct PendingRun {
    let name: String
    let sourceSize: Int
    let sentAt: Date
    var running = false
}

private struct SessionTab: Codable {
    var name: String
    var path: String?
    var text: String?
    // Optional so sessions written before this field still decode. A dirty
    // file-backed tab carries its buffer in `text`; without that, quitting
    // and relaunching silently reset it to what was last written to disk.
    var dirty: Bool?
}

private struct SessionData: Codable {
    var tabs: [SessionTab]
    var active: Int?
}

final class AppModel: ObservableObject {

    // MARK: - State

    @Published var route: Route = .home
    @Published var tabs: [EditorTab] = []
    @Published var activeTabID: UUID?
    @Published var showSidebar = true
    @Published var consoleOpen = false
    @Published var consoleEntries: [ConsoleEntry] = []
    @Published var connection: ConnectionState = .offline
    @Published var connectionDetail = ConnectionState.offline.detail
    @Published var safeMode = true
    @Published var autoexecOnJoin = true
    @Published var autoexecDisabled: Set<String> = []
    @Published var showSettings = false
    @Published var showAbout = false
    @Published var showSearch = false
    @Published var focusMode = false
    @Published var showPalette = false
    @Published var toast: Toast?
    @Published var openFolders: Set<String> = []
    @Published var openSections: Set<String> = ["autoexec", "workspace"]
    @Published var filter = ""
    @Published var fontSize: CGFloat = 12.5
    @Published var showLineNumbers = true
    @Published var running = false
    @Published var lastRun: RunStatus?
    @Published var cursorLine = 1
    @Published var cursorColumn = 1
    @Published var awaitingInput: String?

    // Real folders. The app is a frontend, not a second sandbox: it reads and
    // writes the same paths the dylib does.
    @Published var autoexecFiles: [FileNode] = []
    @Published var workspaceTree: [FileNode] = []
    @Published var libraryTree: [FileNode] = []
    @Published var recentPaths: [String] = []
    @Published var explorerFilter: ExplorerFilter = .scripts
    @Published var refreshingFiles = false

    let link = VantaLink()
    var consoleClient: ConsoleClient?
    private let journal = JournalWatcher()
    private var pendingRuns: [PendingRun] = []
    let search = ScriptSearchStore()

    private let recentKey = "vanta.ui.recent"

    // Window actions installed by AppDelegate.
    var closeWindow: (() -> Void)?
    var minimizeWindow: (() -> Void)?
    var zoomWindow: (() -> Void)?

    private var toastWork: DispatchWorkItem?
    private var runWork: DispatchWorkItem?

    private static let clock: DateFormatter = {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "HH:mm:ss.SSS"
        return formatter
    }()

    init() {
        VantaPaths.ensure()

        let config = VantaAutoexecConfig.read()
        autoexecOnJoin = config.enabled
        autoexecDisabled = config.off
        safeMode = VantaExecutorSettings.read().safeMode
        recentPaths = UserDefaults.standard.stringArray(forKey: recentKey) ?? []

        tabs = []
        activeTabID = nil
        route = .home
        restoreSession()

        reloadFiles()

        connection = link.state
        connectionDetail = link.status?.detail ?? connection.detail
        link.onStatus = { [weak self] status in
            guard let self else { return }
            self.connectionDetail = status?.detail ?? self.connection.detail
        }
        link.onChange = { [weak self] state in
            guard let self else { return }
            self.connection = state
            let detail = self.link.status?.detail ?? state.detail
            self.connectionDetail = detail
            switch state {
            case .attached:
                self.appendConsole(.link, "attached: \(detail)")
            case .waiting:
                self.appendConsole(.link, "waiting: \(detail)")
            case .reinstall:
                self.appendConsole(.error, "REINSTALL: \(detail)")
                self.showToast("Reinstall the pinned Roblox build, then relaunch.",
                               icon: "exclamationmark.triangle")
            case .failed:
                self.appendConsole(.error, "executor failed: \(detail)")
            case .offline:
                self.appendConsole(.link, "control server is offline")
            }
        }
        link.refresh()

        search.localProvider = { [weak self] query in
            self?.localSearch(query) ?? []
        }

        startConsoleClient()

        journal.onLine = { [weak self] line in
            self?.handleJournal(line)
        }
        journal.start()
    }

    // MARK: - Real files

    func startConsoleClient() {
        guard consoleClient == nil else { return }
        let client = ConsoleClient()
        client.onRecord = { [weak self] name, message in
            self?.handleConsole(name: name, message: message)
        }
        client.start()
        consoleClient = client
        appendConsole(.boot, "control server http://\(VantaPaths.controlHost):\(VantaPaths.controlPort)")
    }

    func reloadFiles() {
        let disabled = autoexecDisabled
        refreshingFiles = true
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let auto = VantaDisk.listAutoexec(disabled: disabled)
            let workspace = VantaDisk.listTree(VantaPaths.workspace)
            let library = VantaDisk.listTree(VantaPaths.library)
            DispatchQueue.main.async {
                guard let self else { return }
                self.autoexecFiles = auto
                self.workspaceTree = workspace
                self.libraryTree = library
                self.refreshingFiles = false
            }
        }
    }

    func toggleExplorerFilter() {
        explorerFilter = explorerFilter == .scripts ? .all : .scripts
    }

    var recentNodes: [FileNode] {
        recentPaths.compactMap { path in
            guard FileManager.default.fileExists(atPath: path) else { return nil }
            return .file(path, URL(fileURLWithPath: path).lastPathComponent)
        }
    }

    func addRecent(_ url: URL) {
        let path = url.standardizedFileURL.path
        recentPaths.removeAll { $0 == path }
        recentPaths.insert(path, at: 0)
        if recentPaths.count > 12 { recentPaths.removeLast(recentPaths.count - 12) }
        UserDefaults.standard.set(recentPaths, forKey: recentKey)
    }

    func clearRecent() {
        recentPaths.removeAll()
        UserDefaults.standard.removeObject(forKey: recentKey)
        showToast("Recent list cleared", icon: "clock")
    }

    func openURL(_ url: URL) {
        let path = url.standardizedFileURL.path
        if let existing = tabs.first(where: { $0.url?.standardizedFileURL.path == path }) {
            selectTab(existing.id)
            return
        }
        guard let text = VantaDisk.readText(url) else {
            showToast("\(url.lastPathComponent) is not a text file",
                      icon: "exclamationmark.triangle")
            return
        }
        let tab = EditorTab(name: url.lastPathComponent, path: path, url: url,
                            text: text,
                            language: SampleContent.language(for: url.lastPathComponent))
        tabs.append(tab)
        activeTabID = tab.id
        route = .editor
        addRecent(url)
        appendConsole(.link, "opened \(url.lastPathComponent)")
    }

    // MARK: - Derived

    var activeTab: EditorTab? {
        guard let id = activeTabID else { return nil }
        return tabs.first { $0.id == id }
    }

    var selectedPath: String? {
        route == .editor ? activeTab?.path : nil
    }

    var workspaceScripts: Int { Self.countScripts(workspaceTree) }
    var libraryScripts: Int { Self.countScripts(libraryTree) }

    private static func countScripts(_ nodes: [FileNode]) -> Int {
        nodes.reduce(0) { count, node in
            count + (node.isDirectory ? countScripts(node.children)
                                      : (node.isScript ? 1 : 0))
        }
    }

    var lineCount: Int {
        guard let text = activeTab?.text, !text.isEmpty else { return 1 }
        return text.components(separatedBy: "\n").count
    }

    var runSummary: String? {
        guard let run = lastRun else { return nil }
        switch run.phase {
        case .sent:
            return "Sent \(run.name)"
        case .running:
            return "Running \(run.name)…"
        case .executed:
            if let duration = run.duration {
                return String(format: "Executed \(run.name) · %.1fs", duration)
            }
            return "Executed \(run.name)"
        case .failed:
            return "Failed \(run.name)"
        }
    }

    // MARK: - Session

    private var sessionURL: URL? {
        guard let support = FileManager.default.urls(for: .applicationSupportDirectory,
                                                      in: .userDomainMask).first
        else { return nil }
        let folder = support.appendingPathComponent("VantaUI", isDirectory: true)
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        return folder.appendingPathComponent("session.json")
    }

    func saveSession() {
        guard let url = sessionURL else { return }
        guard !tabs.isEmpty else {
            try? FileManager.default.removeItem(at: url)
            return
        }
        let saved = tabs.map { tab in
            SessionTab(name: tab.name,
                       path: tab.url?.path,
                       text: tab.isDirty || tab.url == nil ? tab.text : nil,
                       dirty: tab.isDirty)
        }
        let active = activeTabID.flatMap { id in tabs.firstIndex { $0.id == id } }
        let data = SessionData(tabs: saved, active: active)
        guard let encoded = try? JSONEncoder().encode(data) else { return }
        try? encoded.write(to: url, options: .atomic)
    }

    private func restoreSession() {
        guard let url = sessionURL,
              let data = try? Data(contentsOf: url),
              let saved = try? JSONDecoder().decode(SessionData.self, from: data)
        else { return }
        var restored: [EditorTab] = []
        for entry in saved.tabs {
            if let path = entry.path {
                let file = URL(fileURLWithPath: path)
                if let text = entry.text {
                    restored.append(EditorTab(name: entry.name, path: path, url: file,
                                              text: text, isDirty: entry.dirty ?? true,
                                              language: SampleContent.language(for: entry.name)))
                } else if let text = VantaDisk.readText(file) {
                    restored.append(EditorTab(name: entry.name, path: path, url: file,
                                              text: text,
                                              language: SampleContent.language(for: entry.name)))
                }
            } else if let text = entry.text {
                restored.append(EditorTab(name: entry.name, path: nil, text: text,
                                          isDirty: entry.dirty ?? true,
                                          language: .luau))
            }
        }
        guard !restored.isEmpty else { return }
        tabs = restored
        if let index = saved.active, restored.indices.contains(index) {
            activeTabID = restored[index].id
        } else {
            activeTabID = restored.first?.id
        }
        route = .editor
        if let tab = activeTab {
            if let url = tab.url { addRecent(url) }
        }
    }

    // MARK: - Tabs

    func selectTab(_ id: UUID) {
        guard tabs.contains(where: { $0.id == id }) else { return }
        activeTabID = id
        route = .editor
        closeMenu()
    }

    func showHome() {
        route = .home
        closeMenu()
    }

    func showEditor() {
        route = .editor
        closeMenu()
    }

    func newScript() {
        let taken = Set(tabs.map(\.name))
        var name = "Untitled"
        var n = 2
        while taken.contains(name) { name = "Untitled \(n)"; n += 1 }
        let tab = EditorTab(name: name, path: nil, text: SampleContent.starter,
                            language: .luau)
        tabs.append(tab)
        activeTabID = tab.id
        route = .editor
        appendConsole(.link, "new buffer \(name)")
        closeMenu()
    }

    func open(_ node: FileNode) {
        if node.isDirectory {
            toggleFolder(node.id)
            return
        }
        openURL(URL(fileURLWithPath: node.path))
        closeMenu()
    }

    func openFromDisk() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.title = "Open Script"
        panel.prompt = "Open"
        panel.allowedContentTypes = [.plainText, .sourceCode, .json]
        panel.allowsOtherFileTypes = true
        guard panel.runModal() == .OK, let url = panel.url else { return }
        openURL(url)
        closeMenu()
    }

    func setText(_ text: String, for id: UUID) {
        guard let index = tabs.firstIndex(where: { $0.id == id }) else { return }
        guard tabs[index].text != text else { return }
        tabs[index].text = text
        tabs[index].isDirty = true
    }

    func closeTab(_ id: UUID) {
        guard let index = tabs.firstIndex(where: { $0.id == id }) else { return }
        guard confirmClose(id, at: index) else { return }
        tabs.remove(at: index)
        if activeTabID == id {
            if tabs.isEmpty {
                activeTabID = nil
                route = .home
            } else {
                activeTabID = tabs[min(index, tabs.count - 1)].id
            }
        }
    }

    /// True when the tab may be closed: it is clean, was saved, or the user
    /// chose to discard it. A cancelled Save As returns false.
    private func confirmClose(_ id: UUID, at index: Int) -> Bool {
        let tab = tabs[index]
        guard tab.isDirty else { return true }
        let alert = NSAlert()
        alert.messageText = "Save changes to \(tab.name)?"
        alert.informativeText = "Your changes will be lost if you don't save them."
        alert.addButton(withTitle: "Save")
        alert.addButton(withTitle: "Don't Save")
        alert.addButton(withTitle: "Cancel")
        switch alert.runModal() {
        case .alertFirstButtonReturn:
            return saveTab(id)
        case .alertSecondButtonReturn:
            return true
        default:
            return false
        }
    }

    func closeActiveTab() {
        guard let id = activeTabID else { return }
        closeTab(id)
    }

    func closeOtherTabs() {
        guard let active = activeTabID else { return }
        let unsaved = tabs.filter { $0.id != active && $0.isDirty }
        if !unsaved.isEmpty {
            let alert = NSAlert()
            alert.messageText = unsaved.count == 1
                ? "Close 1 unsaved tab?"
                : "Close \(unsaved.count) unsaved tabs?"
            alert.informativeText = "Their changes will be lost."
            alert.addButton(withTitle: "Cancel")
            alert.addButton(withTitle: "Close Without Saving")
            guard alert.runModal() == .alertSecondButtonReturn else { return }
        }
        tabs.removeAll { $0.id != active }
    }

    @discardableResult
    func saveTab(_ id: UUID) -> Bool {
        guard let index = tabs.firstIndex(where: { $0.id == id }) else { return false }
        guard let url = tabs[index].url else { return saveTabAs(id) }
        guard VantaDisk.writeText(tabs[index].text, to: url) else {
            showToast("Could not save \(tabs[index].name)",
                      icon: "exclamationmark.triangle")
            return false
        }
        tabs[index].isDirty = false
        addRecent(url)
        reloadFiles()
        appendConsole(.link, "saved \(tabs[index].name)")
        showToast("Saved \(tabs[index].name)", icon: "checkmark.circle")
        return true
    }

    @discardableResult
    func saveTabAs(_ id: UUID) -> Bool {
        guard let index = tabs.firstIndex(where: { $0.id == id }) else { return false }
        let tab = tabs[index]
        let panel = NSSavePanel()
        panel.title = "Save Script"
        panel.prompt = "Save"
        panel.nameFieldStringValue = tab.name.contains(".") ? tab.name : tab.name + ".luau"
        panel.allowedContentTypes = [.sourceCode, .plainText]
        panel.allowsOtherFileTypes = true
        guard panel.runModal() == .OK, let url = panel.url else { return false }
        guard VantaDisk.writeText(tab.text, to: url) else {
            showToast("Could not save \(tab.name)", icon: "exclamationmark.triangle")
            return false
        }
        tabs[index].url = url
        tabs[index].path = url.path
        tabs[index].name = url.lastPathComponent
        tabs[index].language = SampleContent.language(for: url.lastPathComponent)
        tabs[index].isDirty = false
        addRecent(url)
        reloadFiles()
        appendConsole(.link, "saved \(url.lastPathComponent)")
        showToast("Saved \(url.lastPathComponent)", icon: "checkmark.circle")
        return true
    }

    func saveActive() {
        guard let tab = activeTab else { return }
        saveTab(tab.id)
    }

    func saveActiveAs() {
        guard let tab = activeTab else { return }
        saveTabAs(tab.id)
    }

    // MARK: - Sidebar

    func toggleFolder(_ path: String) {
        if openFolders.contains(path) { openFolders.remove(path) }
        else { openFolders.insert(path) }
    }

    func toggleSection(_ key: String) {
        withAnimation(.easeOut(duration: 0.16)) {
            if openSections.contains(key) { openSections.remove(key) }
            else { openSections.insert(key) }
        }
    }

    func isAutoexecEnabled(_ name: String) -> Bool {
        !autoexecDisabled.contains(name)
    }

    func toggleAutoexec(_ name: String) {
        if autoexecDisabled.contains(name) { autoexecDisabled.remove(name) }
        else { autoexecDisabled.insert(name) }
        saveAutoexecConfig()
        reloadFiles()
    }

    func toggleAutoexecOnJoin() {
        autoexecOnJoin.toggle()
        saveAutoexecConfig()
    }

    func toggleSafeMode() {
        safeMode.toggle()
        VantaExecutorSettings(safeMode: safeMode).write()
    }

    private func saveAutoexecConfig() {
        VantaAutoexecConfig(enabled: autoexecOnJoin, off: autoexecDisabled).write()
    }

    func toggleSidebar() {
        withAnimation(.easeOut(duration: 0.16)) { showSidebar.toggle() }
        closeMenu()
    }

    func toggleFocusMode() {
        withAnimation(.easeOut(duration: 0.18)) { focusMode.toggle() }
        closeMenu()
    }

    func showSidebarSection(_ key: String) {
        withAnimation(.easeOut(duration: 0.16)) {
            showSidebar = true
            openSections.insert(key)
        }
    }

    func updateCursor(line: Int, column: Int) {
        if cursorLine != line { cursorLine = line }
        if cursorColumn != column { cursorColumn = column }
    }

    // MARK: - Console

    func toggleConsole() {
        withAnimation(.easeOut(duration: 0.16)) { consoleOpen.toggle() }
        closeMenu()
    }

    func appendConsole(_ level: ConsoleLevel, _ message: String) {
        consoleEntries.append(ConsoleEntry(time: Self.clock.string(from: Date()),
                                           level: level, message: message))
        if consoleEntries.count > 400 {
            consoleEntries.removeFirst(consoleEntries.count - 400)
        }
    }

    func clearConsole() {
        consoleEntries.removeAll()
        closeMenu()
    }

    func copyConsole() {
        let text = consoleEntries.map { "\($0.time)  \($0.level.rawValue)  \($0.message)" }
            .joined(separator: "\n")
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
        showToast("Console copied", icon: "doc.on.doc")
        closeMenu()
    }

    /// The wire names the dylib sends. `rconsolecreate` and `rconsoleshow`
    /// are the same C function, and likewise destroy/hide and name/title.
    func handleConsole(name: String, message: String) {
        switch name {
        case "rconsoleprint":
            appendConsole(.print, message)
        case "rconsoleinfo":
            appendConsole(.link, message)
        case "rconsolewarn":
            appendConsole(.warn, message)
        case "rconsoleclear":
            // The rconsole family expects a console window of its own. Vanta
            // only has the user's console, so a script clearing "its" output
            // would wipe everything the user has collected; the Clear button is
            // the only thing that empties this panel.
            break
        case "rconsoleerror", "rconsoleerr":
            appendConsole(.error, message)
            if !consoleOpen { consoleOpen = true }
        case "rconsoleinput":
            appendConsole(.link, message.isEmpty ? "input requested" : message)
            awaitingInput = message.isEmpty ? ">" : message
            if !consoleOpen { consoleOpen = true }
        case "rconsoleshow", "rconsolecreate":
            if !consoleOpen { consoleOpen = true }
        case "rconsolehide", "rconsoledestroy":
            // Visibility belongs to the user for the same reason: a script
            // calling rconsolehide at the end of a run must not make the
            // output it just produced disappear.
            break
        case "rconsolename", "rconsolesettitle":
            break
        default:
            appendConsole(.boot, "\(name): \(message)")
        }
    }

    func answerConsoleInput(_ text: String) {
        if !ConsoleClient.answer(text) {
            VantaDisk.writeText(text + "\n", to: VantaPaths.consoleInputFile)
        }
        appendConsole(.link, "→ \(text)")
        awaitingInput = nil
    }

    // MARK: - Run / launch

    func run() {
        guard let tab = activeTab else {
            showToast("Nothing to run", icon: "exclamationmark.triangle")
            return
        }
        executeText(tab.text, name: tab.name)
    }

    func runFile(_ node: FileNode) {
        guard let text = VantaDisk.readText(URL(fileURLWithPath: node.path)) else {
            showToast("\(node.name) is not a text file", icon: "exclamationmark.triangle")
            return
        }
        executeText(text, name: node.name)
    }

    /// Sends a script to the dylib's loopback HTTP server. The response is the
    /// queue acknowledgement; rconsole output and RUN lines arrive separately.
    func executeText(_ text: String, name: String) {
        guard !running else {
            showToast("Still sending the last script", icon: "hourglass")
            return
        }
        guard !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            showToast("\(name) is empty", icon: "exclamationmark.triangle")
            return
        }
        guard connection.canExecute else {
            appendConsole(.error, "not ready: \(connectionDetail)")
            showToast(connectionDetail, icon: "exclamationmark.triangle")
            return
        }

        closeMenu()
        running = true
        appendConsole(.exec, "\(name)  →  sending  (\(max(1, text.utf8.count / 1024)) KB)")

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let result = self?.link.send(text)
            DispatchQueue.main.async {
                guard let self else { return }
                self.running = false
                switch result {
                case .success:
                    self.appendConsole(.exec, "\(name)  →  sent")
                    self.markSent(name: name, source: text)
                    self.link.refresh()
                case .failure(let error):
                    self.appendConsole(.error, error.localizedDescription)
                    self.showToast(error.localizedDescription,
                                   icon: "exclamationmark.triangle")
                case .none:
                    break
                }
            }
        }
    }

    func refreshConnection() {
        link.refresh()
    }

    private func markSent(name: String, source: String) {
        let pending = PendingRun(name: name,
                                 sourceSize: source.utf8.count,
                                 sentAt: Date())
        pendingRuns.append(pending)
        lastRun = RunStatus(name: name, phase: .sent, at: Date(),
                            duration: nil, error: nil)
    }

    private func handleJournal(_ line: String) {
        guard let range = line.range(of: "RUN ") else { return }
        let rest = String(line[range.upperBound...])
        if rest.hasPrefix("start ") {
            guard let size = Int(rest.dropFirst("start ".count)
                .trimmingCharacters(in: .whitespaces)) else { return }
            guard let index = pendingRuns.firstIndex(where: {
                $0.sourceSize == size && !$0.running
            }) else { return }
            pendingRuns[index].running = true
            lastRun = RunStatus(name: pendingRuns[index].name,
                                phase: .running, at: Date(),
                                duration: nil, error: nil)
        } else if rest.hasPrefix("done ") {
            guard let size = Int(rest.dropFirst("done ".count)
                .trimmingCharacters(in: .whitespaces)) else { return }
            guard let index = pendingRuns.firstIndex(where: {
                $0.sourceSize == size && $0.running
            }) else { return }
            let pending = pendingRuns.remove(at: index)
            lastRun = RunStatus(name: pending.name, phase: .executed,
                                at: Date(),
                                duration: Date().timeIntervalSince(pending.sentAt),
                                error: nil)
            appendConsole(.exec, "\(pending.name)  →  executed")
        } else if rest.hasPrefix("failed ") {
            let payload = rest.dropFirst("failed ".count)
            let parts = payload.split(separator: ":", maxSplits: 1,
                                      omittingEmptySubsequences: false)
            guard let size = Int(parts[0].trimmingCharacters(in: .whitespaces))
            else { return }
            let message = parts.count > 1
                ? String(parts[1]).trimmingCharacters(in: .whitespaces)
                : "unknown error"
            guard let index = pendingRuns.firstIndex(where: {
                $0.sourceSize == size
            }) else { return }
            let pending = pendingRuns.remove(at: index)
            lastRun = RunStatus(name: pending.name, phase: .failed,
                                at: Date(), duration: nil, error: message)
            appendConsole(.error, "\(pending.name)  →  failed: \(message)")
        }
    }

    // MARK: - Window

    func minimize() { minimizeWindow?() }
    func zoom() { zoomWindow?() }
    func closeWindowAction() { closeWindow?() }

    // MARK: - Menus / overlays

    func closeMenu() {}

    func openSettings() {
        withAnimation(.easeOut(duration: 0.18)) { showSettings = true }
        closeMenu()
    }

    func closeSettings() {
        withAnimation(.easeOut(duration: 0.16)) { showSettings = false }
    }

    // MARK: - Script search

    func openSearch() {
        withAnimation(.easeOut(duration: 0.18)) { showSearch = true }
        closeMenu()
        search.openIfNeeded()
    }

    func openPalette() {
        withAnimation(.easeOut(duration: 0.16)) { showPalette = true }
        closeMenu()
    }

    func closePalette() {
        withAnimation(.easeOut(duration: 0.14)) { showPalette = false }
    }

    func closeSearch() {
        withAnimation(.easeOut(duration: 0.16)) { showSearch = false }
        search.cancel()
    }

    func localSearch(_ query: String) -> [ScriptResult] {
        let trimmed = query.trimmingCharacters(in: .whitespacesAndNewlines)
        var files: [FileNode] = []
        files.append(contentsOf: autoexecFiles)
        files.append(contentsOf: flattenFiles(workspaceTree))
        files.append(contentsOf: flattenFiles(libraryTree))

        var results: [ScriptResult] = []
        for node in files {
            if results.count >= 60 { break }
            let nameMatches = trimmed.isEmpty
                || node.name.localizedCaseInsensitiveContains(trimmed)
            let url = URL(fileURLWithPath: node.path)
            guard VantaDisk.isTextFile(url) else { continue }
            let text = VantaDisk.readText(url) ?? ""
            let contentMatches = !trimmed.isEmpty
                && text.localizedCaseInsensitiveContains(trimmed)
            guard trimmed.isEmpty || nameMatches || contentMatches else { continue }
            let folder = url.deletingLastPathComponent().lastPathComponent
            let attributes = try? url.resourceValues(
                forKeys: [.contentModificationDateKey, .creationDateKey])
            results.append(ScriptResult(
                id: node.path,
                title: node.name,
                game: folder,
                source: text,
                rawURL: nil,
                path: node.path,
                thumbnail: nil,
                views: 0, likes: 0,
                hasKey: false, isPatched: false, isVerified: false,
                isUniversal: false, isPaid: false, isMobileReady: false,
                risk: nil,
                uploaded: attributes?.creationDate,
                updated: attributes?.contentModificationDate,
                matched: nameMatches ? ["title"] : ["content"]))
        }
        return results
    }

    private func flattenFiles(_ nodes: [FileNode]) -> [FileNode] {
        var out: [FileNode] = []
        for node in nodes {
            if node.isDirectory { out.append(contentsOf: flattenFiles(node.children)) }
            else { out.append(node) }
        }
        return out
    }

    func newFile(in folder: URL, named name: String = "script.lua") {
        var url = folder.appendingPathComponent(name)
        let stem = (name as NSString).deletingPathExtension
        let ext = (name as NSString).pathExtension
        var index = 2
        while FileManager.default.fileExists(atPath: url.path) {
            url = folder.appendingPathComponent(
                ext.isEmpty ? "\(stem) \(index)" : "\(stem) \(index).\(ext)")
            index += 1
        }
        guard VantaDisk.writeText(SampleContent.starter, to: url) else {
            appendConsole(.error, "could not create \(url.lastPathComponent)")
            return
        }
        reloadFiles()
        openURL(url)
    }

    func newFolder(in folder: URL) {
        var url = folder.appendingPathComponent("New Folder")
        var index = 2
        while FileManager.default.fileExists(atPath: url.path) {
            url = folder.appendingPathComponent("New Folder \(index)")
            index += 1
        }
        do {
            try FileManager.default.createDirectory(at: url, withIntermediateDirectories: false)
            reloadFiles()
        } catch {
            appendConsole(.error, "could not create folder: \(error.localizedDescription)")
        }
    }

    func reveal(_ url: URL) {
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }

    func trash(_ url: URL) {
        do {
            var resulting: NSURL?
            try FileManager.default.trashItem(at: url, resultingItemURL: &resulting)
            tabs.removeAll { $0.url?.standardizedFileURL == url.standardizedFileURL }
            if tabs.isEmpty {
                activeTabID = nil
                route = .home
            } else if !tabs.contains(where: { $0.id == activeTabID }) {
                activeTabID = tabs.first?.id
            }
            reloadFiles()
        } catch {
            appendConsole(.error, "could not move to Trash: \(error.localizedDescription)")
        }
    }

    func importFiles(_ urls: [URL], into folder: URL) {
        for source in urls {
            var target = folder.appendingPathComponent(source.lastPathComponent)
            let stem = (source.lastPathComponent as NSString).deletingPathExtension
            let ext = (source.lastPathComponent as NSString).pathExtension
            var index = 2
            while FileManager.default.fileExists(atPath: target.path) {
                target = folder.appendingPathComponent(
                    ext.isEmpty ? "\(stem) \(index)" : "\(stem) \(index).\(ext)")
                index += 1
            }
            try? FileManager.default.copyItem(at: source, to: target)
        }
        reloadFiles()
    }

    func saveSearchResultToLibrary(_ result: ScriptResult, source: String) {
        var base = result.title.trimmingCharacters(in: .whitespacesAndNewlines)
        if base.isEmpty { base = "Script" }
        let invalid = CharacterSet(charactersIn: "/\\:*?\"<>|")
        base = base.components(separatedBy: invalid).joined(separator: "-")
        if !base.lowercased().hasSuffix(".lua") && !base.lowercased().hasSuffix(".luau") {
            base += ".luau"
        }
        var url = VantaPaths.library.appendingPathComponent(base)
        var index = 2
        while FileManager.default.fileExists(atPath: url.path) {
            let stem = (base as NSString).deletingPathExtension
            let ext = (base as NSString).pathExtension
            url = VantaPaths.library.appendingPathComponent(
                ext.isEmpty ? "\(stem) \(index)" : "\(stem) \(index).\(ext)")
            index += 1
        }
        if VantaDisk.writeText(source, to: url) {
            reloadFiles()
            appendConsole(.link, "saved \(url.lastPathComponent) to Library")
        } else {
            appendConsole(.error, "could not save \(url.lastPathComponent) to Library")
        }
    }

    func openSearchResult(_ result: ScriptResult, source: String) {
        if let path = result.path {
            openURL(URL(fileURLWithPath: path))
            closeSearch()
            return
        }
        var name = result.title.trimmingCharacters(in: .whitespacesAndNewlines)
        if name.isEmpty { name = "Untitled" }
        if !name.contains(".") { name += ".luau" }
        let tab = EditorTab(name: name, path: nil, text: source, language: .luau)
        tabs.append(tab)
        activeTabID = tab.id
        route = .editor
        appendConsole(.link, "opened \(name) from the script index")
        closeSearch()
    }

    func openAbout() {
        withAnimation(.easeOut(duration: 0.18)) { showAbout = true }
        closeMenu()
    }

    func closeAbout() {
        withAnimation(.easeOut(duration: 0.16)) { showAbout = false }
    }

    // MARK: - Editor commands (responder chain)

    func perform(_ selector: Selector) {
        NSApp.sendAction(selector, to: nil, from: nil)
        closeMenu()
    }

    func undo()    { perform(Selector(("undo:"))) }
    func redo()    { perform(Selector(("redo:"))) }
    func cut()     { perform(#selector(NSText.cut(_:))) }
    func copy()    { perform(#selector(NSText.copy(_:))) }
    func paste()   { perform(#selector(NSText.paste(_:))) }
    func selectAll() { perform(#selector(NSText.selectAll(_:))) }
    func find()    { perform(#selector(NSTextView.performFindPanelAction(_:))) }

    // MARK: - Toast

    func showToast(_ message: String, icon: String = "checkmark.circle") {
        toastWork?.cancel()
        withAnimation(.easeOut(duration: 0.16)) {
            toast = Toast(message: message, icon: icon)
        }
        let work = DispatchWorkItem { [weak self] in
            withAnimation(.easeIn(duration: 0.18)) { self?.toast = nil }
        }
        toastWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.6, execute: work)
    }
}
