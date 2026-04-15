import SwiftUI

/// Shows previously connected PocketPall devices.
struct SavedDevicesView: View {
    @EnvironmentObject var wifiManager: WiFiTransportManager
    @EnvironmentObject var appState: AppState

    @StateObject private var savedStore = SavedDeviceStore.shared

    @State private var connectingDeviceId: String?
    @State private var connectionError: String?

    var body: some View {
        VStack(spacing: 20) {
            if savedStore.devices.isEmpty {
                Spacer()

                Image(systemName: "tray")
                    .font(.system(size: 48))
                    .foregroundStyle(.gray)

                Text("No saved devices")
                    .font(.title3)
                    .foregroundStyle(.gray)

                Text("Your PocketPall will appear here after first connection.")
                    .font(.subheadline)
                    .foregroundStyle(.gray.opacity(0.7))
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 40)

                Spacer()
            } else {
                List {
                    ForEach(sortedDevices) { device in
                        HStack(spacing: 14) {
                            Image(systemName: "wifi")
                                .font(.title3)
                                .foregroundStyle(.cyan)

                            VStack(alignment: .leading, spacing: 3) {
                                Text(device.name)
                                    .font(.headline)
                                    .foregroundStyle(.white)

                                Text(timeAgoString(device.lastConnected))
                                    .font(.caption)
                                    .foregroundStyle(.gray)
                            }

                            Spacer()
                        }
                        .padding(.vertical, 4)
                        .listRowBackground(Color(white: 0.12))
                    }
                    .onDelete(perform: deleteDevices)
                }
                .listStyle(.plain)
                .scrollContentBackground(.hidden)
            }

            if let error = connectionError {
                Text(error)
                    .font(.caption)
                    .foregroundStyle(.red)
                    .padding(.horizontal, 20)
            }
        }
        .background(Color.black.ignoresSafeArea())
        .navigationTitle("Your PocketPall")
        .navigationBarTitleDisplayMode(.large)
        .toolbarColorScheme(.dark, for: .navigationBar)
        .onChange(of: wifiManager.isConnected) { _, connected in
            if connected {
                connectionError = nil
                if appState.selectedProvider == nil {
                    appState.setRootRoute(.setup)
                } else {
                    appState.setRootRoute(.main)
                }
            }
        }
    }

    private var sortedDevices: [SavedDevice] {
        savedStore.devices.sorted { $0.lastConnected > $1.lastConnected }
    }

    private func deleteDevices(at offsets: IndexSet) {
        let sorted = sortedDevices
        for index in offsets {
            savedStore.removeDevice(sorted[index])
        }
    }

    private func timeAgoString(_ date: Date) -> String {
        let interval = Date().timeIntervalSince(date)

        if interval < 60 {
            return "Just now"
        } else if interval < 3600 {
            let mins = Int(interval / 60)
            return "\(mins) min ago"
        } else if interval < 86400 {
            let hours = Int(interval / 3600)
            return "\(hours) hour\(hours == 1 ? "" : "s") ago"
        } else {
            let days = Int(interval / 86400)
            return "\(days) day\(days == 1 ? "" : "s") ago"
        }
    }
}
