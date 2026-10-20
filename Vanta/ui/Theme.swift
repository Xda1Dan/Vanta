import SwiftUI
import AppKit

extension Color {
    init(hex: UInt32, opacity: Double = 1) {
        self.init(.sRGB,
                  red: Double((hex >> 16) & 0xFF) / 255,
                  green: Double((hex >> 8) & 0xFF) / 255,
                  blue: Double(hex & 0xFF) / 255,
                  opacity: opacity)
    }
}

extension NSColor {
    convenience init(hex: UInt32, alpha: CGFloat = 1) {
        self.init(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255,
                  green: CGFloat((hex >> 8) & 0xFF) / 255,
                  blue: CGFloat(hex & 0xFF) / 255,
                  alpha: alpha)
    }
}

/// Vanta / Signal — the one place every colour and metric lives.
enum V {
    // Surfaces, darkest first.
    static let base = Color(hex: 0x09090B)
    static let chrome = Color(hex: 0x0C0C0E)
    static let panel = Color(hex: 0x0F0F12)
    static let surface = Color(hex: 0x141417)
    static let surfaceHigh = Color(hex: 0x18181C)
    static let card = Color(hex: 0x1F1F24)
    static let cardHover = Color(hex: 0x26262C)
    static let raised = Color(hex: 0x232329)
    static let border = Color(hex: 0x26262C)
    static let borderSubtle = Color(hex: 0x1B1B20)

    // Type.
    static let text = Color(hex: 0xF5F5F6)
    static let secondary = Color(hex: 0xC7C7CE)
    static let muted = Color(hex: 0x8B8B94)
    static let faint = Color(hex: 0x5C5C65)
    static let ghost = Color(hex: 0x3A3A41)

    // Signal.
    static let signal = Color(hex: 0xC8FF3D)
    /// The working accent is Madium-style off-white. Lime is brand/editor only.
    static let accent = Color(hex: 0xF2F2F2)
    static let accentSoft = Color(hex: 0xFFFFFF, opacity: 0.08)
    static let accentEdge = Color(hex: 0xFFFFFF, opacity: 0.22)
    static let onAccent = Color(hex: 0x121212)
    static let success = Color(hex: 0x6EE7A0)
    static let warning = Color(hex: 0xF5B85D)
    static let danger = Color(hex: 0xF27575)
    static let info = Color(hex: 0x8AB4F8)

    // Syntax. Deliberately separate from the UI accent.
    static let keyword = Color(hex: 0xB6A6FF)
    static let string = Color(hex: 0xB8E986)
    static let number = Color(hex: 0xFFC978)
    static let comment = Color(hex: 0x55555D)
    static let function = Color(hex: 0x8FD3FF)
    static let property = Color(hex: 0xD7D7E0)
    static let global = Color(hex: 0x7FE7D4)
    static let op = Color(hex: 0x85858D)

    // AppKit twins for the editor.
    static let nsBase = NSColor(hex: 0x09090B)
    static let nsPanel = NSColor(hex: 0x0F0F12)
    static let nsBorder = NSColor(hex: 0x26262C)
    static let nsBorderSubtle = NSColor(hex: 0x1B1B20)
    static let nsText = NSColor(hex: 0xF5F5F6)
    static let nsMuted = NSColor(hex: 0x8B8B94)
    static let nsFaint = NSColor(hex: 0x5C5C65)
    static let nsGhost = NSColor(hex: 0x3A3A41)
    static let nsAccent = NSColor(hex: 0xF2F2F2)
    static let nsKeyword = NSColor(hex: 0xB6A6FF)
    static let nsString = NSColor(hex: 0xB8E986)
    static let nsNumber = NSColor(hex: 0xFFC978)
    static let nsComment = NSColor(hex: 0x55555D)
    static let nsFunction = NSColor(hex: 0x8FD3FF)
    static let nsProperty = NSColor(hex: 0xD7D7E0)
    static let nsGlobal = NSColor(hex: 0x7FE7D4)
    static let nsOp = NSColor(hex: 0x85858D)
}

enum F {
    static func ui(_ size: CGFloat, _ weight: Font.Weight = .regular) -> Font {
        .system(size: size, weight: weight)
    }
    static func mono(_ size: CGFloat, _ weight: Font.Weight = .regular) -> Font {
        .system(size: size, weight: weight, design: .monospaced)
    }
}

struct VMark: View {
    var size: CGFloat = 22
    var color: Color = V.text

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: size * 0.27, style: .continuous)
                .fill(color.opacity(0.075))
            RoundedRectangle(cornerRadius: size * 0.27, style: .continuous)
                .strokeBorder(color.opacity(0.55), lineWidth: 0.8)
            Path { path in
                path.move(to: CGPoint(x: size * 0.27, y: size * 0.28))
                path.addLine(to: CGPoint(x: size * 0.50, y: size * 0.73))
                path.addLine(to: CGPoint(x: size * 0.73, y: size * 0.28))
            }
            .stroke(color, style: StrokeStyle(lineWidth: size * 0.085,
                                              lineCap: .square, lineJoin: .miter))
        }
        .frame(width: size, height: size)
    }
}

enum Metrics {
    static let menuBar: CGFloat = 30
    static let tabBar: CGFloat = 46
    static let rail: CGFloat = 50
    static let sidebar: CGFloat = 228
    static let status: CGFloat = 28
    static let console: CGFloat = 160
    static let radius: CGFloat = 7
    static let cardRadius: CGFloat = 14
    static let panelRadius: CGFloat = 11
}

struct PressableButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.99 : 1)
            .opacity(configuration.isPressed ? 0.94 : 1)
            .animation(.easeOut(duration: 0.1), value: configuration.isPressed)
    }
}

/// A reusable hover fill for icon buttons and rows.
struct HoverSurface: ViewModifier {
    var radius: CGFloat = 6
    var normal: Color = .clear
    var hover: Color = V.surface
    var border: Color = .clear
    var hoverBorder: Color = .clear

    @State private var hovering = false

    func body(content: Content) -> some View {
        content
            .background(
                RoundedRectangle(cornerRadius: radius, style: .continuous)
                    .fill(hovering ? hover : normal)
            )
            .overlay(
                RoundedRectangle(cornerRadius: radius, style: .continuous)
                    .strokeBorder(hovering ? hoverBorder : border, lineWidth: 0.8)
            )
            .onHover { hovering = $0 }
            .animation(.easeOut(duration: 0.16), value: hovering)
    }
}

extension View {
    func hoverSurface(radius: CGFloat = 6,
                      normal: Color = .clear,
                      hover: Color = V.surface,
                      border: Color = .clear,
                      hoverBorder: Color = .clear) -> some View {
        modifier(HoverSurface(radius: radius, normal: normal, hover: hover,
                              border: border, hoverBorder: hoverBorder))
    }
}
