import Foundation
import Combine
import UIKit

protocol DeviceTransport: AnyObject {
    var onAudioReceived: ((Data) -> Void)? { get set }
    var onCommandReceived: (([String: Any]) -> Void)? { get set }
    var hasPendingAudioWrites: Bool { get }
    var hasPendingImageWrites: Bool { get }

    func sendAudio(_ data: Data)
    func sendCommand(_ json: [String: Any])
    func sendImageData(_ data: Data)
    func sendDisplayCommand(_ data: Data)
}

// MARK: - IMA-ADPCM Encoder (4:1 compression for BLE audio)

/// Encodes 16-bit PCM to 4-bit IMA-ADPCM, reducing BLE bandwidth from 48 KB/s to 12 KB/s.
struct ADPCMEncoder {
    private var predicted: Int32 = 0
    private var stepIndex: Int32 = 0

    private static let indexTable: [Int32] = [
        -1, -1, -1, -1, 2, 4, 6, 8,
        -1, -1, -1, -1, 2, 4, 6, 8
    ]

    private static let stepTable: [Int32] = [
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
        19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
        130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
        337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
        2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
        5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
    ]

    /// Encode PCM data (16-bit LE samples) to ADPCM (4-bit packed).
    mutating func encode(_ pcmData: Data) -> Data {
        let sampleCount = pcmData.count / 2
        guard sampleCount > 0 else { return Data() }
        let outBytes = (sampleCount + 1) / 2
        var output = Data(count: outBytes)

        pcmData.withUnsafeBytes { raw in
            let samples = raw.bindMemory(to: Int16.self)
            output.withUnsafeMutableBytes { outRaw in
                let out = outRaw.bindMemory(to: UInt8.self)
                for i in 0..<sampleCount {
                    let nibble = encodeSample(Int32(samples[i]))
                    if i & 1 == 0 {
                        out[i >> 1] = nibble
                    } else {
                        out[i >> 1] |= nibble << 4
                    }
                }
            }
        }
        return output
    }

    mutating func reset() {
        predicted = 0
        stepIndex = 0
    }

    private mutating func encodeSample(_ sample: Int32) -> UInt8 {
        var diff = sample - predicted
        var code: UInt8 = 0
        let step = Self.stepTable[Int(stepIndex)]

        if diff < 0 { code = 8; diff = -diff }
        if diff >= step       { code |= 4; diff -= step }
        if diff >= step >> 1  { code |= 2; diff -= step >> 1 }
        if diff >= step >> 2  { code |= 1 }

        // Reconstruct (must match decoder exactly)
        var diffq = step >> 3
        if code & 4 != 0 { diffq += step }
        if code & 2 != 0 { diffq += step >> 1 }
        if code & 1 != 0 { diffq += step >> 2 }
        if code & 8 != 0 { diffq = -diffq }

        predicted = max(-32768, min(32767, predicted + diffq))
        stepIndex = max(0, min(88, stepIndex + Self.indexTable[Int(code & 0x0F)]))
        return code & 0x0F
    }
}

// MARK: - AudioBridge

/// Bridges audio between device transport (WiFi TCP) and AI providers.
class AudioBridge: ObservableObject {
    private weak var transport: (any DeviceTransport)?
    private var currentProvider: AIProvider?
    private var hasProviderError = false
    private var bridgeActive = false
    private var lastForwardedAIState: AIState?
    private let stateQueue = DispatchQueue(label: "AudioBridge.state")
    private let aiAudioQueue = DispatchQueue(label: "AudioBridge.aiAudio")
    private var aiAudioPending = Data()
    private var aiAudioTimer: DispatchSourceTimer?
    private let aiAudioChunkBytes = 1920 // 40ms PCM @ 24kHz → 480 ADPCM bytes = 1 BLE packet
    private let aiAudioTickMs: UInt64 = 20
    private let aiAudioMaxBacklogBytes = 480_000 // ~10 seconds at 24kHz mono
    private var pendingReadyForDevice = false
    private var adpcmEncoder = ADPCMEncoder()

    @Published var isActive = false

    init() {}

    private var audioToAICount = 0
    private var audioFromAICount = 0
    private var lastBridgeLogTime = Date()
    private var deviceActivityOpen = false
    private var lastUserStateValue: String?
    private var suppressedDuplicateUserStateCount = 0

