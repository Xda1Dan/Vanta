import Foundation
import SwiftUI

// MARK: - Result

struct ScriptResult: Identifiable, Equatable {
    let id: String
    let title: String
    let game: String
    let source: String?
    let rawURL: URL?
    let path: String?
    let thumbnail: URL?
    let views: Int
    let likes: Int
    let executes: Int
    let hasKey: Bool
    let isPatched: Bool
    let isVerified: Bool
    let isUniversal: Bool
    let isPaid: Bool
    let isMobileReady: Bool
    let risk: String?
    let uploaded: Date?
    let updated: Date?
    let matched: [String]

    init(id: String, title: String, game: String, source: String? = nil,
         rawURL: URL? = nil, path: String? = nil, thumbnail: URL? = nil,
         views: Int = 0, likes: Int = 0, executes: Int = 0,
         hasKey: Bool = false, isPatched: Bool = false,
         isVerified: Bool = false, isUniversal: Bool = false,
         isPaid: Bool = false, isMobileReady: Bool = false,
         risk: String? = nil, uploaded: Date? = nil, updated: Date? = nil,
         matched: [String] = []) {
        self.id = id
        self.title = title
        self.game = game
        self.source = source
        self.rawURL = rawURL
        self.path = path
        self.thumbnail = thumbnail
        self.views = views
        self.likes = likes
        self.executes = executes
        self.hasKey = hasKey
        self.isPatched = isPatched
        self.isVerified = isVerified
        self.isUniversal = isUniversal
        self.isPaid = isPaid
        self.isMobileReady = isMobileReady
        self.risk = risk
        self.uploaded = uploaded
        self.updated = updated
        self.matched = matched
    }

    static func == (a: ScriptResult, b: ScriptResult) -> Bool { a.id == b.id }

    var originLabel: String { path != nil ? "LOCAL" : "SCRIPTBLOX" }
    var isLocal: Bool { path != nil }
    var uploadedText: String { RelativeTime.short(uploaded) }
    var updatedText: String { RelativeTime.short(updated) }
}

// MARK: - Relative dates

enum RelativeTime {
    static func short(_ date: Date?) -> String {
        guard let date else { return "" }
        let seconds = max(0, Date().timeIntervalSince(date))
        if seconds < 45 { return "just now" }
        if seconds < 3600 { return "\(max(1, Int(seconds / 60)))m ago" }
        if seconds < 86_400 { return "\(Int(seconds / 3600))h ago" }
        if seconds < 7 * 86_400 { return "\(Int(seconds / 86_400))d ago" }
        if seconds < 30 * 86_400 { return "\(Int(seconds / (7 * 86_400)))w ago" }
        if seconds < 365 * 86_400 { return "\(Int(seconds / (30 * 86_400)))mo ago" }
        return "\(Int(seconds / (365 * 86_400)))y ago"
    }
}

enum SearchSource: String, CaseIterable, Identifiable, Codable {
    case local
    case scriptblox

    var id: String { rawValue }
    var label: String {
        switch self {
        case .local: return "Local"
        case .scriptblox: return "ScriptBlox"
        }
    }
}

// MARK: - Filters and sort

struct SearchFilters: Codable, Equatable {
    var working = false
    var noKey = false
    var verified = false
    var universal = false
    var free = false

    enum Toggle: String, CaseIterable, Identifiable {
        case working, noKey, verified, universal, free
        var id: String { rawValue }
        var label: String {
            switch self {
            case .working: return "Working"
            case .noKey: return "No key"
            case .verified: return "Verified"
            case .universal: return "Universal"
            case .free: return "Free"
            }
        }
    }

    subscript(toggle: Toggle) -> Bool {
        get {
            switch toggle {
            case .working: return working
            case .noKey: return noKey
            case .verified: return verified
            case .universal: return universal
            case .free: return free
            }
        }
        set {
            switch toggle {
            case .working: working = newValue
            case .noKey: noKey = newValue
            case .verified: verified = newValue
            case .universal: universal = newValue
            case .free: free = newValue
            }
        }
    }

    var activeCount: Int {
        Toggle.allCases.filter { self[$0] }.count
    }

    var isActive: Bool { activeCount > 0 }

    func matches(_ result: ScriptResult) -> Bool {
        if working && result.isPatched { return false }
        if noKey && result.hasKey { return false }
        if verified && !result.isVerified { return false }
        if universal && !result.isUniversal { return false }
        if free && result.isPaid { return false }
        return true
    }

    mutating func clear() {
        self = SearchFilters()
    }
}

enum SearchSort: String, CaseIterable, Identifiable, Codable {
    case best, views, likes, newest, updated

