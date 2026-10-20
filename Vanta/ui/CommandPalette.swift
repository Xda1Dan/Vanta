import SwiftUI
import AppKit

struct PaletteCommand: Identifiable {
    let id = UUID()
    let title: String
    let subtitle: String
    let icon: String
    let shortcut: String?
    let action: () -> Void
}

struct CommandPaletteView: View {
    @ObservedObject var model: AppModel
    @State private var query = ""
    @State private var selection = 0
    @FocusState private var focused: Bool
    @State private var keyMonitor: Any?

    private var commands: [PaletteCommand] {
        [
            .init(title: "New Script", subtitle: "Create a blank buffer",
                  icon: "doc.badge.plus", shortcut: "⌘N") { model.newScript() },
            .init(title: "Open File…", subtitle: "Open a script from disk",
                  icon: "folder", shortcut: "⌘O") { model.openFromDisk() },
            .init(title: "Script Search", subtitle: "Local files or ScriptBlox",
                  icon: "magnifyingglass", shortcut: "⌘K") { model.openSearch() },
            .init(title: model.showSidebar ? "Hide Explorer" : "Show Explorer",
                  subtitle: "Left sidebar", icon: "sidebar.left", shortcut: "⌘B") {
                model.toggleSidebar()
            },
            .init(title: model.consoleOpen ? "Hide Console" : "Show Console",
                  subtitle: "Executor output and rconsole input",
                  icon: "terminal", shortcut: "⌘J") { model.toggleConsole() },
            .init(title: model.focusMode ? "Leave Focus Mode" : "Focus Mode",
                  subtitle: "Hide the rail, explorer and console",
                  icon: "rectangle.center.inset.filled", shortcut: "⇧⌘F") {
                model.toggleFocusMode()
            },
            .init(title: "Save", subtitle: "Write the active buffer",
                  icon: "square.and.arrow.down", shortcut: "⌘S") { model.saveActive() },
            .init(title: "Run Script", subtitle: "Send the active buffer to the executor",
                  icon: "play.fill", shortcut: "⌘↩") { model.run() },
            .init(title: "Clear Console", subtitle: "Remove console history",
                  icon: "trash", shortcut: nil) { model.clearConsole() },
            .init(title: "Settings", subtitle: "Window, executor and editor",
                  icon: "gearshape", shortcut: "⌘,") { model.openSettings() },
            .init(title: "About Vanta", subtitle: "Version and runtime details",
                  icon: "info.circle", shortcut: nil) { model.openAbout() }
        ]
    }

    private var filtered: [PaletteCommand] {
        let text = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return commands }
        return commands.filter {
            $0.title.localizedCaseInsensitiveContains(text)
                || $0.subtitle.localizedCaseInsensitiveContains(text)
        }
    }

    var body: some View {
        ZStack(alignment: .top) {
            Color.black.opacity(0.5)
                .ignoresSafeArea()
                .contentShape(Rectangle())
                .onTapGesture { model.closePalette() }

            VStack(spacing: 0) {
                HStack(spacing: 9) {
                    Image(systemName: "command")
                        .font(.system(size: 11, weight: .medium))
                        .foregroundStyle(V.faint)
                    TextField("Run a command…", text: $query)
                        .textFieldStyle(.plain)
                        .font(F.ui(13))
                        .foregroundStyle(V.text)
                        .focused($focused)
                        .onSubmit {
                            guard filtered.indices.contains(selection) else { return }
                            filtered[selection].action()
                            model.closePalette()
                        }
                    Text("esc")
                        .font(F.mono(8.5))
                        .foregroundStyle(V.ghost)
                }
                .padding(.horizontal, 13)
                .frame(height: 42)

                Rectangle().fill(V.borderSubtle).frame(height: 1)

                ScrollView(.vertical, showsIndicators: false) {
                    LazyVStack(spacing: 1) {
                        ForEach(Array(filtered.enumerated()), id: \.element.id) { index, command in
                            PaletteRow(command: command, selected: index == selection) {
                                command.action()
                                model.closePalette()
                            }
                            .onHover { inside in
                                if inside { selection = index }
                            }
                        }
                    }
                    .padding(.horizontal, 6)
                    .padding(.vertical, 6)
                }
                .frame(maxHeight: 360)
            }
            .frame(width: 560)
            .background(
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(V.panel)
                    .shadow(color: Color.black.opacity(0.65), radius: 40, y: 18)
            )
            .overlay(
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .strokeBorder(V.border, lineWidth: 0.9)
            )
            .padding(.top, 92)
        }
        .onAppear {
            selection = 0
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) { focused = true }
            keyMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { event in
                switch event.keyCode {
                case 125: // down
                    if !filtered.isEmpty {
                        selection = min(selection + 1, filtered.count - 1)
                    }
                    return nil
                case 126: // up
                    if !filtered.isEmpty {
                        selection = max(selection - 1, 0)
                    }
                    return nil
                case 53: // escape
                    model.closePalette()
                    return nil
                default:
                    return event
                }
            }
        }
        .onDisappear {
            if let keyMonitor { NSEvent.removeMonitor(keyMonitor) }
            keyMonitor = nil
        }
        .onChange(of: query) { _ in selection = 0 }
    }
}

struct PaletteRow: View {
    let command: PaletteCommand
    let selected: Bool
    let action: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            HStack(spacing: 9) {
                Image(systemName: command.icon)
                    .font(.system(size: 10.5, weight: .medium))
                    .foregroundStyle(selected ? V.secondary : V.faint)
                    .frame(width: 16)
                VStack(alignment: .leading, spacing: 1) {
                    Text(command.title)
                        .font(F.ui(11.5, .medium))
                        .foregroundStyle(selected ? V.text : V.secondary)
                    Text(command.subtitle)
                        .font(F.ui(9.5))
                        .foregroundStyle(V.faint)
                }
                Spacer()
                if let shortcut = command.shortcut {
                    Text(shortcut)
                        .font(F.mono(8.5))
                        .foregroundStyle(V.ghost)
                }
            }
            .padding(.horizontal, 9)
            .frame(height: 38)
            .background(
                RoundedRectangle(cornerRadius: 6, style: .continuous)
                    .fill(selected || hovering ? V.surface : .clear)
            )
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
    }
}
