import AppKit
import ApplicationServices
import Foundation
import ImageIO
import UniformTypeIdentifiers

struct DesktopFailure: Error {
    let code: String
}

struct WindowRef {
    let pid: pid_t
    let element: AXUIElement?
    var number: CGWindowID? = nil
    var title: String = ""
}

struct ElementRef {
    let window: String
    let element: AXUIElement
    let role: String
    let name: String
}

var windows: [String: WindowRef] = [:]
var elements: [String: ElementRef] = [:]
var previousElements: [String: ElementRef] = [:]

func invalidateElements() {
    if !elements.isEmpty { previousElements = elements }
    elements.removeAll()
}

func textSlices(_ text: String) -> [String] {
    let characters = Array(text)
    let step = max(1, Int(ceil(Double(characters.count) / 50)))
    return stride(from: 0, to: characters.count, by: step).map { String(characters[$0..<min(characters.count, $0 + step)]) }
}

func feedback(_ point: CGPoint, _ ref: WindowRef, click: Bool = false) throws {
    DesktopFeedback.shared.move(point, pid: ref.pid, click: click)
    guard !isCancelled() else { throw DesktopFailure(code: "cancelled") }
    guard focused(ref) else { throw DesktopFailure(code: "focus") }
}
struct FrameRef {
    let window: String
    let bounds: CGRect
    let created: Date
    let visual: Bool
}
var frames: [String: FrameRef] = [:]
func rememberFrame(_ window: String, bounds: CGRect, visual: Bool) -> String {
    let id = UUID().uuidString
    frames = [id: FrameRef(window: window, bounds: bounds, created: Date(), visual: visual)]
    return id
}
let cancellation = NSLock()
var cancelled = false
var cancellationGeneration = 0
var heldKeys: Set<CGKeyCode> = []
var heldButtons: Set<Int> = []

func isCancelled() -> Bool {
    cancellation.lock()
    defer { cancellation.unlock() }
    return cancelled
}
func sessionLocked() -> Bool {
    let session = CGSessionCopyCurrentDictionary() as? [String: Any] ?? [:]
    return (session["CGSSessionScreenIsLocked"] as? Bool) == true || NSWorkspace.shared.frontmostApplication?.bundleIdentifier == "com.apple.loginwindow"
}
func cancelInput() {
    cancellation.lock()
    cancelled = true
    cancellationGeneration += 1
    cancellation.unlock()
}
func beginInput() {
    cancellation.lock()
    cancelled = false
    cancellation.unlock()
}
let blockedApps: Set<String> = ["com.apple.systempreferences", "com.apple.SecurityAgent", "com.apple.loginwindow", "com.apple.Passwords", "com.apple.keychainaccess"]

func attribute(_ element: AXUIElement, _ name: String) -> AnyObject? {
    var value: CFTypeRef?
    guard AXUIElementCopyAttributeValue(element, name as CFString, &value) == .success else { return nil }
    return value
}

func string(_ element: AXUIElement, _ name: String) -> String {
    (attribute(element, name) as? String) ?? ""
}

func secure(_ element: AXUIElement) -> Bool {
    let role = string(element, kAXRoleAttribute)
    let subrole = string(element, kAXSubroleAttribute)
    return role.contains("Secure") || subrole.contains("Secure") || (attribute(element, "AXProtectedContent") as? Bool) == true
}

func label(_ element: AXUIElement) -> String {
    for name in [kAXTitleAttribute, kAXDescriptionAttribute, kAXHelpAttribute] {
        let value = string(element, name)
        if !value.isEmpty { return String(value.prefix(160)) }
    }
    return ""
}

func operations(_ element: AXUIElement) -> [String] {
    var names: CFArray?
    var result: [String] = []
    if AXUIElementCopyActionNames(element, &names) == .success,
       (names as? [String])?.contains(kAXPressAction) == true { result.append("press") }
    for (attribute, operation) in [(kAXValueAttribute, "set_value"), (kAXFocusedAttribute, "focus")] {
        var settable = DarwinBoolean(false)
        if AXUIElementIsAttributeSettable(element, attribute as CFString, &settable) == .success && settable.boolValue { result.append(operation) }
    }
    return result
}

func appAllowed(_ pid: pid_t) -> Bool {
    guard let app = NSRunningApplication(processIdentifier: pid), !app.isTerminated else { return false }
    return !blockedApps.contains(app.bundleIdentifier ?? "")
}

