import Foundation
import AppKit

// MARK: - Paths

enum VantaPaths {
    static let home = FileManager.default.homeDirectoryForCurrentUser
    static let root = home.appendingPathComponent("Documents/Vanta", isDirectory: true)
    static let workspace = root.appendingPathComponent("workspace", isDirectory: true)
    static let autoexec = root.appendingPathComponent("autoexec", isDirectory: true)
    static let library = root.appendingPathComponent("library", isDirectory: true)
    static let console = root.appendingPathComponent("console", isDirectory: true)
    static let settingsFile = root.appendingPathComponent(".vanta-settings")
    static let tokenFile = root.appendingPathComponent(".vanta-token")
    static let autoexecConfigFile = autoexec.appendingPathComponent(".vanta-autoexec")
    static let consoleInputFile = console.appendingPathComponent("input.txt")

    static let controlHost = "127.0.0.1"
    static let controlPort = 5553
    static let controlBase = URL(string: "http://\(controlHost):\(controlPort)")!

    static func ensure() {
        let fm = FileManager.default
        for folder in [root, workspace, autoexec, library, console] {
            try? fm.createDirectory(at: folder, withIntermediateDirectories: true)
        }
    }
}

enum VantaEnvironment {
    static var robloxVersion: String? {
        guard let info = NSDictionary(
            contentsOfFile: "/Applications/Roblox.app/Contents/Info.plist") else {
            return nil
        }
        return info["CFBundleShortVersionString"] as? String
    }

    static var robloxVersionLine: String {
        if let version = robloxVersion { return "Roblox \(version)" }
        return "Roblox not detected"
    }

    static let journalPath = "/tmp/vanta_executor.log"
}

// MARK: - HTTP

enum VantaHTTP {
    static let session: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 3
        config.timeoutIntervalForResource = 5
        config.waitsForConnectivity = false
        config.connectionProxyDictionary = [:]
        config.httpShouldUsePipelining = false
        return URLSession(configuration: config)
    }()

    static func request(_ method: String, _ path: String, body: Data? = nil,
                        timeout: TimeInterval = 3) -> (Int, Data)? {
        guard let url = URL(string: path, relativeTo: VantaPaths.controlBase) else {
            return nil
        }
        var request = URLRequest(url: url)
        request.httpMethod = method
        request.timeoutInterval = timeout
        request.cachePolicy = .reloadIgnoringLocalCacheData
        // The dylib writes this before it starts listening. Until it exists
        // (Roblox not started, or the dylib not loaded) every request would
        // be answered 401.
        if let token = VantaDisk.readText(VantaPaths.tokenFile)?
                .trimmingCharacters(in: .whitespacesAndNewlines),
           !token.isEmpty {
            request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        }
        if let body {
            request.httpBody = body
            request.setValue("text/plain; charset=utf-8",
                             forHTTPHeaderField: "Content-Type")
        }
        let semaphore = DispatchSemaphore(value: 0)
        var result: (Int, Data)?
        session.dataTask(with: request) { data, response, _ in
            if let response = response as? HTTPURLResponse {
                result = (response.statusCode, data ?? Data())
            }
            semaphore.signal()
        }.resume()
        _ = semaphore.wait(timeout: .now() + timeout + 1)
        return result
    }

    static func status() -> ExecutorStatus? {
        guard let (code, data) = request("GET", "/status", timeout: 1.2),
              code == 200 else {
            return nil
        }
        guard let object = try? JSONSerialization.jsonObject(with: data),
              let fields = object as? [String: Any],
              let state = fields["state"] as? String else {
            // Older dylib: HTTP answered but does not report a state.
            return ExecutorStatus(state: "ready", detail: "executor attached",
                                  game: false, armed: true, queued: 0)
        }
        return ExecutorStatus(state: state,
                              detail: fields["detail"] as? String ?? "",
                              game: fields["game"] as? Bool ?? false,
                              armed: fields["armed"] as? Bool ?? false,
                              queued: fields["queued"] as? Int ?? 0)
    }

    static func post(_ path: String, _ body: String) -> Bool {
        guard let (code, _) = request("POST", path, body: Data(body.utf8)) else {
            return false
        }
        return code == 200
    }
}

// MARK: - Executor link

enum VantaLinkError: LocalizedError {
    case notAttached
    case empty
    case http(Int, String)
    case unreachable

    var errorDescription: String? {
        switch self {
        case .notAttached:
            return "Vanta is not attached. Start Roblox with Vanta first."
        case .empty:
            return "The script is empty."
        case .http(let code, let body):
            return body.isEmpty ? "Executor answered HTTP \(code)."
                                : "Executor answered HTTP \(code): \(body)"
        case .unreachable:
            return "Could not reach the executor on 127.0.0.1:5553."
        }
    }
}

/// The executor's own answer to "can a script run right now". The listener
/// comes up before the environment does, so HTTP 200 alone is not readiness.
struct ExecutorStatus {
    let state: String
    let detail: String
    let game: Bool
    let armed: Bool
    let queued: Int

    var connection: ConnectionState {
        switch state {
        case "ready":     return .attached
        case "reinstall": return .reinstall
        case "failed":    return .failed
        default:          return .waiting
        }
    }
}

