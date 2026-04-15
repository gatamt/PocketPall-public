import Foundation
import Network
import Combine
import UIKit

/// Manages WiFi TCP transport to ESP32 via iPhone Personal Hotspot.
/// Runs an NWListener on port 5050 with Bonjour advertisement `_pocketai._tcp`.
/// ESP32 connects as TCP client after joining the hotspot.
/// Conforms to DeviceTransport for use with AudioBridge.
final class WiFiTransportManager: NSObject, ObservableObject, DeviceTransport {
    @Published var isConnected = false
    @Published var connectedDeviceName: String?
    @Published var connectionError: String?
    @Published var deviceBattery: DeviceBattery?

    // DeviceTransport callbacks
    var onAudioReceived: ((Data) -> Void)?
    var onCommandReceived: (([String: Any]) -> Void)?
    var onInputEvent: ((UInt8, Data) -> Void)?

    // Runtime lifecycle callbacks
    var onTransportReady: (() -> Void)?
    var onTransportLost: (() -> Void)?

    private var listener: NWListener?
    private var connection: NWConnection?
    private var rxBuffer = Data()
    private var didNotifyTransportReady = false
    private var transportTransitionID: UInt64 = 0

    private let workQueue = DispatchQueue(label: "WiFiTransport.work")
    private var pingTimer: DispatchSourceTimer?
    private var ioWatchdogTimer: DispatchSourceTimer?
    private var isBackgroundMode = false
    private var lastRxProgressAt = Date()
    private var lastTxProgressAt = Date()
    private let foregroundPingSeconds: TimeInterval = 10.0
    private let backgroundPingSeconds: TimeInterval = 5.0
    private let backgroundRXStallTimeoutSeconds: TimeInterval = 20.0

    // TX queue
    private struct TxEntry {
        let data: Data
        let isImage: Bool
    }
    private var txQueue: [TxEntry] = []
    private var isSending = false
    private var txImageFrames = 0
    private let txLock = NSLock()

    override init() {
        super.init()
        startListener()
    }

    // MARK: - Public API

    func disconnect() {
        stopPingTimer()
        stopIOWatchdog()
        closeConnection(reason: "manual_disconnect")
    }

    func setBackgroundMode(_ enabled: Bool) {
        let applyMode = {
            if self.isBackgroundMode == enabled { return }
            self.isBackgroundMode = enabled
            if self.isConnected {
                self.startPingTimer()
            }
            if enabled {
                self.startIOWatchdog()
            } else {
                self.stopIOWatchdog()
            }
            print("[WiFi] background mode = \(enabled)")
        }

        if Thread.isMainThread {
            applyMode()
        } else {
            DispatchQueue.main.async(execute: applyMode)
        }
    }

    // MARK: - DeviceTransport

    func sendAudio(_ data: Data) {
        let frame = L2CAPProtocol.encodeAudioSpk(data)
        enqueueFrame(frame, isImage: false)
    }

    func sendCommand(_ json: [String: Any]) {
        guard let frame = L2CAPProtocol.encodeCommand(json) else { return }
        enqueueFrame(frame, isImage: false)
    }

    func sendImageData(_ data: Data) {
        let chunkSize = 4096  // TCP can handle larger chunks than BLE
        var offset = 0
        var frames: [Data] = []
        while offset < data.count {
            let end = min(offset + chunkSize, data.count)
            let chunk = data.subdata(in: offset..<end)
            frames.append(L2CAPProtocol.encodeImageData(chunk))
            offset = end
        }
        txLock.lock()
        for frame in frames {
            txQueue.append(TxEntry(data: frame, isImage: true))
            txImageFrames += 1
        }
        txLock.unlock()
        print("[WiFi] Image TX: \(frames.count) frames queued")
        drainTxQueue()
    }

    func sendDisplayCommand(_ cmdData: Data) {
        let frame = L2CAPProtocol.encodeDisplayCmd(cmdData)
        enqueueFrame(frame, isImage: false)
    }

    var hasPendingAudioWrites: Bool {
        txLock.lock()
        let pending = !txQueue.isEmpty
        txLock.unlock()
        return pending
    }

    var hasPendingImageWrites: Bool {
        txLock.lock()
        let pending = txImageFrames > 0
        txLock.unlock()
        return pending
    }