func accessibleApp(_ pid: pid_t) -> AXUIElement {
    let root = AXUIElementCreateApplication(pid)
    AXUIElementSetMessagingTimeout(root, 0.25)
    AXUIElementSetAttributeValue(root, "AXManualAccessibility" as CFString, kCFBooleanTrue)
    AXUIElementSetAttributeValue(root, "AXEnhancedUserInterface" as CFString, kCFBooleanTrue)
    return root
}

func windowTitle(_ ref: WindowRef) -> String {
    let title = ref.element.map { string($0, kAXTitleAttribute) } ?? ""
    return title.isEmpty ? ref.title : title
}
func isModalWindow(_ ref: WindowRef) -> Bool {
    guard let element = ref.element else { return false }
    return string(element, kAXRoleAttribute) == "AXSheet" || (attribute(element, "AXModal") as? Bool) == true
}
func windowMetadata(_ ref: WindowRef) -> [String: Any] {
    var result: [String: Any] = ["title": windowTitle(ref), "application": NSRunningApplication(processIdentifier: ref.pid)?.bundleIdentifier ?? "", "accessibility": ref.element != nil, "modal": isModalWindow(ref), "active": focused(ref)]
    guard let element = ref.element else { return result }
    var queue = [(element, 0)]
    var visited = 0
    let started = Date()
    while !queue.isEmpty && visited < 200 && Date().timeIntervalSince(started) < 1 {
        let (element, depth) = queue.removeFirst()
        visited += 1
        if secure(element) { continue }
        if string(element, kAXRoleAttribute) == "AXWebArea" {
            result["documentTitle"] = label(element)
            if let url = attribute(element, kAXURLAttribute) as? URL { result["documentUrl"] = String(url.absoluteString.prefix(2000)) }
            else if let url = attribute(element, kAXURLAttribute) as? String { result["documentUrl"] = String(url.prefix(2000)) }
            break
        }
        if depth < 12, let children = attribute(element, kAXChildrenAttribute) as? [AXUIElement] {
            queue.append(contentsOf: children.prefix(100).map { ($0, depth + 1) })
        }
    }
    return result
}

func listWindows(_ application: String?) -> [String: Any] {
    beginInput()
    windows.removeAll()
    frames.removeAll()
    elements.removeAll()
    previousElements.removeAll()
    var result: [[String: Any]] = []
    let graphics = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] ?? []
    for app in NSWorkspace.shared.runningApplications where app.activationPolicy == .regular && appAllowed(app.processIdentifier) && (application == nil || app.bundleIdentifier == application || String(app.processIdentifier) == application) {
        let root = accessibleApp(app.processIdentifier)
        var children = attribute(root, kAXWindowsAttribute) as? [AXUIElement] ?? []
        if let focused = attribute(root, kAXFocusedWindowAttribute), CFGetTypeID(focused) == AXUIElementGetTypeID() {
            let element = focused as! AXUIElement
            var owner: pid_t = 0
            if AXUIElementGetPid(element, &owner) == .success && owner == app.processIdentifier && !children.contains(where: { CFEqual($0, element) }) { children.insert(element, at: 0) }
        }
        let native = graphics.filter { $0[kCGWindowOwnerPID as String] as? Int == Int(app.processIdentifier) && $0[kCGWindowLayer as String] as? Int == 0 }
        var discovered: [WindowRef] = []
        for entry in native.prefix(20) {
            guard let number = entry[kCGWindowNumber as String] as? CGWindowID, let raw = entry[kCGWindowBounds as String] as? NSDictionary, let box = CGRect(dictionaryRepresentation: raw), box.width > 1, box.height > 1 else { continue }
            let child = children.first { element in
                guard let rect = try? bounds(WindowRef(pid: app.processIdentifier, element: element)) else { return false }
                return abs(rect.minX - box.minX) < 2 && abs(rect.minY - box.minY) < 2 && abs(rect.width - box.width) < 2 && abs(rect.height - box.height) < 2
            }
            discovered.append(WindowRef(pid: app.processIdentifier, element: child, number: number, title: entry[kCGWindowName as String] as? String ?? ""))
        }
        if discovered.isEmpty { discovered = children.prefix(20).map { WindowRef(pid: app.processIdentifier, element: $0) } }
        for ref in discovered {
            let id = UUID().uuidString
            windows[id] = ref
            result.append(["windowId": id, "processId": app.processIdentifier, "app": app.localizedName ?? "", "title": String(windowTitle(ref).prefix(160)), "accessibility": ref.element != nil, "active": focused(ref), "modal": isModalWindow(ref)])
            if result.count >= 100 { return ["windows": result, "truncated": true] }
        }
    }
    return ["windows": result, "truncated": false]
}

