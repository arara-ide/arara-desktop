import AppKit
import WebKit

final class FeedbackPanel: NSPanel {
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}

final class DesktopFeedback: NSObject, WKNavigationDelegate {
    static let shared = DesktopFeedback()
    private var panel: FeedbackPanel?
    private var view: WKWebView?
    private var loaded = false
    private var pending: (() -> Void)?
    private var idle: Timer?
    private var target: pid_t?
    private var revision = 0
    private var origin = CGPoint.zero
    private let resources = Bundle.main.bundleURL.appendingPathComponent("Contents/Resources")

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        loaded = true
        let run = pending
        pending = nil
        run?()
    }

    func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        decisionHandler(navigationAction.request.url == resources.appendingPathComponent("cursor.html") ? .allow : .cancel)
    }

    private func prepare() -> Bool {
        if panel != nil { return true }
        guard FileManager.default.fileExists(atPath: resources.appendingPathComponent("browser-cursor.js").path), let primary = NSScreen.screens.first else { return false }
        let bounds = NSScreen.screens.reduce(primary.frame) { $0.union($1.frame) }
        origin = CGPoint(x: bounds.minX, y: primary.frame.maxY - bounds.maxY)
        let panel = FeedbackPanel(contentRect: bounds, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        panel.level = .screenSaver
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false
        panel.ignoresMouseEvents = true
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .ignoresCycle]
        let configuration = WKWebViewConfiguration()
        configuration.websiteDataStore = .nonPersistent()
        let view = WKWebView(frame: NSRect(origin: .zero, size: bounds.size), configuration: configuration)
        view.setValue(false, forKey: "drawsBackground")
        view.navigationDelegate = self
        panel.contentView = view
        self.panel = panel
        self.view = view
        view.loadFileURL(resources.appendingPathComponent("cursor.html"), allowingReadAccessTo: resources)
        return true
    }

    func move(_ point: CGPoint, pid: pid_t, click: Bool = false) {
        let finished = DispatchSemaphore(value: 0)
        DispatchQueue.main.async {
            guard self.prepare() else { finished.signal(); return }
            self.revision += 1
            let revision = self.revision
            self.target = pid
            self.panel?.alphaValue = 1
            self.panel?.orderFrontRegardless()
            self.idle?.invalidate()
            self.idle = Timer.scheduledTimer(withTimeInterval: 10, repeats: false) { _ in self.hide() }
            let run = {
                guard self.revision == revision else { finished.signal(); return }
                self.view?.callAsyncJavaScript("AraraCursor.render({active:true}); await AraraCursor.moveTo(x,y); if(click) await AraraCursor.press();", arguments: ["x": point.x - self.origin.x, "y": point.y - self.origin.y, "click": click], in: nil, in: .page) { _ in finished.signal() }
            }
            if self.loaded { run() } else { self.pending = run }
        }
        if finished.wait(timeout: .now() + 2) == .timedOut {
            DispatchQueue.main.async { self.revision += 1; self.pending = nil; self.hide() }
        }
    }

    func hide() {
        revision += 1
        pending = nil
        idle?.invalidate()
        idle = nil
        target = nil
        let revision = revision
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.2
            panel?.animator().alphaValue = 0
        } completionHandler: {
            if self.revision == revision { self.panel?.orderOut(nil) }
        }
    }

    func stop() {
        let finished = DispatchSemaphore(value: 0)
        DispatchQueue.main.async {
            self.hide()
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.22) {
                self.view?.evaluateJavaScript("AraraCursor.render({active:false})")
                finished.signal()
            }
        }
        _ = finished.wait(timeout: .now() + 0.4)
    }

    func checkFocus() {
        if let target = target, NSWorkspace.shared.frontmostApplication?.processIdentifier != target { hide() }
    }
}
