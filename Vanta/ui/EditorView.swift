import SwiftUI
import AppKit

// MARK: - Editor screen

struct EditorView: View {
    @ObservedObject var model: AppModel

    private var consoleHeight: CGFloat {
        let rows = model.consoleEntries.isEmpty ? 3 : min(model.consoleEntries.count, 7)
        let input = model.awaitingInput == nil ? 0 : 28
        return 42 + CGFloat(rows) * 16 + CGFloat(input)
    }

    var body: some View {
        VStack(spacing: 0) {
            if let tab = model.activeTab {
                EditorHeader(model: model, tab: tab)
                Rectangle().fill(V.borderSubtle).frame(height: 1)

                ZStack {
                    if model.tabs.isEmpty {
                        EmptyEditor(model: model)
                    } else {
                        ForEach(model.tabs) { item in
                            CodeEditor(
                                text: Binding(
                                    get: { model.tabs.first(where: { $0.id == item.id })?.text ?? "" },
                                    set: { model.setText($0, for: item.id) }
                                ),
                                language: item.language,
                                fontSize: model.fontSize,
                                showLineNumbers: model.showLineNumbers,
                                onCursorChange: { line, column in
                                    if model.activeTabID == item.id {
                                        model.updateCursor(line: line, column: column)
                                    }
                                }
                            )
                            .opacity(model.activeTabID == item.id ? 1 : 0)
                            .allowsHitTesting(model.activeTabID == item.id)
                        }
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)

                if model.consoleOpen {
                    ConsolePanel(model: model, height: consoleHeight)
                        .padding(.horizontal, 10)
                        .padding(.top, 8)
                        .padding(.bottom, 8)
                        .transition(.move(edge: .bottom).combined(with: .opacity))
                } else {
                    ConsoleBar(model: model)
                        .padding(.horizontal, 10)
                        .padding(.top, 6)
                        .padding(.bottom, 8)
                        .transition(.opacity)
                }
            } else {
                EmptyEditor(model: model)
            }
        }
        .background(V.base)
    }
}

struct EditorHeader: View {
    @ObservedObject var model: AppModel
    let tab: EditorTab

    private var crumbs: [String] {
        let path = tab.path ?? tab.name
        return path.split(separator: "/").map(String.init)
    }

    var body: some View {
        HStack(spacing: 7) {
            Image(systemName: "folder")
                .font(.system(size: 9.5, weight: .medium))
                .foregroundStyle(V.ghost)
            ForEach(Array(crumbs.enumerated()), id: \.offset) { index, crumb in
                if index > 0 {
                    Text("/")
                        .font(F.ui(11))
                        .foregroundStyle(V.ghost)
                }
                Text(crumb)
                    .font(F.ui(11, index == crumbs.count - 1 ? .medium : .regular))
                    .foregroundStyle(index == crumbs.count - 1 ? V.secondary : V.faint)
            }

            Text(tab.language.label)
                .font(F.ui(8.5, .semibold))
                .tracking(0.7)
                .foregroundStyle(V.faint)
                .padding(.horizontal, 5)
                .frame(height: 16)
                .overlay(
                    RoundedRectangle(cornerRadius: 4)
                        .strokeBorder(V.border, lineWidth: 0.8)
                )

            Spacer()

            if let summary = model.runSummary {
                Text(summary)
                    .font(F.ui(10))
                    .foregroundStyle(model.lastRun?.phase == .failed ? V.danger : V.faint)
                    .lineLimit(1)
                    .padding(.trailing, 8)
                    .help(summary)
            }

            Text("\(model.lineCount) lines")
                .font(F.mono(9))
                .foregroundStyle(V.ghost)

            SmallIconButton(icon: "magnifyingglass", help: "Find (⌘F)") {
                model.find()
            }
        }
        .padding(.leading, 15)
        .padding(.trailing, 7)
        .frame(height: 34)
        .background(V.base)
    }
}

struct SmallIconButton: View {
    let icon: String
    let help: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: icon)
                .font(.system(size: 9.5, weight: .medium))
                .foregroundStyle(V.faint)
                .frame(width: 22, height: 22)
        }
        .buttonStyle(.plain)
        .hoverSurface(radius: 5, hover: V.surface)
        .help(help)
    }
}