final class VantaLink {
    private(set) var state: ConnectionState = .offline
    private(set) var robloxRunning = false
    private(set) var status: ExecutorStatus?
    var onChange: ((ConnectionState) -> Void)?
    var onStatus: ((ExecutorStatus?) -> Void)?

    private var timer: Timer?
    private let probeQueue = DispatchQueue(label: "vanta.http.probe", qos: .utility)
    private var probing = false

    init() {
        refresh()
        let timer = Timer(timeInterval: 1.5, repeats: true) { [weak self] _ in
            self?.refresh()
        }
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer
    }

    deinit { timer?.invalidate() }

    func refresh() {
        guard !probing else { return }
        probing = true
        probeQueue.async { [weak self] in
            guard let self else { return }
            let reported = VantaHTTP.status()
            let running = Self.robloxIsRunning()
            let next: ConnectionState = reported?.connection
                ?? (running ? .waiting : .offline)
            DispatchQueue.main.async {
                self.probing = false
                self.robloxRunning = running
                self.status = reported
                self.onStatus?(reported)
                if next != self.state {
                    self.state = next
                    self.onChange?(next)
                }
            }
        }
    }

    /// Blocking; call from a background queue.
    func send(_ script: String) -> Result<String, VantaLinkError> {
        let payload = Data(script.utf8)
        guard !payload.isEmpty else { return .failure(.empty) }

        guard let (code, data) = VantaHTTP.request("POST", "/execute", body: payload) else {
            return .failure(.unreachable)
        }
        let body = String(decoding: data, as: UTF8.self)
        guard code == 200 else {
            return .failure(.http(code, body))
        }
        if let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           let status = object["status"] as? String {
            return .success(status)
        }
        return .success("accepted")
    }

    static func robloxIsRunning() -> Bool {
        NSWorkspace.shared.runningApplications.contains {
            $0.bundleIdentifier == "com.roblox.RobloxPlayer"
                || $0.executableURL?.lastPathComponent == "RobloxPlayer"
        }
    }
}

// MARK: - Console polling

/// The dylib keeps a bounded console history and exposes it at
/// `/console?since=<seq>`. Polling is a few hundred bytes on loopback and,
/// unlike a socket, a missed poll simply catches up on the next one.
final class ConsoleClient {
    var onRecord: ((String, String) -> Void)?

    private var lastSeq = 0
    private var timer: Timer?
    private let queue = DispatchQueue(label: "vanta.console.poll", qos: .utility)
    private var polling = false

    func start() {
        guard timer == nil else { return }
        let timer = Timer(timeInterval: 0.28, repeats: true) { [weak self] _ in
            self?.poll()
        }
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer
        poll()
    }

    deinit { timer?.invalidate() }

    private func poll() {
        guard !polling else { return }
        polling = true
        queue.async { [weak self] in
            guard let self else { return }
            defer { self.polling = false }
            guard let (code, data) = VantaHTTP.request(
                "GET", "/console?since=\(self.lastSeq)", timeout: 1.2),
                  code == 200,
                  let object = try? JSONSerialization.jsonObject(with: data)
                    as? [String: Any],
                  let records = object["records"] as? [[String: Any]] else {
                return
            }
            var nextSeq = self.lastSeq
            for record in records {
                let seq = record["seq"] as? Int ?? 0
                let name = record["name"] as? String ?? ""
                let message = record["message"] as? String ?? ""
                nextSeq = max(nextSeq, seq)
                DispatchQueue.main.async {
                    self.onRecord?(name, message)
                }
            }
            self.lastSeq = nextSeq
        }
    }

    static func answer(_ text: String) -> Bool {
        VantaHTTP.post("/console/input", text + "\n")
    }
}

// MARK: - Disk

enum VantaDisk {
    static let fm = FileManager.default

    static func readText(_ url: URL) -> String? {
        guard let data = try? Data(contentsOf: url) else { return nil }
        if data.prefix(256 * 1024).contains(0) { return nil }
        return String(data: data, encoding: .utf8)
    }

    @discardableResult
    static func writeText(_ text: String, to url: URL) -> Bool {
        do {
            try text.write(to: url, atomically: true, encoding: .utf8)
            return true
        } catch {
            return false
        }
    }

    static func isTextFile(_ url: URL) -> Bool {
        guard let handle = try? FileHandle(forReadingFrom: url) else { return false }
        defer { try? handle.close() }
        guard let head = try? handle.read(upToCount: 256 * 1024) else { return false }
        return !head.contains(0)
    }

