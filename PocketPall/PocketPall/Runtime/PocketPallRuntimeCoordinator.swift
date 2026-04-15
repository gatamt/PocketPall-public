import Foundation
import SwiftUI
import Combine
import UIKit

final class PocketPallRuntimeCoordinator: NSObject, ObservableObject {
    enum RuntimeState: String {
        case idle
        case waitingForDevice
        case tcpConnected
        case voiceStarting
        case voiceActive
        case degraded
    }

    @Published private(set) var runtimeState: RuntimeState = .idle
    @Published private(set) var runtimeStatusText: String = "Idle"
    @Published private(set) var backgroundReliabilityText: String = "Checking permissions..."
    @Published private(set) var backgroundReliabilityNeedsAttention = false
    @Published private(set) var canOpenSystemSettings = false

    let wifiManager: WiFiTransportManager
    let appState: AppState
    let audioBridge: AudioBridge
    let profileStore: RuntimeProfileStore

    private var currentProvider: AIProvider?
    private var bridgeStartTask: Task<Void, Never>?
    private var bridgeRetryWorkItem: DispatchWorkItem?
    private var bridgeRetryDelaySeconds: TimeInterval = 1.0
    private var scenePhase: ScenePhase = .active
    private var cancellables = Set<AnyCancellable>()
    private var hasStarted = false
    private var sessionGeneration: UInt64 = 0
    private var transportTransitionID: UInt64 = 0

    private let permissionCoordinator = RuntimePermissionCoordinator()
    private let backgroundKeeper = BackgroundVoiceSessionKeeper()
    private let runtimeNotifier = RuntimeNotifier()
    private var currentPermissionSnapshot = RuntimePermissionSnapshot(
        degradedMessage: nil,
        needsSettings: false,
        notificationsAuthorized: false
    )

    private var degradedNotificationWorkItem: DispatchWorkItem?
    private var degradedReasonIsTransport = false

    init(wifiManager: WiFiTransportManager,
         appState: AppState,
         audioBridge: AudioBridge = AudioBridge(),
         profileStore: RuntimeProfileStore = .shared) {
        self.wifiManager = wifiManager
        self.appState = appState
        self.audioBridge = audioBridge
        self.profileStore = profileStore
        super.init()
    }

    func start() {
        guard !hasStarted else { return }
        hasStarted = true

        bindRuntime()
        bindPermissions()
        refreshSelectedProviderFromProfile()
        refreshPermissionContext()

        // TCP listener is always active in WiFiTransportManager.
        // Just wait for ESP32 to connect.
        if !wifiManager.isConnected {
            setState(.waitingForDevice, "Waiting for PocketPall to connect")
        }
    }

    func handleScenePhase(_ phase: ScenePhase) {
        scenePhase = phase
        switch phase {
        case .active:
            refreshSelectedProviderFromProfile()
            refreshPermissionContext()
            if wifiManager.isConnected {
                startVoiceSessionIfNeeded(trigger: "scene_active")
            }
            routeToMainIfReady()
        case .inactive:
            if wifiManager.isConnected {
                startVoiceSessionIfNeeded(trigger: "scene_inactive")
            }
        case .background:
            refreshPermissionContext()
            if wifiManager.isConnected {
                startVoiceSessionIfNeeded(trigger: "scene_background")
            }
        @unknown default:
            break
        }

        updateBackgroundRuntimeControls(reason: "scene_\(phase)")
    }

    func manualDisconnect() {
        cancelVoiceStart()
        audioBridge.stop()
        currentProvider = nil
        bridgeRetryWorkItem?.cancel()
        bridgeRetryWorkItem = nil
        bridgeRetryDelaySeconds = 1.0
        wifiManager.setBackgroundMode(false)
        backgroundKeeper.stop(reason: "manual_disconnect")
        wifiManager.disconnect()
        setState(.idle, "Disconnected")
    }

    func prepareForProviderChange() {
        cancelVoiceStart()
        audioBridge.stop()
        currentProvider = nil
        if wifiManager.isConnected {
            setState(.tcpConnected, "Connected, provider setup needed")
        } else {
            setState(.idle, "Provider setup needed")
        }
    }

    func openSystemSettings() {
        guard let url = URL(string: UIApplication.openSettingsURLString) else { return }
        UIApplication.shared.open(url)
    }

    // MARK: - Binding

    private func bindRuntime() {
        wifiManager.onTransportReady = { [weak self] in
            DispatchQueue.main.async {
                self?.handleTransportReady()
            }
        }
        wifiManager.onTransportLost = { [weak self] in
            DispatchQueue.main.async {
                self?.handleTransportLost()
            }
        }

        audioBridge.$isActive
            .receive(on: DispatchQueue.main)
            .sink { [weak self] active in
                guard let self else { return }
                if active {
                    self.bridgeRetryDelaySeconds = 1.0
                    self.setState(.voiceActive, "Voice session active")
                    self.permissionCoordinator.requestNotificationPermissionIfNeeded()
                } else if self.wifiManager.isConnected &&
                          self.profileStore.profile.autoVoiceEnabled {
                    self.scheduleVoiceRetry(reason: "bridge_inactive")
                }
                self.updateBackgroundRuntimeControls(reason: "bridge_active_change")
            }
            .store(in: &cancellables)

        profileStore.$profile
            .receive(on: DispatchQueue.main)
            .sink { [weak self] profile in
                guard let self else { return }
                if let selected = profile.selectedProvider {
                    self.appState.selectedProvider = selected
                }
                self.refreshPermissionContext()
            }
            .store(in: &cancellables)

        wifiManager.$isConnected
            .receive(on: DispatchQueue.main)
            .sink { [weak self] connected in
                guard let self else { return }
                if connected {
                    self.routeToMainIfReady()
                }
                self.updateBackgroundRuntimeControls(reason: "transport_connected_change")
            }
            .store(in: &cancellables)
    }

