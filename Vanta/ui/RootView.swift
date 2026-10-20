import SwiftUI

struct RootView: View {
    @ObservedObject var model: AppModel
    @State private var appeared = false

    var body: some View {
        ZStack {
            VStack(spacing: 0) {
                TopBar(model: model)

                HStack(spacing: 0) {
                    if !model.focusMode {
                        ActivityRail(model: model)
                    }
                    if model.showSidebar && !model.focusMode {
                        Sidebar(model: model)
                            .transition(.move(edge: .leading).combined(with: .opacity))
                    }
                    VStack(spacing: 0) {
                        if model.route == .home {
                            HomeView(model: model)
                        } else {
                            EditorView(model: model)
                        }
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)

                StatusFooter(model: model)
            }
            .opacity(appeared ? 1 : 0)
            .scaleEffect(appeared ? 1 : 0.997)
            .onAppear {
                withAnimation(.easeOut(duration: 0.36)) { appeared = true }
            }
        }
        .overlay {
            if model.showSettings {
                SettingsOverlay(model: model)
                    .transition(.opacity.combined(with: .scale(scale: 0.985)))
            }
        }
        .overlay {
            if model.showAbout {
                AboutOverlay(model: model)
                    .transition(.opacity)
            }
        }
        .overlay {
            if model.showSearch {
                ScriptSearchOverlay(model: model, store: model.search)
                    .transition(.opacity.combined(with: .scale(scale: 0.985)))
            }
        }
        .overlay {
            if model.showPalette {
                CommandPaletteView(model: model)
                    .transition(.opacity.combined(with: .scale(scale: 0.985)))
            }
        }
        .overlay(alignment: .bottom) {
            if let toast = model.toast {
                ToastView(toast: toast)
                    .id(toast.id)
                    .transition(.move(edge: .bottom).combined(with: .opacity))
                    .zIndex(30)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(V.base)
        .overlay {
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .strokeBorder(Color.white.opacity(0.055), lineWidth: 1)
                .allowsHitTesting(false)
        }
        .ignoresSafeArea(.all)
    }
}

// MARK: - Settings

struct SettingsOverlay: View {
    @ObservedObject var model: AppModel

    var body: some View {
        ZStack {
            Color.black.opacity(0.56)
                .ignoresSafeArea()
                .contentShape(Rectangle())
                .onTapGesture { model.closeSettings() }

            VStack(spacing: 0) {
                HStack(spacing: 9) {
                    Image(systemName: "gearshape")
                        .font(.system(size: 12, weight: .medium))
                        .foregroundStyle(V.accent)
                    Text("Settings")
                        .font(F.ui(15, .semibold))
                        .foregroundStyle(V.text)
                    Spacer()
                    Button(action: model.closeSettings) {
                        Image(systemName: "xmark")
                            .font(.system(size: 9, weight: .semibold))
                            .foregroundStyle(V.faint)
                            .frame(width: 24, height: 24)
                    }
                    .buttonStyle(.plain)
                    .hoverSurface(radius: 5, hover: V.surface)
                }
                .padding(.horizontal, 20)
                .frame(height: 54)

                Rectangle().fill(V.borderSubtle).frame(height: 1)

                ScrollView(.vertical, showsIndicators: false) {
                    VStack(alignment: .leading, spacing: 22) {
                        connectionSection
                        windowSection
                        executorSection
                        editorSection
                    }
                    .padding(20)
                }

                Rectangle().fill(V.borderSubtle).frame(height: 1)

                HStack {
                    Text("Changes apply to this window immediately.")
                        .font(F.ui(10))
                        .foregroundStyle(V.faint)
                    Spacer()
                    Button(action: model.closeSettings) {
                        Text("Done")
                            .font(F.ui(10.5, .semibold))
                            .foregroundStyle(V.onAccent)
                            .padding(.horizontal, 16)
                            .frame(height: 27)
                            .background(RoundedRectangle(cornerRadius: 6.5).fill(V.accent))
                    }
                    .buttonStyle(.plain)
                }
                .padding(.horizontal, 20)
                .frame(height: 50)
            }
            .frame(width: 540)
            .frame(maxHeight: 590)
            .background(
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .fill(V.panel)
                    .shadow(color: Color.black.opacity(0.65), radius: 44, y: 20)
            )
            .overlay(
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .strokeBorder(V.border, lineWidth: 0.9)
            )
        }
    }

    private var connectionSection: some View {
        VStack(alignment: .leading, spacing: 9) {
            SettingsLabel("Connection")
            HStack(spacing: 9) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(model.connection == .offline ? "No client" : model.connection.label.capitalized)
                        .font(F.ui(11.5, .medium))
                        .foregroundStyle(model.connection == .reinstall || model.connection == .failed
                                         ? V.danger : V.secondary)
                    Text(model.connectionDetail.isEmpty
                         ? model.connection.detail : model.connectionDetail)
                        .font(F.ui(10.5))
                        .foregroundStyle(V.faint)
                }
                Spacer()
                Button(action: model.refreshConnection) {
                    HStack(spacing: 5) {
                        Image(systemName: "arrow.clockwise")
                            .font(.system(size: 8.5, weight: .semibold))
                        Text("Refresh")
                            .font(F.ui(9.5, .medium))
                    }
                    .foregroundStyle(V.secondary)
                    .padding(.horizontal, 9)
                    .frame(height: 25)
                    .background(RoundedRectangle(cornerRadius: 6)
                        .fill(V.surface))
                    .overlay(RoundedRectangle(cornerRadius: 6)
                        .strokeBorder(V.border, lineWidth: 0.8))
                }
                .buttonStyle(.plain)
            }
            Text(VantaEnvironment.robloxVersionLine)
                .font(F.ui(10.5))
                .foregroundStyle(V.faint)
            Text("Control  http://\(VantaPaths.controlHost):\(VantaPaths.controlPort)")
                .font(F.mono(8.5))
                .foregroundStyle(V.ghost)
            Text("Journal  \(VantaEnvironment.journalPath)")
                .font(F.mono(8.5))
                .foregroundStyle(V.ghost)
        }
    }

    private var windowSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            SettingsLabel("Window")
            SettingsToggle(title: "Explorer", on: model.showSidebar) {
                model.toggleSidebar()
            }
            SettingsToggle(title: "Console", on: model.consoleOpen) {
                model.toggleConsole()
            }
        }
    }

    private var executorSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            SettingsLabel("Executor")
            SettingsToggle(title: "Run autoexec on join", on: model.autoexecOnJoin) {
                model.toggleAutoexecOnJoin()
            }
            SettingsToggle(title: "Safe mode", on: model.safeMode) {
                model.toggleSafeMode()
            }
            Text("Blocks unsafe engine members for scripts you run. Applied on the next join.")
                .font(F.ui(10))
                .foregroundStyle(V.faint)
                .font(F.ui(10))
                .foregroundStyle(V.faint)
        }
    }

    private var editorSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            SettingsLabel("Editor")
            HStack {
                Text("Font size")
                    .font(F.ui(11))
                    .foregroundStyle(V.secondary)
                Spacer()
                HStack(spacing: 6) {
                    StepButton(icon: "minus") {
                        model.fontSize = max(11, model.fontSize - 1)
                    }
                    Text("\(Int(model.fontSize)) pt")
                        .font(F.mono(10))
                        .foregroundStyle(V.muted)
                        .frame(width: 40)
                    StepButton(icon: "plus") {
                        model.fontSize = min(17, model.fontSize + 1)
                    }
                }
            }
            .frame(height: 24)
            SettingsToggle(title: "Line numbers", on: model.showLineNumbers) {
                model.showLineNumbers.toggle()
            }
        }
    }

}

