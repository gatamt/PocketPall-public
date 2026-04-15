import SwiftUI

struct MainView: View {
    @EnvironmentObject var wifiManager: WiFiTransportManager
    @EnvironmentObject var appState: AppState
    @EnvironmentObject var runtimeCoordinator: PocketPallRuntimeCoordinator

    @State private var showDisconnectAlert = false
    @State private var showSettings = false
    @State private var hadConnectedSession = false

    var body: some View {
        VStack(spacing: 32) {
            Spacer()

            Image(systemName: "checkmark.circle.fill")
                .font(.system(size: 64))
                .foregroundStyle(.green)

            Text("You're all set.\nTalk to your PocketPall!")
                .font(.title2.bold())
                .foregroundStyle(.white)
                .multilineTextAlignment(.center)

            VStack(spacing: 12) {
                HStack(spacing: 8) {
                    Circle()
                        .fill(wifiManager.isConnected ? Color.green : Color.orange)
                        .frame(width: 10, height: 10)

                    Text(wifiManager.isConnected
                         ? "Connected to \(wifiManager.connectedDeviceName ?? "PocketPall")"
                         : "Waiting for connection...")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                }

                HStack(spacing: 6) {
                    Text("Runtime:")
                        .font(.caption)
                        .foregroundStyle(.gray)
                    Text(runtimeCoordinator.runtimeState.rawValue)
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.cyan)
                }

                Text(runtimeCoordinator.runtimeStatusText)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 8)

                HStack(spacing: 8) {
                    Circle()
                        .fill(runtimeCoordinator.backgroundReliabilityNeedsAttention ? Color.orange : Color.green)
                        .frame(width: 8, height: 8)
                    Text(runtimeCoordinator.backgroundReliabilityText)
                        .font(.caption2)
                        .foregroundStyle(runtimeCoordinator.backgroundReliabilityNeedsAttention ? .orange : .secondary)
                        .multilineTextAlignment(.leading)
                    Spacer(minLength: 0)
                    if runtimeCoordinator.canOpenSystemSettings {
                        Button("Open Settings") {
                            runtimeCoordinator.openSystemSettings()
                        }
                        .font(.caption2.weight(.semibold))
                        .foregroundStyle(.cyan)
                    }
                }

                if let provider = appState.selectedProvider {
                    HStack(spacing: 6) {
                        Image(systemName: providerIcon(provider))
                            .foregroundStyle(providerColor(provider))
                        Text(provider.rawValue)
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }
                }

                if let battery = wifiManager.deviceBattery {
                    HStack(spacing: 6) {
                        Image(systemName: batteryIcon(battery))
                            .foregroundStyle(batteryColor(battery))
                        Text("\(battery.percent)%")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            .padding()
            .background(Color(white: 0.12))
            .clipShape(RoundedRectangle(cornerRadius: 16))

            Spacer()
        }
        .padding()
        .navigationTitle("PocketPall")
        .navigationBarBackButtonHidden(true)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button(action: { showSettings = true }) {
                    Image(systemName: "gearshape")
                        .foregroundStyle(.secondary)
                }
            }
        }
        .background(Color.black)
        .alert("Connection Lost", isPresented: $showDisconnectAlert) {
            Button("Wait") { }
            Button("Disconnect", role: .cancel) {
                runtimeCoordinator.manualDisconnect()
                appState.popToRoot()
            }
        } message: {
            Text("Connection to your PocketPall was lost. The app keeps trying to reconnect automatically.")
        }
        .confirmationDialog("Settings", isPresented: $showSettings) {
            Button("Change AI Provider") {
                runtimeCoordinator.prepareForProviderChange()
                appState.setRootRoute(.setup)
            }
            Button("Disconnect device") {
                runtimeCoordinator.manualDisconnect()
                appState.popToRoot()
            }
            Button("Cancel", role: .cancel) {}
        }
        .onAppear {
            hadConnectedSession = wifiManager.isConnected
        }
        .onChange(of: wifiManager.isConnected) { _, connected in
            if connected {
                hadConnectedSession = true
            } else if hadConnectedSession {
                hadConnectedSession = false
                showDisconnectAlert = true
            }
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

    private func batteryIcon(_ battery: DeviceBattery) -> String {
        if battery.charging { return "battery.100.bolt" }
        if battery.percent > 75 { return "battery.100" }
        if battery.percent > 50 { return "battery.75" }
        if battery.percent > 25 { return "battery.50" }
        return "battery.25"
    }

    private func batteryColor(_ battery: DeviceBattery) -> Color {
        if battery.charging { return .green }
        if battery.percent > 20 { return .white }
        return .red
    }
}
