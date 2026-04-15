import Foundation

/// Display command sub-protocol for iPhone→ESP32 display updates.
/// Sent as payload inside FRAME_DISPLAY_CMD (0x04) frames.
enum DisplayCommand {
    // Command IDs (must match ESP32 ui_manager_handle_display_cmd)
    static let cmdSetState: UInt8     = 0x01
    static let cmdSetText: UInt8      = 0x02
    static let cmdClearText: UInt8    = 0x03
    static let cmdSetSoundbar: UInt8  = 0x04
    static let cmdShowError: UInt8    = 0x05
    static let cmdSetBattery: UInt8   = 0x06
    static let cmdShowVolume: UInt8   = 0x07
    static let cmdImageStart: UInt8   = 0x08
    static let cmdImageDone: UInt8    = 0x09

    // State values (matches app_state_t on ESP32)
    static let stateAdvertising: UInt8  = 1
    static let stateReady: UInt8        = 3
    static let stateListening: UInt8    = 4
    static let stateProcessing: UInt8   = 5
    static let stateResponding: UInt8   = 6

    /// SET_STATE: [0x01][state:1]
    static func setState(_ state: UInt8) -> Data {
        return Data([cmdSetState, state])
    }

    /// SET_TEXT: [0x02][len:2LE][utf8...]
    static func setText(_ text: String) -> Data {
        let utf8 = Array(text.utf8)
        let len = UInt16(utf8.count)
        var data = Data(capacity: 3 + utf8.count)
        data.append(cmdSetText)
        data.append(UInt8(len & 0xFF))
        data.append(UInt8((len >> 8) & 0xFF))
        data.append(contentsOf: utf8)
        return data
    }

    /// CLEAR_TEXT: [0x03]
    static func clearText() -> Data {
        return Data([cmdClearText])
    }

    /// SET_SOUNDBAR: [0x04][energy:2LE]
    static func setSoundbar(energy: UInt16) -> Data {
        return Data([cmdSetSoundbar, UInt8(energy & 0xFF), UInt8((energy >> 8) & 0xFF)])
    }

    /// SHOW_ERROR: [0x05][len:2LE][utf8...]
    static func showError(_ text: String) -> Data {
        let utf8 = Array(text.utf8)
        let len = UInt16(utf8.count)
        var data = Data(capacity: 3 + utf8.count)
        data.append(cmdShowError)
        data.append(UInt8(len & 0xFF))
        data.append(UInt8((len >> 8) & 0xFF))
        data.append(contentsOf: utf8)
        return data
    }

    /// SET_BATTERY: [0x06][percent:1][usb:1][charging:1]
    static func setBattery(percent: UInt8, usb: Bool, charging: Bool) -> Data {
        return Data([cmdSetBattery, percent, usb ? 1 : 0, charging ? 1 : 0])
    }

    /// SHOW_VOLUME: [0x07][percent:1]
    static func showVolume(percent: UInt8) -> Data {
        return Data([cmdShowVolume, percent])
    }

    /// IMAGE_START: [0x08][width:2LE][height:2LE][size:4LE]
    static func imageStart(width: UInt16, height: UInt16, size: UInt32) -> Data {
        var data = Data(capacity: 9)
        data.append(cmdImageStart)
        data.append(UInt8(width & 0xFF))
        data.append(UInt8((width >> 8) & 0xFF))
        data.append(UInt8(height & 0xFF))
        data.append(UInt8((height >> 8) & 0xFF))
        data.append(UInt8(size & 0xFF))
        data.append(UInt8((size >> 8) & 0xFF))
        data.append(UInt8((size >> 16) & 0xFF))
        data.append(UInt8((size >> 24) & 0xFF))
        return data
    }

    /// IMAGE_DONE: [0x09]
    static func imageDone() -> Data {
        return Data([cmdImageDone])
    }
}

/// Input event sub-protocol for ESP32→iPhone events.
/// Received as payload inside FRAME_INPUT_EVENT (0x05) frames.
enum InputEvent {
    static let evtGesture: UInt8   = 0x01
    static let evtButton: UInt8    = 0x05
    static let evtRotation: UInt8  = 0x06
    static let evtBattery: UInt8   = 0x07

    // Gesture IDs
    static let gestureSwipeLeft: UInt8    = 0x01
    static let gestureSwipeRight: UInt8   = 0x02
    static let gestureSwipeUp: UInt8      = 0x03
    static let gestureSwipeDown: UInt8    = 0x04
    static let gestureTap: UInt8          = 0x05
    static let gestureDoubleTap: UInt8    = 0x06

    struct ParsedEvent {
        let type: UInt8
        let data: Data
    }

    /// Parse an input event from raw payload
    static func parse(_ data: Data) -> ParsedEvent? {
        guard !data.isEmpty else { return nil }
        return ParsedEvent(type: data[data.startIndex], data: data)
    }
}
