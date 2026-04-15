import Foundation
import UIKit
import AVFoundation

class ClaudeProvider: AIProvider {
    let name = "Claude"
    let providerType: AIProviderType = .claude
    private(set) var isConnected = false

    var onAudioReceived: ((Data) -> Void)?
    var onTranscriptReceived: ((String) -> Void)?
    var onImageGenerated: ((UIImage) -> Void)?
    var onError: ((Error) -> Void)?
    var onStateChanged: ((AIState) -> Void)?

    private var apiKey: String?
    private let synthesizer = AVSpeechSynthesizer()
    private let apiURL = "https://api.anthropic.com/v1/messages"

    init() {
        apiKey = KeychainService.shared.get(key: "claude_api_key")
    }

    func connect() async throws {
        guard let key = apiKey, !key.isEmpty else {
            throw AIProviderError.notAuthenticated
        }

        // Validate API key with a minimal request
        var request = URLRequest(url: URL(string: apiURL)!)
        request.httpMethod = "POST"
        request.setValue(key, forHTTPHeaderField: "x-api-key")
        request.setValue("2023-06-01", forHTTPHeaderField: "anthropic-version")
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")

        let body: [String: Any] = [
            "model": "claude-sonnet-4-5-20250929",
            "max_tokens": 10,
            "messages": [["role": "user", "content": "Hi"]]
        ]
        request.httpBody = try? JSONSerialization.data(withJSONObject: body)

        let (_, response) = try await URLSession.shared.data(for: request)
        guard let httpResponse = response as? HTTPURLResponse,
              httpResponse.statusCode == 200 else {
            throw AIProviderError.invalidCredentials
        }

        isConnected = true
    }

    func disconnect() {
        isConnected = false
        synthesizer.stopSpeaking(at: .immediate)
    }

    func startVoiceSession() async throws {
        guard apiKey != nil else {
            throw AIProviderError.notAuthenticated
        }
        onStateChanged?(.listening)
    }

    func sendAudio(_ pcmData: Data) async throws {
        // Claude doesn't support real-time audio WebSocket yet
        // Accumulate audio and transcribe when silence is detected
    }

    func stopVoiceSession() {
        onStateChanged?(.idle)
    }

    /// Send text prompt to Claude (used when device sends user_text command)
    func sendTextMessage(_ text: String) async {
        guard let key = apiKey else {
            onError?(AIProviderError.notAuthenticated)
            return
        }

        onStateChanged?(.processing)

        var request = URLRequest(url: URL(string: apiURL)!)
        request.httpMethod = "POST"
        request.setValue(key, forHTTPHeaderField: "x-api-key")
        request.setValue("2023-06-01", forHTTPHeaderField: "anthropic-version")
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")

        let body: [String: Any] = [
            "model": "claude-sonnet-4-5-20250929",
            "max_tokens": 1024,
            "stream": true,
            "messages": [["role": "user", "content": text]]
        ]
        request.httpBody = try? JSONSerialization.data(withJSONObject: body)

        do {
            let (bytes, _) = try await URLSession.shared.bytes(for: request)
            var fullText = ""
            onStateChanged?(.responding)

            for try await line in bytes.lines {
                guard line.hasPrefix("data: ") else { continue }
                let jsonStr = String(line.dropFirst(6))
                guard jsonStr != "[DONE]",
                      let data = jsonStr.data(using: .utf8),
                      let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                      let type = json["type"] as? String else { continue }

                if type == "content_block_delta",
                   let delta = json["delta"] as? [String: Any],
                   let text = delta["text"] as? String {
                    fullText += text
                    onTranscriptReceived?(fullText)
                }
            }

            // Use TTS for the response
            speakText(fullText)
            onStateChanged?(.listening)
        } catch {
            onError?(error)
            onStateChanged?(.error)
        }
    }

    private func speakText(_ text: String) {
        let utterance = AVSpeechUtterance(string: text)
        utterance.voice = AVSpeechSynthesisVoice(language: "en-US")
        utterance.rate = AVSpeechUtteranceDefaultSpeechRate
        synthesizer.speak(utterance)
    }

    func setAPIKey(_ key: String) {
        apiKey = key
        KeychainService.shared.set(key: "claude_api_key", value: key)
    }
}
