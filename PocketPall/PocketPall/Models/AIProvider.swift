import Foundation
import UIKit

protocol AIProvider: AnyObject {
    var name: String { get }
    var providerType: AIProviderType { get }
    var isConnected: Bool { get }

    func connect() async throws
    func disconnect()

    // Voice conversation (streaming audio)
    func startVoiceSession() async throws
    func sendAudio(_ pcmData: Data) async throws  // 16kHz 16-bit mono PCM
    func stopVoiceSession()

    // Callbacks
    var onAudioReceived: ((Data) -> Void)? { get set }      // 24kHz 16-bit mono PCM
    var onTranscriptReceived: ((String) -> Void)? { get set }
    var onImageGenerated: ((UIImage) -> Void)? { get set }
    var onError: ((Error) -> Void)? { get set }
    var onStateChanged: ((AIState) -> Void)? { get set }
}

enum AIState: String {
    case idle
    case listening
    case processing
    case responding
    case error
}

enum AIProviderError: LocalizedError {
    case invalidCredentials
    case connectionFailed
    case apiError(String)
    case notAuthenticated

    var errorDescription: String? {
        switch self {
        case .invalidCredentials: return "Invalid credentials"
        case .connectionFailed: return "Connection failed"
        case .apiError(let msg): return msg
        case .notAuthenticated: return "Not authenticated"
        }
    }
}
