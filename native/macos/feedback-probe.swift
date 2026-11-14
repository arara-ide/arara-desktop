import Foundation
import CoreGraphics

let pid = Int(CommandLine.arguments.dropFirst().first ?? "") ?? -1
let windows = CGWindowListCopyWindowInfo(.optionOnScreenOnly, kCGNullWindowID) as? [[String: Any]] ?? []
let visible = windows.filter {
    ($0[kCGWindowOwnerPID as String] as? NSNumber)?.intValue == pid
        && (($0[kCGWindowLayer as String] as? NSNumber)?.intValue ?? 0) >= 1000
        && (($0[kCGWindowAlpha as String] as? NSNumber)?.doubleValue ?? 0) > 0
}
if CommandLine.arguments.contains("--ink") || CommandLine.arguments.contains("--position") {
    var sumX = 0
    var sumY = 0
    let pixels = visible.reduce(0) { total, window in
        guard let number = window[kCGWindowNumber as String] as? NSNumber,
              let image = CGWindowListCreateImage(.null, .optionIncludingWindow, CGWindowID(number.uint32Value), [.boundsIgnoreFraming, .nominalResolution]) else { return total }
        let width = image.width
        let height = image.height
        var data = [UInt8](repeating: 0, count: width * height * 4)
        return data.withUnsafeMutableBytes { buffer in
            guard let context = CGContext(data: buffer.baseAddress, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return total }
            context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
            var ink = 0
            for offset in stride(from: 0, to: buffer.count, by: 4) {
                if buffer[offset + 3] > 16 && max(buffer[offset], buffer[offset + 1], buffer[offset + 2]) > 16 {
                    ink += 1
                    sumX += (offset / 4) % width
                    sumY += (offset / 4) / width
                }
            }
            return total + ink
        }
    }
    if CommandLine.arguments.contains("--position") { print("\(pixels > 0 ? sumX / pixels : -1),\(pixels > 0 ? sumY / pixels : -1)") }
    else { print(pixels) }
} else {
    print(visible.count)
}