    func configure(transport: any DeviceTransport, provider: AIProvider) {
        self.transport = transport
        self.currentProvider = provider
        self.lastForwardedAIState = nil
        setState(active: false, providerError: false)
        stateQueue.sync {
            self.deviceActivityOpen = false
            self.lastUserStateValue = nil
            self.suppressedDuplicateUserStateCount = 0
        }
        resetAIAudioPump()
        print("[AudioBridge] Configured with provider: \(provider.name)")

        // Device -> AI: Forward mic audio from device to AI provider
        transport.onAudioReceived = { [weak self] pcmData in
            guard let self else { return }
            let state = self.stateSnapshot()
            guard state.active, !state.providerError else { return }
            self.audioToAICount += 1
            let now = Date()
            if now.timeIntervalSince(self.lastBridgeLogTime) >= 3.0 {
                print("[AudioBridge] Stats: toAI=\(self.audioToAICount), fromAI=\(self.audioFromAICount) in last 3s")
                self.audioToAICount = 0
                self.audioFromAICount = 0
                self.lastBridgeLogTime = now
            }
            Task {
                do {
                    try await self.currentProvider?.sendAudio(pcmData)
                } catch {
                    print("[AudioBridge] sendAudio error: \(error)")
                }
            }
        }

        // Device -> App: Handle commands from device
        transport.onCommandReceived = { [weak self] json in
            print("[AudioBridge] Device command: \(json)")
            self?.handleDeviceCommand(json)
        }

        // AI -> Device: Forward AI audio response to device
        provider.onAudioReceived = { [weak self] audioData in
            guard let self else { return }
            self.audioFromAICount += 1
            self.enqueueAIAudioForDevice(audioData)
        }

        // AI -> Device: Forward transcript to device display
        provider.onTranscriptReceived = { [weak self] text in
            guard let self else { return }
            print("[AudioBridge] AI transcript: \(text.prefix(80))")
            self.transport?.sendDisplayCommand(DisplayCommand.setText(text))
        }

        // AI -> Device: Handle image generation
        provider.onImageGenerated = { [weak self] image in
            guard let self else { return }
            print("[AudioBridge] AI image generated: \(image.size)")
            self.sendImageToDevice(image)
        }

        // AI -> Device: Forward state changes as display commands
        provider.onStateChanged = { [weak self] state in
            guard let self else { return }
            if state == .error {
                self.setState(active: false, providerError: true)
            }
            // Avoid spamming identical state updates for every audio frame.
            if self.lastForwardedAIState == state {
                return
            }
            self.lastForwardedAIState = state

            if state == .listening {
                // Defer "ready" until the audio pump has fully drained so that
                // all buffered AI audio reaches the device before it ends playback.
                self.aiAudioQueue.async {
                    if self.aiAudioPending.isEmpty {
                        print("[AudioBridge] AI state changed: \(state.rawValue) -> device:ready")
                        self.transport?.sendCommand(["cmd": "ai_state", "state": "listening"])
                        self.transport?.sendDisplayCommand(DisplayCommand.setState(DisplayCommand.stateReady))
                        self.transport?.sendDisplayCommand(DisplayCommand.clearText())
                    } else {
                        self.pendingReadyForDevice = true
                        print("[AudioBridge] AI state changed: \(state.rawValue) -> device:ready (deferred, \(self.aiAudioPending.count) bytes pending)")
                    }
                }
            } else {
                // For non-listening states, send immediately.
                self.aiAudioQueue.async {
                    self.pendingReadyForDevice = false
                }
                if state == .responding {
                    self.transport?.sendCommand(["cmd": "ai_state", "state": "responding"])
                } else if state == .error {
                    self.transport?.sendCommand(["cmd": "ai_state", "state": "error"])
                }
                let displayState = Self.mapAIStateToDisplay(state)
                print("[AudioBridge] AI state changed: \(state.rawValue) -> display:0x\(String(format: "%02X", displayState))")
                self.transport?.sendDisplayCommand(DisplayCommand.setState(displayState))
            }
        }

        // AI errors
        provider.onError = { [weak self] error in
            guard let self else { return }
            print("[AudioBridge] AI error: \(error)")
            if !self.stateSnapshot().providerError {
                self.setState(active: false, providerError: true)
                self.transport?.sendDisplayCommand(
                    DisplayCommand.showError(String(error.localizedDescription.prefix(140)))
                )
            }
        }
    }