    func cancelPendingImageFrames() {
        txLock.lock()
        let before = txQueue.count
        txQueue.removeAll { $0.isImage }
        txImageFrames = 0
        txLock.unlock()
        if before != txQueue.count {
            print("[WiFi] Cancelled \(before - txQueue.count) pending image frames")
        }
    }

    var txQueueDepth: Int {
        txLock.lock()
        let depth = txQueue.count
        txLock.unlock()
        return depth
    }

    /// Send a JPEG video frame, fragmenting into chunks with SOF/EOF flags
    func sendVideoFrame(_ jpegData: Data) {
        let chunkSize = 4096  // Larger chunks over TCP
        var offset = 0
        var frames: [Data] = []

        while offset < jpegData.count {
            let end = min(offset + chunkSize, jpegData.count)
            let chunk = jpegData.subdata(in: offset..<end)

            var flags: UInt8 = 0
            if offset == 0 { flags |= L2CAPProtocol.videoFlagSOF }
            if end >= jpegData.count { flags |= L2CAPProtocol.videoFlagEOF }

            let frame = L2CAPProtocol.encodeVideoJPEG(chunk, flags: flags)
            frames.append(frame)
            offset = end
        }

        txLock.lock()
        for frame in frames {
            txQueue.append(TxEntry(data: frame, isImage: false))
        }
        txLock.unlock()
        drainTxQueue()
    }

    // MARK: - NWListener

