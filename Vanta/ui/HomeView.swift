import SwiftUI

struct HomeView: View {
    @ObservedObject var model: AppModel

    private let columns = [
        GridItem(.flexible(), spacing: 20),
        GridItem(.flexible(), spacing: 20)
    ]

    var body: some View {
        ScrollView(.vertical, showsIndicators: false) {
            VStack(alignment: .leading, spacing: 0) {
                header
                Spacer().frame(height: 28)

                LazyVGrid(columns: columns, spacing: 16) {
                    HomeCard(icon: "doc.badge.plus", title: "New script",
                             subtitle: "Start with a blank buffer.", primary: true) {
                        model.newScript()
                    }
                    HomeCard(icon: "magnifyingglass", title: "Script search",
                             subtitle: "Search local files and ScriptBlox.") {
                        model.openSearch()
                    }
                    HomeCard(icon: "folder", title: "Open file",
                             subtitle: "Open a script from anywhere on disk.") {
                        model.openFromDisk()
                    }
                    HomeCard(icon: "gearshape", title: "Settings",
                             subtitle: "Editor, explorer, appearance.") {
                        model.openSettings()
                    }
                }

                Spacer().frame(height: 30)

                HStack(alignment: .top, spacing: 54) {
                    VStack(alignment: .leading, spacing: 8) {
                        SectionLabel(title: "Quick actions")
                            .padding(.bottom, 2)
                        QuickActionRow(icon: "folder", title: "Open file…", shortcut: "⌘O") {
                            model.openFromDisk()
                        }
                        QuickActionRow(icon: "magnifyingglass", title: "Search scripts",
                                       shortcut: "⌘K") {
                            model.openSearch()
                        }
                        QuickActionRow(icon: "gearshape", title: "Settings") {
                            model.openSettings()
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)

                    VStack(alignment: .leading, spacing: 8) {
                        HStack {
                            SectionLabel(title: "Recent")
                            Spacer()
                            if !model.recentPaths.isEmpty {
                                Button {
                                    model.clearRecent()
                                } label: {
                                    Text("Clear")
                                        .font(F.ui(10, .medium))
                                        .foregroundStyle(V.faint)
                                }
                                .buttonStyle(.plain)
                            }
                        }
                        .padding(.bottom, 2)

                        if model.recentPaths.isEmpty {
                            HStack(spacing: 8) {
                                Image(systemName: "clock")
                                    .font(.system(size: 10))
                                    .foregroundStyle(V.faint)
                                Text("No scripts opened yet.")
                                    .font(F.ui(11.5))
                                    .foregroundStyle(V.faint)
                                Spacer()
                            }
                            .frame(height: 32)
                        } else {
                            ForEach(Array(model.recentPaths.prefix(4)), id: \.self) { path in
                                let url = URL(fileURLWithPath: path)
                                RecentRow(name: url.lastPathComponent,
                                          when: url.deletingLastPathComponent().lastPathComponent) {
                                    model.addRecent(url)
                                    model.openURL(url)
                                }
                            }
                        }
                    }
                    .frame(width: 380, alignment: .leading)
                }

                Spacer().frame(height: 26)
                Rectangle().fill(V.borderSubtle).frame(height: 1)
                Spacer().frame(height: 15)

                HStack(alignment: .top, spacing: 34) {
                    FolderStat(icon: "bolt.fill", title: "Autoexec",
                               count: model.autoexecFiles.count,
                               path: VantaPaths.autoexec.path) {
                        model.showSidebarSection("autoexec")
                    }
                    FolderStat(icon: "folder", title: "Workspace",
                               count: model.workspaceScripts,
                               path: VantaPaths.workspace.path) {
                        model.showSidebarSection("workspace")
                    }
                    FolderStat(icon: "books.vertical", title: "Library",
                               count: model.libraryScripts,
                               path: VantaPaths.library.path) {
                        model.search.source = .local
                        model.openSearch()
                    }
                    Spacer()
                }
            }
            .padding(.horizontal, 52)
            .padding(.top, 36)
            .padding(.bottom, 24)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .background(
            ZStack {
                V.base
                RadialGradient(colors: [Color.white.opacity(0.025), .clear],
                               center: .topTrailing, startRadius: 0, endRadius: 560)
                VMark(size: 260)
                    .opacity(0.018)
                    .frame(maxWidth: .infinity, maxHeight: .infinity,
                           alignment: .topTrailing)
                    .offset(x: 50, y: -72)
            }
        )
    }

    private var header: some View {
        HStack(alignment: .top, spacing: 14) {
            VMark(size: 36)
                .padding(.top, 3)
            VStack(alignment: .leading, spacing: 6) {
                Text("Welcome to Vanta")
                    .font(F.ui(30, .semibold))
                    .tracking(-0.7)
                    .foregroundStyle(V.text)
                Text("A native Luau workspace for the macOS Roblox client.")
                    .font(F.ui(12.5))
                    .foregroundStyle(V.muted)
            }
        }
    }
}

// MARK: - Home pieces

struct HomeCard: View {
    let icon: String
    let title: String
    let subtitle: String
    var primary = false
    let action: () -> Void

    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            ZStack(alignment: .topLeading) {
                RoundedRectangle(cornerRadius: Metrics.cardRadius, style: .continuous)
                    .fill(LinearGradient(
                        colors: hovering ? [V.cardHover, V.card] : [V.card, V.surfaceHigh],
                        startPoint: .topLeading, endPoint: .bottomTrailing))

                if primary {
                    RadialGradient(colors: [V.accent.opacity(0.055), .clear],
                                   center: .bottomTrailing, startRadius: 0, endRadius: 260)
                        .clipShape(RoundedRectangle(cornerRadius: Metrics.cardRadius,
                                                   style: .continuous))
                }

                Image(systemName: icon)
                    .font(.system(size: 88, weight: .ultraLight))
                    .foregroundStyle(Color.white.opacity(hovering ? 0.075 : 0.055))
                    .frame(maxWidth: .infinity, maxHeight: .infinity,
                           alignment: .bottomTrailing)
                    .padding(.trailing, 20)
                    .padding(.bottom, 10)

                VStack(alignment: .leading, spacing: 0) {
                    ZStack {
                        RoundedRectangle(cornerRadius: 7, style: .continuous)
                            .fill(primary ? V.accentSoft : Color.white.opacity(0.05))
                        Image(systemName: icon)
                            .font(.system(size: 12.5, weight: .medium))
                            .foregroundStyle(primary ? V.accent : V.secondary)
                    }
                    .frame(width: 30, height: 30)

                    Spacer(minLength: 0)

                    Text(title)
                        .font(F.ui(15.5, .semibold))
                        .foregroundStyle(V.text)
                    Text(subtitle)
                        .font(F.ui(12))
                        .foregroundStyle(V.muted)
                        .padding(.top, 4)
                }
                .padding(22)
            }
            .frame(height: 148)
            .overlay(
                RoundedRectangle(cornerRadius: Metrics.cardRadius, style: .continuous)
                    .strokeBorder(hovering ? V.border : V.borderSubtle,
                                  lineWidth: 0.9)
            )
            .overlay(alignment: .top) {
                RoundedRectangle(cornerRadius: Metrics.cardRadius, style: .continuous)
                    .fill(LinearGradient(colors: [Color.white.opacity(0.085), .clear],
                                         startPoint: .top, endPoint: .bottom))
                    .frame(height: 1)
                    .padding(.horizontal, 16)
            }
            .contentShape(Rectangle())
            .shadow(color: Color.black.opacity(hovering ? 0.26 : 0.16), radius: 16, y: 6)
            .animation(.easeOut(duration: 0.18), value: hovering)
        }
        .buttonStyle(PressableButtonStyle())
        .onHover { hovering = $0 }
    }
}

