import SwiftUI
import AppKit

struct ScriptSearchOverlay: View {
    @ObservedObject var model: AppModel
    @ObservedObject var store: ScriptSearchStore
    @FocusState private var searchFocused: Bool
    @State private var sortOpen = false

    var body: some View {
        ZStack {
            Color.black.opacity(0.62)
                .ignoresSafeArea()
                .contentShape(Rectangle())
                .onTapGesture { model.closeSearch() }

            VStack(spacing: 0) {
                header
                Rectangle().fill(V.borderSubtle).frame(height: 1)
                HStack(spacing: 0) {
                    resultsPane
                    Rectangle().fill(V.borderSubtle).frame(width: 1)
                    previewPane
                }
                .frame(maxHeight: .infinity)
                footer
            }
            .frame(width: 1020, height: 680)
            .background(
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .fill(V.panel)
                    .shadow(color: Color.black.opacity(0.7), radius: 52, y: 24)
            )
            .overlay(
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .strokeBorder(V.border, lineWidth: 0.9)
            )
        }
    }

    // MARK: - Header

    private var header: some View {
        VStack(spacing: 10) {
            HStack(spacing: 9) {
                Image(systemName: "books.vertical")
                    .font(.system(size: 12, weight: .medium))
                    .foregroundStyle(V.secondary)
                Text("Script search")
                    .font(F.ui(16, .semibold))
                    .foregroundStyle(V.text)

                HStack(spacing: 1) {
                    ForEach(SearchSource.allCases) { source in
                        Button {
                            store.source = source
                            sortOpen = false
                        } label: {
                            Text(source.label)
                                .font(F.ui(10, .medium))
                                .foregroundStyle(store.source == source ? V.text : V.muted)
                                .padding(.horizontal, 11)
                                .frame(height: 24)
                                .background(
                                    RoundedRectangle(cornerRadius: 6, style: .continuous)
                                        .fill(store.source == source ? V.surface : .clear)
                                )
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(2)
                .background(
                    RoundedRectangle(cornerRadius: 8, style: .continuous)
                        .fill(V.base)
                )
                .overlay(
                    RoundedRectangle(cornerRadius: 8, style: .continuous)
                        .strokeBorder(V.borderSubtle, lineWidth: 0.8)
                )

                Spacer()

                Button(action: model.closeSearch) {
                    Image(systemName: "xmark")
                        .font(.system(size: 9, weight: .semibold))
                        .foregroundStyle(V.faint)
                        .frame(width: 25, height: 25)
                }
                .buttonStyle(.plain)
                .hoverSurface(radius: 5, hover: V.surface)
            }

            HStack(spacing: 8) {
                Image(systemName: "magnifyingglass")
                    .font(.system(size: 10, weight: .medium))
                    .foregroundStyle(searchFocused ? V.secondary : V.faint)
                TextField(store.source == .local
                          ? "Search autoexec, workspace and library…"
                          : "Search ScriptBlox…",
                          text: $store.query)
                    .textFieldStyle(.plain)
                    .font(F.ui(13))
                    .foregroundStyle(V.text)
                    .focused($searchFocused)
                    .onSubmit { store.reload() }
                if store.loading {
                    ProgressView()
                        .controlSize(.small)
                        .scaleEffect(0.65)
                } else if !store.query.isEmpty {
                    Button { store.query = "" } label: {
                        Image(systemName: "xmark.circle.fill")
                            .font(.system(size: 10))
                            .foregroundStyle(V.faint)
                    }
                    .buttonStyle(.plain)
                }
                Text("↵ search")
                    .font(F.mono(8.5))
                    .foregroundStyle(V.ghost)
            }
            .padding(.horizontal, 11)
            .frame(height: 34)
            .background(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .fill(V.surface)
            )
            .overlay(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .strokeBorder(searchFocused ? V.secondary.opacity(0.35) : V.borderSubtle,
                                  lineWidth: 0.8)
            )

            if store.source == .scriptblox {
                filterRow
                if sortOpen { sortRow }
            }

            if store.source == .scriptblox,
               store.query.isEmpty,
               !store.recentQueries.isEmpty {
                recentRow
            }
        }
        .padding(.horizontal, 16)
        .padding(.top, 14)
        .padding(.bottom, 12)
        .background(V.chrome)
        .onAppear {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) {
                searchFocused = true
            }
        }
    }

    private var filterRow: some View {
        HStack(spacing: 6) {
            ForEach(SearchFilters.Toggle.allCases) { toggle in
                FilterChip(label: toggle.label,
                           selected: store.filters[toggle]) {
                    store.filters[toggle].toggle()
                }
            }
            if store.filters.isActive {
                Button(action: store.clearFilters) {
                    Text("Clear")
                        .font(F.ui(9.5, .medium))
                        .foregroundStyle(V.faint)
                        .padding(.horizontal, 5)
                }
                .buttonStyle(.plain)
            }
            Spacer()
            Button {
                sortOpen.toggle()
            } label: {
                HStack(spacing: 6) {
                    Text(store.sort.label)
                        .font(F.ui(10, .medium))
                    Image(systemName: "chevron.down")
                        .font(.system(size: 7.5, weight: .bold))
                        .rotationEffect(.degrees(sortOpen ? 180 : 0))
                }
                .foregroundStyle(V.muted)
                .padding(.horizontal, 8)
                .frame(height: 25)
                .background(
                    RoundedRectangle(cornerRadius: 6, style: .continuous)
                        .fill(V.surface)
                )
                .overlay(
                    RoundedRectangle(cornerRadius: 6, style: .continuous)
                        .strokeBorder(V.borderSubtle, lineWidth: 0.8)
                )
            }
            .buttonStyle(.plain)
            .animation(.easeOut(duration: 0.14), value: sortOpen)
        }
    }

    private var sortRow: some View {
        HStack(spacing: 6) {
            Text("Sort")
                .font(F.ui(9.5, .semibold))
                .foregroundStyle(V.ghost)
            ForEach(SearchSort.allCases) { option in
                FilterChip(label: option.label,
                           selected: store.sort == option) {
                    store.sort = option
                    sortOpen = false
                }
            }
            Spacer()
        }
        .transition(.opacity.combined(with: .move(edge: .top)))
    }

    private var recentRow: some View {
        HStack(spacing: 6) {
            Image(systemName: "clock")
                .font(.system(size: 9))
                .foregroundStyle(V.ghost)
            ForEach(store.recentQueries.prefix(5), id: \.self) { query in
                Button {
                    store.query = query
                    store.reload()
                } label: {
                    Text(query)
                        .font(F.ui(9.5))
                        .foregroundStyle(V.muted)
                        .padding(.horizontal, 7)
                        .frame(height: 22)
                        .background(
                            RoundedRectangle(cornerRadius: 5, style: .continuous)
                                .fill(V.surface.opacity(0.7))
                        )
                }
                .buttonStyle(.plain)
            }
            Spacer()
        }
    }

    // MARK: - Results

    private var resultsPane: some View {
        VStack(spacing: 0) {
            HStack(spacing: 7) {
                Text(store.source == .local ? "FILES" : "RESULTS")
                    .font(F.ui(8.5, .semibold))
                    .tracking(1.1)
                    .foregroundStyle(V.ghost)
                Text("\(store.results.count)")
                    .font(F.mono(9))
                    .foregroundStyle(V.faint)
                if store.filters.isActive {
                    Text("· filtered")
                        .font(F.ui(9))
                        .foregroundStyle(V.ghost)
                }
                Spacer()
                if store.loading && !store.results.isEmpty {
                    ProgressView().controlSize(.small).scaleEffect(0.65)
                }
            }
            .padding(.horizontal, 14)
            .frame(height: 32)

            if store.loading && store.results.isEmpty {
                skeletonList
            } else if let failure = store.failure, store.results.isEmpty {
                emptyState(failure)
            } else {
                ScrollView(.vertical, showsIndicators: false) {
                    LazyVStack(spacing: 2) {
                        ForEach(store.results) { result in
                            SearchResultRow(result: result,
                                            selected: store.selected?.id == result.id) {
                                store.select(result)
                            }
                        }
                        if store.canLoadMore {
                            Button(action: store.loadMore) {
                                HStack(spacing: 6) {
                                    if store.loadingMore {
                                        ProgressView().controlSize(.small).scaleEffect(0.6)
                                    }
                                    Text(store.loadingMore ? "Loading…" : "Load more results")
                                        .font(F.ui(10, .medium))
                                        .foregroundStyle(V.muted)
                                }
                                .frame(maxWidth: .infinity)
                                .frame(height: 38)
                            }
                            .buttonStyle(.plain)
                            .hoverSurface(radius: 7, hover: V.surface)
                            .padding(.horizontal, 6)
                            .padding(.top, 6)
                        }
                    }
                    .padding(.horizontal, 7)
                    .padding(.bottom, 8)
                }
            }
        }
        .frame(width: 398)
        .background(V.panel)
    }

    private var skeletonList: some View {
        VStack(spacing: 8) {
            ForEach(0..<7, id: \.self) { _ in
                HStack(spacing: 10) {
                    RoundedRectangle(cornerRadius: 8)
                        .fill(V.surface)
                        .frame(width: 42, height: 42)
                    VStack(alignment: .leading, spacing: 7) {
                        RoundedRectangle(cornerRadius: 3)
                            .fill(V.surface)
                            .frame(height: 10)
                        RoundedRectangle(cornerRadius: 3)
                            .fill(V.surface.opacity(0.65))
                            .frame(width: 150, height: 8)
                    }
                }
                .padding(.horizontal, 10)
                .frame(height: 58)
            }
            Spacer()
        }
        .padding(.top, 8)
        .padding(.horizontal, 7)
    }

    private func emptyState(_ message: String) -> some View {
        VStack(spacing: 10) {
            Image(systemName: "tray")
                .font(.system(size: 22, weight: .ultraLight))
                .foregroundStyle(V.faint)
            Text(message)
                .font(F.ui(11))
                .foregroundStyle(V.muted)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 34)
            HStack(spacing: 8) {
                Button(action: store.retry) {
                    Text("Retry")
                        .font(F.ui(10, .medium))
                        .foregroundStyle(V.secondary)
                        .padding(.horizontal, 12)
                        .frame(height: 26)
                        .background(RoundedRectangle(cornerRadius: 6).fill(V.surface))
                        .overlay(RoundedRectangle(cornerRadius: 6)
                            .strokeBorder(V.border, lineWidth: 0.8))
                }
                .buttonStyle(.plain)
                if store.filters.isActive {
                    Button(action: store.clearFilters) {
                        Text("Clear filters")
                            .font(F.ui(10, .medium))
                            .foregroundStyle(V.muted)
                            .padding(.horizontal, 10)
                            .frame(height: 26)
                    }
                    .buttonStyle(.plain)
                }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    // MARK: - Preview

    private var previewPane: some View {
        VStack(spacing: 0) {
            if let selected = store.selected {
                previewHeader(selected)
                Rectangle().fill(V.borderSubtle).frame(height: 1)

                if store.loadingSource {
                    VStack(spacing: 10) {
                        ProgressView().controlSize(.small)
                        Text("Fetching source…")
                            .font(F.ui(10.5))
                            .foregroundStyle(V.faint)
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else if let source = store.sourceText {
                    CodeEditor(text: .constant(source),
                               language: .luau,
                               fontSize: 11.5,
                               showLineNumbers: true,
                               readOnly: true)
                        .id(selected.id)
                } else {
                    Text("Select the result to load its source.")
                        .font(F.ui(10.5))
                        .foregroundStyle(V.faint)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                }

                Rectangle().fill(V.borderSubtle).frame(height: 1)
                previewFooter(selected)
            } else {
                VStack(spacing: 11) {
                    VMark(size: 40).opacity(0.8)
                    Text("Select a script")
                        .font(F.ui(12, .medium))
                        .foregroundStyle(V.secondary)
                    Text(store.source == .local
                         ? "Search your own folders."
                         : "Search ScriptBlox, inspect the source, then open, save or run it.")
                        .font(F.ui(10.5))
                        .foregroundStyle(V.faint)
                        .multilineTextAlignment(.center)
                        .padding(.horizontal, 40)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(V.base)
    }

    private func previewHeader(_ result: ScriptResult) -> some View {
        HStack(spacing: 12) {
            ScriptThumbnail(url: result.thumbnail, size: 52)
            VStack(alignment: .leading, spacing: 3) {
                Text(result.title)
                    .font(F.ui(13.5, .semibold))
                    .foregroundStyle(V.text)
                    .lineLimit(1)
                HStack(spacing: 6) {
                    if !result.game.isEmpty {
                        Text(result.game)
                            .font(F.ui(10.5))
                            .foregroundStyle(V.faint)
                            .lineLimit(1)
                    }
                    if result.isVerified {
                        MiniTag(text: "VERIFIED", color: V.secondary)
                    }
                    if result.hasKey {
                        MiniTag(text: "KEY", color: V.warning)
                    }
                    if result.isPatched {
                        MiniTag(text: "PATCHED", color: V.danger)
                    }
                    if result.isPaid {
                        MiniTag(text: "PAID", color: V.warning)
                    }
                }
                Text(datesLine(result))
                    .font(F.mono(8.5))
                    .foregroundStyle(V.ghost)
            }
            Spacer()
            VStack(alignment: .trailing, spacing: 4) {
                Text("\(compact(result.views)) views")
                    .font(F.mono(9))
                    .foregroundStyle(V.muted)
                Text("\(compact(result.likes)) likes")
                    .font(F.mono(9))
                    .foregroundStyle(V.ghost)
            }
        }
        .padding(.horizontal, 15)
        .frame(height: 76)
    }

    private func previewFooter(_ result: ScriptResult) -> some View {
        HStack(spacing: 8) {
            Text("\(compact(result.executes)) executes")
                .font(F.mono(9))
                .foregroundStyle(V.ghost)
            Spacer()

            Button {
                guard let source = store.sourceText else { return }
                model.saveSearchResultToLibrary(result, source: source)
            } label: {
                HStack(spacing: 5) {
                    Image(systemName: "arrow.down.to.line")
                        .font(.system(size: 8.5, weight: .medium))
                    Text("Save")
                        .font(F.ui(10, .medium))
                }
                .foregroundStyle(V.secondary)
                .padding(.horizontal, 10)
                .frame(height: 27)
                .background(RoundedRectangle(cornerRadius: 6.5).fill(V.surface))
                .overlay(RoundedRectangle(cornerRadius: 6.5)
                    .strokeBorder(V.border, lineWidth: 0.8))
            }
            .buttonStyle(.plain)
            .disabled(store.sourceText == nil || result.isLocal)

            Button {
                guard let source = store.sourceText else { return }
                model.openSearchResult(result, source: source)
            } label: {
                Text("Open")
                    .font(F.ui(10, .medium))
                    .foregroundStyle(V.secondary)
                    .padding(.horizontal, 12)
                    .frame(height: 27)
                    .background(RoundedRectangle(cornerRadius: 6.5).fill(V.surface))
                    .overlay(RoundedRectangle(cornerRadius: 6.5)
                        .strokeBorder(V.border, lineWidth: 0.8))
            }
            .buttonStyle(.plain)
            .disabled(store.sourceText == nil)

            Button {
                guard let source = store.sourceText else { return }
                model.executeText(source, name: result.title)
                model.closeSearch()
            } label: {
                HStack(spacing: 5) {
                    Image(systemName: "play.fill")
                        .font(.system(size: 8, weight: .bold))
                    Text("Run")
                        .font(F.ui(10, .semibold))
                }
                .foregroundStyle(V.onAccent)
                .padding(.horizontal, 13)
                .frame(height: 27)
                .background(RoundedRectangle(cornerRadius: 6.5).fill(V.accent))
            }
            .buttonStyle(.plain)
            .opacity(model.connection.canExecute ? 1 : 0.4)
            .disabled(store.sourceText == nil || !model.connection.canExecute)
        }
        .padding(.horizontal, 15)
        .frame(height: 52)
    }

    private var footer: some View {
        HStack(spacing: 8) {
            Text(store.loading ? "Searching…" : (store.failure ?? "Ready"))
                .font(F.ui(10))
                .foregroundStyle(V.faint)
            Spacer()
            Text(store.source == .local
                 ? "Reads the same autoexec, workspace and library as the executor."
                 : "Third-party scripts. Inspect before running.")
                .font(F.ui(9))
                .foregroundStyle(V.ghost)
            if store.source == .scriptblox {
                Text("Powered by ScriptBlox.com")
                    .font(F.ui(9))
                    .foregroundStyle(V.ghost)
            }
        }
        .padding(.horizontal, 14)
        .frame(height: 32)
        .background(V.chrome)
        .overlay(alignment: .top) {
            Rectangle().fill(V.borderSubtle).frame(height: 1)
        }
    }

    private func datesLine(_ result: ScriptResult) -> String {
        if result.isLocal {
            return "modified \(result.updatedText)"
        }
        var parts: [String] = []
        if !result.uploadedText.isEmpty { parts.append("uploaded \(result.uploadedText)") }
        if !result.updatedText.isEmpty { parts.append("updated \(result.updatedText)") }
        return parts.joined(separator: "  ·  ")
    }

    private func compact(_ value: Int) -> String {
        if value >= 1_000_000 { return String(format: "%.1fM", Double(value) / 1_000_000) }
        if value >= 1_000 { return String(format: "%.1fk", Double(value) / 1_000) }
        return "\(value)"
    }
}

// MARK: - Pieces

struct ScriptThumbnail: View {
    let url: URL?
    var size: CGFloat = 40

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: size * 0.18, style: .continuous)
                .fill(V.surface)
            if let url {
                AsyncImage(url: url) { phase in
                    switch phase {
                    case .success(let image):
                        image.resizable().scaledToFill()
                    case .failure:
                        fallback
                    default:
                        fallback.opacity(0.5)
                    }
                }
            } else {
                fallback
            }
        }
        .frame(width: size, height: size)
        .clipShape(RoundedRectangle(cornerRadius: size * 0.18, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: size * 0.18, style: .continuous)
                .strokeBorder(V.borderSubtle, lineWidth: 0.8)
        )
    }

    private var fallback: some View {
        Image(systemName: "doc.text")
            .font(.system(size: size * 0.32, weight: .regular))
            .foregroundStyle(V.faint)
    }
}

struct SearchResultRow: View {
    let result: ScriptResult
    let selected: Bool
    let action: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            HStack(spacing: 10) {
                ScriptThumbnail(url: result.thumbnail, size: 42)

                VStack(alignment: .leading, spacing: 3) {
                    Text(result.title)
                        .font(F.ui(12, .medium))
                        .foregroundStyle(selected ? V.text : V.secondary)
                        .lineLimit(1)
                    HStack(spacing: 5) {
                        if !result.game.isEmpty {
                            Text(result.game)
                                .font(F.ui(9.5))
                                .foregroundStyle(V.faint)
                                .lineLimit(1)
                        }
                        if result.hasKey { MiniTag(text: "KEY", color: V.warning) }
                        if result.isPatched { MiniTag(text: "PATCHED", color: V.danger) }
                    }
                    HStack(spacing: 6) {
                        Text("\(compact(result.views)) views")
                            .font(F.mono(8.5))
                            .foregroundStyle(V.ghost)
                        if !result.uploadedText.isEmpty {
                            Text("·")
                                .font(F.mono(8.5))
                                .foregroundStyle(V.ghost)
                            Text(result.uploadedText)
                                .font(F.mono(8.5))
                                .foregroundStyle(V.ghost)
                        }
                    }
                }
                Spacer(minLength: 2)
            }
            .padding(.horizontal, 8)
            .frame(height: 62)
            .background(
                RoundedRectangle(cornerRadius: 7, style: .continuous)
                    .fill(selected ? V.surface
                          : (hovering ? V.surface.opacity(0.55) : .clear))
            )
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .animation(.easeOut(duration: 0.14), value: hovering)
    }

    private func compact(_ value: Int) -> String {
        if value >= 1_000_000 { return String(format: "%.1fM", Double(value) / 1_000_000) }
        if value >= 1_000 { return String(format: "%.1fk", Double(value) / 1_000) }
        return "\(value)"
    }
}

struct FilterChip: View {
    let label: String
    let selected: Bool
    let action: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            Text(label)
                .font(F.ui(9.5, selected ? .medium : .regular))
                .foregroundStyle(selected ? V.text : V.muted)
                .padding(.horizontal, 8)
                .frame(height: 24)
                .background(
                    RoundedRectangle(cornerRadius: 6, style: .continuous)
                        .fill(selected ? V.surfaceHigh
                              : (hovering ? V.surface.opacity(0.7) : .clear))
                )
                .overlay(
                    RoundedRectangle(cornerRadius: 6, style: .continuous)
                        .strokeBorder(selected ? V.border : V.borderSubtle,
                                      lineWidth: 0.8)
                )
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .animation(.easeOut(duration: 0.14), value: hovering)
    }
}

struct MiniTag: View {
    let text: String
    let color: Color
    var body: some View {
        Text(text)
            .font(F.ui(8, .bold))
            .tracking(0.5)
            .fixedSize()
            .foregroundStyle(color)
    }
}

