import AppKit

func record(_ kind: String) { print("\(kind):\(Date().timeIntervalSince1970)"); fflush(stdout) }
final class Canvas: NSView {
    var typed = ""
    var scroll: CGFloat = 0
    override var acceptsFirstResponder: Bool { true }
    override func mouseDown(with event: NSEvent) { window?.makeFirstResponder(self); record("mouse_down") }
    override func mouseUp(with event: NSEvent) { record("mouse_up") }
    override func keyDown(with event: NSEvent) { typed += event.characters ?? ""; needsDisplay = true; record("key_down") }
    override func keyUp(with event: NSEvent) { record("key_up") }
    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        if event.modifierFlags.contains([.command, .shift]) && event.charactersIgnoringModifiers?.lowercased() == "g" { record("key_chord"); return true }
        return super.performKeyEquivalent(with: event)
    }
    override func scrollWheel(with event: NSEvent) { scroll += event.scrollingDeltaY; needsDisplay = true; record("scroll") }
    override func draw(_ dirtyRect: NSRect) { NSColor.windowBackgroundColor.setFill(); dirtyRect.fill(); NSColor.systemBlue.setFill(); NSRect(x: 40, y: 40 + scroll, width: 120, height: 80).fill(); (typed as NSString).draw(at: NSPoint(x: 20, y: 15), withAttributes: [.foregroundColor: NSColor.labelColor]) }
}
final class Fixture: NSObject, NSApplicationDelegate {
    var window: NSWindow!
    let field = NSTextField(string: "Initial")
    let state = NSTextField(labelWithString: "Waiting")
    func applicationDidFinishLaunching(_ notification: Notification) {
        window = NSWindow(contentRect: NSRect(x: 80, y: 80, width: 420, height: 420), styleMask: [.titled, .closable], backing: .buffered, defer: false)
        window.title = "Arara Desktop Fixture"
        field.frame = NSRect(x: 20, y: 350, width: 350, height: 28)
        field.setAccessibilityLabel("Fixture input")
        let button = NSButton(title: "Apply fixture", target: self, action: #selector(apply))
        button.frame = NSRect(x: 20, y: 300, width: 180, height: 30)
        let open = NSButton(title: "Open fixture dialog", target: self, action: #selector(openFile))
        open.frame = NSRect(x: 210, y: 275, width: 190, height: 22)
        state.frame = NSRect(x: 20, y: 250, width: 350, height: 24)
        state.setAccessibilityLabel("Fixture result")
        let password = NSSecureTextField(string: "fixture-secret")
        password.frame = NSRect(x: 210, y: 300, width: 160, height: 28)
        password.setAccessibilityLabel("Fixture protected")
        window.contentView!.addSubview(Canvas(frame: NSRect(x: 0, y: 0, width: 420, height: 240)))
        for view in [field, button, open, state, password] { window.contentView!.addSubview(view) }
        record("fixture_ready")
        if let flag = CommandLine.arguments.firstIndex(of: "--pid-file"), CommandLine.arguments.count > flag + 1 { try? String(getpid()).write(toFile: CommandLine.arguments[flag + 1], atomically: true, encoding: .utf8) }
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }
    @objc func apply() { state.stringValue = "Applied: \(field.stringValue)" }
    @objc func openFile() {
        let panel = NSOpenPanel()
        if let flag = CommandLine.arguments.firstIndex(of: "--pid-file"), CommandLine.arguments.count > flag + 1 { panel.directoryURL = URL(fileURLWithPath: CommandLine.arguments[flag + 1]).deletingLastPathComponent() }
        panel.beginSheetModal(for: window) { _ in record("dialog_closed") }
    }
}
let app = NSApplication.shared
let delegate = Fixture()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