struct EmptyEditor: View {
    @ObservedObject var model: AppModel

    var body: some View {
        VStack(spacing: 12) {
            VMark(size: 46).opacity(0.8)
            Text("No script open")
                .font(F.ui(13, .medium))
                .foregroundStyle(V.secondary)
            Text("Start something new or open a file.")
                .font(F.ui(11))
                .foregroundStyle(V.faint)
            HStack(spacing: 7) {
                Button(action: model.newScript) {
                    HStack(spacing: 5) {
                        Image(systemName: "plus").font(.system(size: 9, weight: .bold))
                        Text("New script").font(F.ui(10.5, .semibold))
                    }
                    .foregroundStyle(V.onAccent)
                    .padding(.horizontal, 11)
                    .frame(height: 27)
                    .background(RoundedRectangle(cornerRadius: 6.5).fill(V.accent))
                }
                .buttonStyle(.plain)

                Button(action: model.openFromDisk) {
                    Text("Open…")
                        .font(F.ui(10.5, .medium))
                        .foregroundStyle(V.secondary)
                        .padding(.horizontal, 11)
                        .frame(height: 27)
                        .background(RoundedRectangle(cornerRadius: 6.5).fill(V.surface))
                        .overlay(RoundedRectangle(cornerRadius: 6.5)
                            .strokeBorder(V.border, lineWidth: 0.8))
                }
                .buttonStyle(.plain)
            }
            .padding(.top, 4)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(V.base)
    }
}

// MARK: - Console

struct ConsolePanel: View {
    @ObservedObject var model: AppModel
    var height: CGFloat = 160
    @State private var answer = ""

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 8) {
                Image(systemName: "terminal")
                    .font(.system(size: 9.5, weight: .medium))
                    .foregroundStyle(V.faint)
                Text("Console")
                    .font(F.ui(10.5, .semibold))
                    .foregroundStyle(V.secondary)

                Text("\(model.consoleEntries.count)")
                    .font(F.mono(9))
                    .foregroundStyle(V.faint)

                Spacer()

                Button(action: model.clearConsole) {
                    Text("CLEAR")
                        .font(F.ui(8, .semibold))
                        .tracking(0.9)
                        .foregroundStyle(V.faint)
                }
                .buttonStyle(.plain)
                .hoverSurface(radius: 4, hover: V.surface)

                SmallIconButton(icon: "doc.on.doc", help: "Copy console",
                                action: model.copyConsole)
                SmallIconButton(icon: "chevron.down", help: "Collapse console",
                                action: model.toggleConsole)
            }
            .padding(.leading, 12)
            .padding(.trailing, 6)
            .frame(height: 30)

