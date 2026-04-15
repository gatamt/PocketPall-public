import Foundation

/// L2CAP CoC frame protocol with 8-byte header.
///
/// Frame layout: [Type:1][Flags:1][SeqNum:2 LE][Length:4 LE][Payload:N]
struct L2CAPProtocol {
    // Frame types
    static let frameAudioMic: UInt8     = 0x01  // ESP32→iPhone: 16kHz PCM
    static let frameAudioSpk: UInt8     = 0x02  // iPhone→ESP32: ADPCM
    static let frameCmdJSON: UInt8      = 0x03  // Bidirectional: JSON
    static let frameDisplayCmd: UInt8   = 0x04  // iPhone→ESP32: display commands
    static let frameInputEvent: UInt8   = 0x05  // ESP32→iPhone: gesture/button/rotation
    static let frameImageData: UInt8    = 0x06  // iPhone→ESP32: RGB565 image
    static let frameStatus: UInt8       = 0x07  // Bidirectional: status
    static let framePing: UInt8         = 0x08  // Keep-alive
    static let framePong: UInt8         = 0x09  // Keep-alive response
    static let frameVideoJPEG: UInt8   = 0x0A  // iPhone→ESP32: JPEG video frames

    // Video frame flags
    static let videoFlagSOF: UInt8      = 0x01  // Start of JPEG frame
    static let videoFlagEOF: UInt8      = 0x02  // End of JPEG frame

    static let headerSize = 8

    // Display command IDs
    static let displaySetState: UInt8    = 0x01
    static let displaySetText: UInt8     = 0x02
    static let displayClearText: UInt8   = 0x03
    static let displaySetSoundbar: UInt8 = 0x04
    static let displayShowError: UInt8   = 0x05
    static let displaySetBattery: UInt8  = 0x06
    static let displayShowVolume: UInt8  = 0x07
    static let displayImageStart: UInt8  = 0x08
    static let displayImageDone: UInt8   = 0x09

    // Input event types
    static let inputGesture: UInt8   = 0x01
    static let inputButton: UInt8    = 0x05
    static let inputRotation: UInt8  = 0x06
    static let inputBattery: UInt8   = 0x07

    // Sequence counter
    private static var txSeq: UInt16 = 0

    /// Encode a frame with the 8-byte header.
    static func encode(type: UInt8, payload: Data, flags: UInt8 = 0) -> Data {
        let seq = txSeq
        txSeq &+= 1

        var frame = Data(capacity: headerSize + payload.count)
        frame.append(type)
        frame.append(flags)
        frame.append(UInt8(seq & 0xFF))
        frame.append(UInt8((seq >> 8) & 0xFF))
        let length = UInt32(payload.count)
        frame.append(UInt8(length & 0xFF))
        frame.append(UInt8((length >> 8) & 0xFF))
        frame.append(UInt8((length >> 16) & 0xFF))
        frame.append(UInt8((length >> 24) & 0xFF))
        frame.append(payload)
        return frame
    }

    /// Decode a frame. Returns (type, flags, seq, payload) or nil.
    static func decode(_ data: Data) -> (type: UInt8, flags: UInt8, seq: UInt16, payload: Data)? {
        guard data.count >= headerSize else { return nil }

        let type = data[data.startIndex]
        let flags = data[data.startIndex + 1]
        let seq = UInt16(data[data.startIndex + 2]) | (UInt16(data[data.startIndex + 3]) << 8)
        let length = UInt32(data[data.startIndex + 4]) |
                     (UInt32(data[data.startIndex + 5]) << 8) |
                     (UInt32(data[data.startIndex + 6]) << 16) |
                     (UInt32(data[data.startIndex + 7]) << 24)

        let expectedEnd = headerSize + Int(length)
        guard data.count >= expectedEnd else { return nil }

        let payload = data.subdata(in: (data.startIndex + headerSize)..<(data.startIndex + expectedEnd))
        return (type, flags, seq, payload)
    }

    /// Encode a JSON command frame
    static func encodeCommand(_ json: [String: Any]) -> Data? {
        guard let jsonData = try? JSONSerialization.data(withJSONObject: json) else { return nil }
        return encode(type: frameCmdJSON, payload: jsonData)
    }

    /// Encode ADPCM audio for sending to device
    static func encodeAudioSpk(_ adpcmData: Data) -> Data {
        return encode(type: frameAudioSpk, payload: adpcmData)
    }

    /// Encode image data for sending to device
    static func encodeImageData(_ data: Data) -> Data {
        return encode(type: frameImageData, payload: data)
    }

    /// Encode a display command frame
    static func encodeDisplayCmd(_ cmdData: Data) -> Data {
        return encode(type: frameDisplayCmd, payload: cmdData)
    }

    /// Encode a video JPEG fragment with SOF/EOF flags
    static func encodeVideoJPEG(_ data: Data, flags: UInt8) -> Data {
        return encode(type: frameVideoJPEG, payload: data, flags: flags)
    }

    /// Encode a ping frame
    static func encodePing() -> Data {
        return encode(type: framePing, payload: Data())
    }

    /// Encode a pong frame (echo the received seq)
    static func encodePong(seq: UInt16) -> Data {
        var frame = Data(capacity: headerSize)
        frame.append(framePong)
        frame.append(0) // flags
        frame.append(UInt8(seq & 0xFF))
        frame.append(UInt8((seq >> 8) & 0xFF))
        // length = 0
        frame.append(contentsOf: [0, 0, 0, 0])
        return frame
    }

    /// Reset sequence counter (e.g., on new connection)
    static func resetSequence() {
        txSeq = 0
    }
}
