import SwiftUI
import AppKit

// MARK: - Window drag

/// A transparent AppKit view behind the menu bar. Pressing it moves the window;
/// double-clicking zooms, which is the behaviour people expect from a title bar
/// even when the title bar is drawn by us.
struct WindowDragArea: NSViewRepresentable {
    final class DragView: NSView {
        private var grab: NSPoint = .zero
        private var origin: NSPoint = .zero

        override var mouseDownCanMoveWindow: Bool { false }
        override func hitTest(_ point: NSPoint) -> NSView? {
            guard let event = NSApp.currentEvent else { return nil }
            switch event.type {
            case .leftMouseDown, .leftMouseUp, .leftMouseDragged:
                return super.hitTest(point)
            default:
                return nil
            }
        }
        override func mouseDown(with event: NSEvent) {
            guard let window else { return }
            if event.clickCount == 2 { window.zoom(nil); return }
            grab = NSEvent.mouseLocation
            origin = window.frame.origin
        }
        override func mouseDragged(with event: NSEvent) {
            guard let window else { return }
            let now = NSEvent.mouseLocation
            window.setFrameOrigin(NSPoint(x: origin.x + (now.x - grab.x),
                                          y: origin.y + (now.y - grab.y)))
        }
    }
    func makeNSView(context: Context) -> NSView { DragView() }
    func updateNSView(_ view: NSView, context: Context) {}
}

// MARK: - Menu bar

struct WindowControls: View {
    @ObservedObject var model: AppModel

    var body: some View {
        HStack(spacing: 11) {
            WindowControl(icon: "minus", hover: V.warning) { model.minimize() }
                .help("Minimize")
            WindowControl(icon: "square", hover: V.secondary) { model.zoom() }
                .help("Zoom")
            WindowControl(icon: "xmark", hover: V.danger) { model.closeWindowAction() }
                .help("Close")
        }
    }
}

struct WindowControl: View {
    let icon: String
    let hover: Color
    let action: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            Image(systemName: icon)
                .font(.system(size: 8.5, weight: .medium))
                .foregroundStyle(hovering ? hover : V.muted.opacity(0.65))
                .frame(width: 19, height: 19)
                .background(
                    RoundedRectangle(cornerRadius: 5, style: .continuous)
                        .fill(hovering ? hover.opacity(0.12) : .clear)
                )
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .animation(.easeOut(duration: 0.14), value: hovering)
    }
}

// MARK: - Activity rail

enum RailItemKind {
    case home, editor, library, scripts, console, appearance, settings
}

struct ActivityRail: View {
    @ObservedObject var model: AppModel

    var body: some View {
        VStack(spacing: 4) {
            Spacer().frame(height: 8)
            RailButton(icon: "house", active: model.route == .home,
                       help: "Home (⌘1)") { model.showHome() }
            RailButton(icon: "chevron.left.forwardslash.chevron.right",
                       active: model.route == .editor,
                       help: "Editor (⌘2)") { model.showEditor() }
            RailButton(icon: "magnifyingglass",
                       active: model.showSearch,
                       help: "Search scripts (⌘K)") { model.openSearch() }
            RailButton(icon: "terminal",
                       active: model.route == .editor && model.consoleOpen,
                       help: "Console (⌘J)") {
                if model.route != .editor { model.showEditor() }
                model.toggleConsole()
            }
            Spacer()
            RailButton(icon: "gearshape", active: model.showSettings,
                       help: "Settings") { model.openSettings() }
            Spacer().frame(height: 8)
        }
        .frame(width: Metrics.rail)
        .background(V.base)
        .overlay(alignment: .trailing) {
            Rectangle().fill(V.borderSubtle).frame(width: 1)
        }
    }
}

struct RailButton: View {
    let icon: String
    let active: Bool
    let help: String
    let action: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: 7, style: .continuous)
                    .fill(active ? V.surface : (hovering ? V.surface.opacity(0.65) : .clear))
                    .frame(width: 34, height: 32)
                Image(systemName: icon)
                    .font(.system(size: 13.5, weight: .regular))
                    .foregroundStyle(active ? V.text : (hovering ? V.secondary : V.muted.opacity(0.75)))
                    .frame(width: 34, height: 32)
                if active {
                    RoundedRectangle(cornerRadius: 1)
                        .fill(V.accent)
                        .frame(width: 2, height: 15)
                        .offset(x: -8)
                }
            }
            .frame(width: 34, height: 32)
        }
        .buttonStyle(PressableButtonStyle())
        .onHover { hovering = $0 }
        .animation(.easeOut(duration: 0.15), value: hovering)
        .animation(.easeOut(duration: 0.15), value: active)
        .help(help)
    }
}

// MARK: - Top bar

struct TopBar: View {
    @ObservedObject var model: AppModel