    static func listAutoexec(disabled: Set<String>) -> [FileNode] {
        guard let entries = try? fm.contentsOfDirectory(
            at: VantaPaths.autoexec,
            includingPropertiesForKeys: [.isRegularFileKey, .fileSizeKey],
            options: []) else { return [] }

        var nodes: [FileNode] = []
        var order = 0
        for url in entries.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
            let name = url.lastPathComponent
            if name.hasPrefix(".") { continue }
            let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
            guard values?.isRegularFile == true else { continue }
            if disabled.contains(name) {
                nodes.append(.file(url.path, name, order: nil))
            } else {
                order += 1
                nodes.append(.file(url.path, name, order: order))
            }
        }
        return nodes
    }

    static func listTree(_ root: URL, depth: Int = 0) -> [FileNode] {
        guard depth < 7 else { return [] }
        guard let entries = try? fm.contentsOfDirectory(
            at: root,
            includingPropertiesForKeys: [.isDirectoryKey, .fileSizeKey],
            options: [.skipsHiddenFiles]) else { return [] }

        var nodes: [FileNode] = []
        for url in entries {
            let values = try? url.resourceValues(forKeys: [.isDirectoryKey, .fileSizeKey])
            let isDirectory = values?.isDirectory ?? false
            if isDirectory {
                nodes.append(.folder(url.path, url.lastPathComponent,
                                     listTree(url, depth: depth + 1)))
            } else {
                nodes.append(.file(url.path, url.lastPathComponent))
            }
            if nodes.count >= 500 { break }
        }
        return nodes.sorted {
            if $0.isDirectory != $1.isDirectory { return $0.isDirectory }
            return $0.name.localizedStandardCompare($1.name) == .orderedAscending
        }
    }

    static func countFiles(_ nodes: [FileNode]) -> Int {
        nodes.reduce(0) { partial, node in
            partial + (node.isDirectory ? countFiles(node.children) : 1)
        }
    }
}

// MARK: - Settings files

struct VantaAutoexecConfig {
    var enabled = true
    var off: Set<String> = []

    static func read() -> VantaAutoexecConfig {
        var config = VantaAutoexecConfig()
        guard let text = try? String(contentsOf: VantaPaths.autoexecConfigFile,
                                     encoding: .utf8) else { return config }
        for raw in text.split(separator: "\n", omittingEmptySubsequences: false) {
            let line = raw.trimmingCharacters(in: .whitespacesAndNewlines)
            if line.isEmpty || line.hasPrefix("#") { continue }
            if line.hasPrefix("enabled=") {
                config.enabled = line.dropFirst("enabled=".count) != "0"
            } else if line.hasPrefix("off=") {
                config.off.insert(String(line.dropFirst("off=".count)))
            }
        }
        return config
    }

    func write() {
        var lines = ["# Written by Vanta. Read by the executor at join.",
                     "enabled=\(enabled ? 1 : 0)"]
        lines.append(contentsOf: off.sorted().map { "off=\($0)" })
        VantaDisk.writeText(lines.joined(separator: "\n") + "\n",
                            to: VantaPaths.autoexecConfigFile)
    }
}

struct VantaExecutorSettings {
    var safeMode = true

    static func read() -> VantaExecutorSettings {
        var settings = VantaExecutorSettings()
        guard let text = try? String(contentsOf: VantaPaths.settingsFile,
                                     encoding: .utf8) else { return settings }
        for raw in text.split(separator: "\n", omittingEmptySubsequences: false) {
            let line = raw.trimmingCharacters(in: .whitespacesAndNewlines)
            if line.isEmpty || line.hasPrefix("#") { continue }
            if line.hasPrefix("safemode=") {
                settings.safeMode = line.dropFirst("safemode=".count) != "0"
            }
        }
        return settings
    }

    func write() {
        let text = "# Written by Vanta. Read by the executor at join.\n"
                 + "safemode=\(safeMode ? 1 : 0)\n"
        VantaDisk.writeText(text, to: VantaPaths.settingsFile)
    }
}

// MARK: - Journal tail

/// Keeps up with new lines in the executor journal. The pump writes RUN lines
/// when it takes a queued script, so the UI can distinguish "sent to the HTTP
/// server" from "the executor ran it".
final class JournalWatcher {
    var onLine: ((String) -> Void)?

    private var offset: UInt64 = 0
    private var timer: Timer?
    private let queue = DispatchQueue(label: "vanta.journal", qos: .utility)

    func start() {
        offset = Self.size()
        let timer = Timer(timeInterval: 0.35, repeats: true) { [weak self] _ in
            self?.poll()
        }
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer
    }

    deinit { timer?.invalidate() }

    private static func size() -> UInt64 {
        ((try? FileManager.default.attributesOfItem(
            atPath: VantaEnvironment.journalPath))?[.size] as? UInt64) ?? 0
    }

    private func poll() {
        queue.async { [weak self] in
            guard let self else { return }
            guard let handle = FileHandle(forReadingAtPath: VantaEnvironment.journalPath) else {
                return
            }
            defer { try? handle.close() }
            let end = (try? handle.seekToEnd()) ?? 0
            if end < self.offset { self.offset = 0 }
            if end == self.offset { return }
            try? handle.seek(toOffset: self.offset)
            let data = (try? handle.readToEnd()) ?? Data()
            self.offset = end
            guard let text = String(data: data, encoding: .utf8), !text.isEmpty else {
                return
            }
            let lines = text.split(separator: "\n", omittingEmptySubsequences: true)
                .map(String.init)
            DispatchQueue.main.async {
                for line in lines { self.onLine?(line) }
            }
        }
    }
}
