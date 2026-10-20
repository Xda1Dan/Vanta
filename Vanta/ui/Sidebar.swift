import SwiftUI
import UniformTypeIdentifiers

struct Sidebar: View {
    @ObservedObject var model: AppModel
    @FocusState private var searchFocused: Bool
    @State private var dropActive = false

    private var filtering: Bool {
        !model.filter.trimmingCharacters(in: .whitespaces).isEmpty
    }

    private var query: String {
        model.filter.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private func matches(_ name: String) -> Bool {
        query.isEmpty || name.localizedCaseInsensitiveContains(query)
    }

    var body: some View {
        VStack(spacing: 0) {
            header
            search

            ScrollView(.vertical, showsIndicators: false) {
                LazyVStack(alignment: .leading, spacing: 0) {
                    autoexecSection
                    workspaceSection
                    librarySection
                    recentSection
                    Spacer(minLength: 12)
                }
                .padding(.top, 6)
            }
            .frame(maxHeight: .infinity)
        }
        .frame(width: Metrics.sidebar)
        .background(V.panel)
        .overlay(alignment: .trailing) {
            Rectangle().fill(V.borderSubtle).frame(width: 1)
        }
        .overlay {
            if dropActive {
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .strokeBorder(V.secondary.opacity(0.45), lineWidth: 1.4)
                    .padding(4)
                    .allowsHitTesting(false)
            }
        }
        .onDrop(of: [UTType.fileURL.identifier], isTargeted: $dropActive) { providers in
            let lock = NSLock()
            var urls: [URL] = []
            let group = DispatchGroup()
            for provider in providers {
                group.enter()
                provider.loadItem(forTypeIdentifier: UTType.fileURL.identifier,
                                  options: nil) { item, _ in
                    defer { group.leave() }
                    var found: URL?
                    if let data = item as? Data {
                        found = URL(dataRepresentation: data, relativeTo: nil)
                    } else if let url = item as? URL {
                        found = url
                    }
                    if let found {
                        lock.lock()
                        urls.append(found)
                        lock.unlock()
                    }
                }
            }
            group.notify(queue: .main) {
                if !urls.isEmpty {
                    model.importFiles(urls, into: VantaPaths.workspace)
                }
            }
            return true
        }
    }

    private var header: some View {
        HStack(spacing: 7) {
            Text("Explorer")
                .font(F.ui(17, .bold))
                .tracking(-0.25)
                .foregroundStyle(V.text)
            Spacer()
            Button(action: model.reloadFiles) {
                Image(systemName: "arrow.clockwise")
                    .font(.system(size: 10, weight: .medium))
                    .foregroundStyle(model.refreshingFiles ? V.secondary : V.faint)
                    .rotationEffect(.degrees(model.refreshingFiles ? 360 : 0))
                    .animation(.easeOut(duration: 0.35), value: model.refreshingFiles)
                    .frame(width: 22, height: 22)
            }
            .buttonStyle(.plain)
            .hoverSurface(radius: 5, hover: V.surface)
            .help("Reload files")
            SidebarIconButton(icon: "line.3.horizontal.decrease",
                              help: model.explorerFilter.label) {
                model.toggleExplorerFilter()
            }
            SidebarIconButton(icon: "plus", help: "New script") { model.newScript() }
                .contextMenu(ContextMenu {
                    Button("New Script in Workspace") {
                        model.newFile(in: VantaPaths.workspace)
                    }
                    Button("New Folder in Workspace") {
                        model.newFolder(in: VantaPaths.workspace)
                    }
                    Button("New Script in Autoexec") {
                        model.newFile(in: VantaPaths.autoexec, named: "autoexec.lua")
                    }
                    Divider()
                    Button("Reveal Workspace in Finder") {
                        model.reveal(VantaPaths.workspace)
                    }
                })
        }
        .padding(.horizontal, 14)
        .frame(height: 47)
    }

    private var search: some View {
        HStack(spacing: 6) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 9, weight: .medium))
                .foregroundStyle(searchFocused ? V.accent : V.ghost)
            TextField("Search files…", text: $model.filter)
                .textFieldStyle(.plain)
                .font(F.ui(11.5))
                .foregroundStyle(V.secondary)
                .focused($searchFocused)
            if !model.filter.isEmpty {
                Button { model.filter = "" } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.system(size: 9))
                        .foregroundStyle(V.faint)
                }
                .buttonStyle(.plain)
            } else {
                Text("⌘P")
                    .font(F.mono(8.5))
                    .foregroundStyle(V.ghost)
            }
        }
        .padding(.horizontal, 10)
        .frame(height: 30)
        .background(
            RoundedRectangle(cornerRadius: 7, style: .continuous)
                .fill(V.surface)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 7, style: .continuous)
                .strokeBorder(searchFocused ? V.accentEdge : V.borderSubtle, lineWidth: 0.8)
        )
        .padding(.horizontal, 12)
        .padding(.bottom, 10)
    }

    // MARK: - Autoexec

    private var autoexecSection: some View {
        VStack(alignment: .leading, spacing: 0) {
            SidebarSectionHeader(icon: "bolt.fill", title: "Autoexec",
                                 open: model.openSections.contains("autoexec")) {
                model.toggleSection("autoexec")
            }
            if model.openSections.contains("autoexec") || filtering {
                let rows = model.autoexecFiles.filter { matches($0.name) }
                ForEach(rows) { node in
                    AutoexecRow(node: node, model: model,
                                selected: model.selectedPath == node.path,
                                enabled: model.isAutoexecEnabled(node.name),
                                toggle: { model.toggleAutoexec(node.name) },
                                open: { model.open(node) })
                }
                if rows.isEmpty {
                    SidebarNote(text: filtering ? "No matches" : "No scripts yet")
                }
            }
        }
    }

    // MARK: - Workspace

    private var workspaceSection: some View {
        VStack(alignment: .leading, spacing: 0) {
            SidebarSectionHeader(icon: "folder", title: "Workspace",
                                 open: model.openSections.contains("workspace")) {
                model.toggleSection("workspace")
            }
            if model.openSections.contains("workspace") || filtering {
                if filtering {
                    let flat = flatten(model.workspaceTree)
                        .filter { model.explorerFilter == .all || $0.isScript }
                        .filter { matches($0.name) }
                    ForEach(flat) { node in
                        WorkspaceRow(node: node, model: model, depth: 0, isOpen: false,
                                     selected: model.selectedPath == node.path,
                                     open: { model.open(node) })
                    }
                    if flat.isEmpty { SidebarNote(text: "No matches") }
                } else {
                    WorkspaceRows(nodes: model.workspaceTree, depth: 0, model: model,
                                  scriptsOnly: model.explorerFilter == .scripts)
                    if model.explorerFilter == .scripts && model.workspaceScripts == 0 {
                        SidebarNote(text: "No scripts yet · Show all files") {
                            model.toggleExplorerFilter()
                        }
                    }
                }
            }
        }
    }

    private func flatten(_ nodes: [FileNode]) -> [FileNode] {
        var out: [FileNode] = []
        for node in nodes {
            if node.isDirectory {
                out.append(contentsOf: flatten(node.children))
            } else {
                out.append(node)
            }
        }
        return out
    }

    // MARK: - Library / Recent

    private var librarySection: some View {
        VStack(alignment: .leading, spacing: 0) {
            SidebarSectionHeader(icon: "books.vertical", title: "Library",
                                 open: model.openSections.contains("library")) {
                model.toggleSection("library")
            }
            if model.openSections.contains("library") || filtering {
                let rows = model.libraryTree
                    .filter { model.explorerFilter == .all || $0.isScript }
                    .filter { matches($0.name) }
                ForEach(rows) { node in
                    WorkspaceRow(node: node, model: model, depth: 0, isOpen: false,
                                 selected: model.selectedPath == node.path,
                                 open: { model.open(node) })
                }
                if rows.isEmpty {
                    SidebarNote(text: filtering ? "No matches" : "No library scripts")
                }
            }
        }
    }

    private var recentSection: some View {
        VStack(alignment: .leading, spacing: 0) {
            SidebarSectionHeader(icon: "clock", title: "Recent",
                                 open: model.openSections.contains("recent")) {
                model.toggleSection("recent")
            }
            if model.openSections.contains("recent") || filtering {
                let rows = model.recentNodes.filter { matches($0.name) }
                ForEach(rows) { node in
                    WorkspaceRow(node: node, model: model, depth: 0, isOpen: false,
                                 selected: model.selectedPath == node.path,
                                 open: { model.open(node) })
                }
                if rows.isEmpty {
                    SidebarNote(text: filtering ? "No matches" : "Nothing opened yet")
                }
            }
        }
    }


}

