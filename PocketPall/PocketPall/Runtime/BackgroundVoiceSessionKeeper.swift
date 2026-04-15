import Foundation
import AVFoundation

/// Keeps the app process alive during active background voice sessions by holding
/// an active playback audio session and running a silent audio engine.
final class BackgroundVoiceSessionKeeper {
    private let queue = DispatchQueue(label: "BackgroundVoiceSessionKeeper.queue")

    private var engine: AVAudioEngine?
    private var sourceNode: AVAudioSourceNode?
    private var shouldBeRunning = false
    private var isRunning = false
    private var didInstallObservers = false

    func startIfNeeded(reason: String) {
        queue.async {
            self.shouldBeRunning = true
            guard !self.isRunning else { return }
            self.installObserversIfNeeded()
            self.startEngineLocked(reason: reason)
        }
    }

    func stop(reason: String) {
        queue.async {
            self.shouldBeRunning = false
            guard self.isRunning else { return }
            self.stopEngineLocked(reason: reason)
        }
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    private func installObserversIfNeeded() {
        guard !didInstallObservers else { return }
        didInstallObservers = true

        NotificationCenter.default.addObserver(
            self,
            selector: #selector(handleAudioInterruption(_:)),
            name: AVAudioSession.interruptionNotification,
            object: nil
        )
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(handleAudioRouteChange(_:)),
            name: AVAudioSession.routeChangeNotification,
            object: nil
        )
    }

    private func startEngineLocked(reason: String) {
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
            try session.setActive(true, options: [])

            let newEngine = AVAudioEngine()
            let outputFormat = newEngine.outputNode.outputFormat(forBus: 0)
            let zeroSource = AVAudioSourceNode { _, _, frameCount, audioBufferList -> OSStatus in
                let bufferList = UnsafeMutableAudioBufferListPointer(audioBufferList)
                for buffer in bufferList {
                    guard let mData = buffer.mData else { continue }
                    memset(mData, 0, Int(buffer.mDataByteSize))
                }
                return noErr
            }

            newEngine.attach(zeroSource)
            newEngine.connect(zeroSource, to: newEngine.mainMixerNode, format: outputFormat)
            newEngine.mainMixerNode.outputVolume = 0.0
            try newEngine.start()

            engine = newEngine
            sourceNode = zeroSource
            isRunning = true
            print("[BackgroundKeepalive] started (\(reason))")
        } catch {
            isRunning = false
            engine = nil
            sourceNode = nil
            print("[BackgroundKeepalive] failed to start: \(error)")
        }
    }

    private func stopEngineLocked(reason: String) {
        engine?.stop()
        engine = nil
        sourceNode = nil
        isRunning = false
        do {
            try AVAudioSession.sharedInstance().setActive(false, options: [.notifyOthersOnDeactivation])
        } catch {
            print("[BackgroundKeepalive] session deactivation failed: \(error)")
        }
        print("[BackgroundKeepalive] stopped (\(reason))")
    }

    @objc
    private func handleAudioInterruption(_ notification: Notification) {
        queue.async {
            guard self.shouldBeRunning else { return }
            guard let userInfo = notification.userInfo,
                  let raw = userInfo[AVAudioSessionInterruptionTypeKey] as? UInt,
                  let type = AVAudioSession.InterruptionType(rawValue: raw) else {
                return
            }

            switch type {
            case .began:
                if self.isRunning {
                    self.engine?.pause()
                    self.isRunning = false
                    print("[BackgroundKeepalive] interruption began")
                }
            case .ended:
                print("[BackgroundKeepalive] interruption ended, restarting")
                self.startEngineLocked(reason: "interruption_end")
            @unknown default:
                break
            }
        }
    }

    @objc
    private func handleAudioRouteChange(_ notification: Notification) {
        queue.async {
            guard self.shouldBeRunning else { return }
            guard let userInfo = notification.userInfo,
                  let raw = userInfo[AVAudioSessionRouteChangeReasonKey] as? UInt,
                  let reason = AVAudioSession.RouteChangeReason(rawValue: raw) else {
                return
            }

            switch reason {
            case .oldDeviceUnavailable, .newDeviceAvailable, .categoryChange, .override:
                print("[BackgroundKeepalive] route change (\(reason.rawValue)), restarting")
                self.stopEngineLocked(reason: "route_change")
                self.startEngineLocked(reason: "route_change")
            default:
                break
            }
        }
    }
}