    func start() async throws {
        guard let provider = currentProvider else {
            print("[AudioBridge] No provider configured!")
            return
        }
        print("[AudioBridge] Starting voice session with \(provider.name)...")
        lastForwardedAIState = nil
        setState(providerError: false)
        try await provider.startVoiceSession()
        setState(active: true)
        print("[AudioBridge] Voice session active!")
    }

    func stop() {
        print("[AudioBridge] Stopping voice session")
        currentProvider?.stopVoiceSession()
        stateQueue.sync {
            self.deviceActivityOpen = false
            self.lastUserStateValue = nil
            self.suppressedDuplicateUserStateCount = 0
        }
        resetAIAudioPump()
        setState(active: false, providerError: false)
    }

    // MARK: - Private

    private func handleDeviceCommand(_ json: [String: Any]) {
        guard let cmd = json["cmd"] as? String else { return }

        switch cmd {
        case "user_state":
            if let state = json["state"] as? String,
               let gemini = currentProvider as? GeminiProvider {
                if state == "listening" {
                    var shouldStartActivity = false
                    var logSuppressed: String?
                    stateQueue.sync {
                        if !deviceActivityOpen {
                            deviceActivityOpen = true
                            lastUserStateValue = state
                            shouldStartActivity = true
                        } else {
                            suppressedDuplicateUserStateCount += 1
                            let isIdenticalBurst = (lastUserStateValue == state)
                            lastUserStateValue = state
                            logSuppressed = "[AudioBridge] Suppressed duplicate user_state listening (burst=\(isIdenticalBurst), total=\(suppressedDuplicateUserStateCount))"
                        }
                    }
                    if let logSuppressed {
                        print(logSuppressed)
                    }
                    if !shouldStartActivity {
                        return
                    }
                    // User started speaking — flush stale AI audio (barge-in).
                    // The ESP32 already flushed its ring buffer, so any pending
                    // audio in the iOS pump is outdated.
                    resetAIAudioPump()
                    gemini.beginUserActivity()
                } else if state == "processing" {
                    var shouldEndActivity = false
                    var logSuppressed: String?
                    stateQueue.sync {
                        if deviceActivityOpen {
                            deviceActivityOpen = false
                            lastUserStateValue = state
                            shouldEndActivity = true
                        } else {
                            suppressedDuplicateUserStateCount += 1
                            let isIdenticalBurst = (lastUserStateValue == state)
                            lastUserStateValue = state
                            logSuppressed = "[AudioBridge] Suppressed duplicate user_state processing (burst=\(isIdenticalBurst), total=\(suppressedDuplicateUserStateCount))"
                        }
                    }
                    if let logSuppressed {
                        print(logSuppressed)
                    }
                    if !shouldEndActivity {
                        return
                    }
                    gemini.endUserActivity()
                }
            }

        case "user_text":
            if let text = json["text"] as? String {
                if let claude = currentProvider as? ClaudeProvider {
                    Task { await claude.sendTextMessage(text) }
                }
            }

        case "video_stop_request":
            // ESP32 user double-tapped to dismiss video
            if let gemini = currentProvider as? GeminiProvider {
                gemini.stopVideoFromDevice()
            }

        case "generate_image":
            if let prompt = json["prompt"] as? String {
                _ = prompt
            }

        default:
            break
        }
    }

    private func sendImageToDevice(_ image: UIImage) {
        guard let rgb565Data = ImageProcessor.processForDevice(image) else {
            print("[AudioBridge] Failed to process image for device")
            return
        }

        print("[AudioBridge] Sending image to device: \(rgb565Data.count) bytes RGB565")

        // Send image_start as JSON command (ESP32 only handles this via JSON, not binary display cmd)
        transport?.sendCommand([
            "cmd": "image_start",
            "width": ImageProcessor.displayWidth,
            "height": ImageProcessor.displayHeight,
            "size": rgb565Data.count
        ])

        // Stream image data through active device transport.
        transport?.sendImageData(rgb565Data)

        // Wait for all image data to be transmitted, then send image_done
        waitForImageDrainThenFinish()
    }

