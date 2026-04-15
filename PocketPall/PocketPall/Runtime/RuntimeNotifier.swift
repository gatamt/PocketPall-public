import Foundation
import UserNotifications

final class RuntimeNotifier {
    private let notificationCenter: UNUserNotificationCenter
    private let queue = DispatchQueue(label: "RuntimeNotifier.queue")

    private var lastLossNotificationAt: Date = .distantPast
    private var didSendLossNotification = false
    private let lossCooldown: TimeInterval = 10 * 60

    init(notificationCenter: UNUserNotificationCenter = .current()) {
        self.notificationCenter = notificationCenter
    }

    func notifyBackgroundConnectionLost() {
        queue.async {
            let now = Date()
            guard now.timeIntervalSince(self.lastLossNotificationAt) >= self.lossCooldown else { return }
            self.lastLossNotificationAt = now
            self.didSendLossNotification = true
            self.sendNotification(
                idPrefix: "bg_connection_lost",
                title: "PocketPall lost connection",
                body: "The background connection became unstable. The app is trying to reconnect automatically."
            )
        }
    }

    func notifyBackgroundConnectionRestoredIfNeeded() {
        queue.async {
            guard self.didSendLossNotification else { return }
            self.didSendLossNotification = false
            self.sendNotification(
                idPrefix: "bg_connection_restored",
                title: "PocketPall reconnected",
                body: "The background connection is restored and the voice session is back."
            )
        }
    }

    private func sendNotification(idPrefix: String, title: String, body: String) {
        notificationCenter.getNotificationSettings { [weak self] settings in
            guard let self else { return }
            let allowed: Bool = {
                switch settings.authorizationStatus {
                case .authorized, .provisional, .ephemeral:
                    return true
                default:
                    return false
                }
            }()
            guard allowed else { return }

            let content = UNMutableNotificationContent()
            content.title = title
            content.body = body
            content.sound = .default

            let request = UNNotificationRequest(
                identifier: "\(idPrefix)_\(UUID().uuidString)",
                content: content,
                trigger: nil
            )
            self.notificationCenter.add(request) { error in
                if let error {
                    print("[RuntimeNotifier] failed to post notification: \(error)")
                }
            }
        }
    }
}
