import SwiftUI

/// Start screen: Hotspot setup guide — blocks until ESP32 connects via WiFi.
struct ConnectView: View {
    @EnvironmentObject var wifiManager: WiFiTransportManager
    @EnvironmentObject var appState: AppState

    var body: some View {
        VStack(spacing: 28) {
            Spacer()

            Image(systemName: "wifi")
                .font(.system(size: 72))
                .foregroundStyle(.cyan)

            Text("Set Up PocketPall")
                .font(.largeTitle)
                .fontWeight(.bold)
                .foregroundStyle(.white)

            VStack(alignment: .leading, spacing: 16) {
                stepRow(number: 1, text: "Open Settings > Personal Hotspot")
                stepRow(number: 2, text: "Enable \"Allow Others to Join\"")
                stepRow(number: 3, text: "Your PocketPall will connect automatically")
            }
            .padding(.horizontal, 24)

            Spacer()

            if wifiManager.isConnected {
                HStack(spacing: 8) {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                    Text("PocketPall connected!")
                        .foregroundStyle(.green)
                        .font(.headline)
                }
            } else {
                HStack(spacing: 12) {
                    ProgressView()
                        .tint(.cyan)
                    Text("Waiting for PocketPall to connect...")
                        .font(.subheadline)
                        .foregroundStyle(.gray)
                }
            }

            Spacer().frame(height: 40)
        }
        .background(Color.black.ignoresSafeArea())
        .navigationBarHidden(true)
        .onAppear {
            if wifiManager.isConnected {
                routeAfterConnect()
            }
        }
        .onChange(of: wifiManager.isConnected) { _, connected in
            if connected {
                routeAfterConnect()
            }
        }
    }

    private func stepRow(number: Int, text: String) -> some View {
        HStack(alignment: .top, spacing: 14) {
            Text("\(number)")
                .font(.headline)
                .foregroundStyle(.black)
                .frame(width: 28, height: 28)
                .background(Circle().fill(.cyan))

            Text(text)
                .font(.body)
                .foregroundStyle(.white)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private func routeAfterConnect() {
        if appState.selectedProvider == nil {
            appState.setRootRoute(.setup)
        } else {
            appState.setRootRoute(.main)
        }
    }
}