func window(_ id: String) throws -> WindowRef {
    guard let ref = windows[id], appAllowed(ref.pid) else { throw DesktopFailure(code: "stale") }
    if let element = ref.element { guard !string(element, kAXRoleAttribute).isEmpty else { throw DesktopFailure(code: "stale") } }
    else { _ = try bounds(ref) }
    return ref
}

func snapshot(_ id: String) throws -> [String: Any] {
    let ref = try window(id)
    let inputFrameId = rememberFrame(id, bounds: try bounds(ref), visual: false)
    guard let element = ref.element else { return ["windowId": id, "nodes": [], "truncated": false, "accessibility": false, "inputFrameId": inputFrameId] }
    var queue = [(element, 0)]
    var nodes: [[String: Any]] = []
    var visited: [AXUIElement] = []
    var retained: [String: ElementRef] = [:]
    let start = Date()
    var truncated = false
    while !queue.isEmpty {
        if visited.count >= 600 || nodes.count >= 100 || Date().timeIntervalSince(start) > 3 { truncated = true; break }
        let (element, depth) = queue.removeFirst()
        if visited.contains(where: { CFEqual($0, element) }) { continue }
        visited.append(element)
        if secure(element) { continue }
        let role = string(element, kAXRoleAttribute)
        let name = label(element)
        let actions = operations(element)
        if !role.isEmpty && (!name.isEmpty || !actions.isEmpty || role == kAXStaticTextRole || role == kAXTextAreaRole) {
            let known = elements.isEmpty ? previousElements : elements
            let uid = known.first(where: { $0.value.window == id && CFEqual($0.value.element, element) })?.key ?? UUID().uuidString
            retained[uid] = ElementRef(window: id, element: element, role: role, name: name)
            var node: [String: Any] = ["uid": uid, "role": role, "name": name, "enabled": (attribute(element, kAXEnabledAttribute) as? Bool) ?? true, "operations": actions]
            if let value = attribute(element, kAXValueAttribute) as? String { node["value"] = String(value.prefix(240)) }
            nodes.append(node)
        }
        if let children = attribute(element, kAXChildrenAttribute) as? [AXUIElement], !children.isEmpty {
            if depth >= 12 { truncated = true }
            else {
                if children.count > 100 { truncated = true }
                queue.append(contentsOf: children.prefix(100).map { ($0, depth + 1) })
            }
        }
    }
    elements = retained
    previousElements = retained
    return ["windowId": id, "nodes": nodes, "truncated": truncated, "inputFrameId": inputFrameId]
}

func target(_ request: [String: Any]) throws -> (WindowRef, ElementRef) {
    let id = request["windowId"] as? String ?? ""
    let ref = try window(id)
    guard let item = elements[request["uid"] as? String ?? ""], item.window == id,
          string(item.element, kAXRoleAttribute) == item.role, label(item.element) == item.name else { throw DesktopFailure(code: "stale") }
    guard !secure(item.element) else { throw DesktopFailure(code: "secure") }
    var pid: pid_t = 0
    guard AXUIElementGetPid(item.element, &pid) == .success && pid == ref.pid else { throw DesktopFailure(code: "stale") }
    return (ref, item)
}

func act(_ request: [String: Any]) throws -> [String: Any] {
    let (ref, item) = try target(request)
    let operation = request["operation"] as? String ?? ""
    guard NSWorkspace.shared.frontmostApplication?.processIdentifier == ref.pid else { throw DesktopFailure(code: "focus") }
    let root = AXUIElementCreateApplication(ref.pid)
    guard let focusedWindow = attribute(root, kAXFocusedWindowAttribute), let element = ref.element, CFEqual(focusedWindow, element) else { throw DesktopFailure(code: "focus") }
    guard operations(item.element).contains(operation) else { throw DesktopFailure(code: "unsupported") }
    guard (attribute(item.element, kAXEnabledAttribute) as? Bool) != false else { throw DesktopFailure(code: "stale") }
    if let rect = try? bounds(WindowRef(pid: ref.pid, element: item.element)) {
        try feedback(CGPoint(x: rect.midX, y: rect.midY), ref, click: operation == "press")
    }
    var result: AXError = .success
    switch operation {
    case "press": result = AXUIElementPerformAction(item.element, kAXPressAction as CFString)
    case "focus": result = AXUIElementSetAttributeValue(item.element, kAXFocusedAttribute as CFString, kCFBooleanTrue)
    case "set_value":
        guard let value = request["value"] as? String, value.count <= 8000 else { throw DesktopFailure(code: "unsupported") }
        var typed = ""
        let slices = textSlices(value)
        for part in slices {
            guard !isCancelled(), focused(ref) else { throw DesktopFailure(code: "focus") }
            typed += part
            result = AXUIElementSetAttributeValue(item.element, kAXValueAttribute as CFString, typed as CFString)
            guard result == .success else { throw DesktopFailure(code: "failed") }
            if !NSWorkspace.shared.accessibilityDisplayShouldReduceMotion { Thread.sleep(forTimeInterval: 0.008) }
        }
        if slices.isEmpty { result = AXUIElementSetAttributeValue(item.element, kAXValueAttribute as CFString, "" as CFString) }
    default: throw DesktopFailure(code: "unsupported")
    }
    guard result == .success else { throw DesktopFailure(code: "failed") }
    invalidateElements()
    frames.removeAll()
    return ["performed": operation, "verified": false]
}


