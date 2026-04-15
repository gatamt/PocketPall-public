import Foundation
import UIKit

class OpenAIProvider: AIProvider {
    let name = "ChatGPT"
    let providerType: AIProviderType = .openai
    private(set) var isConnected = false

    var onAudioReceived: ((Data) -> Void)?
    var onTranscriptReceived: ((String) -> Void)?
    var onImageGenerated: ((UIImage) -> Void)?
    var onError: ((Error) -> Void)?
    var onStateChanged: ((AIState) -> Void)?

    private var webSocket: URLSessionWebSocketTask?
    private var apiKey: String?

    private let realtimeURL = "wss://api.openai.com/v1/realtime?model=gpt-4o-realtime-preview"

    init() {
        apiKey = KeychainService.shared.get(key: "openai_api_key")
    }

    func connect() async throws {
        guard let key = apiKey, !key.isEmpty else {
            throw AIProviderError.notAuthenticated
        }

        // Validate API key
        var request = URLRequest(url: URL(string: "https://api.openai.com/v1/models")!)
        request.setValue("Bearer \(key)", forHTTPHeaderField: "Authorization")

        let (_, response) = try await URLSession.shared.data(for: request)
        guard let httpResponse = response as? HTTPURLResponse, httpResponse.statusCode == 200 else {
            throw AIProviderError.invalidCredentials
        }

        isConnected = true
    }

    func disconnect() {
        webSocket?.cancel(with: .goingAway, reason: nil)
        webSocket = nil
        isConnected = false
    }

    func startVoiceSession() async throws {
        guard let key = apiKey else {
            throw AIProviderError.notAuthenticated
        }

        var request = URLRequest(url: URL(string: realtimeURL)!)
        request.setValue("Bearer \(key)", forHTTPHeaderField: "Authorization")
        request.setValue("realtime=v1", forHTTPHeaderField: "OpenAI-Beta")

        webSocket = URLSession.shared.webSocketTask(with: request)
        webSocket?.resume()

        // Send session configuration
        let config: [String: Any] = [
            "type": "session.update",
            "session": [
                "modalities": ["text", "audio"],
                "instructions": "You are a helpful AI assistant. Respond concisely.",
                "voice": "alloy",
                "input_audio_format": "pcm16",
                "output_audio_format": "pcm16",
                "input_audio_transcription": ["model": "whisper-1"],
                "turn_detection": [
                    "type": "server_vad",
                    "threshold": 0.5,
                    "silence_duration_ms": 500
                ]
            ]
        ]

        if let data = try? JSONSerialization.data(withJSONObject: config),
           let text = String(data: data, encoding: .utf8) {
            try await webSocket?.send(.string(text))
        }

        receiveMessages()
        onStateChanged?(.listening)
    }

    func sendAudio(_ pcmData: Data) async throws {
        guard let ws = webSocket else { return }

        let message: [String: Any] = [
            "type": "input_audio_buffer.append",
            "audio": pcmData.base64EncodedString()
        ]

        if let data = try? JSONSerialization.data(withJSONObject: message),
           let text = String(data: data, encoding: .utf8) {
            try await ws.send(.string(text))
        }
    }

    func stopVoiceSession() {
        webSocket?.cancel(with: .goingAway, reason: nil)
        webSocket = nil
        onStateChanged?(.idle)
    }

    // MARK: - Private

    private func receiveMessages() {
        webSocket?.receive { [weak self] result in
            switch result {
            case .success(let message):
                self?.handleMessage(message)
                self?.receiveMessages()
            case .failure(let error):
                self?.onError?(error)
            }
        }
    }

    private func handleMessage(_ message: URLSessionWebSocketTask.Message) {
        var text: String?
        switch message {
        case .string(let str): text = str
        case .data(let data): text = String(data: data, encoding: .utf8)
        @unknown default: return
        }

        guard let text, let data = text.data(using: .utf8),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let type = json["type"] as? String else { return }

        switch type {
        case "response.audio.delta":
            if let b64 = json["delta"] as? String,
               let audioData = Data(base64Encoded: b64) {
                onAudioReceived?(audioData)
                onStateChanged?(.responding)
            }

        case "response.audio_transcript.delta":
            if let delta = json["delta"] as? String {
                onTranscriptReceived?(delta)
            }

        case "response.done":
            onStateChanged?(.listening)

        case "input_audio_buffer.speech_started":
            onStateChanged?(.listening)

        case "error":
            if let errorInfo = json["error"] as? [String: Any],
               let msg = errorInfo["message"] as? String {
                onError?(AIProviderError.apiError(msg))
            }

        default:
            break
        }
    }

    func setAPIKey(_ key: String) {
        apiKey = key
        KeychainService.shared.set(key: "openai_api_key", value: key)
    }
}