// MARK: - Rows

struct SidebarSectionHeader: View {
    let icon: String
    let title: String
    let open: Bool
    let action: () -> Void

    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            HStack(spacing: 6) {
                Image(systemName: open ? "chevron.down" : "chevron.right")
                    .font(.system(size: 7, weight: .bold))
                    .foregroundStyle(V.ghost)
                    .frame(width: 9)
                Image(systemName: icon)
                    .font(.system(size: 9, weight: .semibold))
                    .foregroundStyle(V.faint)
                Text(title)
                    .font(F.ui(12.5, .semibold))
                    .foregroundStyle(hovering ? V.text : V.secondary)
                Spacer()
            }
            .padding(.horizontal, 14)
            .frame(height: 31)
            .contentShape(Rectangle())
            .background(hovering ? V.surface.opacity(0.65) : .clear)
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .animation(.easeOut(duration: 0.15), value: open)
        .animation(.easeOut(duration: 0.15), value: hovering)
    }
}

struct AutoexecRow: View {
    let node: FileNode
    @ObservedObject var model: AppModel
    let selected: Bool
    let enabled: Bool
    let toggle: () -> Void
    let open: () -> Void

    @State private var hovering = false

    var body: some View {
        HStack(spacing: 8) {
            Text(node.autoexecOrder.map { String(format: "%02d", $0) } ?? "—")
                .font(F.mono(8.5))
                .foregroundStyle(selected ? V.accent : V.ghost)
                .frame(width: 16, alignment: .trailing)
            Image(systemName: "doc.text")
                .font(.system(size: 9.5))
                .foregroundStyle(selected ? V.secondary : V.faint)
            Button(action: open) {
                Text(node.name)
                    .font(F.ui(11.5, selected ? .medium : .regular))
                    .foregroundStyle(selected ? V.text : V.muted)
                    .lineLimit(1)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            Button(action: toggle) {
                Circle()
                    .fill(enabled ? V.accent : V.ghost)
                    .frame(width: 5, height: 5)
                    .shadow(color: enabled ? V.accent.opacity(0.55) : .clear, radius: 3)
                    .frame(width: 18, height: 18)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .help(enabled ? "Disable on join" : "Enable on join")
        }
        .padding(.leading, 12)
        .padding(.trailing, 10)
        .frame(height: 30)
        .background(selected ? V.surface : (hovering ? V.surface.opacity(0.45) : .clear))
        .overlay(alignment: .leading) {
            if selected { Rectangle().fill(V.accent).frame(width: 2) }
        }
        .onHover { hovering = $0 }
        .contextMenu(ContextMenu {
            Button("Open") { model.open(node) }
            Button("Run") { model.runFile(node) }
            Button(enabled ? "Disable on Join" : "Enable on Join") {
                model.toggleAutoexec(node.name)
            }
            Divider()
            Button("Reveal in Finder") { model.reveal(node.url) }
            Button("Move to Trash", role: .destructive) {
                model.trash(node.url)
            }
        })
    }
}

struct WorkspaceRows: View {
    let nodes: [FileNode]
    let depth: Int
    @ObservedObject var model: AppModel
    var scriptsOnly = false

    var body: some View {
        ForEach(nodes) { node in
            if !scriptsOnly
                || (node.isDirectory && Self.containsScripts(node))
                || node.isScript {
                let open = node.isDirectory && model.openFolders.contains(node.id)
                WorkspaceRow(node: node, model: model, depth: depth, isOpen: open,
                             selected: model.selectedPath == node.path,
                             open: { model.open(node) })
                if open {
                    WorkspaceRows(nodes: node.children, depth: depth + 1, model: model,
                                  scriptsOnly: scriptsOnly)
                }
            }
        }
    }

    private static func containsScripts(_ node: FileNode) -> Bool {
        for child in node.children {
            if child.isDirectory {
                if containsScripts(child) { return true }
            } else if child.isScript {
                return true
            }
        }
        return false
    }
}

struct WorkspaceRow: View {
    let node: FileNode
    @ObservedObject var model: AppModel
    let depth: Int
    let isOpen: Bool
    let selected: Bool
    let open: () -> Void

    @State private var hovering = false

    var body: some View {
        Button(action: open) {
            HStack(spacing: 6) {
                Spacer().frame(width: CGFloat(depth) * 14)
                if node.isDirectory {
                    Image(systemName: "chevron.right")
                        .font(.system(size: 7, weight: .bold))
                        .foregroundStyle(V.ghost)
                        .frame(width: 9)
                        .rotationEffect(.degrees(isOpen ? 90 : 0))
                    Image(systemName: isOpen ? "folder.fill" : "folder")
                        .font(.system(size: 10))
                        .foregroundStyle(isOpen ? V.secondary.opacity(0.8) : V.faint)
                } else {
                    Spacer().frame(width: 9)
                    Image(systemName: icon)
                        .font(.system(size: 9.5))
                        .foregroundStyle(V.faint)
                }
                Text(node.name)
                    .font(F.ui(11.5, selected ? .medium : .regular))
                    .foregroundStyle(selected ? V.text : V.muted)
                    .lineLimit(1)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 13)
            .frame(height: 26)
            .contentShape(Rectangle())
            .background(selected ? V.surface : (hovering ? V.surface.opacity(0.45) : .clear))
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .animation(.easeOut(duration: 0.14), value: hovering)
        .contextMenu(ContextMenu {
            if node.isDirectory {
                Button("New Script Here") {
                    model.newFile(in: node.url)
                }
                Button("New Folder Here") {
                    model.newFolder(in: node.url)
                }
                Divider()
            } else {
                Button("Open") { model.open(node) }
                Button("Run") { model.runFile(node) }
                Divider()
            }
            Button("Reveal in Finder") { model.reveal(node.url) }
            Button("Move to Trash", role: .destructive) {
                model.trash(node.url)
            }
        })
    }

    private var icon: String {
        node.name.hasSuffix(".json") ? "curlybraces" : "doc.text"
    }
}

struct SidebarNote: View {
    let text: String
    var action: (() -> Void)? = nil

    var body: some View {
        Group {
            if let action {
                Button(action: action) {
                    Text(text)
                        .font(F.ui(10.5))
                        .foregroundStyle(V.muted)
                        .padding(.leading, 31)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .frame(height: 26)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            } else {
                Text(text)
                    .font(F.ui(10.5))
                    .foregroundStyle(V.ghost)
                    .padding(.leading, 31)
                    .frame(height: 26)
            }
        }
    }
}

struct SidebarIconButton: View {
    let icon: String
    let help: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: icon)
                .font(.system(size: 10, weight: .medium))
                .foregroundStyle(V.faint)
                .frame(width: 22, height: 22)
        }
        .buttonStyle(.plain)
        .hoverSurface(radius: 5, hover: V.surface)
        .help(help)
    }
}