    var body: some View {
        ZStack(alignment: .leading) {
            WindowDragArea()

            HStack(spacing: 0) {
                Button(action: model.showHome) {
                    HStack(spacing: 8) {
                        VMark(size: 18)
                        Text("VANTA")
                            .font(F.ui(10.5, .bold))
                            .tracking(2.2)
                            .foregroundStyle(V.text)
                    }
                }
                .buttonStyle(.plain)
                .padding(.leading, 14)

                HStack(spacing: 2) {
                    TabPill(icon: "house", name: "Home",
                            active: model.route == .home) {
                        model.showHome()
                    }

                    ScrollView(.horizontal, showsIndicators: false) {
                        HStack(spacing: 1) {
                            ForEach(model.tabs) { tab in
                                TabPill(name: tab.name,
                                        active: model.route == .editor
                                            && model.activeTabID == tab.id,
                                        dirty: tab.isDirty,
                                        close: { model.closeTab(tab.id) }) {
                                    model.selectTab(tab.id)
                                }
                            }
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)

                    Button(action: model.newScript) {
                        Image(systemName: "plus")
                            .font(.system(size: 9, weight: .medium))
                            .foregroundStyle(V.ghost)
                            .frame(width: 27, height: 27)
                    }
                    .buttonStyle(.plain)
                    .hoverSurface(radius: 6, hover: V.surface)
                    .help("New tab (⌘N)")
                }
                .padding(.leading, 18)
                .frame(maxWidth: .infinity, alignment: .leading)

                Spacer(minLength: 12)

                HStack(spacing: 9) {
                    if model.route == .editor {
                        RunButton(running: model.running,
                                  enabled: model.connection.canExecute,
                                  action: model.run)
                    }
                    WindowControls(model: model)
                }
                .padding(.trailing, 13)
            }
        }
        .frame(height: 48)
        .background(V.base)
        .overlay(alignment: .bottom) {
            Rectangle().fill(V.borderSubtle).frame(height: 1)
        }
    }
}

struct TabPill: View {
    var icon: String? = nil
    let name: String
    var active = false
    var dirty = false
    var close: (() -> Void)? = nil
    let action: () -> Void

    @State private var hovering = false

    var body: some View {
        HStack(spacing: 6) {
            if let icon {
                Image(systemName: icon)
                    .font(.system(size: 9, weight: .medium))
                    .foregroundStyle(active ? V.secondary : V.faint)
            }
            Text(name)
                .font(F.ui(11.5, active ? .medium : .regular))
                .foregroundStyle(active ? V.text : V.muted)
                .lineLimit(1)
            if dirty {
                Circle().fill(V.accent).frame(width: 3.5, height: 3.5)
            }
            if let close {
                Button(action: close) {
                    Image(systemName: "xmark")
                        .font(.system(size: 7.5, weight: .bold))
                        .foregroundStyle(V.faint)
                        .frame(width: 14, height: 14)
                }
                .buttonStyle(.plain)
                .opacity(hovering || active ? 1 : 0)
                .help("Close tab")
            }
        }
        .padding(.leading, active ? 8 : 10)
        .padding(.trailing, close == nil ? 10 : 6)
        .frame(height: 27)
        .background(active ? V.surface : (hovering ? V.surface.opacity(0.55) : .clear))
        .clipShape(RoundedRectangle(cornerRadius: 7, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 7, style: .continuous)
                .strokeBorder(active ? V.border : Color.clear, lineWidth: 0.8)
        )
        .contentShape(Rectangle())
        .onHover { hovering = $0 }
        .animation(.easeOut(duration: 0.14), value: hovering)
        .animation(.easeOut(duration: 0.14), value: active)
        .onTapGesture(perform: action)
    }
}

struct RunButton: View {
    let running: Bool
    var enabled = true
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 6) {
                Image(systemName: running ? "hourglass" : "play.fill")
                    .font(.system(size: 8, weight: .bold))
                Text(running ? "SENDING" : "RUN")
                    .font(F.ui(9.5, .bold))
                    .tracking(1.1)
            }
            .foregroundStyle(V.onAccent)
            .padding(.horizontal, 10)
            .frame(height: 28)
            .background(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(V.accent)
            )
            .opacity(enabled ? 1 : 0.38)
        }
        .buttonStyle(PressableButtonStyle())
        .disabled(running || !enabled)
        .help(enabled ? "Run in game (⌘↩)" : "The executor is not ready — check the status footer")
    }
}

// MARK: - Status footer

struct StatusFooter: View {
    @ObservedObject var model: AppModel

    private var status: String {
        switch model.connection {
        case .attached:  return "Attached"
        case .waiting:   return "Waiting for game"
        case .offline:   return "No client"
        case .reinstall: return "Reinstall required"
        case .failed:    return "Executor failed"
        }
    }

    private var statusColor: Color {
        switch model.connection {
        case .attached, .waiting: return V.secondary
        case .offline:            return V.faint
        case .reinstall, .failed: return V.danger
        }
    }

    var body: some View {
        HStack(spacing: 0) {
            Button(action: model.refreshConnection) {
                Text(status)
                    .font(F.ui(11, .medium))
                    .foregroundStyle(statusColor)
            }
            .buttonStyle(.plain)
            .help(model.connectionDetail.isEmpty
                  ? "Refresh the executor connection" : model.connectionDetail)
            .padding(.leading, 12)

            Spacer()

            if model.route == .editor {
                Text("Ln \(model.cursorLine), Col \(model.cursorColumn)")
                    .font(F.mono(9.5))
                    .foregroundStyle(V.faint)
                    .padding(.trailing, 12)
            }
        }
        .frame(height: 22)
        .background(V.base)
        .overlay(alignment: .top) {
            Rectangle().fill(V.borderSubtle).frame(height: 1)
        }
    }
}

struct FooterDatum: View {
    let label: String
    let value: String?

    var body: some View {
        HStack(spacing: 4) {
            Text(label)
                .font(F.mono(9))
                .foregroundStyle(V.ghost)
            if let value {
                Text(value)
                    .font(F.mono(9, .medium))
                    .foregroundStyle(V.muted)
            }
        }
    }
}

// MARK: - Toast

struct ToastView: View {
    let toast: Toast

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: toast.icon)
                .font(.system(size: 10.5, weight: .medium))
                .foregroundStyle(V.secondary)
            Text(toast.message)
                .font(F.ui(11, .medium))
                .foregroundStyle(V.secondary)
        }
        .padding(.horizontal, 13)
        .frame(height: 30)
        .background(
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .fill(V.surfaceHigh)
                .shadow(color: Color.black.opacity(0.42), radius: 18, y: 8)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .strokeBorder(V.border, lineWidth: 0.8)
        )
        .padding(.bottom, 18)
    }
}