    private func startListener() {
        do {
            let params = NWParameters.tcp
            params.includePeerToPeer = true

            // Enable TCP_NODELAY for low-latency audio
            if let tcpOptions = params.defaultProtocolStack.transportProtocol as? NWProtocolTCP.Options {
                tcpOptions.noDelay = true
                tcpOptions.enableKeepalive = true
                tcpOptions.keepaliveInterval = 10
            }

            listener = try NWListener(using: params, on: NWEndpoint.Port(integerLiteral: 5050))

            // Advertise via Bonjour so ESP32 can discover us via mDNS
            listener?.service = NWListener.Service(
                name: "PocketPall",
                type: "_pocketai._tcp"
            )

            listener?.stateUpdateHandler = { [weak self] state in
                switch state {
                case .ready:
                    if let port = self?.listener?.port {
                        print("[WiFi] Listener ready on port \(port)")
                    }
                case .failed(let error):
                    print("[WiFi] Listener failed: \(error)")
                    // Restart listener after failure
                    self?.listener?.cancel()
                    DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) {
                        self?.startListener()
                    }
                case .cancelled:
                    print("[WiFi] Listener cancelled")
                default:
                    break
                }
            }

            listener?.newConnectionHandler = { [weak self] newConnection in
                guard let self else { return }
                DispatchQueue.main.async {
                    self.handleNewConnection(newConnection)
                }
            }

            listener?.start(queue: workQueue)
            print("[WiFi] Starting TCP listener on port 5050 with Bonjour _pocketai._tcp")

        } catch {
            print("[WiFi] Failed to create listener: \(error)")
        }
    }

    private func handleNewConnection(_ newConnection: NWConnection) {
        // Only allow one connection at a time
        if let existing = connection {
            print("[WiFi] Replacing existing connection with new one")
            closeConnection(reason: "new_connection_incoming")
        }

        connection = newConnection
        setupConnection(newConnection)
    }

    // MARK: - NWConnection

    private func setupConnection(_ connection: NWConnection) {
        connection.stateUpdateHandler = { [weak self] state in
            DispatchQueue.main.async {
                self?.handleConnectionStateChange(state)
            }
        }

        connection.start(queue: workQueue)
    }

    private func handleConnectionStateChange(_ state: NWConnection.State) {
        switch state {
        case .ready:
            print("[WiFi] Connection ready!")
            L2CAPProtocol.resetSequence()
            rxBuffer.removeAll(keepingCapacity: true)
            lastRxProgressAt = Date()
            lastTxProgressAt = Date()

            txLock.lock()
            txQueue.removeAll()
            txImageFrames = 0
            isSending = false
            txLock.unlock()

            isConnected = true
            connectionError = nil
            connectedDeviceName = "Pocket AI"

            // Tell ESP32 who we are
            sendCommand(["cmd": "device_info", "name": UIDevice.current.name])

            // Start ping/pong
            startPingTimer()
            if isBackgroundMode {
                startIOWatchdog()
            }

            notifyTransportReadyIfNeeded(source: "tcp_connection_ready")

            // Start receiving
            startReceiving()

        case .failed(let error):
            print("[WiFi] Connection failed: \(error)")
            connectionError = error.localizedDescription
            handleConnectionLost(reason: "connection_failed")

        case .cancelled:
            print("[WiFi] Connection cancelled")
            handleConnectionLost(reason: "connection_cancelled")

        case .waiting(let error):
            print("[WiFi] Connection waiting: \(error)")

        default:
            break
        }
    }

    private func handleConnectionLost(reason: String) {
        let wasConnected = isConnected
        isConnected = false
        connectedDeviceName = nil
        stopPingTimer()
        stopIOWatchdog()

        txLock.lock()
        txQueue.removeAll()
        txImageFrames = 0
        isSending = false
        txLock.unlock()

        rxBuffer.removeAll(keepingCapacity: false)

        if wasConnected {
            notifyTransportLostIfNeeded(source: reason)
        }
        // Listener stays active — ESP32 will reconnect automatically
    }

    private func closeConnection(reason: String) {
        connection?.cancel()
        connection = nil
        handleConnectionLost(reason: reason)
    }

    // MARK: - Receiving

    private func startReceiving() {
        guard let connection = connection else { return }

        connection.receive(minimumIncompleteLength: 1, maximumLength: 16384) { [weak self] content, _, isComplete, error in
            guard let self else { return }

            if let data = content, !data.isEmpty {
                DispatchQueue.main.async {
                    self.rxBuffer.append(data)
                    self.lastRxProgressAt = Date()
                    self.parseRxBuffer()
                }
            }

            if isComplete {
                DispatchQueue.main.async {
                    self.handleConnectionLost(reason: "receive_complete")
                }
                return
            }

            if let error = error {
                print("[WiFi] Receive error: \(error)")
                DispatchQueue.main.async {
                    self.handleConnectionLost(reason: "receive_error")
                }
                return
            }

            // Continue receiving
            self.startReceiving()
        }
    }

    private func parseRxBuffer() {
        while rxBuffer.count >= L2CAPProtocol.headerSize {
            let lengthOffset = rxBuffer.startIndex + 4
            let length = UInt32(rxBuffer[lengthOffset]) |
                         (UInt32(rxBuffer[lengthOffset + 1]) << 8) |
                         (UInt32(rxBuffer[lengthOffset + 2]) << 16) |
                         (UInt32(rxBuffer[lengthOffset + 3]) << 24)

            let frameLen = L2CAPProtocol.headerSize + Int(length)
            guard rxBuffer.count >= frameLen else { return }

            let frameData = rxBuffer.prefix(frameLen)
            rxBuffer.removeFirst(frameLen)
            handleFrame(Data(frameData))
        }
    }

    private func handleFrame(_ data: Data) {
        guard let (type, _, seq, payload) = L2CAPProtocol.decode(data) else {
            print("[WiFi] Failed to decode frame (\(data.count) bytes)")
            return
        }

        switch type {
        case L2CAPProtocol.frameAudioMic:
            onAudioReceived?(payload)

        case L2CAPProtocol.frameCmdJSON, L2CAPProtocol.frameStatus:
            if let json = try? JSONSerialization.jsonObject(with: payload) as? [String: Any] {
                print("[WiFi] Command RX: \(json)")
                onCommandReceived?(json)
            }

        case L2CAPProtocol.frameInputEvent:
            if payload.count >= 1 {
                let evtType = payload[payload.startIndex]
                let evtData = payload.count > 1 ? payload.subdata(in: 1..<payload.count) : Data()
                handleInputEvent(evtType, data: evtData)
                onInputEvent?(evtType, evtData)
            }

        case L2CAPProtocol.framePing:
            let pong = L2CAPProtocol.encodePong(seq: seq)
            enqueueFrame(pong, isImage: false)

        case L2CAPProtocol.framePong:
            break

        default:
            print("[WiFi] Unknown frame type: 0x\(String(format: "%02X", type))")
        }
    }

    private func handleInputEvent(_ evtType: UInt8, data: Data) {
        switch evtType {
        case L2CAPProtocol.inputBattery:
            if data.count >= 3 {
                DispatchQueue.main.async { [weak self] in
                    self?.deviceBattery = DeviceBattery(
                        percent: data[data.startIndex],
                        usbPresent: data[data.startIndex + 1] != 0,
                        charging: data[data.startIndex + 2] != 0
                    )
                }
            }
        default:
            break
        }
    }

    // MARK: - TX Queue

    private func enqueueFrame(_ data: Data, isImage: Bool) {
        txLock.lock()
        txQueue.append(TxEntry(data: data, isImage: isImage))
        if isImage { txImageFrames += 1 }
        txLock.unlock()
        drainTxQueue()
    }

    private func drainTxQueue() {
        txLock.lock()
        guard !isSending, !txQueue.isEmpty, let connection = connection else {
            txLock.unlock()
            return
        }
        isSending = true
        let entry = txQueue.removeFirst()
        if entry.isImage { txImageFrames -= 1 }
        txLock.unlock()

        connection.send(content: entry.data, completion: .contentProcessed { [weak self] error in
            guard let self else { return }
            self.txLock.lock()
            self.isSending = false
            self.txLock.unlock()

            if let error = error {
                print("[WiFi] Send error: \(error)")
                DispatchQueue.main.async {
                    self.handleConnectionLost(reason: "send_error")
                }
                return
            }

            self.lastTxProgressAt = Date()
            self.drainTxQueue()
        })
    }

    // MARK: - Transport notifications

    @discardableResult
    private func nextTransitionID() -> UInt64 {
        transportTransitionID &+= 1
        return transportTransitionID
    }

    private func notifyTransportReadyIfNeeded(source: String) {
        guard !didNotifyTransportReady else { return }
        didNotifyTransportReady = true
        let id = nextTransitionID()
        print("[WiFi] transport transition #\(id): ready (\(source))")
        onTransportReady?()
    }

    private func notifyTransportLostIfNeeded(source: String) {
        guard didNotifyTransportReady else { return }
        didNotifyTransportReady = false
        let id = nextTransitionID()
        print("[WiFi] transport transition #\(id): lost (\(source))")
        onTransportLost?()
    }

    // MARK: - Ping/Pong

    private func startPingTimer() {
        stopPingTimer()
        let interval = isBackgroundMode ? backgroundPingSeconds : foregroundPingSeconds
        let timer = DispatchSource.makeTimerSource(queue: workQueue)
        timer.schedule(deadline: .now() + interval, repeating: interval)
        timer.setEventHandler { [weak self] in
            guard let self, self.isConnected else { return }
            let ping = L2CAPProtocol.encodePing()
            self.enqueueFrame(ping, isImage: false)
        }
        pingTimer = timer
        timer.resume()
    }

    private func stopPingTimer() {
        pingTimer?.setEventHandler {}
        pingTimer?.cancel()
        pingTimer = nil
    }

    // MARK: - IO Watchdog

    private func startIOWatchdog() {
        stopIOWatchdog()
        guard isBackgroundMode else { return }

        let timer = DispatchSource.makeTimerSource(queue: workQueue)
        timer.schedule(deadline: .now() + 5.0, repeating: 5.0)
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            DispatchQueue.main.async {
                self.checkBackgroundIOStall()
            }
        }
        ioWatchdogTimer = timer
        timer.resume()
    }

    private func stopIOWatchdog() {
        ioWatchdogTimer?.setEventHandler {}
        ioWatchdogTimer?.cancel()
        ioWatchdogTimer = nil
    }

    private func checkBackgroundIOStall() {
        guard isBackgroundMode, isConnected else { return }

        let now = Date()
        let rxIdle = now.timeIntervalSince(lastRxProgressAt)
        if rxIdle <= backgroundRXStallTimeoutSeconds { return }

        print("[WiFi] background IO stall detected (rx_idle=\(Int(rxIdle))s), closing connection")
        closeConnection(reason: "background_io_stall")
    }
}

// MARK: - Supporting Types

struct DeviceBattery {
    var percent: UInt8
    var usbPresent: Bool
    var charging: Bool
}