            if model.consoleEntries.isEmpty {
                VStack(spacing: 7) {
                    Image(systemName: "terminal")
                        .font(.system(size: 18, weight: .ultraLight))
                        .foregroundStyle(V.faint)
                    Text("No console output yet")
                        .font(F.ui(11, .medium))
                        .foregroundStyle(V.muted)
                    Text("rconsoleprint and executor events appear here.")
                        .font(F.ui(10))
                        .foregroundStyle(V.faint)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ScrollViewReader { proxy in
                    ScrollView(.vertical, showsIndicators: false) {
                        LazyVStack(alignment: .leading, spacing: 2) {
                            ForEach(model.consoleEntries) { entry in
                                ConsoleLineRow(entry: entry)
                                    .id(entry.id)
                            }
                        }
                        .padding(.leading, 12)
                        .padding(.trailing, 10)
                        .padding(.top, 4)
                        .padding(.bottom, 8)
                    }
                    .onChange(of: model.consoleEntries.count) { _ in
                        guard let last = model.consoleEntries.last else { return }
                        withAnimation(.easeOut(duration: 0.12)) {
                            proxy.scrollTo(last.id, anchor: .bottom)
                        }
                    }
                }
            }

            if let prompt = model.awaitingInput {
                HStack(spacing: 8) {
                    Text(prompt)
                        .font(F.mono(9.5, .medium))
                        .foregroundStyle(V.accent)
                        .lineLimit(1)
                    TextField("answer the executor…", text: $answer)
                        .textFieldStyle(.plain)
                        .font(F.mono(9.5))
                        .foregroundStyle(V.text)
                        .onSubmit {
                            guard !answer.isEmpty else { return }
                            model.answerConsoleInput(answer)
                            answer = ""
                        }
                    Text("↵ send")
                        .font(F.mono(8))
                        .foregroundStyle(V.ghost)
                }
                .padding(.horizontal, 12)
                .frame(height: 28)
                .background(V.chrome)
                .overlay(alignment: .top) {
                    Rectangle().fill(V.borderSubtle).frame(height: 1)
                }
            }
        }
        .frame(height: height)
        .background(V.panel)
        .clipShape(RoundedRectangle(cornerRadius: Metrics.panelRadius, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: Metrics.panelRadius, style: .continuous)
                .strokeBorder(V.border, lineWidth: 0.8)
        )
        .shadow(color: Color.black.opacity(0.28), radius: 14, y: 6)
    }
}

struct ConsoleBar: View {
    @ObservedObject var model: AppModel

    var body: some View {
        Button(action: model.toggleConsole) {
            HStack(spacing: 8) {
                Image(systemName: "terminal")
                    .font(.system(size: 9.5, weight: .medium))
                    .foregroundStyle(V.faint)
                Text("Console")
                    .font(F.ui(10.5, .semibold))
                    .foregroundStyle(V.secondary)
                Text("\(model.consoleEntries.count)")
                    .font(F.mono(8.5))
                    .foregroundStyle(V.ghost)
                Spacer()
                Image(systemName: "chevron.up")
                    .font(.system(size: 8, weight: .bold))
                    .foregroundStyle(V.faint)
            }
            .padding(.horizontal, 12)
            .frame(height: 30)
            .background(V.panel)
            .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .strokeBorder(V.border, lineWidth: 0.8)
            )
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help("Show console (⌘J)")
    }
}

struct ConsoleLineRow: View {
    let entry: ConsoleEntry

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 11) {
            Text(entry.time)
                .font(F.mono(8.5))
                .foregroundStyle(V.ghost)
                .frame(width: 67, alignment: .leading)
            Text(entry.level.rawValue)
                .font(F.mono(8.5, .medium))
                .foregroundStyle(entry.level.color)
                .frame(width: 30, alignment: .leading)
            Text(entry.message)
                .font(F.mono(9.5))
                .foregroundStyle(entry.level == .print ? V.muted : V.secondary)
                .lineLimit(1)
            Spacer(minLength: 0)
        }
        .frame(height: 16)
    }
}

// MARK: - Native text editor