struct FolderStat: View {
    let icon: String
    let title: String
    let count: Int
    let path: String
    let action: () -> Void
    @State private var hovering = false

    private var displayPath: String {
        path.replacingOccurrences(of: VantaPaths.home.path, with: "~")
    }

    var body: some View {
        Button(action: action) {
            HStack(alignment: .top, spacing: 9) {
                Image(systemName: icon)
                    .font(.system(size: 10.5, weight: .medium))
                    .foregroundStyle(hovering ? V.accent : V.faint)
                    .frame(width: 15)
                VStack(alignment: .leading, spacing: 3) {
                    HStack(spacing: 6) {
                        Text(title)
                            .font(F.ui(10.5, .medium))
                            .foregroundStyle(hovering ? V.text : V.secondary)
                        Text("\(count)")
                            .font(F.mono(9, .medium))
                            .foregroundStyle(V.faint)
                    }
                    Text(displayPath)
                        .font(F.mono(8))
                        .foregroundStyle(V.ghost)
                        .lineLimit(1)
                }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .animation(.easeOut(duration: 0.14), value: hovering)
    }
}

struct SectionLabel: View {
    let title: String
    var body: some View {
        Text(title)
            .font(F.ui(11, .semibold))
            .tracking(0.2)
            .foregroundStyle(V.muted)
    }
}

struct QuickActionRow: View {
    let icon: String
    let title: String
    var shortcut: String? = nil
    let action: () -> Void

    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            HStack(spacing: 8) {
                Image(systemName: icon)
                    .font(.system(size: 10.5))
                    .foregroundStyle(V.faint)
                    .frame(width: 15)
                Text(title)
                    .font(F.ui(12))
                    .foregroundStyle(hovering ? V.text : V.secondary)
                Spacer()
                if let shortcut {
                    Text(shortcut)
                        .font(F.mono(9))
                        .foregroundStyle(V.ghost)
                }
            }
            .frame(height: 31)
            .contentShape(Rectangle())
        }
        .buttonStyle(PressableButtonStyle())
        .onHover { hovering = $0 }
        .animation(.easeOut(duration: 0.14), value: hovering)
    }
}

struct RecentRow: View {
    let name: String
    let when: String
    let action: () -> Void

    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            HStack(spacing: 8) {
                Image(systemName: "doc.text")
                    .font(.system(size: 9.5))
                    .foregroundStyle(V.faint)
                Text(name)
                    .font(F.ui(11.5))
                    .foregroundStyle(hovering ? V.text : V.secondary)
                Spacer()
                Text(when)
                    .font(F.mono(8.5))
                    .foregroundStyle(V.ghost)
            }
            .frame(height: 31)
            .contentShape(Rectangle())
        }
        .buttonStyle(PressableButtonStyle())
        .onHover { hovering = $0 }
        .animation(.easeOut(duration: 0.14), value: hovering)
    }
}