    /// Polls the BLE image write queue and sends `image_done` once fully drained.
    private func waitForImageDrainThenFinish() {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in
            guard let self else { return }
            if self.transport?.hasPendingImageWrites == true {
                self.waitForImageDrainThenFinish()
            } else {
                // Small extra margin so the last packet reaches the device.
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { [weak self] in
                    print("[AudioBridge] Image data fully sent, sending image_done")
                    self?.transport?.sendCommand(["cmd": "image_done"])
                }
            }
        }
    }

    private func enqueueAIAudioForDevice(_ data: Data) {
        aiAudioQueue.async { [weak self] in
            guard let self else { return }
            if self.aiAudioPending.count + data.count > self.aiAudioMaxBacklogBytes {
                let overflow = (self.aiAudioPending.count + data.count) - self.aiAudioMaxBacklogBytes
                self.aiAudioPending.removeFirst(min(overflow, self.aiAudioPending.count))
                print("[AudioBridge] AI audio backlog overflow, dropping old audio")
            }

            self.aiAudioPending.append(data)
            self.ensureAIAudioPumpRunning()
        }
    }

    private func ensureAIAudioPumpRunning() {
        guard aiAudioTimer == nil else { return }

        // Fresh ADPCM state for each new response burst.
        adpcmEncoder.reset()

        let timer = DispatchSource.makeTimerSource(queue: aiAudioQueue)
        timer.schedule(deadline: .now(), repeating: .milliseconds(Int(aiAudioTickMs)))
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            guard !self.aiAudioPending.isEmpty else {
                self.aiAudioTimer?.cancel()
                self.aiAudioTimer = nil

                // All buffered audio has been handed to transport.
                // Wait for the write queue to drain before telling the
                // device that the response is done, otherwise the "ready"
                // command can overtake the last audio packets.
                if self.pendingReadyForDevice {
                    self.pendingReadyForDevice = false
                    self.waitForTransportDrainThenSendReady()
                }
                return
            }

            // Dequeue PCM, align to whole samples (2 bytes each).
            var sendBytes = min(self.aiAudioChunkBytes, self.aiAudioPending.count)
            sendBytes &= ~1
            guard sendBytes > 0 else { return }

            let chunk = self.aiAudioPending.prefix(sendBytes)
            self.aiAudioPending.removeFirst(sendBytes)

            // IMA-ADPCM: 4:1 compression — 1920 PCM bytes → 480 ADPCM bytes (1 BLE packet)
            let compressed = self.adpcmEncoder.encode(Data(chunk))
            self.transport?.sendAudio(compressed)
        }
        aiAudioTimer = timer
        timer.resume()
    }

    /// Polls the transport write queue and sends `ai_state=ready` once drained.
    private func waitForTransportDrainThenSendReady() {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.06) { [weak self] in
            guard let self else { return }
            if self.transport?.hasPendingAudioWrites == true {
                // Still flushing – check again shortly.
                self.waitForTransportDrainThenSendReady()
            } else {
                // Small extra margin so the last packet reaches the device.
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) { [weak self] in
                    print("[AudioBridge] Transport queue drained, sending deferred state=ready")
                    self?.transport?.sendCommand(["cmd": "ai_state", "state": "listening"])
                    self?.transport?.sendDisplayCommand(DisplayCommand.setState(DisplayCommand.stateReady))
                    self?.transport?.sendDisplayCommand(DisplayCommand.clearText())
                }
            }
        }
    }

    private func resetAIAudioPump() {
        aiAudioQueue.async { [weak self] in
            guard let self else { return }
            self.aiAudioTimer?.cancel()
            self.aiAudioTimer = nil
            self.aiAudioPending.removeAll(keepingCapacity: false)
            self.pendingReadyForDevice = false
            self.adpcmEncoder.reset()
        }
    }

    private static func mapAIStateToDisplay(_ state: AIState) -> UInt8 {
        switch state {
        case .idle:       return DisplayCommand.stateReady
        case .listening:  return DisplayCommand.stateListening
        case .processing: return DisplayCommand.stateProcessing
        case .responding: return DisplayCommand.stateResponding
        case .error:      return DisplayCommand.stateReady
        }
    }

    private func stateSnapshot() -> (active: Bool, providerError: Bool) {
        stateQueue.sync {
            (bridgeActive, hasProviderError)
        }
    }

    private func setState(active: Bool? = nil, providerError: Bool? = nil) {
        var nextActiveForUI: Bool?

        stateQueue.sync {
            if let providerError {
                self.hasProviderError = providerError
            }
            if let active {
                self.bridgeActive = active
                nextActiveForUI = active
            }
        }

        guard let active = nextActiveForUI else { return }
        if Thread.isMainThread {
            isActive = active
        } else {
            DispatchQueue.main.async { [weak self] in
                self?.isActive = active
            }
        }
    }
}