func bounds(_ ref: WindowRef) throws -> CGRect {
    if let number = ref.number {
        let info = CGWindowListCopyWindowInfo(.optionIncludingWindow, number) as? [[String: Any]] ?? []
        guard let entry = info.first, entry[kCGWindowOwnerPID as String] as? Int == Int(ref.pid), let raw = entry[kCGWindowBounds as String] as? NSDictionary, let box = CGRect(dictionaryRepresentation: raw), box.width > 0, box.height > 0 else { throw DesktopFailure(code: "stale") }
        return box
    }
    guard let element = ref.element, let position = attribute(element, kAXPositionAttribute), let size = attribute(element, kAXSizeAttribute) else { throw DesktopFailure(code: "stale") }
    var point = CGPoint.zero
    var dimensions = CGSize.zero
    guard CFGetTypeID(position) == AXValueGetTypeID(), CFGetTypeID(size) == AXValueGetTypeID(), AXValueGetValue(position as! AXValue, .cgPoint, &point), AXValueGetValue(size as! AXValue, .cgSize, &dimensions), dimensions.width > 0, dimensions.height > 0 else { throw DesktopFailure(code: "stale") }
    return CGRect(origin: point, size: dimensions)
}

func screenshot(_ request: [String: Any]) throws -> [String: Any] {
    guard CGPreflightScreenCaptureAccess() else { throw DesktopFailure(code: "screen_permission") }
    let id = request["windowId"] as? String ?? ""
    let ref = try window(id)
    let rect = try bounds(ref)
    let info = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] ?? []
    guard let item = info.first(where: { entry in
        guard entry[kCGWindowOwnerPID as String] as? Int == Int(ref.pid), let raw = entry[kCGWindowBounds as String] as? NSDictionary, let box = CGRect(dictionaryRepresentation: raw) else { return false }
        return abs(box.origin.x - rect.origin.x) < 2 && abs(box.origin.y - rect.origin.y) < 2 && abs(box.width - rect.width) < 2 && abs(box.height - rect.height) < 2
    }), let number = item[kCGWindowNumber as String] as? UInt32,
    let image = CGWindowListCreateImage(.null, .optionIncludingWindow, number, [.boundsIgnoreFraming, .bestResolution]) else { throw DesktopFailure(code: "stale") }
    let factor = min(1.0, 1280.0 / Double(image.width))
    let width = max(1, Int(Double(image.width) * factor))
    let height = max(1, Int(Double(image.height) * factor))
    guard let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { throw DesktopFailure(code: "failed") }
    context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
    let data = NSMutableData()
    guard let resized = context.makeImage(), let destination = CGImageDestinationCreateWithData(data, UTType.png.identifier as CFString, 1, nil) else { throw DesktopFailure(code: "failed") }
    CGImageDestinationAddImage(destination, resized, nil)
    guard CGImageDestinationFinalize(destination), data.length < 4000000 else { throw DesktopFailure(code: "failed") }
    let frameId = rememberFrame(id, bounds: rect, visual: true)
    return ["frameId": frameId, "windowId": id, "width": width, "height": height, "coordinateWidth": rect.width, "coordinateHeight": rect.height, "images": [["data": data.base64EncodedString(), "mimeType": "image/png"]]]
}

let keyCodes: [String: CGKeyCode] = ["A": 0, "S": 1, "D": 2, "F": 3, "H": 4, "G": 5, "Z": 6, "X": 7, "C": 8, "V": 9, "B": 11, "Q": 12, "W": 13, "E": 14, "R": 15, "Y": 16, "T": 17, "1": 18, "2": 19, "3": 20, "4": 21, "6": 22, "5": 23, "9": 25, "7": 26, "8": 28, "0": 29, "O": 31, "U": 32, "I": 34, "P": 35, "L": 37, "J": 38, "K": 40, "N": 45, "M": 46, "ENTER": 36, "TAB": 48, "SPACE": 49, "BACKSPACE": 51, "ESCAPE": 53, "META": 55, "SHIFT": 56, "ALT": 58, "CTRL": 59, "LEFT": 123, "RIGHT": 124, "DOWN": 125, "UP": 126]