    var id: String { rawValue }
    var label: String {
        switch self {
        case .best: return "Best match"
        case .views: return "Most views"
        case .likes: return "Most likes"
        case .newest: return "Newest"
        case .updated: return "Recently updated"
        }
    }
}

// MARK: - Store

struct SavedSearchState: Codable {
    var source: SearchSource
    var query: String
    var filters: SearchFilters
    var sort: SearchSort
}

final class ScriptSearchStore: ObservableObject {
    @Published var source: SearchSource = .local {
        didSet {
            guard oldValue != source, !restoring else { return }
            saveState()
            selected = nil
            sourceText = nil
            reload()
        }
    }

    @Published var query = "" {
        didSet {
            guard oldValue != query, !restoring else { return }
            saveState()
            scheduleSearch()
        }
    }

    @Published var filters = SearchFilters() {
        didSet {
            guard oldValue != filters, !restoring else { return }
            saveState()
            applyFiltersAndSort()
        }
    }

    @Published var sort: SearchSort = .best {
        didSet {
            guard oldValue != sort, !restoring else { return }
            saveState()
            applyFiltersAndSort()
        }
    }

    @Published private(set) var results: [ScriptResult] = []
    @Published private(set) var loading = false
    @Published private(set) var loadingMore = false
    @Published private(set) var failure: String?
    @Published private(set) var totalResults = 0
    @Published var selected: ScriptResult?
    @Published private(set) var sourceText: String?
    @Published private(set) var loadingSource = false
    @Published private(set) var recentQueries: [String] = []

    var localProvider: ((String) -> [ScriptResult])?

    var canLoadMore: Bool {
        source == .scriptblox && hasMore && !loading && !loadingMore
    }

    private var opened = false
    private var debounce: DispatchWorkItem?
    private var generation = 0
    private var page = 1
    private var hasMore = false
    private var rawResults: [ScriptResult] = []
    private var restoring = false

    private static let stateKey = "vanta.search.state"
    private static let recentKey = "vanta.search.recent"

