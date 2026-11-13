import CoreGraphics

let desktopModifierCodes: [CGKeyCode: CGEventFlags] = [55: .maskCommand, 56: .maskShift, 58: .maskAlternate, 59: .maskControl]

func desktopModifierFlags(_ keys: Set<CGKeyCode>) -> CGEventFlags {
    keys.reduce(into: CGEventFlags()) { flags, key in flags.formUnion(desktopModifierCodes[key] ?? []) }
}
