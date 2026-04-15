import Foundation
import Combine

struct RuntimeProfile: Codable {
    var autoConnectEnabled: Bool = true
    var autoVoiceEnabled: Bool = true
    var lastDeviceIdentifier: String?
    var selectedProviderRawValue: String?
    var lastSuccessfulConnectAt: Date?
    var webResearchV2Enabled: Bool = true
    var webResearchShadowMode: Bool = false
    var webResearchEndpoint: String?
    var webResearchLocale: String = "da-DK"

    var selectedProvider: AIProviderType? {
        get {
            guard let selectedProviderRawValue else { return nil }
            return AIProviderType(rawValue: selectedProviderRawValue)
        }
        set {
            selectedProviderRawValue = newValue?.rawValue
        }
    }
}

final class RuntimeProfileStore: ObservableObject {
    static let shared = RuntimeProfileStore()

    @Published private(set) var profile: RuntimeProfile

    private let key = "runtime_profile_v1"
    private let defaults: UserDefaults

    private init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        if let data = defaults.data(forKey: key),
           let decoded = try? JSONDecoder().decode(RuntimeProfile.self, from: data) {
            self.profile = decoded
        } else {
            self.profile = RuntimeProfile()
        }
    }

    func update(_ transform: (inout RuntimeProfile) -> Void) {
        var updated = profile
        transform(&updated)
        profile = updated
        persist()
    }

    func setSelectedProvider(_ provider: AIProviderType?) {
        update { profile in
            profile.selectedProvider = provider
        }
    }

    func setLastDeviceIdentifier(_ identifier: UUID?) {
        update { profile in
            profile.lastDeviceIdentifier = identifier?.uuidString
        }
    }

    func markSuccessfulConnectNow() {
        update { profile in
            profile.lastSuccessfulConnectAt = Date()
        }
    }

    func setWebResearchEndpoint(_ endpoint: String?) {
        update { profile in
            let trimmed = endpoint?.trimmingCharacters(in: .whitespacesAndNewlines)
            profile.webResearchEndpoint = (trimmed?.isEmpty == false) ? trimmed : nil
        }
    }

    func setWebResearchEnabled(_ enabled: Bool) {
        update { profile in
            profile.webResearchV2Enabled = enabled
        }
    }

    func setWebResearchShadowMode(_ enabled: Bool) {
        update { profile in
            profile.webResearchShadowMode = enabled
        }
    }

    func setWebResearchLocale(_ locale: String) {
        update { profile in
            let trimmed = locale.trimmingCharacters(in: .whitespacesAndNewlines)
            profile.webResearchLocale = trimmed.isEmpty ? "da-DK" : trimmed
        }
    }

    private func persist() {
        guard let data = try? JSONEncoder().encode(profile) else { return }
        defaults.set(data, forKey: key)
    }
}