func keyEvent(_ code: CGKeyCode, _ down: Bool) {
    guard let event = CGEvent(keyboardEventSource: nil, virtualKey: code, keyDown: down) else { return }
    event.flags = desktopModifierFlags(heldKeys)
    if desktopModifierCodes[code] != nil { event.type = .flagsChanged }
    event.post(tap: .cghidEventTap)
}
func mouseEvent(_ button: Int, _ down: Bool, _ point: CGPoint) {
    let type: CGEventType = button == 0 ? (down ? .leftMouseDown : .leftMouseUp) : button == 1 ? (down ? .rightMouseDown : .rightMouseUp) : (down ? .otherMouseDown : .otherMouseUp)
    CGEvent(mouseEventSource: nil, mouseType: type, mouseCursorPosition: point, mouseButton: CGMouseButton(rawValue: UInt32(button))!)?.post(tap: .cghidEventTap)
}
func releaseInput() {
    let codes = heldKeys.sorted { left, right in
        let leftModifier = desktopModifierCodes[left] != nil
        let rightModifier = desktopModifierCodes[right] != nil
        return leftModifier == rightModifier ? left < right : !leftModifier
    }
    for code in codes { heldKeys.remove(code); keyEvent(code, false) }
    let point = CGEvent(source: nil)?.location ?? .zero
    for button in heldButtons { mouseEvent(button, false, point) }
    heldKeys.removeAll()
    heldButtons.removeAll()
}
func checkedFrame(_ request: [String: Any]) throws -> (WindowRef, FrameRef) {
    let ref = try window(request["windowId"] as? String ?? "")
    guard let frame = frames[request["frameId"] as? String ?? ""], frame.window == request["windowId"] as? String, Date().timeIntervalSince(frame.created) < 15, try bounds(ref) == frame.bounds else { throw DesktopFailure(code: "stale") }
    guard NSWorkspace.shared.frontmostApplication?.processIdentifier == ref.pid else { throw DesktopFailure(code: "focus") }
    let root = AXUIElementCreateApplication(ref.pid)
    guard focused(ref) else { throw DesktopFailure(code: "focus") }
    if let focusedElement = attribute(root, kAXFocusedUIElementAttribute), CFGetTypeID(focusedElement) == AXUIElementGetTypeID(), secure(focusedElement as! AXUIElement) { throw DesktopFailure(code: "secure") }
    return (ref, frame)
}
func focused(_ ref: WindowRef) -> Bool {
    let front = NSWorkspace.shared.frontmostApplication?.processIdentifier
    guard front == ref.pid else { return false }
    if let number = ref.number, ref.element == nil {
        let graphics = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] ?? []
        return graphics.first { $0[kCGWindowOwnerPID as String] as? Int == Int(ref.pid) && $0[kCGWindowLayer as String] as? Int == 0 }?[kCGWindowNumber as String] as? CGWindowID == number
    }
    let root = AXUIElementCreateApplication(ref.pid)
    guard let current = attribute(root, kAXFocusedWindowAttribute) else { return false }
    return ref.element.map { CFEqual(current, $0) } ?? false
}
func input(_ request: [String: Any]) throws -> [String: Any] {
    let (ref, frame) = try checkedFrame(request)
    let operation = request["operation"] as? String ?? ""
    guard frame.visual || ["keys", "text"].contains(operation) && (request["dx"] as? Int ?? 0) == 0 && (request["dy"] as? Int ?? 0) == 0 else { throw DesktopFailure(code: "visual_frame_required") }
    let duration = request["durationMs"] as? Int ?? 0
    guard duration >= 0 && duration <= 5000 else { throw DesktopFailure(code: "unsupported") }
    guard !isCancelled() else { throw DesktopFailure(code: "cancelled") }
    defer { releaseInput(); frames.removeAll(); invalidateElements() }
    if operation == "move" || operation == "click" {
        guard let x = request["x"] as? Double, let y = request["y"] as? Double, x.isFinite, y.isFinite, x >= 0, y >= 0, x < frame.bounds.width, y < frame.bounds.height else { throw DesktopFailure(code: "unsupported") }
        let point = CGPoint(x: frame.bounds.minX + x, y: frame.bounds.minY + y)
        try feedback(point, ref, click: operation == "click")
        CGEvent(mouseEventSource: nil, mouseType: .mouseMoved, mouseCursorPosition: point, mouseButton: .left)?.post(tap: .cghidEventTap)
        if operation == "click" {
            let button = request["button"] as? String ?? "left"
            guard ["left", "right", "middle"].contains(button) else { throw DesktopFailure(code: "unsupported") }
            let code = button == "left" ? 0 : button == "right" ? 1 : 2
            heldButtons.insert(code)
            mouseEvent(code, true, point)
        }
    } else if operation == "scroll" {
        let dx = request["dx"] as? Int ?? 0
        let dy = request["dy"] as? Int ?? 0
        guard abs(dx) <= 2000 && abs(dy) <= 2000, dx != 0 || dy != 0 else { throw DesktopFailure(code: "unsupported") }
        let point = CGPoint(x: frame.bounds.midX, y: frame.bounds.midY)
        try feedback(point, ref)
        CGEvent(mouseEventSource: nil, mouseType: .mouseMoved, mouseCursorPosition: point, mouseButton: .left)?.post(tap: .cghidEventTap)
        let steps = NSWorkspace.shared.accessibilityDisplayShouldReduceMotion ? 1 : 24
        var movedX = 0
        var movedY = 0
        for step in 1...steps {
            guard !isCancelled(), focused(ref), try bounds(ref) == frame.bounds else { throw DesktopFailure(code: "focus") }
            let progress = Double(step) / Double(steps)
            let eased = 1 - pow(1 - progress, 3)
            let nextX = Int((Double(dx) * eased).rounded())
            let nextY = Int((Double(dy) * eased).rounded())
            CGEvent(scrollWheelEvent2Source: nil, units: .pixel, wheelCount: 2, wheel1: Int32(movedY - nextY), wheel2: Int32(movedX - nextX), wheel3: 0)?.post(tap: .cghidEventTap)
            movedX = nextX; movedY = nextY
            if steps > 1 { Thread.sleep(forTimeInterval: 0.012) }
        }
    } else if operation == "relative_move" {
        let dx = request["dx"] as? Int ?? 0
        let dy = request["dy"] as? Int ?? 0
        guard abs(dx) <= 2000 && abs(dy) <= 2000 else { throw DesktopFailure(code: "unsupported") }
        let current = CGEvent(source: nil)?.location ?? frame.bounds.origin
        let point = CGPoint(x: min(max(current.x + Double(dx), frame.bounds.minX), frame.bounds.maxX - 1), y: min(max(current.y + Double(dy), frame.bounds.minY), frame.bounds.maxY - 1))
        try feedback(point, ref)
        guard let event = CGEvent(mouseEventSource: nil, mouseType: .mouseMoved, mouseCursorPosition: point, mouseButton: .left) else { throw DesktopFailure(code: "failed") }
        event.setIntegerValueField(.mouseEventDeltaX, value: Int64(dx))
        event.setIntegerValueField(.mouseEventDeltaY, value: Int64(dy))
        event.post(tap: .cghidEventTap)
    } else if operation == "keys" {
        guard let keys = request["keys"] as? [String], !keys.isEmpty, keys.count <= 6 else { throw DesktopFailure(code: "unsupported") }
        guard !desktopModifierCodes.keys.contains(where: { CGEventSource.keyState(.combinedSessionState, key: $0) }) else { throw DesktopFailure(code: "intervention") }
        let codes = try keys.map { key -> CGKeyCode in
            guard let code = keyCodes[key.uppercased()] else { throw DesktopFailure(code: "unsupported") }
            guard !CGEventSource.keyState(.combinedSessionState, key: code) else { throw DesktopFailure(code: "intervention") }
            return code
        }
        for code in codes { heldKeys.insert(code); keyEvent(code, true) }
    } else if operation == "text" {
        guard let text = request["value"] as? String, text.utf16.count <= 8000 else { throw DesktopFailure(code: "unsupported") }
        for part in textSlices(text) {
            guard !isCancelled(), focused(ref) else { throw DesktopFailure(code: "focus") }
            let values = Array(part.utf16)
            for down in [true, false] {
                guard let event = CGEvent(keyboardEventSource: nil, virtualKey: 0, keyDown: down) else { throw DesktopFailure(code: "failed") }
                values.withUnsafeBufferPointer { buffer in event.keyboardSetUnicodeString(stringLength: buffer.count, unicodeString: buffer.baseAddress) }
                event.post(tap: .cghidEventTap)
            }
            if !NSWorkspace.shared.accessibilityDisplayShouldReduceMotion { Thread.sleep(forTimeInterval: 0.008) }
        }
    } else { throw DesktopFailure(code: "unsupported") }
    let cameraX = operation == "keys" ? request["dx"] as? Int ?? 0 : 0
    let cameraY = operation == "keys" ? request["dy"] as? Int ?? 0 : 0
    guard abs(cameraX) <= 2000 && abs(cameraY) <= 2000 else { throw DesktopFailure(code: "unsupported") }
    let started = Date()
    let deadline = started.addingTimeInterval(Double(duration) / 1000)
    var movedX = 0
    var movedY = 0
    while Date() < deadline {
        guard !isCancelled() else { throw DesktopFailure(code: "cancelled") }
        guard focused(ref), try bounds(ref) == frame.bounds else { throw DesktopFailure(code: "focus") }
        if cameraX != 0 || cameraY != 0 {
            let progress = min(1.0, Date().timeIntervalSince(started) / max(0.001, Double(duration) / 1000))
            let nextX = Int(Double(cameraX) * progress)
            let nextY = Int(Double(cameraY) * progress)
            let current = CGEvent(source: nil)?.location ?? frame.bounds.origin
            let point = CGPoint(x: min(max(current.x + Double(nextX - movedX), frame.bounds.minX), frame.bounds.maxX - 1), y: min(max(current.y + Double(nextY - movedY), frame.bounds.minY), frame.bounds.maxY - 1))
            if let move = CGEvent(mouseEventSource: nil, mouseType: .mouseMoved, mouseCursorPosition: point, mouseButton: .left) {
                move.setIntegerValueField(.mouseEventDeltaX, value: Int64(nextX - movedX))
                move.setIntegerValueField(.mouseEventDeltaY, value: Int64(nextY - movedY))
                move.post(tap: .cghidEventTap)
            }
            movedX = nextX; movedY = nextY
        }
        Thread.sleep(forTimeInterval: 0.01)
    }
    return ["performed": operation, "released": true, "verified": false]
}