    private static let session: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 20
        config.timeoutIntervalForResource = 30
        config.waitsForConnectivity = false
        config.connectionProxyDictionary = [:]
        return URLSession(configuration: config)
    }()

    private static let iso: ISO8601DateFormatter = {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return formatter
    }()

    private static let isoPlain: ISO8601DateFormatter = {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime]
        return formatter
    }()

    init() {
        restoreState()
    }

    func openIfNeeded() {
        guard !opened else { return }
        opened = true
        reload()
    }

    func cancel() {
        debounce?.cancel()
        generation += 1
        // A request already in flight is ignored by the generation check, but
        // nothing else clears its spinner flags - so reopening the search
        // would show "loading" for a request that no longer exists.
        loading = false
        loadingMore = false
    }

    private func scheduleSearch() {
        debounce?.cancel()
        let work = DispatchWorkItem { [weak self] in self?.reload() }
        debounce = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.32, execute: work)
    }

    func reload() {
        debounce?.cancel()
        generation += 1
        let mine = generation
        selected = nil
        sourceText = nil
        failure = nil
        // Switching away from ScriptBlox while a fetch is in flight used to
        // leave `loading` true: the completion is generation-guarded and
        // returns without clearing it.
        loading = false
        loadingMore = false
        page = 1
        hasMore = false
        rawResults = []
        results = []
        totalResults = 0

        if source == .local {
            let local = localProvider?(query) ?? []
            rawResults = local
            totalResults = local.count
            applyFiltersAndSort()
            if local.isEmpty {
                failure = query.trimmingCharacters(in: .whitespaces).isEmpty
                    ? "No scripts in autoexec, workspace or library yet."
                    : "No local scripts match."
            }
            return
        }

        loading = true
        recordQuery(query)
        fetchScriptBlox(query: query, page: 1, generation: mine)
    }

    func loadMore() {
        guard source == .scriptblox, hasMore, !loading, !loadingMore else { return }
        loadingMore = true
        let next = page + 1
        fetchScriptBlox(query: query, page: next, generation: generation)
    }

    func retry() { reload() }

    private func fetchScriptBlox(query: String, page: Int, generation mine: Int) {
        var components = URLComponents(
            string: "https://scriptblox.com/api/script/"
                + (query.isEmpty ? "fetch" : "search"))!
        var items = [URLQueryItem(name: "page", value: String(page)),
                     URLQueryItem(name: "max", value: "50")]
        if !query.isEmpty {
            items.insert(URLQueryItem(name: "q", value: query), at: 0)
        }
        components.queryItems = items

        guard let url = components.url else {
            loading = false
            loadingMore = false
            failure = "Could not build the search URL."
            return
        }

        var request = URLRequest(url: url)
        request.setValue("VantaUI/1.0 (native macOS)", forHTTPHeaderField: "User-Agent")
        request.cachePolicy = .reloadIgnoringLocalCacheData
        let finalRequest = request

        Task { [weak self] in
            do {
                let (data, response) = try await Self.session.data(for: finalRequest)
                let code = (response as? HTTPURLResponse)?.statusCode ?? 0
                guard (200..<300).contains(code) else {
                    throw SearchError.http(code)
                }
                let object = try JSONSerialization.jsonObject(with: data)
                    as? [String: Any] ?? [:]
                guard let result = object["result"] as? [String: Any],
                      let scripts = result["scripts"] as? [[String: Any]] else {
                    throw SearchError.badResponse
                }

                var list: [ScriptResult] = []
                list.reserveCapacity(scripts.count)
                for script in scripts {
                    list.append(Self.parseScriptBlox(script))
                }
                let totalPages = result["totalPages"] as? Int ?? page
                let more = page < totalPages
                let finalList = list

                DispatchQueue.main.async { [weak self] in
                    guard let self, mine == self.generation else { return }
                    self.loading = false
                    self.loadingMore = false
                    if page == 1 {
                        self.rawResults = finalList
                    } else {
                        self.rawResults.append(contentsOf: finalList)
                    }
                    self.page = page
                    self.hasMore = more
                    self.applyFiltersAndSort()
                    if self.rawResults.isEmpty {
                        self.failure = "Nothing found."
                    }
                }
            } catch {
                DispatchQueue.main.async { [weak self] in
                    guard let self, mine == self.generation else { return }
                    self.loading = false
                    self.loadingMore = false
                    self.failure = error.localizedDescription
                }
            }
        }
    }

    private func applyFiltersAndSort() {
        if source == .local {
            results = rawResults
            totalResults = rawResults.count
            return
        }

        var filtered = rawResults.filter { filters.matches($0) }
        if query.isEmpty {
            if sort != .best {
                filtered = sorted(filtered)
            }
        } else {
            switch sort {
            case .best:
                filtered = filtered.sorted {
                    let a = relevance($0)
                    let b = relevance($1)
                    return a == b ? $0.views > $1.views : a > b
                }
            default:
                filtered = sorted(filtered)
            }
        }
        results = filtered
        totalResults = filtered.count
        if let selected, !filtered.contains(where: { $0.id == selected.id }) {
            self.selected = nil
            sourceText = nil
        }
        if filtered.isEmpty && !loading && failure == nil {
            failure = filters.isActive ? "No results with those filters." : "Nothing found."
        }
        if !filtered.isEmpty {
            failure = nil
        }
    }

    private func sorted(_ list: [ScriptResult]) -> [ScriptResult] {
        switch sort {
        case .views:
            return list.sorted { $0.views > $1.views }
        case .likes:
            return list.sorted { $0.likes > $1.likes }
        case .newest:
            return list.sorted { ($0.uploaded ?? .distantPast) > ($1.uploaded ?? .distantPast) }
        case .updated:
            return list.sorted { ($0.updated ?? .distantPast) > ($1.updated ?? .distantPast) }
        case .best:
            return list
        }
    }

    private func relevance(_ result: ScriptResult) -> Double {
        let q = query.lowercased().trimmingCharacters(in: .whitespacesAndNewlines)
        guard !q.isEmpty else { return 0 }
        let title = result.title.lowercased()
        let game = result.game.lowercased()
        let tokens = q.split(whereSeparator: { !$0.isLetter && !$0.isNumber })
            .map(String.init)
        let matched = Set(result.matched.map { $0.lowercased() })

        var score = 0.0
        if title == q { score += 220 }
        if title.contains(q) { score += 100 }
        if !tokens.isEmpty && tokens.allSatisfy({ title.contains($0) }) {
            score += 60
        }
        for token in tokens where title.contains(token) {
            score += 12
        }
        if matched.contains("title") { score += 25 }
        if matched.contains("features") || matched.contains("tags") { score -= 12 }
        if !q.isEmpty && game.contains(q) { score += 10 }
        for token in tokens where game.contains(token) { score += 4 }
        if result.isVerified { score += 6 }
        if !result.isPatched { score += 4 }
        if result.isUniversal { score += 2 }
        score += log10(Double(result.views + 1)) * 3
        score += log10(Double(result.likes + 1)) * 2
        if let updated = result.updated {
            let days = Date().timeIntervalSince(updated) / 86_400
            if days < 7 { score += 5 } else if days < 30 { score += 2 }
        }
        return score
    }

    func clearFilters() {
        filters.clear()
    }

    // MARK: Selection

    func select(_ result: ScriptResult) {
        selected = result
        sourceText = nil
        generation += 1
        let mine = generation

        if let inline = result.source, !inline.isEmpty {
            sourceText = inline
            return
        }
        guard let url = result.rawURL else {
            sourceText = "-- this result has no source attached"
            return
        }

        loadingSource = true
        Task { [weak self] in
            var text = "-- could not fetch the script source"
            if let (data, response) = try? await Self.session.data(from: url),
               (200..<300).contains((response as? HTTPURLResponse)?.statusCode ?? 0),
               let body = String(data: data, encoding: .utf8) {
                if let object = try? JSONSerialization.jsonObject(with: data)
                    as? [String: Any] {
                    text = (object["script"] as? String)
                        ?? (object["result"] as? [String: Any])?["script"] as? String
                        ?? body
                } else {
                    text = body
                }
            }
            let fetched = text
            DispatchQueue.main.async { [weak self] in
                guard let self, mine == self.generation else { return }
                self.sourceText = fetched
                self.loadingSource = false
            }
        }
    }

    // MARK: Parsing

    private static func parseScriptBlox(_ script: [String: Any]) -> ScriptResult {
        let game = script["game"] as? [String: Any]
        let slug = script["slug"] as? String
        let image = (script["image"] as? String)
            ?? (game?["imageUrl"] as? String)
        let site = URL(string: "https://scriptblox.com")!
        let thumbnail = image.flatMap {
            URL(string: $0, relativeTo: site)?.absoluteURL
        }
        let created = (script["createdAt"] as? String).flatMap(parseDate)
        let reviewed = (script["reviewedAt"] as? String).flatMap(parseDate)
        let bumped = (script["lastBump"] as? String).flatMap(parseDate)

        return ScriptResult(
            id: script["_id"] as? String ?? UUID().uuidString,
            title: script["title"] as? String ?? "Untitled",
            game: game?["name"] as? String ?? "",
            source: script["script"] as? String,
            rawURL: slug.flatMap {
                URL(string: "https://scriptblox.com/api/script/raw/\($0)")
            },
            path: nil,
            thumbnail: thumbnail,
            views: script["views"] as? Int ?? 0,
            likes: script["likeCount"] as? Int ?? 0,
            executes: script["executes"] as? Int ?? 0,
            hasKey: script["key"] as? Bool ?? false,
            isPatched: script["isPatched"] as? Bool ?? false,
            isVerified: script["verified"] as? Bool ?? false,
            isUniversal: script["isUniversal"] as? Bool ?? false,
            isPaid: (script["scriptType"] as? String) == "paid",
            isMobileReady: false,
            risk: nil,
            uploaded: created,
            updated: reviewed ?? bumped,
            matched: script["matched"] as? [String] ?? [])
    }

    private static func parseDate(_ text: String) -> Date? {
        iso.date(from: text) ?? isoPlain.date(from: text)
    }

    // MARK: Persistence

    private func saveState() {
        guard !restoring else { return }
        let state = SavedSearchState(source: source, query: query,
                                     filters: filters, sort: sort)
        guard let data = try? JSONEncoder().encode(state) else { return }
        UserDefaults.standard.set(data, forKey: Self.stateKey)
    }

    private func restoreState() {
        restoring = true
        defer { restoring = false }
        recentQueries = UserDefaults.standard.stringArray(forKey: Self.recentKey) ?? []
        guard let data = UserDefaults.standard.data(forKey: Self.stateKey),
              let saved = try? JSONDecoder().decode(SavedSearchState.self, from: data)
        else { return }
        source = saved.source
        query = saved.query
        filters = saved.filters
        sort = saved.sort
    }

    private func recordQuery(_ text: String) {
        let clean = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard clean.count > 1 else { return }
        recentQueries.removeAll { $0.caseInsensitiveCompare(clean) == .orderedSame }
        recentQueries.insert(clean, at: 0)
        if recentQueries.count > 8 { recentQueries.removeLast(recentQueries.count - 8) }
        UserDefaults.standard.set(recentQueries, forKey: Self.recentKey)
    }

    enum SearchError: LocalizedError {
        case http(Int)
        case badResponse

        var errorDescription: String? {
            switch self {
            case .http(let code):
                return "ScriptBlox answered HTTP \(code)."
            case .badResponse:
                return "ScriptBlox sent an unexpected response."
            }
        }
    }
}
