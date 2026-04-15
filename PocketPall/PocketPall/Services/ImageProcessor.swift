import UIKit

class ImageProcessor {
    static let displayWidth = 368
    static let displayHeight = 448

    /// Scale image to fit display dimensions while preserving aspect ratio, then convert to RGB565
    static func processForDevice(_ image: UIImage) -> Data? {
        guard let scaled = scaleToFit(image, width: displayWidth, height: displayHeight),
              let cgImage = scaled.cgImage else { return nil }
        return convertToRGB565(cgImage)
    }

    /// Scale image to fit within bounds, preserving aspect ratio
    static func scaleToFit(_ image: UIImage, width: Int, height: Int) -> UIImage? {
        let targetSize = CGSize(width: width, height: height)
        let imageSize = image.size

        let widthRatio = targetSize.width / imageSize.width
        let heightRatio = targetSize.height / imageSize.height
        let scale = min(widthRatio, heightRatio)

        let newSize = CGSize(
            width: imageSize.width * scale,
            height: imageSize.height * scale
        )

        let format = UIGraphicsImageRendererFormat()
        format.scale = 1.0
        let renderer = UIGraphicsImageRenderer(size: targetSize, format: format)
        return renderer.image { context in
            // Fill with black background
            UIColor.black.setFill()
            context.fill(CGRect(origin: .zero, size: targetSize))

            // Center the scaled image
            let origin = CGPoint(
                x: (targetSize.width - newSize.width) / 2,
                y: (targetSize.height - newSize.height) / 2
            )
            image.draw(in: CGRect(origin: origin, size: newSize))
        }
    }

    /// Convert CGImage to RGB565 data (big-endian, matching ESP32 LVGL LV_COLOR_16_SWAP=1)
    static func convertToRGB565(_ cgImage: CGImage) -> Data? {
        let width = cgImage.width
        let height = cgImage.height

        guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB),
              let context = CGContext(
                data: nil,
                width: width,
                height: height,
                bitsPerComponent: 8,
                bytesPerRow: width * 4,
                space: colorSpace,
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
              ) else { return nil }

        context.draw(cgImage, in: CGRect(x: 0, y: 0, width: width, height: height))

        guard let pixels = context.data else { return nil }
        let pixelBuffer = pixels.bindMemory(to: UInt8.self, capacity: width * height * 4)

        var rgb565Data = Data(capacity: width * height * 2)

        for i in 0..<(width * height) {
            let offset = i * 4
            let r = pixelBuffer[offset]
            let g = pixelBuffer[offset + 1]
            let b = pixelBuffer[offset + 2]

            let r5 = UInt16(r >> 3) & 0x1F
            let g6 = UInt16(g >> 2) & 0x3F
            let b5 = UInt16(b >> 3) & 0x1F
            let rgb565 = (r5 << 11) | (g6 << 5) | b5

            // Big-endian (matches ESP32 LVGL with LV_COLOR_16_SWAP=1)
            rgb565Data.append(UInt8((rgb565 >> 8) & 0xFF))
            rgb565Data.append(UInt8(rgb565 & 0xFF))
        }

        return rgb565Data
    }

    /// Total size of RGB565 image data for the display
    static var displayImageSize: Int {
        return displayWidth * displayHeight * 2  // 329,728 bytes
    }
}