func execute(_ request: [String: Any]) throws -> [String: Any] {
    guard request["version"] as? Int == 1 else { throw DesktopFailure(code: "unsupported") }
    let action = request["action"] as? String ?? ""
    if action == "status" {
        let locked = sessionLocked()
        var status: [String: Any] = ["appVersion": appVersion, "platform": "darwin", "accessibility": AXIsProcessTrusted(), "actions": ["list_windows", "focus_window", "take_snapshot", "read", "act", "take_screenshot", "input", "launch_app", "stop"], "screenshots": CGPreflightScreenCaptureAccess(), "input": AXIsProcessTrusted() && !locked, "sessionLocked": locked, "audio": false]
        if let url = NSWorkspace.shared.urlForApplication(toOpen: URL(string: "https://example.com")!), let identifier = Bundle(url: url)?.bundleIdentifier { status["defaultBrowser"] = identifier }
        return status
    }
    if action == "stop" { releaseInput(); frames.removeAll(); DesktopFeedback.shared.stop(); return ["stopped": true] }
    if ["input", "act", "focus_window"].contains(action) && sessionLocked() { throw DesktopFailure(code: "session_locked") }
    guard AXIsProcessTrusted() else { throw DesktopFailure(code: "permission") }
    switch action {
    case "window_info":
        let ref = try window(request["windowId"] as? String ?? "")
        return windowMetadata(ref)
    case "take_screenshot": return try screenshot(request)
    case "input": return try input(request)
    case "launch_app":
        guard let identifier = request["application"] as? String, !blockedApps.contains(identifier), let url = NSWorkspace.shared.urlForApplication(withBundleIdentifier: identifier) else { throw DesktopFailure(code: "unsupported") }
        let configuration = NSWorkspace.OpenConfiguration()
        configuration.activates = true
        NSWorkspace.shared.openApplication(at: url, configuration: configuration)
        return ["requested": true]
    case "list_windows": return listWindows(request["application"] as? String)
    case "focus_window":
        let ref = try window(request["windowId"] as? String ?? "")
        guard let app = NSRunningApplication(processIdentifier: ref.pid), DispatchQueue.main.sync(execute: { app.activate(options: [.activateIgnoringOtherApps]) }) else { throw DesktopFailure(code: "failed") }
        if let element = ref.element, !focused(ref) { AXUIElementPerformAction(element, kAXRaiseAction as CFString) }
        let deadline = Date().addingTimeInterval(0.6)
        while !focused(ref) && Date() < deadline { Thread.sleep(forTimeInterval: 0.01) }
        guard focused(ref) else { throw DesktopFailure(code: "focus") }
        elements.removeAll()
        return ["focused": true]
    case "take_snapshot": return try snapshot(request["windowId"] as? String ?? "")
    case "act": return try act(request)
    case "read":
        let (_, item) = try target(request)
        let value = string(item.element, kAXValueAttribute)
        let text = value.isEmpty ? item.name : value
        let offset = min(max(request["offset"] as? Int ?? 0, 0), text.count)
        let count = min(max(request["limit"] as? Int ?? 100, 1), 100) * 40
        return ["value": String(text.dropFirst(offset).prefix(count)), "offset": offset, "truncated": text.count > offset + count]
    default: throw DesktopFailure(code: "unsupported")
    }
}

