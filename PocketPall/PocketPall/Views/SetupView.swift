import SwiftUI

/// Screen 2: AI provider selection + API key entry.
struct SetupView: View {
    @EnvironmentObject var appState: AppState
    @EnvironmentObject var wifiManager: WiFiTransportManager

    @State private var selectedProvider: AIProviderType = .gemini
    @State private var apiKey = ""
    @State private var webResearchV2Enabled = true
    @State private var webResearchShadowMode = false
    @State private var webResearchLocale = "da-DK"
    @State private var webResearchEndpoint = "http://127.0.0.1:8787"
    @State private var webAgentAPIKey = ""
    @State private var serperAPIKey = ""
    @State private var isValidating = false
    @State private var errorMessage: String?

    var body: some View {
        ScrollView {
            VStack(spacing: 24) {
                Text("Choose your AI")
                    .font(.title2.bold())
                    .foregroundStyle(.white)
                    .padding(.top, 12)

                ForEach(AIProviderType.allCases, id: \.self) { provider in
                    providerRow(provider)
                }

                Divider()
                    .background(Color.white.opacity(0.2))

                apiKeySection
                webResearchSection

                if let error = errorMessage {
                    HStack {
                        Image(systemName: "exclamationmark.triangle")
                        Text(error)
                    }
                    .font(.caption)
                    .foregroundStyle(.red)
                    .padding(.horizontal)
                }

                Button(action: validateAndContinue) {
                    if isValidating {
                        ProgressView()
                            .tint(.white)
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 14)
                    } else {
                        Text("Next")
                            .font(.headline)
                            .foregroundStyle(canContinue ? .black : .gray)
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 14)
                    }
                }
                .background(canContinue ? Color.cyan : Color(white: 0.2))
                .clipShape(RoundedRectangle(cornerRadius: 12))
                .disabled(!canContinue || isValidating)
                .padding(.horizontal)
            }
            .padding()
        }
        .navigationTitle("AI Setup")
        .background(Color.black.ignoresSafeArea())
        .onAppear {
            if let saved = appState.selectedProvider {
                selectedProvider = saved
            }
            apiKey = KeychainService.shared.get(key: keychainKey(for: selectedProvider)) ?? ""
            let profile = RuntimeProfileStore.shared.profile
            webResearchV2Enabled = profile.webResearchV2Enabled
            webResearchShadowMode = profile.webResearchShadowMode
            webResearchLocale = profile.webResearchLocale
            webResearchEndpoint = profile.webResearchEndpoint ?? "http://127.0.0.1:8787"
            webAgentAPIKey = KeychainService.shared.get(key: webAgentAPIKeyKey()) ?? ""
            serperAPIKey = KeychainService.shared.get(key: "serper_api_key") ?? ""
        }
        .onChange(of: selectedProvider) { _, provider in
            apiKey = KeychainService.shared.get(key: keychainKey(for: provider)) ?? ""
            errorMessage = nil
        }
    }

    private var canContinue: Bool {
        !apiKey.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    private func providerRow(_ provider: AIProviderType) -> some View {
        Button {
            selectedProvider = provider
        } label: {
            HStack(spacing: 14) {
                Image(systemName: providerIcon(provider))
                    .font(.title2)
                    .foregroundStyle(providerColor(provider))
                    .frame(width: 44, height: 44)
                    .background(providerColor(provider).opacity(0.15))
                    .clipShape(RoundedRectangle(cornerRadius: 10))

                Text(provider.rawValue)
                    .font(.headline)
                    .foregroundStyle(.white)

                Spacer()

                if selectedProvider == provider {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(.cyan)
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)
            .background(
                RoundedRectangle(cornerRadius: 14)
                    .fill(Color(white: 0.12))
                    .overlay(
                        RoundedRectangle(cornerRadius: 14)
                            .stroke(selectedProvider == provider ? providerColor(provider) : .clear, lineWidth: 2)
                    )
            )
        }
        .buttonStyle(.plain)
        .padding(.horizontal)
    }

    @ViewBuilder
    private var apiKeySection: some View {
        VStack(spacing: 12) {
            SecureField(apiKeyPlaceholder, text: $apiKey)
                .textFieldStyle(.plain)
                .padding()
                .background(Color(white: 0.12))
                .clipShape(RoundedRectangle(cornerRadius: 12))
                .foregroundStyle(.white)
                .padding(.horizontal)

            if let url = URL(string: apiKeyHelpURL) {
                Link(apiKeyHelpText, destination: url)
                    .font(.caption)
                    .foregroundStyle(.cyan)
            }
        }
    }

    private func validateAndContinue() {
        let key = apiKey.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !key.isEmpty else { return }
        let normalizedEndpoint = webResearchEndpoint.trimmingCharacters(in: .whitespacesAndNewlines)
        let normalizedLocale = webResearchLocale.trimmingCharacters(in: .whitespacesAndNewlines)
        let normalizedWebAgentKey = webAgentAPIKey.trimmingCharacters(in: .whitespacesAndNewlines)
        let normalizedSerperKey = serperAPIKey.trimmingCharacters(in: .whitespacesAndNewlines)

        isValidating = true
        errorMessage = nil

        Task {
            do {
                let provider = createProvider(type: selectedProvider, key: key)
                try await provider.connect()

                await MainActor.run {
                    isValidating = false
                    KeychainService.shared.set(key: keychainKey(for: selectedProvider), value: key)
                    if normalizedWebAgentKey.isEmpty {
                        KeychainService.shared.delete(key: webAgentAPIKeyKey())
                    } else {
                        KeychainService.shared.set(key: webAgentAPIKeyKey(), value: normalizedWebAgentKey)
                    }
                    if normalizedSerperKey.isEmpty {
                        KeychainService.shared.delete(key: "serper_api_key")
                    } else {
                        KeychainService.shared.set(key: "serper_api_key", value: normalizedSerperKey)
                    }
                    appState.selectedProvider = selectedProvider
                    RuntimeProfileStore.shared.update { profile in
                        profile.selectedProvider = selectedProvider
                        profile.autoConnectEnabled = true
                        profile.autoVoiceEnabled = true
                        profile.webResearchV2Enabled = webResearchV2Enabled
                        profile.webResearchShadowMode = webResearchShadowMode
                        profile.webResearchLocale = normalizedLocale.isEmpty ? "da-DK" : normalizedLocale
                        profile.webResearchEndpoint = normalizedEndpoint.isEmpty ? nil : normalizedEndpoint
                    }
                    appState.setRootRoute(.main)
                }
            } catch {
                await MainActor.run {
                    isValidating = false
                    errorMessage = error.localizedDescription
                }
            }
        }
    }

    @ViewBuilder
    private var webResearchSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Web Research v2")
                .font(.headline)
                .foregroundStyle(.white)
                .padding(.horizontal)

            Toggle("Enable web_research v2", isOn: $webResearchV2Enabled)
                .toggleStyle(.switch)
                .foregroundStyle(.white)
                .padding(.horizontal)

            Toggle("Shadow mode (log only)", isOn: $webResearchShadowMode)
                .toggleStyle(.switch)
                .foregroundStyle(.white.opacity(0.9))
                .padding(.horizontal)

            TextField("Web agent endpoint (e.g. https://your-host)", text: $webResearchEndpoint)
                .textFieldStyle(.plain)
                .padding()
                .background(Color(white: 0.12))
                .clipShape(RoundedRectangle(cornerRadius: 12))
                .foregroundStyle(.white)
                .padding(.horizontal)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .keyboardType(.URL)

            SecureField("Web agent API key (optional)", text: $webAgentAPIKey)
                .textFieldStyle(.plain)
                .padding()
                .background(Color(white: 0.12))
                .clipShape(RoundedRectangle(cornerRadius: 12))
                .foregroundStyle(.white)
                .padding(.horizontal)

            TextField("Locale (e.g. da-DK)", text: $webResearchLocale)
                .textFieldStyle(.plain)
                .padding()
                .background(Color(white: 0.12))
                .clipShape(RoundedRectangle(cornerRadius: 12))
                .foregroundStyle(.white)
                .padding(.horizontal)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()

            Divider()
                .background(Color.white.opacity(0.2))
                .padding(.horizontal)

            Text("Web Search (Serper.dev)")
                .font(.headline)
                .foregroundStyle(.white)
                .padding(.horizontal)

            SecureField("Serper API key (optional, 2500 free queries)", text: $serperAPIKey)
                .textFieldStyle(.plain)
                .padding()
                .background(Color(white: 0.12))
                .clipShape(RoundedRectangle(cornerRadius: 12))
                .foregroundStyle(.white)
                .padding(.horizontal)

            if let url = URL(string: "https://serper.dev") {
                Link("Get a free key at serper.dev", destination: url)
                    .font(.caption)
                    .foregroundStyle(.cyan)
            }
        }
    }

    // MARK: - Helpers

    private var apiKeyPlaceholder: String {
        switch selectedProvider {
        case .gemini: return "Enter Gemini API Key"
        case .openai: return "Enter OpenAI API Key"
        case .claude: return "Enter Anthropic API Key"
        }
    }

    private var apiKeyHelpURL: String {
        switch selectedProvider {
        case .gemini: return "https://aistudio.google.com/apikey"
        case .openai: return "https://platform.openai.com/api-keys"
        case .claude: return "https://console.anthropic.com/settings/keys"
        }
    }

    private var apiKeyHelpText: String {
        switch selectedProvider {
        case .gemini: return "Get a free key at aistudio.google.com"
        case .openai: return "Get a key at platform.openai.com"
        case .claude: return "Get a key at console.anthropic.com"
        }
    }

    private func providerIcon(_ type: AIProviderType) -> String {
        switch type {
        case .gemini: return "sparkles"
        case .openai: return "bubble.left.and.bubble.right"
        case .claude: return "brain"
        }
    }

    private func providerColor(_ type: AIProviderType) -> Color {
        switch type {
        case .gemini: return .blue
        case .openai: return .green
        case .claude: return .purple
        }
    }

    private func keychainKey(for type: AIProviderType) -> String {
        switch type {
        case .gemini: return "gemini_api_key"
        case .openai: return "openai_api_key"
        case .claude: return "claude_api_key"
        }
    }

    private func webAgentAPIKeyKey() -> String {
        "web_agent_api_key"
    }

    private func createProvider(type: AIProviderType, key: String) -> AIProvider {
        switch type {
        case .gemini:
            let p = GeminiProvider()
            p.setAPIKey(key)
            return p
        case .openai:
            let p = OpenAIProvider()
            p.setAPIKey(key)
            return p
        case .claude:
            let p = ClaudeProvider()
            p.setAPIKey(key)
            return p
        }
    }
}