    private func bindPermissions() {
        permissionCoordinator.onSnapshotChanged = { [weak self] snapshot in
            guard let self else { return }
            DispatchQueue.main.async {
                self.currentPermissionSnapshot = snapshot
                if let degradedMessage = snapshot.degradedMessage {
                    self.backgroundReliabilityText = degradedMessage
                    self.backgroundReliabilityNeedsAttention = true
                    self.canOpenSystemSettings = snapshot.needsSettings
                } else {
                    self.backgroundReliabilityText = "Background reliability: OK"
                    self.backgroundReliabilityNeedsAttention = false
                    self.canOpenSystemSettings = false
                }

                if self.runtimeState == .degraded,
                   snapshot.degradedMessage == nil,
                   !self.wifiManager.isConnected {
                    self.setState(.waitingForDevice, "Waiting for PocketPall to connect")
                }
            }
        }
    }

    private func refreshPermissionContext() {
        let profile = profileStore.profile
        let sceneIsActive = scenePhase == .active
        permissionCoordinator.updateContext(autoConnectEnabled: profile.autoConnectEnabled, sceneIsActive: sceneIsActive)
    }

    // MARK: - Transport lifecycle

    private func handleTransportReady() {
        transportTransitionID &+= 1
        let transitionID = transportTransitionID
        profileStore.markSuccessfulConnectNow()
        setState(.tcpConnected, "PocketPall transport ready")
        print("[Runtime] transport #\(transitionID) ready (generation=\(sessionGeneration))")

        let generation = sessionGeneration
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) { [weak self] in
            guard let self else { return }
            guard self.sessionGeneration == generation else { return }
            guard self.wifiManager.isConnected else { return }
            self.startVoiceSessionIfNeeded(trigger: "transport_ready")
        }
    }

    private func handleTransportLost() {
        transportTransitionID &+= 1
        let transitionID = transportTransitionID
        sessionGeneration &+= 1
        cancelVoiceStart()
        if audioBridge.isActive {
            audioBridge.stop()
        }
        currentProvider = nil
        print("[Runtime] transport #\(transitionID) lost (generation=\(sessionGeneration))")
        // TCP listener stays active — ESP32 will reconnect automatically
        setState(.waitingForDevice, "Transport lost, waiting for reconnection")
    }

    // MARK: - Voice runtime

    private func startVoiceSessionIfNeeded(trigger: String) {
        guard wifiManager.isConnected else { return }
        let profile = profileStore.profile
        guard profile.autoVoiceEnabled else { return }
        guard !audioBridge.isActive else { return }
        guard bridgeStartTask == nil else { return }
        let generation = sessionGeneration

        guard let providerType = effectiveProviderType() else {
            setState(.degraded, "No AI provider selected")
            return
        }
        guard hasKey(for: providerType) else {
            setState(.degraded, "\(providerType.rawValue) API key missing")
            return
        }

        let provider = buildProvider(for: providerType)
        currentProvider = provider
        audioBridge.configure(transport: wifiManager, provider: provider)
        setState(.voiceStarting, "Starting voice (\(providerType.rawValue))")
        print("[Runtime] voice start requested (trigger=\(trigger), generation=\(generation))")

        bridgeStartTask = Task { [weak self] in
            guard let self else { return }
            defer {
                DispatchQueue.main.async { [weak self] in
                    self?.bridgeStartTask = nil
                    self?.updateBackgroundRuntimeControls(reason: "voice_start_defer")
                }
            }

            do {
                try await self.audioBridge.start()
                await MainActor.run {
                    guard self.sessionGeneration == generation else {
                        if self.audioBridge.isActive {
                            self.audioBridge.stop()
                        }
                        return
                    }
                    guard self.wifiManager.isConnected else {
                        if self.audioBridge.isActive {
                            self.audioBridge.stop()
                        }
                        return
                    }
                    self.bridgeRetryDelaySeconds = 1.0
                    self.setState(.voiceActive, "Voice session active")
                }
            } catch {
                await MainActor.run {
                    guard self.sessionGeneration == generation else { return }
                    self.setState(.degraded, "Voice start failed: \(error.localizedDescription)")
                    self.scheduleVoiceRetry(reason: "start_failed_\(trigger)")
                }
            }
        }

        updateBackgroundRuntimeControls(reason: "voice_start_requested")
    }

    private func scheduleVoiceRetry(reason: String) {
        guard wifiManager.isConnected else { return }
        guard profileStore.profile.autoVoiceEnabled else { return }
        guard bridgeRetryWorkItem == nil else { return }
        guard bridgeStartTask == nil else { return }
        let generation = sessionGeneration

        let delay = min(bridgeRetryDelaySeconds, 30.0)
        bridgeRetryDelaySeconds = min(delay * 2.0, 30.0)

        let item = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.bridgeRetryWorkItem = nil
            guard self.sessionGeneration == generation else { return }
            self.startVoiceSessionIfNeeded(trigger: "retry_\(reason)")
        }
        bridgeRetryWorkItem = item
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: item)
    }

    private func cancelVoiceStart() {
        bridgeStartTask?.cancel()
        bridgeStartTask = nil
        bridgeRetryWorkItem?.cancel()
        bridgeRetryWorkItem = nil
    }

    private func effectiveProviderType() -> AIProviderType? {
        if let selected = appState.selectedProvider {
            return selected
        }
        let selected = profileStore.profile.selectedProvider
        appState.selectedProvider = selected
        return selected
    }

    private func refreshSelectedProviderFromProfile() {
        if appState.selectedProvider == nil {
            appState.selectedProvider = profileStore.profile.selectedProvider
        }
    }

    private func buildProvider(for type: AIProviderType) -> AIProvider {
        switch type {
        case .gemini:
            let provider = GeminiProvider()
            if let key = KeychainService.shared.get(key: "gemini_api_key"), !key.isEmpty {
                provider.setAPIKey(key)
            }
            provider.videoTransport = wifiManager
            return provider
        case .openai:
            let provider = OpenAIProvider()
            if let key = KeychainService.shared.get(key: "openai_api_key"), !key.isEmpty {
                provider.setAPIKey(key)
            }
            return provider
        case .claude:
            let provider = ClaudeProvider()
            if let key = KeychainService.shared.get(key: "claude_api_key"), !key.isEmpty {
                provider.setAPIKey(key)
            }
            return provider
        }
    }

    private func hasKey(for provider: AIProviderType) -> Bool {
        let keyName: String
        switch provider {
        case .gemini:
            keyName = "gemini_api_key"
        case .openai:
            keyName = "openai_api_key"
        case .claude:
            keyName = "claude_api_key"
        }
        guard let key = KeychainService.shared.get(key: keyName) else { return false }
        return !key.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    // MARK: - Background runtime controls

    private func updateBackgroundRuntimeControls(reason: String) {
        let background = (scenePhase == .background)
        wifiManager.setBackgroundMode(background)

        if shouldRunBackgroundKeepalive() {
            backgroundKeeper.startIfNeeded(reason: reason)
        } else {
            backgroundKeeper.stop(reason: reason)
        }

        evaluateBackgroundDegradedNotifications()
    }

    private func shouldRunBackgroundKeepalive() -> Bool {
        guard scenePhase == .background else { return false }
        guard wifiManager.isConnected else { return false }
        return runtimeState == .voiceActive ||
               runtimeState == .voiceStarting ||
               audioBridge.isActive ||
               bridgeStartTask != nil
    }

    private func evaluateBackgroundDegradedNotifications() {
        if scenePhase == .background && runtimeState == .degraded && degradedReasonIsTransport {
            if degradedNotificationWorkItem != nil {
                return
            }
            let item = DispatchWorkItem { [weak self] in
                guard let self else { return }
                self.degradedNotificationWorkItem = nil
                guard self.scenePhase == .background, self.runtimeState == .degraded else { return }
                self.runtimeNotifier.notifyBackgroundConnectionLost()
            }
            degradedNotificationWorkItem = item
            DispatchQueue.main.asyncAfter(deadline: .now() + 8.0, execute: item)
            return
        }

        degradedNotificationWorkItem?.cancel()
        degradedNotificationWorkItem = nil

        if runtimeState == .voiceActive || runtimeState == .tcpConnected {
            runtimeNotifier.notifyBackgroundConnectionRestoredIfNeeded()
        }
    }

    // MARK: - Routing

    private func routeToMainIfReady() {
        guard scenePhase == .active else { return }
        guard wifiManager.isConnected else { return }

        if effectiveProviderType() == nil {
            appState.setRootRoute(.setup)
        } else {
            appState.setRootRoute(.main)
        }
    }

    private func setState(_ state: RuntimeState, _ message: String) {
        runtimeState = state
        runtimeStatusText = message
        if state == .degraded {
            degradedReasonIsTransport = !message.hasPrefix("Enable ")
        } else {
            degradedReasonIsTransport = false
        }
        updateBackgroundRuntimeControls(reason: "state_\(state.rawValue)")

        if state == .voiceActive {
            runtimeNotifier.notifyBackgroundConnectionRestoredIfNeeded()
        }

        if state == .degraded, let degradedMessage = currentPermissionSnapshot.degradedMessage {
            backgroundReliabilityText = degradedMessage
            backgroundReliabilityNeedsAttention = true
            canOpenSystemSettings = currentPermissionSnapshot.needsSettings
        }
    }
}