var inputStat = stat()
if let icon = NSImage(contentsOf: Bundle.main.bundleURL.appendingPathComponent("Contents/Resources/arara-desktop.icns")) {
    NSApplication.shared.applicationIconImage = icon
}
let hasInputPipe = fstat(STDIN_FILENO, &inputStat) == 0 && (inputStat.st_mode & S_IFMT == S_IFIFO || inputStat.st_mode & S_IFMT == S_IFSOCK || inputStat.st_mode & S_IFMT == S_IFREG)
if CommandLine.arguments.contains("--permissions") || !hasInputPipe && isatty(STDIN_FILENO) == 0 {
    let locale = Locale.preferredLanguages.first ?? "en"
    let title = "Arara Desktop"
    let language = locale.hasPrefix("pt") ? "pt" : locale.hasPrefix("es") ? "es" : "en"
    let resources = Bundle.main.bundleURL.appendingPathComponent("Contents/Resources/locales")
    let data = try! Data(contentsOf: resources.appendingPathComponent("\(language).json"))
    let catalog = try! JSONSerialization.jsonObject(with: data) as! [String: String]
    let message = catalog["setup.detail"]!
    let alert = NSAlert()
    alert.messageText = title
    alert.informativeText = message
    alert.addButton(withTitle: catalog["setup.continue"]!)
    alert.addButton(withTitle: catalog["setup.cancel"]!)
    NSApplication.shared.activate(ignoringOtherApps: true)
    if alert.runModal() == .alertFirstButtonReturn {
        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: Bundle.main.bundlePath)])
        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility")!)
    }
    exit(0)
}