struct SettingsLabel: View {
    let title: String
    init(_ title: String) { self.title = title }
    var body: some View {
        Text(title.uppercased())
            .font(F.ui(8, .semibold))
            .tracking(1.15)
            .foregroundStyle(V.ghost)
    }
}

struct SettingsToggle: View {
    let title: String
    let on: Bool
    let action: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            HStack {
                Text(title)
                    .font(F.ui(12))
                    .foregroundStyle(hovering ? V.text : V.secondary)
                Spacer()
                Capsule()
                    .fill(on ? V.accentSoft : Color.clear)
                    .overlay(Capsule().strokeBorder(on ? V.accentEdge : V.border,
                                                    lineWidth: 0.8))
                    .frame(width: 26, height: 14)
                    .overlay(alignment: on ? .trailing : .leading) {
                        Circle()
                            .fill(on ? V.accent : V.ghost)
                            .frame(width: 8, height: 8)
                            .padding(.horizontal, 3)
                    }
            }
            .frame(height: 26)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .animation(.easeOut(duration: 0.16), value: on)
    }
}

struct StepButton: View {
    let icon: String
    let action: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            Image(systemName: icon)
                .font(.system(size: 8, weight: .bold))
                .foregroundStyle(hovering ? V.text : V.muted)
                .frame(width: 21, height: 20)
                .background(
                    RoundedRectangle(cornerRadius: 5, style: .continuous)
                        .fill(hovering ? V.raised : V.surface)
                )
                .overlay(
                    RoundedRectangle(cornerRadius: 5, style: .continuous)
                        .strokeBorder(V.border, lineWidth: 0.8)
                )
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
    }
}

// MARK: - About

struct AboutOverlay: View {
    @ObservedObject var model: AppModel

    var body: some View {
        ZStack {
            Color.black.opacity(0.56)
                .ignoresSafeArea()
                .contentShape(Rectangle())
                .onTapGesture { model.closeAbout() }

            VStack(spacing: 13) {
                VMark(size: 52)
                Text("VANTA")
                    .font(F.ui(19, .bold))
                    .tracking(4)
                    .foregroundStyle(V.text)
                Text(VantaEnvironment.robloxVersionLine)
                    .font(F.ui(11.5, .medium))
                    .foregroundStyle(V.accent)
                Text("Native macOS interface for the Vanta Luau executor.")
                    .font(F.ui(10.5))
                    .foregroundStyle(V.muted)
                    .multilineTextAlignment(.center)
                Text("1.0.0")
                    .font(F.mono(8.5))
                    .foregroundStyle(V.faint)
                    .padding(.top, 4)
                Button(action: model.closeAbout) {
                    Text("Close")
                        .font(F.ui(10.5, .semibold))
                        .foregroundStyle(V.onAccent)
                        .padding(.horizontal, 22)
                        .frame(height: 28)
                        .background(RoundedRectangle(cornerRadius: 6.5).fill(V.accent))
                }
                .buttonStyle(.plain)
                .padding(.top, 7)
            }
            .padding(30)
            .frame(width: 330)
            .background(
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .fill(V.panel)
                    .shadow(color: Color.black.opacity(0.65), radius: 40, y: 18)
            )
            .overlay(
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .strokeBorder(V.border, lineWidth: 0.9)
            )
        }
    }
}
