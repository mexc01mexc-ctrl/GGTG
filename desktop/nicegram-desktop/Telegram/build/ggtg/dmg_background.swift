// Draws the GGTG.dmg window background (660x420 pt, rendered at 2x):
//   swift dmg_background.swift out.png
import AppKit

let w: CGFloat = 660, h: CGFloat = 420, scale: CGFloat = 2
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(w * scale), pixelsHigh: Int(h * scale),
    bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
    colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
rep.size = NSSize(width: w, height: h)
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
let ctx = NSGraphicsContext.current!.cgContext

func rgb(_ hex: Int, _ a: CGFloat = 1) -> NSColor {
    NSColor(srgbRed: CGFloat((hex >> 16) & 255) / 255, green: CGFloat((hex >> 8) & 255) / 255,
            blue: CGFloat(hex & 255) / 255, alpha: a)
}

// background: light gradient (Finder draws icon labels dark in light mode) with a soft glow behind the icons
NSGradient(colors: [rgb(0xf7f9fc), rgb(0xe3e9f2)])!.draw(in: NSRect(x: 0, y: 0, width: w, height: h), angle: -90)
let glow = NSGradient(colors: [rgb(0x3d8bfd, 0.10), rgb(0x3d8bfd, 0)])!
glow.draw(fromCenter: NSPoint(x: w / 2, y: 190), radius: 0, toCenter: NSPoint(x: w / 2, y: 190), radius: 300, options: [])

// title
func text(_ s: String, size: CGFloat, weight: NSFont.Weight, color: NSColor, y: CGFloat, kern: CGFloat = 0) {
    let style = NSMutableParagraphStyle(); style.alignment = .center
    let attrs: [NSAttributedString.Key: Any] = [.font: NSFont.systemFont(ofSize: size, weight: weight),
        .foregroundColor: color, .paragraphStyle: style, .kern: kern]
    NSAttributedString(string: s, attributes: attrs).draw(in: NSRect(x: 0, y: y, width: w, height: size * 1.5))
}
text("GGTG", size: 34, weight: .heavy, color: rgb(0x14213d), y: h - 78, kern: 4)
text("Перетащите GGTG в папку «Программы»", size: 14, weight: .medium, color: rgb(0x56657a), y: h - 108)

// arrow between the two icons (icons are centred at x 170 / 490, y 200 from the top)
let ay = h - 200
ctx.setLineCap(.round)
ctx.setLineWidth(4)
ctx.setStrokeColor(rgb(0x2f80ed).cgColor)
ctx.setLineDash(phase: 0, lengths: [2, 11])
ctx.move(to: CGPoint(x: 262, y: ay)); ctx.addLine(to: CGPoint(x: 386, y: ay)); ctx.strokePath()
ctx.setLineDash(phase: 0, lengths: [])
ctx.setLineJoin(.round)
ctx.move(to: CGPoint(x: 382, y: ay + 13)); ctx.addLine(to: CGPoint(x: 398, y: ay)); ctx.addLine(to: CGPoint(x: 382, y: ay - 13))
ctx.strokePath()

// footer
text("Аккаунты, прокси и рассылки в одном окне", size: 11, weight: .regular, color: rgb(0x8a97a8), y: 22)

NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
