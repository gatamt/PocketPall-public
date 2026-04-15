import Foundation
import Combine

struct SavedDevice: Identifiable, Codable {
    let identifier: String  // Device identifier string
    let name: String
    var lastConnected: Date

    var id: String { identifier }
}

class SavedDeviceStore: ObservableObject {
    static let shared = SavedDeviceStore()

    @Published var devices: [SavedDevice] = []

    private let key = "saved_wifi_devices"

    private init() {
        load()
    }

    func saveDevice(identifier: UUID, name: String) {
        let device = SavedDevice(
            identifier: identifier.uuidString,
            name: name,
            lastConnected: Date()
        )

        if let idx = devices.firstIndex(where: { $0.identifier == device.identifier }) {
            devices[idx] = device
        } else {
            devices.append(device)
        }
        persist()
    }

    func removeDevice(_ device: SavedDevice) {
        devices.removeAll { $0.identifier == device.identifier }
        persist()
    }

    private func load() {
        guard let data = UserDefaults.standard.data(forKey: key),
              let decoded = try? JSONDecoder().decode([SavedDevice].self, from: data) else { return }
        devices = decoded
    }

    private func persist() {
        guard let data = try? JSONEncoder().encode(devices) else { return }
        UserDefaults.standard.set(data, forKey: key)
    }
}
