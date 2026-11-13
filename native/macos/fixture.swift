import AppKit

final class Fixture: NSObject, NSApplicationDelegate {
    var window: NSWindow!
    let field = NSTextField(string: "Initial")
    let state = NSTextField(labelWithString: "Waiting")
    func applicationDidFinishLaunching(_ notification: Notification) {
        window = NSWindow(contentRect: NSRect(x: 80, y: 80, width: 420, height: 230), styleMask: [.titled, .closable], backing: .buffered, defer: false)
        window.title = "Arara Desktop Fixture"
        field.frame = NSRect(x: 20, y: 150, width: 350, height: 28)
        field.setAccessibilityLabel("Fixture input")
        let button = NSButton(title: "Apply fixture", target: self, action: #selector(apply))
        button.frame = NSRect(x: 20, y: 100, width: 180, height: 30)
        state.frame = NSRect(x: 20, y: 50, width: 350, height: 24)
        state.setAccessibilityLabel("Fixture result")
        let password = NSSecureTextField(string: "fixture-secret")
        password.frame = NSRect(x: 210, y: 100, width: 160, height: 28)
        password.setAccessibilityLabel("Fixture protected")
        for view in [field, button, state, password] { window.contentView!.addSubview(view) }
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
