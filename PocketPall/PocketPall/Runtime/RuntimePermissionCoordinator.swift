import Foundation
import UserNotifications

struct RuntimePermissionSnapshot {
    let degradedMessage: String?
    let needsSettings: Bool
    let notificationsAuthorized: Bool
}

final class RuntimePermissionCoordinator: NSObject {
    var onSnapshotChanged: ((RuntimePermissionSnapshot) -> Void)?

    private let notificationCenter: UNUserNotificationCenter
    private let defaults: UserDefaults

    private var autoConnectEnabled = false
    private var sceneIsActive = true
    private var notificationStatus: UNAuthorizationStatus = .notDetermined

    private let didPromptNotificationsKey = "runtime_notification_prompted_v1"

    init(notificationCenter: UNUserNotificationCenter = .current(),
         defaults: UserDefaults = .standard) {
        self.notificationCenter = notificationCenter
        self.defaults = defaults
        super.init()
        refreshNotificationStatus()
    }

    func updateContext(autoConnectEnabled: Bool, sceneIsActive: Bool) {
        self.autoConnectEnabled = autoConnectEnabled
        self.sceneIsActive = sceneIsActive
        refreshNotificationStatus()
        emitSnapshot()
    }

    func requestNotificationPermissionIfNeeded() {
        guard !defaults.bool(forKey: didPromptNotificationsKey) else {
            refreshNotificationStatus()
            return
        }
        defaults.set(true, forKey: didPromptNotificationsKey)

        notificationCenter.requestAuthorization(options: [.alert, .sound, .badge]) { [weak self] _, _ in
            self?.refreshNotificationStatus()
        }
    }

    func refreshNotificationStatus() {
        notificationCenter.getNotificationSettings { [weak self] settings in
            guard let self else { return }
            DispatchQueue.main.async {
                self.notificationStatus = settings.authorizationStatus
                self.emitSnapshot()
            }
        }
    }

    private func emitSnapshot() {
        let notificationsAuthorized: Bool = {
            switch notificationStatus {
            case .authorized, .provisional, .ephemeral:
                return true
            default:
                return false
            }
        }()

        var degradedMessage: String?
        var needsSettings = false
        if autoConnectEnabled {
            if notificationStatus == .denied {
                degradedMessage = "Enable Notifications for background reconnect alerts"
                needsSettings = true
            }
        }

        onSnapshotChanged?(RuntimePermissionSnapshot(
            degradedMessage: degradedMessage,
            needsSettings: needsSettings,
            notificationsAuthorized: notificationsAuthorized
        ))
    }
}