signal(SIGTERM, SIG_IGN)
signal(SIGINT, SIG_IGN)
let terminationQueue = DispatchQueue(label: "desktop.signals")
let term = DispatchSource.makeSignalSource(signal: SIGTERM, queue: terminationQueue)
term.setEventHandler { cancelInput() }
term.resume()
let interrupt = DispatchSource.makeSignalSource(signal: SIGINT, queue: terminationQueue)
interrupt.setEventHandler { cancelInput() }
interrupt.resume()
let requests = OperationQueue()
requests.maxConcurrentOperationCount = 1

_ = NSApplication.shared
_ = NSWorkspace.shared
let feedbackFocus = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { _ in DesktopFeedback.shared.checkFocus() }
DispatchQueue.global(qos: .userInitiated).async {
while let line = readLine() {
    guard let data = line.data(using: .utf8), let request = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { continue }
    if request["action"] as? String == "stop" { cancelInput() }
    cancellation.lock()
    let generation = cancellationGeneration
    cancellation.unlock()
    requests.addOperation {
      autoreleasepool {
        var reply: [String: Any]
        do {
            guard line.utf8.count <= 64000, let data = line.data(using: .utf8), let request = try JSONSerialization.jsonObject(with: data) as? [String: Any] else { throw DesktopFailure(code: "unsupported") }
            cancellation.lock()
            let currentGeneration = cancellationGeneration
            cancellation.unlock()
            if currentGeneration != generation && !["status", "stop"].contains(request["action"] as? String ?? "") { throw DesktopFailure(code: "cancelled") }
            reply = ["version": 1, "result": try execute(request)]
        } catch let error as DesktopFailure {
            reply = ["version": 1, "error": error.code]
        } catch {
            reply = ["version": 1, "error": "failed"]
        }
        if let data = try? JSONSerialization.data(withJSONObject: reply, options: [.sortedKeys]), let output = String(data: data, encoding: .utf8) { print(output); fflush(stdout) }
      }
    }
}
cancelInput()
requests.waitUntilAllOperationsAreFinished()
releaseInput()
DispatchQueue.main.async { exit(0) }
}
RunLoop.main.run()
