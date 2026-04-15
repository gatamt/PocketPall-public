import SwiftUI
import Combine
import GoogleSignIn

@main
struct PocketPallApp: App {
    @Environment(\.scenePhase) private var scenePhase

    @StateObject private var wifiManager: WiFiTransportManager
    @StateObject private var appState: AppState
    @StateObject private var runtimeCoordinator: PocketPallRuntimeCoordinator

    init() {
        let manager = WiFiTransportManager()
        let state = AppState()
        let coordinator = PocketPallRuntimeCoordinator(wifiManager: manager, appState: state)
        coordinator.start()
        _wifiManager = StateObject(wrappedValue: manager)
        _appState = StateObject(wrappedValue: state)
        _runtimeCoordinator = StateObject(
            wrappedValue: coordinator
        )
    }

    var body: some Scene {
        WindowGroup {
            NavigationStack(path: $appState.navigationPath) {
                ConnectView()
                    .navigationDestination(for: AppRoute.self) { route in
                        switch route {
                        case .savedDevices:
                            SavedDevicesView()
                        case .setup:
                            SetupView()
                        case .main:
                            MainView()
                        }
                    }
            }
            .environmentObject(wifiManager)
            .environmentObject(appState)
            .environmentObject(runtimeCoordinator)
            .preferredColorScheme(.dark)
            .onOpenURL { url in
                GIDSignIn.sharedInstance.handle(url)
            }
            .onAppear {
                runtimeCoordinator.handleScenePhase(scenePhase)
            }
            .onChange(of: scenePhase) { _, phase in
                runtimeCoordinator.handleScenePhase(phase)
            }
        }
    }
}

enum AppRoute: Hashable {
    case savedDevices
    case setup
    case main
}

enum AIProviderType: String, Hashable, CaseIterable {
    case gemini = "Gemini"
    case openai = "ChatGPT"
    case claude = "Claude"
}

class AppState: ObservableObject {
    @Published var navigationPath = NavigationPath()
    @Published var selectedProvider: AIProviderType?
    @Published private(set) var currentTopRoute: AppRoute?

    init() {
        selectedProvider = RuntimeProfileStore.shared.profile.selectedProvider
    }

    func navigateTo(_ route: AppRoute) {
        if currentTopRoute == route { return }
        navigationPath.append(route)
        currentTopRoute = route
    }

    func setRootRoute(_ route: AppRoute) {
        if currentTopRoute == route && navigationPath.count == 1 { return }
        navigationPath = NavigationPath()
        navigationPath.append(route)
        currentTopRoute = route
    }

    func popToRoot() {
        navigationPath = NavigationPath()
        currentTopRoute = nil
    }
}