struct CodeEditor: NSViewRepresentable {
    @Binding var text: String
    let language: EditorLanguage
    let fontSize: CGFloat
    let showLineNumbers: Bool
    var readOnly = false
    var onCursorChange: ((Int, Int) -> Void)? = nil

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    func makeNSView(context: Context) -> NSScrollView {
        let scroll = NSScrollView()
        scroll.drawsBackground = true
        scroll.backgroundColor = V.nsBase
        scroll.hasVerticalScroller = true
        scroll.hasHorizontalScroller = true
        scroll.autohidesScrollers = true
        scroll.scrollerStyle = .overlay
        scroll.borderType = .noBorder

        let textView = VantaTextView()
        textView.delegate = context.coordinator
        textView.isRichText = false
        textView.usesFontPanel = false
        textView.allowsUndo = true
        textView.usesFindBar = !readOnly
        textView.isIncrementalSearchingEnabled = !readOnly
        textView.isAutomaticQuoteSubstitutionEnabled = false
        textView.isAutomaticDashSubstitutionEnabled = false
        textView.isAutomaticTextReplacementEnabled = false
        textView.isAutomaticSpellingCorrectionEnabled = false
        textView.isContinuousSpellCheckingEnabled = false
        textView.isGrammarCheckingEnabled = false
        textView.isAutomaticLinkDetectionEnabled = false
        textView.isAutomaticDataDetectionEnabled = false
        textView.smartInsertDeleteEnabled = false
        textView.drawsBackground = false
        textView.isEditable = !readOnly
        textView.isSelectable = true
        textView.insertionPointColor = readOnly ? .clear : V.nsAccent
        textView.selectedTextAttributes = [
            .backgroundColor: NSColor(hex: 0xFFFFFF, alpha: 0.15),
            .foregroundColor: V.nsText
        ]
        textView.textContainerInset = NSSize(width: 10, height: 9)
        textView.minSize = NSSize(width: 0, height: 0)
        textView.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude,
                                  height: CGFloat.greatestFiniteMagnitude)
        textView.isVerticallyResizable = true
        textView.isHorizontallyResizable = true
        textView.autoresizingMask = [.width]
        textView.textContainer?.widthTracksTextView = false
        textView.textContainer?.containerSize = NSSize(width: CGFloat.greatestFiniteMagnitude,
                                                       height: CGFloat.greatestFiniteMagnitude)

        scroll.documentView = textView

        let ruler = LineNumberRulerView(scrollView: scroll, textView: textView)
        scroll.verticalRulerView = ruler
        scroll.hasVerticalRuler = showLineNumbers
        scroll.rulersVisible = showLineNumbers

        context.coordinator.textView = textView
        context.coordinator.scrollView = scroll
        context.coordinator.ruler = ruler
        context.coordinator.apply(fontSize: fontSize)
        textView.string = text
        context.coordinator.highlightNow()
        context.coordinator.observeScrolling()

