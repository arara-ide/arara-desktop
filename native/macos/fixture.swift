import AppKit

func record(_ kind: String) { print("\(kind):\(Date().timeIntervalSince1970)"); fflush(stdout) }
final class Canvas: NSView {
    override var acceptsFirstResponder: Bool { true }
    override func mouseDown(with event: NSEvent) { window?.makeFirstResponder(self); record("mouse_down") }
    override func mouseUp(with event: NSEvent) { record("mouse_up") }
    override func keyDown(with event: NSEvent) { record("key_down") }
    override func keyUp(with event: NSEvent) { record("key_up") }
    override func draw(_ dirtyRect: NSRect) { NSColor.windowBackgroundColor.setFill(); dirtyRect.fill(); NSColor.systemBlue.setFill(); NSRect(x: 40, y: 40, width: 120, height: 80).fill() }
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
        state.frame = NSRect(x: 20, y: 250, width: 350, height: 24)
        state.setAccessibilityLabel("Fixture result")
        let password = NSSecureTextField(string: "fixture-secret")
        password.frame = NSRect(x: 210, y: 300, width: 160, height: 28)
        password.setAccessibilityLabel("Fixture protected")
        window.contentView!.addSubview(Canvas(frame: NSRect(x: 0, y: 0, width: 420, height: 220)))
        for view in [field, button, state, password] { window.contentView!.addSubview(view) }
        record("fixture_ready")
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }
    @objc func apply() { state.stringValue = "Applied: \(field.stringValue)" }
}
let app = NSApplication.shared
let delegate = Fixture()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