        textView.setSelectedRange(NSRange(location: 0, length: 0))
        context.coordinator.updateCursor()
        return scroll
    }

    func updateNSView(_ scroll: NSScrollView, context: Context) {
        guard let textView = scroll.documentView as? VantaTextView else { return }
        context.coordinator.parent = self

        if textView.string != text {
            let selection = textView.selectedRange()
            textView.string = text
            let length = (textView.string as NSString).length
            textView.setSelectedRange(NSRange(location: min(selection.location, length),
                                              length: 0))
            context.coordinator.highlightNow()
        }

        context.coordinator.apply(fontSize: fontSize)
        scroll.hasVerticalRuler = showLineNumbers
        scroll.rulersVisible = showLineNumbers
        context.coordinator.ruler?.needsDisplay = true
    }

    final class Coordinator: NSObject, NSTextViewDelegate {
        var parent: CodeEditor
        weak var textView: VantaTextView?
        weak var scrollView: NSScrollView?
        var ruler: LineNumberRulerView?
        private var highlightWork: DispatchWorkItem?
        private var scrollToken: Any?
        private var appliedFontSize: CGFloat = 0

        init(_ parent: CodeEditor) {
            self.parent = parent
        }

        deinit {
            if let token = scrollToken {
                NotificationCenter.default.removeObserver(token)
            }
        }

        func observeScrolling() {
            guard let scrollView else { return }
            scrollView.contentView.postsBoundsChangedNotifications = true
            scrollToken = NotificationCenter.default.addObserver(
                forName: NSView.boundsDidChangeNotification,
                object: scrollView.contentView,
                queue: .main
            ) { [weak self] _ in
                self?.ruler?.needsDisplay = true
            }
        }

        func apply(fontSize: CGFloat) {
            guard fontSize != appliedFontSize else { return }
            appliedFontSize = fontSize
            let font = NSFont.monospacedSystemFont(ofSize: fontSize, weight: .regular)
            textView?.font = font
            textView?.typingAttributes = baseAttributes(fontSize: fontSize)
            ruler?.numberFont = NSFont.monospacedSystemFont(ofSize: 9.5, weight: .regular)
            ruler?.needsDisplay = true
            highlightNow()
        }

        // MARK: Delegate

        func textDidChange(_ notification: Notification) {
            guard let textView else { return }
            parent.text = textView.string
            scheduleHighlight()
            ruler?.needsDisplay = true
            updateCursor()
        }

        func textViewDidChangeSelection(_ notification: Notification) {
            updateCursor()
        }

        func textView(_ textView: NSTextView, doCommandBy commandSelector: Selector) -> Bool {
            if commandSelector == #selector(NSResponder.insertTab(_:)) {
                textView.insertText("    ", replacementRange: textView.selectedRange())
                return true
            }

            if commandSelector == #selector(NSResponder.insertNewline(_:)) {
                let string = textView.string as NSString
                let location = min(textView.selectedRange().location, string.length)
                let lineRange = string.lineRange(for: NSRange(location: location, length: 0))
                let lineStart = lineRange.location
                let here = NSRange(location: lineStart, length: max(0, location - lineStart))
                let lineSoFar = string.substring(with: here)
                var indent = ""
                for character in lineSoFar {
                    if character == " " || character == "\t" { indent.append(character) }
                    else { break }
                }
                textView.insertText("\n" + indent, replacementRange: textView.selectedRange())
                return true
            }

            return false
        }

        func updateCursor() {
            guard let textView else { return }
            let string = textView.string as NSString
            let location = min(textView.selectedRange().location, string.length)
            let prefix = string.substring(to: location)
            let lines = prefix.components(separatedBy: "\n")
            let line = lines.count
            let column = (lines.last ?? "").utf16.count + 1
            parent.onCursorChange?(line, column)
        }

        // MARK: Highlighting

        func scheduleHighlight() {
            highlightWork?.cancel()
            let work = DispatchWorkItem { [weak self] in self?.highlightNow() }
            highlightWork = work
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.07, execute: work)
        }

        func highlightNow() {
            guard let textView, let storage = textView.textStorage else { return }
            let text = textView.string
            let full = NSRange(location: 0, length: (text as NSString).length)
            let font = NSFont.monospacedSystemFont(ofSize: appliedFontSize, weight: .regular)
            let base = baseAttributes(fontSize: appliedFontSize)

            storage.beginEditing()
            storage.setAttributes(base, range: full)

            if parent.language == .luau {
                let ns = text as NSString
                func paint(_ regex: NSRegularExpression, _ color: NSColor) {
                    regex.enumerateMatches(in: text, range: full) { match, _, _ in
                        guard let match else { return }
                        storage.addAttribute(.foregroundColor, value: color, range: match.range)
                    }
                }
                paint(LuauHighlight.number, V.nsNumber)
                paint(LuauHighlight.keyword, V.nsKeyword)
                paint(LuauHighlight.global, V.nsGlobal)
                paint(LuauHighlight.property, V.nsProperty)
                paint(LuauHighlight.function, V.nsFunction)
                paint(LuauHighlight.string, V.nsString)
                paint(LuauHighlight.comment, V.nsComment)
                _ = ns
            }

            storage.endEditing()
            textView.typingAttributes = base
            textView.font = font
            ruler?.needsDisplay = true
        }

        private func baseAttributes(fontSize: CGFloat) -> [NSAttributedString.Key: Any] {
            let font = NSFont.monospacedSystemFont(ofSize: fontSize, weight: .regular)
            let paragraph = NSMutableParagraphStyle()
            let lineHeight = max(19, fontSize + 7.5)
            paragraph.minimumLineHeight = lineHeight
            paragraph.maximumLineHeight = lineHeight
            paragraph.lineSpacing = 0
            return [
                .font: font,
                .foregroundColor: V.nsText,
                .paragraphStyle: paragraph
            ]
        }
    }
}

final class VantaTextView: NSTextView {
    override func drawInsertionPoint(in rect: NSRect, color: NSColor, turnedOn flag: Bool) {
        super.drawInsertionPoint(in: rect, color: color, turnedOn: flag)
    }
}

enum LuauHighlight {
    static let keyword = regex(#"\b(?:local|function|end|if|then|else|elseif|for|in|do|while|repeat|until|return|break|continue|and|or|not|true|false|nil)\b"#)
    static let global = regex(#"\b(?:game|workspace|script|task|wait|print|warn|error|Instance|Enum|Color3|Vector3|UDim2|CFrame|os|math|string|table|pairs|ipairs|typeof|tostring|tonumber|require)\b"#)
    static let number = regex(#"\b\d+(?:\.\d+)?\b"#)
    static let function = regex(#"\b[A-Za-z_][A-Za-z0-9_]*(?=\s*\()"#)
    static let property = regex(#"(?<=[.:])[A-Za-z_][A-Za-z0-9_]*"#)
    static let string = regex(#""(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'"#)
    static let comment = regex(#"--[^\n]*"#)

    private static func regex(_ pattern: String) -> NSRegularExpression {
        try! NSRegularExpression(pattern: pattern, options: [])
    }
}

// MARK: - Line numbers

final class LineNumberRulerView: NSRulerView {
    weak var textView: NSTextView?
    var numberFont = NSFont.monospacedSystemFont(ofSize: 9.5, weight: .regular)

    init(scrollView: NSScrollView, textView: NSTextView) {
        self.textView = textView
        super.init(scrollView: scrollView, orientation: .verticalRuler)
        self.clientView = textView
        self.ruleThickness = 52
    }

    required init(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func drawHashMarksAndLabels(in rect: NSRect) {
        V.nsBase.setFill()
        bounds.fill()

        V.nsBorderSubtle.setFill()
        NSRect(x: bounds.maxX - 0.5, y: bounds.minY, width: 0.5, height: bounds.height).fill()

        guard let textView,
              let layoutManager = textView.layoutManager,
              let textContainer = textView.textContainer,
              let scrollView else { return }

        let visible = scrollView.contentView.bounds
        let glyphRange = layoutManager.glyphRange(forBoundingRect: visible,
                                                  in: textContainer)
        let charRange = layoutManager.characterRange(forGlyphRange: glyphRange,
                                                     actualGlyphRange: nil)
        let string = textView.string as NSString
        guard string.length > 0 else {
            draw(number: 1, y: textView.textContainerInset.height - visible.minY)
            return
        }

        let before = string.substring(to: min(charRange.location, string.length))
        var line = before.components(separatedBy: "\n").count
        var location = min(charRange.location, string.length)

        while location < string.length {
            let lineRange = string.lineRange(for: NSRange(location: location, length: 0))
            let glyphIndex = layoutManager.glyphIndexForCharacter(at: lineRange.location)
            let lineRect = layoutManager.lineFragmentRect(forGlyphAt: glyphIndex,
                                                          effectiveRange: nil)
            let y = lineRect.minY + textView.textContainerInset.height - visible.minY
            draw(number: line, y: y)
            if lineRect.minY > visible.maxY + 24 { break }
            location = lineRange.upperBound
            line += 1
        }
    }

    private func draw(number: Int, y: CGFloat) {
        let attrs: [NSAttributedString.Key: Any] = [
            .font: numberFont,
            .foregroundColor: V.nsFaint
        ]
        let text = "\(number)" as NSString
        let size = text.size(withAttributes: attrs)
        text.draw(at: NSPoint(x: ruleThickness - size.width - 12,
                              y: y - 1),
                  withAttributes: attrs)
    }
}
