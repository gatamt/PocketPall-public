import Foundation
import UIKit
import WebKit
import GoogleSignIn

private enum GeminiLiveError: LocalizedError {
    case timeout(String)
    case server(String)
    case closed(code: Int, reason: String)

    var errorDescription: String? {
        switch self {
        case .timeout(let message):
            return message
        case .server(let message):
            return message
        case .closed(let code, let reason):
            return "WebSocket closed (code \(code)): \(reason)"
        }
    }
}

final class GeminiProvider: NSObject, AIProvider {
    let name = "Gemini"
    let providerType: AIProviderType = .gemini
    private(set) var isConnected = false

    var onAudioReceived: ((Data) -> Void)?
    var onTranscriptReceived: ((String) -> Void)?
    var onImageGenerated: ((UIImage) -> Void)?
    var onError: ((Error) -> Void)?
    var onStateChanged: ((AIState) -> Void)?

    private var webSocket: URLSessionWebSocketTask?
    private var session: URLSession?

    private var socketDidOpen = false
    private var didReceiveSetupComplete = false
    private var isReadyForInput = false
    private var isIntentionalClose = false
    private var didEmitFailure = false
    private var didLogNotReadyForAudio = false
    private var didLogAwaitingTurn = false
    private var didEmitRespondingForCurrentTurn = false
    private var isUserActivityOpen = false
    private var isAwaitingTurnResult = false
    private var activityStartedAt: Date?
    private var activityEndTask: Task<Void, Never>?
    private let activityStateQueue = DispatchQueue(label: "GeminiProvider.activityState")
    private var lastActivityStartSignalAt: Date = .distantPast
    private var openContinuation: CheckedContinuation<Void, Error>?
    private var setupContinuation: CheckedContinuation<Void, Error>?
    private var wsHealthTask: Task<Void, Never>?
    private let wsHealthQueue = DispatchQueue(label: "GeminiProvider.wsHealth")
    private var lastMessageAt: Date = .distantPast
    private var lastSuccessfulSendAt: Date = .distantPast
    private var appInBackground = false

    // MARK: - Persistent Browser
    @MainActor private var persistentWebView: WKWebView?
    @MainActor private var persistentWebViewDelegate: PersistentWebViewDelegate?
    @MainActor private var currentBrowserURL: URL?

    // MARK: - Video Streaming
    private var videoStreamer: VideoStreamer?
    weak var videoTransport: WiFiTransportManager?

    private enum ActivitySignal {
        case start
        case end
    }

    private let autoActivityEndSilenceSeconds: TimeInterval = 0.6
    private let maxTurnOpenSeconds: TimeInterval = 3.5
    private let wsHealthCheckSeconds: TimeInterval = 5.0
    private let wsHealthStallTimeoutSeconds: TimeInterval = 20.0

    // Google AI endpoint using API key auth
    private let model = "gemini-2.5-flash-native-audio-preview-12-2025"
    private let imageModel = "gemini-2.5-flash-image"  // Image generation model
    private let apiKeychainKey = "gemini_api_key"
    private var apiKey: String?

    // JavaScript that auto-dismisses cookie consent banners
    private static let cookieConsentDismissJS = """
    (function() {
        var clicked = false;

        // Phase 1: Known consent platform selectors
        var knownSelectors = [
            '#CybotCookiebotDialogBodyLevelButtonLevelOptinAllowAll',
            '#CybotCookiebotDialogBodyButtonAccept',
            '.coi-banner__accept',
            '#onetrust-accept-btn-handler',
            '.onetrust-close-btn-handler',
            '#accept-recommended-btn-handler',
            '.qc-cmp2-summary-buttons button[mode="primary"]',
            '.qc-cmp-button[data-cmp-button="accept"]',
            '#didomi-notice-agree-button',
            '.cc-btn.cc-allow',
            '.cc-accept-all',
            '[data-cookiefirst-action="accept"]',
            '#consent-accept-all',
            '.consent-accept-all',
            'ytd-consent-bump-v2-lightbox .eom-buttons button[aria-label*="Accept"]',
            'ytd-consent-bump-v2-lightbox .eom-buttons button[aria-label*="ccept"]',
            'tp-yt-paper-dialog .consent-bump-v2-lightbox button[aria-label*="ccept"]',
            'button[jsname="higCR"]',
            'form[action*="consent"] button[type="submit"]',
            '.consent-bump button.yt-spec-button-shape-next--call-to-action'
        ];
        for (var i = 0; i < knownSelectors.length; i++) {
            var el = document.querySelector(knownSelectors[i]);
            if (el && el.offsetParent !== null) {
                el.click();
                clicked = true;
                break;
            }
        }
        if (clicked) return 'dismissed_phase1';

        // Phase 2: Find buttons by visible text
        var acceptTexts = [
            'accepter alle', 'acceptér alle', 'godkend alle', 'tillad alle',
            'accept all', 'allow all', 'accept cookies', 'agree',
            'akzeptieren', 'alle akzeptieren', 'tout accepter'
        ];
        var buttons = document.querySelectorAll('button, a[role="button"], input[type="button"], input[type="submit"]');
        for (var j = 0; j < buttons.length; j++) {
            var btn = buttons[j];
            if (!btn.offsetParent && btn.offsetWidth === 0) continue;
            var txt = (btn.innerText || btn.value || '').toLowerCase().trim();
            for (var k = 0; k < acceptTexts.length; k++) {
                if (txt.indexOf(acceptTexts[k]) !== -1) {
                    btn.click();
                    return 'dismissed_phase2';
                }
            }
        }

        // Phase 3: Fallback — primary button inside cookie/consent overlays
        var overlays = document.querySelectorAll('[class*="cookie"], [class*="consent"], [class*="gdpr"], [id*="cookie"], [id*="consent"], [id*="gdpr"]');
        for (var m = 0; m < overlays.length; m++) {
            var overlay = overlays[m];
            var style = window.getComputedStyle(overlay);
            if (style.display === 'none' || style.visibility === 'hidden') continue;
            var primary = overlay.querySelector('button[class*="primary"], button[class*="accept"], button[class*="agree"], a[class*="primary"], a[class*="accept"]');
            if (primary && primary.offsetParent !== null) {
                primary.click();
                return 'dismissed_phase3';
            }
        }

        return 'no_banner_found';
    })();
    """

    private var wsBaseURL: String {
        "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent"
    }

    private var modelPath: String {
        "models/\(model)"
    }

    override init() {
        apiKey = KeychainService.shared.get(key: apiKeychainKey)
        super.init()
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(handleAppDidEnterBackground),
            name: UIApplication.didEnterBackgroundNotification,
            object: nil
        )
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(handleAppWillEnterForeground),
            name: UIApplication.willEnterForegroundNotification,
            object: nil
        )
    }

    func setAPIKey(_ key: String) {
        apiKey = key
        KeychainService.shared.set(key: apiKeychainKey, value: key)
    }

    func connect() async throws {
        guard let key = normalizedAPIKey else {
            print("[Gemini] connect() failed: missing API key")
            throw AIProviderError.notAuthenticated
        }
        print("[Gemini] connect() — validating API key...")
        try await validateAPIKey(key: key)

        isConnected = true
    }

    func disconnect() {
        closeConnection(closeCode: .normalClosure, reason: "disconnect", emitIdleState: false)
        isConnected = false
    }

    func startVoiceSession() async throws {
        guard let key = normalizedAPIKey else {
            throw AIProviderError.notAuthenticated
        }

        try await validateAPIKey(key: key)

        resetSessionStateForStart()

        let wsURL = try makeWebSocketURL(apiKey: key)
        print("[Gemini] Starting WebSocket to Google AI: \(wsURL)")
        let config = URLSessionConfiguration.default
        config.waitsForConnectivity = true
        session = URLSession(configuration: config, delegate: self, delegateQueue: nil)

        var request = URLRequest(url: wsURL)
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")

        webSocket = session?.webSocketTask(with: request)
        webSocket?.resume()
        receiveMessages()

        do {
            try await waitForSocketOpen()
            try await sendSetupMessage()
            try await waitForSetupComplete()

            isReadyForInput = true
            didLogNotReadyForAudio = false
            markWSSendProgress()
            startWSHealthMonitor()
            onStateChanged?(.listening)
            print("[Gemini] Voice session started, state=listening")
        } catch {
            handleConnectionFailure(error)
            throw error
        }
    }

    private var sendCount = 0

    func sendAudio(_ pcmData: Data) async throws {
        guard isReadyForInput, let ws = webSocket else {
            if !didLogNotReadyForAudio {
                print("[Gemini] sendAudio ignored (session not ready yet)")
                didLogNotReadyForAudio = true
            }
            return
        }

        didLogNotReadyForAudio = false

        if isAwaitingTurnResult {
            if !didLogAwaitingTurn {
                print("[Gemini] sendAudio dropped (awaiting turn result)")
                didLogAwaitingTurn = true
            }
            return
        }
        didLogAwaitingTurn = false

        // With manual turn control enabled, ensure activity is open before audio.
        if !isUserActivityOpen {
            try await sendActivitySignal(.start)
        }

        if let startedAt = activityStartedAt,
           Date().timeIntervalSince(startedAt) >= maxTurnOpenSeconds {
            print("[Gemini] Max turn duration reached (\(maxTurnOpenSeconds)s), forcing activityEnd")
            try await sendActivitySignal(.end)
            return
        }

        let base64Audio = pcmData.base64EncodedString()
        let message: [String: Any] = [
            "realtimeInput": [
                "audio": [
                    "mimeType": "audio/pcm;rate=16000",
                    "data": base64Audio
                ]
            ]
        ]

        do {
            let jsonString = try toJSONString(message)
            try await ws.send(.string(jsonString))
            markWSSendProgress()
            scheduleAutoActivityEnd()
            sendCount += 1
            if sendCount == 1 || sendCount % 100 == 0 {
                print("[Gemini] Sent audio chunk #\(sendCount) (\(pcmData.count) bytes PCM)")
            }
        } catch {
            handleConnectionFailure(error)
            throw error
        }
    }

    func stopVoiceSession() {
        closeConnection(closeCode: .goingAway, reason: "stop_voice_session", emitIdleState: true)
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
        wsHealthTask?.cancel()
    }

    // MARK: - Google Sign-In

    func signIn(presenting viewController: UIViewController) async throws {
        print("[Gemini] signIn() — optional Google sign-in")

        let result = try await GIDSignIn.sharedInstance.signIn(
            withPresenting: viewController,
            hint: nil,
            additionalScopes: []
        )

        print("[Gemini] Sign-in successful, user: \(result.user.profile?.email ?? "?")")
    }

    var isSignedIn: Bool {
        GIDSignIn.sharedInstance.currentUser != nil
    }

    func signOut() {
    }

    func beginUserActivity() {
        Task { [weak self] in
            guard let self else { return }
            do {
                try await self.sendActivitySignal(.start)
            } catch {
                print("[Gemini] beginUserActivity failed: \(error)")
            }
        }
    }

    func endUserActivity() {
        Task { [weak self] in
            guard let self else { return }
            do {
                try await self.sendActivitySignal(.end)
            } catch {
                print("[Gemini] endUserActivity failed: \(error)")
            }
        }
    }

    // MARK: - Private

    private func resetSessionStateForStart() {
        didEmitFailure = false
        isIntentionalClose = false
        socketDidOpen = false
        didReceiveSetupComplete = false
        isReadyForInput = false
        didLogNotReadyForAudio = false
        didLogAwaitingTurn = false
        didEmitRespondingForCurrentTurn = false
        isUserActivityOpen = false
        isAwaitingTurnResult = false
        activityStartedAt = nil
        activityEndTask?.cancel()
        activityEndTask = nil
        lastActivityStartSignalAt = .distantPast
        sendCount = 0
        responseCount = 0
        openContinuation = nil
        setupContinuation = nil
        wsHealthTask?.cancel()
        wsHealthTask = nil
        wsHealthQueue.sync {
            lastMessageAt = .distantPast
            lastSuccessfulSendAt = .distantPast
        }
        videoStreamer?.stop()
        videoStreamer = nil
        Task { @MainActor [weak self] in
            self?.persistentWebView?.stopLoading()
            self?.persistentWebView = nil
            self?.persistentWebViewDelegate = nil
            self?.currentBrowserURL = nil
        }
    }

    private func closeConnection(closeCode: URLSessionWebSocketTask.CloseCode, reason: String, emitIdleState: Bool) {
        isIntentionalClose = true
        isReadyForInput = false
        didReceiveSetupComplete = false
        socketDidOpen = false
        didLogAwaitingTurn = false
        didEmitRespondingForCurrentTurn = false
        isUserActivityOpen = false
        isAwaitingTurnResult = false
        activityStartedAt = nil
        activityEndTask?.cancel()
        activityEndTask = nil
        lastActivityStartSignalAt = .distantPast

        let closeError = GeminiLiveError.closed(code: closeCode.rawValue, reason: reason)
        resumeOpenContinuation(with: .failure(closeError))
        resumeSetupContinuation(with: .failure(closeError))

        let reasonData = reason.data(using: .utf8)
        webSocket?.cancel(with: closeCode, reason: reasonData)
        webSocket = nil
        session?.invalidateAndCancel()
        session = nil
        wsHealthTask?.cancel()
        wsHealthTask = nil
        didEmitFailure = false
        sendCount = 0
        responseCount = 0

        videoStreamer?.stop()
        videoStreamer = nil

        Task { @MainActor [weak self] in
            self?.persistentWebView?.stopLoading()
            self?.persistentWebView = nil
            self?.persistentWebViewDelegate = nil
            self?.currentBrowserURL = nil
        }

        if emitIdleState {
            onStateChanged?(.idle)
        }
    }

    private func handleConnectionFailure(_ error: Error) {
        if didEmitFailure {
            return
        }
        didEmitFailure = true

        isReadyForInput = false
        didReceiveSetupComplete = false
        socketDidOpen = false
        didLogAwaitingTurn = false
        didEmitRespondingForCurrentTurn = false
        isUserActivityOpen = false
        isAwaitingTurnResult = false
        activityStartedAt = nil
        activityEndTask?.cancel()
        activityEndTask = nil
        lastActivityStartSignalAt = .distantPast

        resumeOpenContinuation(with: .failure(error))
        resumeSetupContinuation(with: .failure(error))

        if !isIntentionalClose {
            webSocket?.cancel(with: .goingAway, reason: nil)
        }
        webSocket = nil
        session?.invalidateAndCancel()
        session = nil
        wsHealthTask?.cancel()
        wsHealthTask = nil

        videoStreamer?.stop()
        videoStreamer = nil

        Task { @MainActor [weak self] in
            self?.persistentWebView?.stopLoading()
            self?.persistentWebView = nil
            self?.persistentWebViewDelegate = nil
            self?.currentBrowserURL = nil
        }

        onError?(error)
        onStateChanged?(.error)
    }

    private func sendSetupMessage() async throws {
        guard let ws = webSocket else {
            throw AIProviderError.connectionFailed
        }

        let setup: [String: Any] = [
            "setup": [
                "model": modelPath,
                "generationConfig": [
                    "responseModalities": ["AUDIO"],
                    "speechConfig": [
                        "voiceConfig": [
                            "prebuiltVoiceConfig": [
                                "voiceName": "Aoede"
                            ]
                        ]
                    ]
                ],
                "systemInstruction": [
                    "parts": [
                        [
                            "text": "You are PocketPall, a friendly personal AI assistant. You MUST respond in the same language the user speaks to you. If the user speaks English, respond in English. If the user speaks Danish, respond in Danish. If the user speaks any other language, match that language. Your default language is English. Keep your responses short and natural, suitable for voice conversation. For realtime facts, use tools before answering. You have Google Search built-in for quick factual queries. If a URL is provided, use fetch_web_data with follow_links=true and enough max_pages to reach the relevant subpage. If no URL is provided, use search_web first, then fetch_web_data on the best result. When fetch_web_data returns available_links, you can call fetch_web_data again with any of those URLs to navigate further into the site. If the user wants to SEE a webpage visually, use get_webpage_screenshot to display it on their device screen. Report extracted values only after tool output. VIDEO PLAYBACK: When the user asks to watch, play, or see a video, ALWAYS use the play_video tool. Do NOT browse YouTube interactively with screenshots and clicks. Instead: 1) Use search_web to find a YouTube video URL (youtube.com/watch?v=...), 2) Call play_video with that URL directly. The play_video tool handles YouTube URLs natively — it streams video frames to the device display and plays audio. BROWSER INTERACTION: You have a persistent browser session. After get_webpage_screenshot, the page stays loaded with cookies and login state preserved. Use click_element to click buttons, dismiss popups, or navigate (provide CSS selector OR visible text). Use scroll_page to scroll up/down/top/bottom. Cookie consent banners are auto-dismissed, but if one persists, use click_element to dismiss it. Always describe what you see in the screenshot to the user."
                        ]
                    ]
                ],
                "realtimeInputConfig": [
                    "automaticActivityDetection": [
                        "disabled": true
                    ]
                ],
                "inputAudioTranscription": [:],
                "outputAudioTranscription": [:],
                "tools": [
                    ["google_search": [:]],
                    [
                        "functionDeclarations": [
                            [
                                "name": "generate_image",
                                "description": "Generate an image based on a text description. Use this when the user asks you to create, draw, or generate a picture or image.",
                                "parameters": [
                                    "type": "OBJECT",
                                    "properties": [
                                        "prompt": [
                                            "type": "STRING",
                                            "description": "A detailed description of the image to generate"
                                        ]
                                    ],
                                    "required": ["prompt"]
                                ]
                            ],
                            [
                                "name": "fetch_web_data",
                                "description": "Fetch content from a website URL. Returns text, images, and available links for navigation. Supports JS-rendered pages. Call again with a link URL from available_links to navigate further.",
                                "parameters": [
                                    "type": "OBJECT",
                                    "properties": [
                                        "url": [
                                            "type": "STRING",
                                            "description": "The full website URL to fetch"
                                        ],
                                        "extraction_query": [
                                            "type": "STRING",
                                            "description": "The exact data the user wants extracted from the page"
                                        ],
                                        "max_chars": [
                                            "type": "INTEGER",
                                            "description": "Optional max characters to keep from page text (2000-50000)"
                                        ],
                                        "follow_links": [
                                            "type": "BOOLEAN",
                                            "description": "If true, follow and inspect relevant links from the page to locate the requested data."
                                        ],
                                        "max_pages": [
                                            "type": "INTEGER",
                                            "description": "Maximum number of pages to inspect when follow_links is true (1-8)."
                                        ],
                                        "same_domain_only": [
                                            "type": "BOOLEAN",
                                            "description": "If true, only follow links on the same domain as url."
                                        ]
                                    ],
                                    "required": ["url", "extraction_query"]
                                ]
                            ],
                            [
                                "name": "search_web",
                                "description": "Search the web for relevant pages when the user asks for realtime data but no working URL is known.",
                                "parameters": [
                                    "type": "OBJECT",
                                    "properties": [
                                        "query": [
                                            "type": "STRING",
                                            "description": "The search query, ideally including the exact metric/data needed."
                                        ],
                                        "site": [
                                            "type": "STRING",
                                            "description": "Optional domain constraint, for example tv2.dk."
                                        ],
                                        "max_results": [
                                            "type": "INTEGER",
                                            "description": "Maximum number of search results to return (1-10)."
                                        ]
                                    ],
                                    "required": ["query"]
                                ]
                            ],
                            [
                                "name": "get_webpage_screenshot",
                                "description": "Take a visual screenshot of a webpage and display it on the device screen. Use this when the user wants to SEE a website visually, not just read its text content.",
                                "parameters": [
                                    "type": "OBJECT",
                                    "properties": [
                                        "url": [
                                            "type": "STRING",
                                            "description": "The full website URL to screenshot"
                                        ]
                                    ],
                                    "required": ["url"]
                                ]
                            ],
                            [
                                "name": "click_element",
                                "description": "Click an element on the currently loaded webpage. The browser session is persistent, so this works on the page from the last get_webpage_screenshot. Provide EITHER a CSS selector OR visible text to identify the element. A new screenshot is automatically taken after clicking.",
                                "parameters": [
                                    "type": "OBJECT",
                                    "properties": [
                                        "selector": [
                                            "type": "STRING",
                                            "description": "CSS selector for the element to click (e.g. 'button.primary', '#submit-btn', 'a[href=\"/about\"]')"
                                        ],
                                        "text": [
                                            "type": "STRING",
                                            "description": "Visible text of the element to click (e.g. 'Accept all', 'Next page', 'Log in')"
                                        ]
                                    ]
                                ]
                            ],
                            [
                                "name": "scroll_page",
                                "description": "Scroll the currently loaded webpage. Use after get_webpage_screenshot to see more content. A new screenshot is automatically taken after scrolling.",
                                "parameters": [
                                    "type": "OBJECT",
                                    "properties": [
                                        "direction": [
                                            "type": "STRING",
                                            "description": "Scroll direction: 'up' or 'down'"
                                        ],
                                        "amount": [
                                            "type": "STRING",
                                            "description": "How much to scroll: 'page' (full page), 'half' (half page), 'top' (to top), 'bottom' (to bottom)"
                                        ]
                                    ],
                                    "required": ["direction", "amount"]
                                ]
                            ],
                            [
                                "name": "play_video",
                                "description": "Play a video from a URL. The video frames will be streamed to the device display and audio to its speaker. Use this when the user asks to watch or play a video. Supports YouTube URLs (youtube.com, youtu.be) and direct video file URLs (.mp4, .m3u8, etc.).",
                                "parameters": [
                                    "type": "OBJECT",
                                    "properties": [
                                        "url": [
                                            "type": "STRING",
                                            "description": "URL to the video. Can be a YouTube URL (e.g. https://www.youtube.com/watch?v=...) or a direct video file URL (.mp4, .m3u8)"
                                        ],
                                        "title": [
                                            "type": "STRING",
                                            "description": "Optional title of the video for display"
                                        ]
                                    ],
                                    "required": ["url"]
                                ]
                            ],
                            [
                                "name": "stop_video",
                                "description": "Stop the currently playing video and return to normal mode.",
                                "parameters": [
                                    "type": "OBJECT",
                                    "properties": [:] as [String: Any],
                                    "required": [] as [String]
                                ]
                            ]
                        ]
                    ]
                ]
            ]
        ]

        let jsonString = try toJSONString(setup)
        try await ws.send(.string(jsonString))
        markWSSendProgress()
        print("[Gemini] Setup message sent: \(jsonString.prefix(180))...")
    }

    private var normalizedAPIKey: String? {
        let trimmed = (apiKey ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }

    private func makeWebSocketURL(apiKey: String) throws -> URL {
        guard var components = URLComponents(string: wsBaseURL) else {
            throw AIProviderError.connectionFailed
        }
        components.queryItems = [URLQueryItem(name: "key", value: apiKey)]
        guard let url = components.url else {
            throw AIProviderError.connectionFailed
        }
        return url
    }

    private func validateAPIKey(key: String) async throws {
        guard var components = URLComponents(string: "https://generativelanguage.googleapis.com/v1beta/models") else {
            throw AIProviderError.connectionFailed
        }
        components.queryItems = [URLQueryItem(name: "key", value: key)]
        guard let url = components.url else {
            throw AIProviderError.connectionFailed
        }

        var request = URLRequest(url: url)
        request.httpMethod = "GET"

        let (data, response) = try await URLSession.shared.data(for: request)
        guard let httpResponse = response as? HTTPURLResponse else {
            throw AIProviderError.connectionFailed
        }

        print("[Gemini] API key validate response: \(httpResponse.statusCode)")
        guard httpResponse.statusCode == 200 else {
            let body = String(data: data, encoding: .utf8)?
                .replacingOccurrences(of: "\n", with: " ")
                .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            let bodySnippet = String(body.prefix(220))
            print("[Gemini] API key validate failed body: \(bodySnippet)")

            switch httpResponse.statusCode {
            case 401, 403:
                throw AIProviderError.invalidCredentials
            case 404:
                throw AIProviderError.apiError(
                    "Gemini API endpoint was not found."
                )
            default:
                let fallback = bodySnippet.isEmpty ? "No response body" : bodySnippet
                throw AIProviderError.apiError("Google AI key validation failed (\(httpResponse.statusCode)): \(fallback)")
            }
        }
    }

    private func waitForSocketOpen() async throws {
        if socketDidOpen {
            return
        }
        try await withTimeout(seconds: 8, label: "WebSocket open timeout") {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                self.openContinuation = continuation
            }
        }
    }

    private func waitForSetupComplete() async throws {
        if didReceiveSetupComplete {
            return
        }
        try await withTimeout(seconds: 10, label: "Gemini setupComplete timeout") {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                self.setupContinuation = continuation
            }
        }
    }

    private func withTimeout<T>(seconds: TimeInterval, label: String, operation: @escaping () async throws -> T) async throws -> T {
        try await withThrowingTaskGroup(of: T.self) { group in
            group.addTask {
                try await operation()
            }
            group.addTask {
                let nanos = UInt64(seconds * 1_000_000_000)
                try await Task.sleep(nanoseconds: nanos)
                throw GeminiLiveError.timeout(label)
            }

            guard let first = try await group.next() else {
                throw GeminiLiveError.timeout(label)
            }
            group.cancelAll()
            return first
        }
    }

    private func toJSONString(_ object: [String: Any]) throws -> String {
        let data = try JSONSerialization.data(withJSONObject: object)
        guard let jsonString = String(data: data, encoding: .utf8) else {
            throw AIProviderError.connectionFailed
        }
        return jsonString
    }

    private func sendActivitySignal(_ signal: ActivitySignal) async throws {
        guard isReadyForInput, let ws = webSocket else {
            return
        }

        let now = Date()
        var shouldSend = false
        var key = ""

        activityStateQueue.sync {
            switch signal {
            case .start:
                if isUserActivityOpen {
                    return
                }
                if now.timeIntervalSince(lastActivityStartSignalAt) < 0.2 {
                    return
                }

                isUserActivityOpen = true
                isAwaitingTurnResult = false
                activityStartedAt = now
                activityEndTask?.cancel()
                activityEndTask = nil
                lastActivityStartSignalAt = now
                key = "activityStart"
                shouldSend = true

            case .end:
                if !isUserActivityOpen {
                    return
                }

                isUserActivityOpen = false
                isAwaitingTurnResult = true
                activityStartedAt = nil
                activityEndTask?.cancel()
                activityEndTask = nil
                key = "activityEnd"
                shouldSend = true
            }
        }

        guard shouldSend else { return }

        let message: [String: Any] = [
            "realtimeInput": [
                key: [:]
            ]
        ]
        let jsonString = try toJSONString(message)
        do {
            try await ws.send(.string(jsonString))
            markWSSendProgress()
            if signal == .start {
                print("[Gemini] Sent activityStart")
            } else {
                print("[Gemini] Sent activityEnd")
            }
        } catch {
            // Roll back optimistic state if signal send failed.
            activityStateQueue.sync {
                if signal == .start {
                    isUserActivityOpen = false
                    activityStartedAt = nil
                } else {
                    isAwaitingTurnResult = false
                }
            }
            throw error
        }
    }

    private func scheduleAutoActivityEnd() {
        activityEndTask?.cancel()
        guard isUserActivityOpen else { return }

        let nanos = UInt64(autoActivityEndSilenceSeconds * 1_000_000_000)
        activityEndTask = Task { [weak self] in
            guard let self else { return }
            do {
                try await Task.sleep(nanoseconds: nanos)
                if Task.isCancelled { return }
                try await self.sendActivitySignal(.end)
                print("[Gemini] Auto activityEnd after silence timeout")
            } catch {
                // Ignore cancellation and transient send failures.
            }
        }
    }

    private func resumeOpenContinuation(with result: Result<Void, Error>) {
        guard let continuation = openContinuation else { return }
        openContinuation = nil
        continuation.resume(with: result)
    }

    private func resumeSetupContinuation(with result: Result<Void, Error>) {
        guard let continuation = setupContinuation else { return }
        setupContinuation = nil
        continuation.resume(with: result)
    }

    private func receiveMessages() {
        guard let ws = webSocket else { return }
        ws.receive { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let message):
                self.handleMessage(message)
                self.receiveMessages()
            case .failure(let error):
                print("[Gemini] WebSocket receive error: \(error)")
                self.handleConnectionFailure(error)
            }
        }
    }

    private var responseCount = 0

    private func handleMessage(_ message: URLSessionWebSocketTask.Message) {
        markMessageProgress()
        responseCount += 1
        if responseCount <= 5 {
            print("[Gemini] Received message #\(responseCount)")
        }
        switch message {
        case .data(let data):
            parseResponse(data)
        case .string(let text):
            if responseCount <= 5 {
                print("[Gemini] Message text (first 300): \(text.prefix(300))")
            }
            if let data = text.data(using: .utf8) {
                parseResponse(data)
            }
        @unknown default:
            break
        }
    }

    private func parseResponse(_ data: Data) {
        guard let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            print("[Gemini] Failed to parse response JSON (\(data.count) bytes)")
            return
        }

        if json["setupComplete"] != nil || json["setup_complete"] != nil {
            didReceiveSetupComplete = true
            resumeSetupContinuation(with: .success(()))
            print("[Gemini] setupComplete received")
        }

        if let errorPayload = json["error"] as? [String: Any] {
            let message = (errorPayload["message"] as? String)
                ?? (errorPayload["status"] as? String)
                ?? "Unknown Gemini server error"
            handleConnectionFailure(GeminiLiveError.server(message))
            return
        }

        if let goAway = (json["goAway"] as? [String: Any]) ?? (json["go_away"] as? [String: Any]) {
            let message = (goAway["reason"] as? String)
                ?? (goAway["message"] as? String)
                ?? "Server requested disconnect"
            handleConnectionFailure(GeminiLiveError.server("goAway: \(message)"))
            return
        }

        if responseCount <= 5 {
            let keys = json.keys.joined(separator: ", ")
            print("[Gemini] Response keys: \(keys)")
        }

        let serverContent = (json["serverContent"] as? [String: Any])
            ?? (json["server_content"] as? [String: Any])
        if let serverContent {
            if let modelTurn = (serverContent["modelTurn"] as? [String: Any]) ?? (serverContent["model_turn"] as? [String: Any]),
               let parts = modelTurn["parts"] as? [[String: Any]] {
                for part in parts {
                    let inlineData = (part["inlineData"] as? [String: Any]) ?? (part["inline_data"] as? [String: Any])
                    if let inlineData,
                       let mimeType = (inlineData["mimeType"] as? String) ?? (inlineData["mime_type"] as? String),
                       mimeType.hasPrefix("audio/"),
                       let b64 = inlineData["data"] as? String,
                       let audioData = Data(base64Encoded: b64) {
                        if !didEmitRespondingForCurrentTurn {
                            didEmitRespondingForCurrentTurn = true
                            onStateChanged?(.responding)
                        }
                        // Keep BLE bursts small to avoid long write stalls.
                        let chunkSize = 1920
                        if audioData.count > chunkSize {
                            var offset = 0
                            while offset < audioData.count {
                                let end = min(offset + chunkSize, audioData.count)
                                onAudioReceived?(audioData.subdata(in: offset..<end))
                                offset = end
                            }
                        } else {
                            onAudioReceived?(audioData)
                        }
                    }
                    if let text = part["text"] as? String {
                        onTranscriptReceived?(text)
                    }
                }
            }

            let outputTx = (serverContent["outputTranscription"] as? [String: Any])
                ?? (serverContent["output_transcription"] as? [String: Any])
            if let text = outputTx?["text"] as? String, !text.isEmpty {
                onTranscriptReceived?(text)
            }

            let turnComplete = (serverContent["turnComplete"] as? Bool) ?? (serverContent["turn_complete"] as? Bool)
            if turnComplete == true {
                didEmitRespondingForCurrentTurn = false
                isUserActivityOpen = false
                isAwaitingTurnResult = false
                didLogAwaitingTurn = false
                activityStartedAt = nil
                activityEndTask?.cancel()
                activityEndTask = nil
                lastActivityStartSignalAt = .distantPast
                onStateChanged?(.listening)
            }
        }

        let toolCall = (json["toolCall"] as? [String: Any]) ?? (json["tool_call"] as? [String: Any])
        if let toolCall,
           let functionCalls = (toolCall["functionCalls"] as? [[String: Any]]) ?? (toolCall["function_calls"] as? [[String: Any]]) {
            for call in functionCalls {
                let callId = call["id"] as? String ?? ""
                if let name = call["name"] as? String,
                   let args = call["args"] as? [String: Any] {
                    if name == "generate_image",
                       let prompt = args["prompt"] as? String {
                        print("[Gemini] Tool call: generate_image(prompt: \(prompt.prefix(60))) id=\(callId)")
                        Task { await generateImage(prompt: prompt, callId: callId) }
                    } else if name == "fetch_web_data",
                              let url = args["url"] as? String,
                              let extractionQuery = args["extraction_query"] as? String {
                        let maxChars = args["max_chars"] as? Int
                        let followLinks = args["follow_links"] as? Bool
                        let maxPages = args["max_pages"] as? Int
                        let sameDomainOnly = args["same_domain_only"] as? Bool
                        print("[Gemini] Tool call: fetch_web_data(url: \(url.prefix(80)), query: \(extractionQuery.prefix(60)), follow_links: \(followLinks?.description ?? "default"), max_pages: \(maxPages?.description ?? "default")) id=\(callId)")
                        Task {
                            await fetchWebData(
                                urlString: url,
                                extractionQuery: extractionQuery,
                                maxChars: maxChars,
                                followLinks: followLinks,
                                maxPages: maxPages,
                                sameDomainOnly: sameDomainOnly,
                                callId: callId
                            )
                        }
                    } else if name == "search_web",
                              let query = args["query"] as? String {
                        let site = args["site"] as? String
                        let maxResults = args["max_results"] as? Int
                        let siteLabel = site.map { String($0.prefix(60)) } ?? "none"
                        print("[Gemini] Tool call: search_web(query: \(query.prefix(80)), site: \(siteLabel), max_results: \(maxResults?.description ?? "default")) id=\(callId)")
                        Task {
                            await searchWeb(
                                query: query,
                                site: site,
                                maxResults: maxResults,
                                callId: callId
                            )
                        }
                    } else if name == "get_webpage_screenshot",
                              let url = args["url"] as? String {
                        print("[Gemini] Tool call: get_webpage_screenshot(url: \(url.prefix(80))) id=\(callId)")
                        Task { await takeWebpageScreenshot(urlString: url, callId: callId) }
                    } else if name == "click_element" {
                        let selector = args["selector"] as? String
                        let text = args["text"] as? String
                        print("[Gemini] Tool call: click_element(selector: \(selector ?? "nil"), text: \(text ?? "nil")) id=\(callId)")
                        Task { await clickElement(selector: selector, text: text, callId: callId) }
                    } else if name == "scroll_page" {
                        let direction = args["direction"] as? String ?? "down"
                        let amount = args["amount"] as? String ?? "page"
                        print("[Gemini] Tool call: scroll_page(direction: \(direction), amount: \(amount)) id=\(callId)")
                        Task { await scrollPage(direction: direction, amount: amount, callId: callId) }
                    } else if name == "play_video",
                              let url = args["url"] as? String {
                        let title = args["title"] as? String
                        print("[Gemini] Tool call: play_video(url: \(url.prefix(80)), title: \(title ?? "nil")) id=\(callId)")
                        Task { await handlePlayVideo(urlString: url, title: title, callId: callId) }
                    } else if name == "stop_video" {
                        print("[Gemini] Tool call: stop_video() id=\(callId)")
                        Task { await handleStopVideo(callId: callId) }
                    }
                }
            }
        }
    }

    private func startWSHealthMonitor() {
        wsHealthTask?.cancel()
        wsHealthTask = Task { [weak self] in
            guard let self else { return }
            while !Task.isCancelled {
                let nanos = UInt64(wsHealthCheckSeconds * 1_000_000_000)
                try? await Task.sleep(nanoseconds: nanos)
                if Task.isCancelled { return }

                guard isReadyForInput, webSocket != nil else { continue }
                guard shouldRunBackgroundHealthCheck() else { continue }

                let snapshot = wsHealthQueue.sync {
                    (self.lastMessageAt, self.lastSuccessfulSendAt)
                }
                let latest = max(snapshot.0, snapshot.1)
                if latest == .distantPast {
                    continue
                }
                let idle = Date().timeIntervalSince(latest)
                if idle > wsHealthStallTimeoutSeconds {
                    print("[Gemini] WS health timeout in background (\(Int(idle))s idle), reconnecting")
                    handleConnectionFailure(GeminiLiveError.timeout("background websocket stalled"))
                    return
                }
            }
        }
    }

    private func shouldRunBackgroundHealthCheck() -> Bool {
        let inBackground = wsHealthQueue.sync { appInBackground }
        guard inBackground else { return false }
        return activityStateQueue.sync {
            isUserActivityOpen || isAwaitingTurnResult
        }
    }

    private func markMessageProgress() {
        wsHealthQueue.sync {
            lastMessageAt = Date()
        }
    }

    private func markWSSendProgress() {
        wsHealthQueue.sync {
            lastSuccessfulSendAt = Date()
        }
    }

    @objc
    private func handleAppDidEnterBackground() {
        wsHealthQueue.sync {
            appInBackground = true
        }
    }

    @objc
    private func handleAppWillEnterForeground() {
        wsHealthQueue.sync {
            appInBackground = false
        }
    }

    private func generateImage(prompt: String, callId: String) async {
        guard let key = normalizedAPIKey,
              var components = URLComponents(string: "https://generativelanguage.googleapis.com/v1beta/models/\(imageModel):generateContent") else {
            await sendToolResponse(callId: callId, functionName: "generate_image", result: ["error": "Configuration error"])
            return
        }
        components.queryItems = [URLQueryItem(name: "key", value: key)]
        guard let url = components.url else {
            await sendToolResponse(callId: callId, functionName: "generate_image", result: ["error": "Invalid URL"])
            return
        }

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.timeoutInterval = 30

        let body: [String: Any] = [
            "contents": [["parts": [["text": "Generate an image: \(prompt)"]]]],
            "generationConfig": [
                "responseModalities": ["IMAGE", "TEXT"]
            ]
        ]
        request.httpBody = try? JSONSerialization.data(withJSONObject: body)

        do {
            print("[Gemini] Generating image with \(imageModel)...")
            let (data, response) = try await URLSession.shared.data(for: request)

            if let httpResp = response as? HTTPURLResponse, httpResp.statusCode != 200 {
                let bodySnippet = String(data: data, encoding: .utf8)?.prefix(200) ?? ""
                print("[Gemini] Image generation failed (\(httpResp.statusCode)): \(bodySnippet)")
                await sendToolResponse(callId: callId, functionName: "generate_image", result: ["error": "Image generation failed (\(httpResp.statusCode))"])
                return
            }

            var generatedImage = false
            if let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
               let candidates = json["candidates"] as? [[String: Any]],
               let parts = (candidates.first?["content"] as? [String: Any])?["parts"] as? [[String: Any]] {
                for part in parts {
                    let inlineData = (part["inlineData"] as? [String: Any]) ?? (part["inline_data"] as? [String: Any])
                    if let inlineData,
                       let b64 = inlineData["data"] as? String,
                       let imageData = Data(base64Encoded: b64),
                       let image = UIImage(data: imageData) {
                        print("[Gemini] Image generated successfully: \(image.size)")
                        onImageGenerated?(image)
                        generatedImage = true
                    }
                }
            }

            if generatedImage {
                await sendToolResponse(callId: callId, functionName: "generate_image", result: ["status": "success", "message": "Image generated and sent to device display"])
            } else {
                print("[Gemini] No image data found in response")
                await sendToolResponse(callId: callId, functionName: "generate_image", result: ["error": "No image data in response"])
            }
        } catch {
            print("[Gemini] Image generation error: \(error)")
            onError?(error)
            await sendToolResponse(callId: callId, functionName: "generate_image", result: ["error": error.localizedDescription])
        }
    }

    private func fetchWebData(
        urlString: String,
        extractionQuery: String,
        maxChars: Int?,
        followLinks: Bool?,
        maxPages: Int?,
        sameDomainOnly: Bool?,
        callId: String
    ) async {
        let trimmedURL = urlString.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let url = URL(string: trimmedURL),
              let scheme = url.scheme?.lowercased(),
              scheme == "https" || scheme == "http" else {
            await sendToolResponse(callId: callId, functionName: "fetch_web_data", result: [
                "error": "Invalid URL. Please provide a full http/https URL."
            ])
            return
        }

        let desiredMaxChars = min(max(maxChars ?? 15000, 2000), 50000)
        let shouldFollowLinks = followLinks ?? true
        let desiredMaxPages = min(max(maxPages ?? 4, 1), 8)
        let restrictToSameDomain = sameDomainOnly ?? true

        // Try Jina Reader first for better JS support and markdown output
        if let jinaResult = await fetchViaJinaReader(url: url, maxChars: desiredMaxChars) {
            var result: [String: Any] = [
                "status": "success",
                "url": trimmedURL,
                "extraction_query": extractionQuery,
                "source": "jina_reader"
            ]
            if let title = jinaResult["title"] as? String {
                result["title"] = title
            }
            if let content = jinaResult["content"] as? String {
                let truncated = String(content.prefix(desiredMaxChars))
                let snippets = extractRelevantSnippets(from: truncated, query: extractionQuery, maxSnippets: 8)
                result["snippets"] = snippets
                result["source_excerpt"] = String(truncated.prefix(3000))
            }
            if let links = jinaResult["links"] as? [String: String] {
                let topLinks = Array(links.prefix(15)).map { ["text": $0.key, "url": $0.value] }
                result["available_links"] = topLinks
            }
            if let images = jinaResult["images"] as? [String: String] {
                let topImages = Array(images.prefix(10)).map { ["alt": $0.key, "url": $0.value] }
                result["images"] = topImages
            }

            // If follow_links requested and Jina returned links, offer to crawl further
            if shouldFollowLinks, let links = jinaResult["links"] as? [String: String], !links.isEmpty {
                result["navigation_hint"] = "Links are available. Call fetch_web_data again with any of the available_links URLs to navigate further."
            }

            await sendToolResponse(callId: callId, functionName: "fetch_web_data", result: result)
            return
        }

        // Fallback to original implementation
        print("[Gemini] Jina Reader failed, falling back to direct fetch for \(trimmedURL)")

        if shouldFollowLinks {
            let crawlResult = await crawlWebsite(
                startURL: url,
                extractionQuery: extractionQuery,
                maxCharsPerPage: desiredMaxChars,
                maxPages: desiredMaxPages,
                sameDomainOnly: restrictToSameDomain
            )
            await sendToolResponse(
                callId: callId,
                functionName: "fetch_web_data",
                result: crawlResult
            )
            return
        }

        var request = URLRequest(url: url)
        request.httpMethod = "GET"
        request.timeoutInterval = 20
        request.setValue("PocketPall/2.0 (+mobile tool fetch)", forHTTPHeaderField: "User-Agent")

        do {
            let (data, response) = try await URLSession.shared.data(for: request)
            guard let httpResponse = response as? HTTPURLResponse else {
                await sendToolResponse(callId: callId, functionName: "fetch_web_data", result: [
                    "error": "No HTTP response from website."
                ])
                return
            }

            guard (200...299).contains(httpResponse.statusCode) else {
                let bodySnippet = String(data: data, encoding: .utf8)?
                    .replacingOccurrences(of: "\n", with: " ")
                    .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
                await sendToolResponse(callId: callId, functionName: "fetch_web_data", result: [
                    "error": "Website returned status \(httpResponse.statusCode).",
                    "body_snippet": String(bodySnippet.prefix(240))
                ])
                return
            }

            let contentType = (httpResponse.value(forHTTPHeaderField: "Content-Type") ?? "").lowercased()
            let sourceText: String
            if contentType.contains("application/json") {
                if let obj = try? JSONSerialization.jsonObject(with: data),
                   let jsonData = try? JSONSerialization.data(withJSONObject: obj, options: [.prettyPrinted]),
                   let jsonText = String(data: jsonData, encoding: .utf8) {
                    sourceText = jsonText
                } else {
                    sourceText = String(data: data, encoding: .utf8) ?? ""
                }
            } else {
                let html = String(data: data, encoding: .utf8)
                    ?? String(data: data, encoding: .isoLatin1)
                    ?? ""
                sourceText = extractVisibleText(fromHTML: html)

                // Extract links for navigation context
                let tokens = queryTokens(from: extractionQuery)
                let pageLinks = extractLinksFromHTML(
                    html: html,
                    baseURL: url,
                    tokens: tokens,
                    rootHost: url.host?.lowercased()
                )
                let topLinks = pageLinks.prefix(10).map { ["url": $0.absoluteString] }

                let truncated = String(sourceText.prefix(desiredMaxChars))
                let snippets = extractRelevantSnippets(from: truncated, query: extractionQuery, maxSnippets: 8)

                await sendToolResponse(callId: callId, functionName: "fetch_web_data", result: [
                    "status": "success",
                    "url": trimmedURL,
                    "content_type": contentType,
                    "extraction_query": extractionQuery,
                    "follow_links": false,
                    "snippets": snippets,
                    "source_excerpt": String(truncated.prefix(1800)),
                    "available_links": topLinks,
                    "navigation_hint": "Call fetch_web_data with any available_links URL to navigate further."
                ])
                return
            }

            let truncated = String(sourceText.prefix(desiredMaxChars))
            let snippets = extractRelevantSnippets(from: truncated, query: extractionQuery, maxSnippets: 8)

            await sendToolResponse(callId: callId, functionName: "fetch_web_data", result: [
                "status": "success",
                "url": trimmedURL,
                "content_type": contentType,
                "extraction_query": extractionQuery,
                "follow_links": false,
                "snippets": snippets,
                "source_excerpt": String(truncated.prefix(1800))
            ])
        } catch {
            await sendToolResponse(callId: callId, functionName: "fetch_web_data", result: [
                "error": "Failed to fetch website: \(error.localizedDescription)"
            ])
        }
    }

    /// Fetch a URL via Jina Reader API for JS-rendered content and clean markdown output.
    /// Returns nil if Jina Reader fails (will fall back to direct fetch).
    private func fetchViaJinaReader(url: URL, maxChars: Int) async -> [String: Any]? {
        let jinaURLString = "https://r.jina.ai/\(url.absoluteString)"
        guard let jinaURL = URL(string: jinaURLString) else {
            print("[Gemini] Jina Reader: invalid URL construction")
            return nil
        }

        var request = URLRequest(url: jinaURL)
        request.httpMethod = "GET"
        request.timeoutInterval = 25
        request.setValue("application/json", forHTTPHeaderField: "Accept")
        request.setValue("markdown", forHTTPHeaderField: "X-Return-Format")
        request.setValue("true", forHTTPHeaderField: "X-With-Images")
        request.setValue("true", forHTTPHeaderField: "X-With-Links")

        do {
            print("[Gemini] Jina Reader: fetching \(url.absoluteString)")
            let (data, response) = try await URLSession.shared.data(for: request)

            guard let httpResponse = response as? HTTPURLResponse,
                  (200...299).contains(httpResponse.statusCode) else {
                let status = (response as? HTTPURLResponse)?.statusCode ?? 0
                print("[Gemini] Jina Reader: failed with status \(status)")
                return nil
            }

            guard let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let responseData = json["data"] as? [String: Any] else {
                print("[Gemini] Jina Reader: failed to parse JSON response")
                return nil
            }

            var result: [String: Any] = [:]
            if let title = responseData["title"] as? String {
                result["title"] = title
            }
            if let content = responseData["content"] as? String {
                result["content"] = String(content.prefix(maxChars))
            }
            if let links = responseData["links"] as? [String: String] {
                result["links"] = links
            }
            if let images = responseData["images"] as? [String: String] {
                result["images"] = images
            }

            let contentLength = (result["content"] as? String)?.count ?? 0
            print("[Gemini] Jina Reader: success, title=\((result["title"] as? String ?? "?").prefix(60)), content=\(contentLength) chars")
            return result
        } catch {
            print("[Gemini] Jina Reader: error \(error.localizedDescription)")
            return nil
        }
    }

    private func searchWeb(query: String, site: String?, maxResults: Int?, callId: String) async {
        let trimmedQuery = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmedQuery.isEmpty else {
            await sendToolResponse(callId: callId, functionName: "search_web", result: [
                "error": "Missing query."
            ])
            return
        }

        let limit = min(max(maxResults ?? 5, 1), 10)
        let trimmedSite = site?.trimmingCharacters(in: .whitespacesAndNewlines)

        // Try Serper.dev first if API key is configured
        if let serperKey = KeychainService.shared.get(key: "serper_api_key"),
           !serperKey.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            if let serperResult = await searchViaSerper(
                query: trimmedQuery,
                site: trimmedSite,
                maxResults: limit,
                apiKey: serperKey
            ) {
                await sendToolResponse(callId: callId, functionName: "search_web", result: serperResult)
                return
            }
            print("[Gemini] Serper.dev failed, falling back to DuckDuckGo")
        }

        // Fallback: DuckDuckGo HTML scraping
        let searchQuery: String
        if let trimmedSite, !trimmedSite.isEmpty {
            searchQuery = "\(trimmedQuery) site:\(trimmedSite)"
        } else {
            searchQuery = trimmedQuery
        }

        guard var components = URLComponents(string: "https://duckduckgo.com/html/") else {
            await sendToolResponse(callId: callId, functionName: "search_web", result: [
                "error": "Search endpoint unavailable."
            ])
            return
        }
        components.queryItems = [URLQueryItem(name: "q", value: searchQuery)]
        guard let url = components.url else {
            await sendToolResponse(callId: callId, functionName: "search_web", result: [
                "error": "Failed to build search URL."
            ])
            return
        }

        var request = URLRequest(url: url)
        request.httpMethod = "GET"
        request.timeoutInterval = 20
        request.setValue("PocketPall/2.0 (+mobile web search)", forHTTPHeaderField: "User-Agent")

        do {
            let (data, response) = try await URLSession.shared.data(for: request)
            guard let httpResponse = response as? HTTPURLResponse else {
                await sendToolResponse(callId: callId, functionName: "search_web", result: [
                    "error": "No HTTP response from search endpoint."
                ])
                return
            }

            guard (200...299).contains(httpResponse.statusCode) else {
                await sendToolResponse(callId: callId, functionName: "search_web", result: [
                    "error": "Search endpoint returned status \(httpResponse.statusCode)."
                ])
                return
            }

            let html = String(data: data, encoding: .utf8)
                ?? String(data: data, encoding: .isoLatin1)
                ?? ""

            let results = parseDuckDuckGoResults(fromHTML: html, limit: limit)
            await sendToolResponse(callId: callId, functionName: "search_web", result: [
                "status": "success",
                "query": trimmedQuery,
                "site": trimmedSite ?? "",
                "source": "duckduckgo",
                "results": results
            ])
        } catch {
            await sendToolResponse(callId: callId, functionName: "search_web", result: [
                "error": "Search failed: \(error.localizedDescription)"
            ])
        }
    }

    /// Search via Serper.dev API for structured Google search results.
    /// Returns nil if Serper fails (will fall back to DuckDuckGo).
    private func searchViaSerper(query: String, site: String?, maxResults: Int, apiKey: String) async -> [String: Any]? {
        guard let url = URL(string: "https://google.serper.dev/search") else {
            return nil
        }

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.timeoutInterval = 15
        request.setValue(apiKey, forHTTPHeaderField: "X-API-KEY")
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")

        // Build request with locale for localized Google results
        let locale = RuntimeProfileStore.shared.profile.webResearchLocale
        let localeParts = locale.split(separator: "-")
        let langCode = localeParts.first.map(String.init) ?? "da"
        let countryCode = localeParts.count > 1 ? String(localeParts[1]).lowercased() : "dk"

        var searchQuery = query
        if let site, !site.isEmpty {
            searchQuery = "\(query) site:\(site)"
        }

        let body: [String: Any] = [
            "q": searchQuery,
            "num": min(maxResults, 10),
            "gl": countryCode,
            "hl": langCode
        ]

        guard let bodyData = try? JSONSerialization.data(withJSONObject: body) else {
            return nil
        }
        request.httpBody = bodyData

        do {
            print("[Gemini] Serper.dev: searching '\(searchQuery)' (gl=\(countryCode), hl=\(langCode))")
            let (data, response) = try await URLSession.shared.data(for: request)

            guard let httpResponse = response as? HTTPURLResponse,
                  (200...299).contains(httpResponse.statusCode) else {
                let status = (response as? HTTPURLResponse)?.statusCode ?? 0
                print("[Gemini] Serper.dev: failed with status \(status)")
                return nil
            }

            guard let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                print("[Gemini] Serper.dev: failed to parse response")
                return nil
            }

            var result: [String: Any] = [
                "status": "success",
                "query": query,
                "site": site ?? "",
                "source": "google_via_serper"
            ]

            // Parse organic results
            if let organic = json["organic"] as? [[String: Any]] {
                let results: [[String: Any]] = organic.prefix(maxResults).map { item in
                    var entry: [String: Any] = [:]
                    if let title = item["title"] as? String { entry["title"] = title }
                    if let link = item["link"] as? String { entry["url"] = link }
                    if let snippet = item["snippet"] as? String { entry["snippet"] = snippet }
                    if let position = item["position"] as? Int { entry["position"] = position }
                    if let date = item["date"] as? String { entry["date"] = date }
                    if let sitelinks = item["sitelinks"] as? [[String: Any]] {
                        let links: [[String: String]] = sitelinks.compactMap { sl in
                            guard let title = sl["title"] as? String,
                                  let link = sl["link"] as? String else { return nil }
                            return ["title": title, "url": link]
                        }
                        if !links.isEmpty { entry["sitelinks"] = links }
                    }
                    return entry
                }
                result["results"] = results
            }

            // Include knowledge graph if available
            if let kg = json["knowledgeGraph"] as? [String: Any] {
                var kgResult: [String: Any] = [:]
                if let title = kg["title"] as? String { kgResult["title"] = title }
                if let desc = kg["description"] as? String { kgResult["description"] = desc }
                if let descSource = kg["descriptionSource"] as? String { kgResult["description_source"] = descSource }
                if let descLink = kg["descriptionLink"] as? String { kgResult["description_link"] = descLink }
                if let type = kg["type"] as? String { kgResult["type"] = type }
                if let website = kg["website"] as? String { kgResult["website"] = website }
                if let imageUrl = kg["imageUrl"] as? String { kgResult["image_url"] = imageUrl }
                if let attrs = kg["attributes"] as? [String: String] {
                    kgResult["attributes"] = attrs
                }
                result["knowledge_graph"] = kgResult
            }

            // Include answer box / featured snippet if available
            if let answerBox = json["answerBox"] as? [String: Any] {
                var abResult: [String: Any] = [:]
                if let answer = answerBox["answer"] as? String { abResult["answer"] = answer }
                if let snippet = answerBox["snippet"] as? String { abResult["snippet"] = snippet }
                if let snippetHighlighted = answerBox["snippetHighlighted"] as? [String] {
                    abResult["highlighted"] = snippetHighlighted
                }
                if let title = answerBox["title"] as? String { abResult["title"] = title }
                if let link = answerBox["link"] as? String { abResult["url"] = link }
                result["answer_box"] = abResult
            }

            // Include "People Also Ask" for richer context
            if let paa = json["peopleAlsoAsk"] as? [[String: Any]] {
                let questions: [[String: Any]] = paa.prefix(4).map { item in
                    var entry: [String: Any] = [:]
                    if let question = item["question"] as? String { entry["question"] = question }
                    if let snippet = item["snippet"] as? String { entry["answer"] = snippet }
                    if let link = item["link"] as? String { entry["url"] = link }
                    if let title = item["title"] as? String { entry["title"] = title }
                    return entry
                }
                if !questions.isEmpty { result["people_also_ask"] = questions }
            }

            // Include related searches for follow-up queries
            if let related = json["relatedSearches"] as? [[String: Any]] {
                let queries: [String] = related.prefix(5).compactMap { $0["query"] as? String }
                if !queries.isEmpty { result["related_searches"] = queries }
            }

            let resultCount = (result["results"] as? [[String: Any]])?.count ?? 0
            let hasKG = result["knowledge_graph"] != nil
            let hasAB = result["answer_box"] != nil
            print("[Gemini] Serper.dev: success, \(resultCount) organic, kg=\(hasKG), answer_box=\(hasAB)")
            return result
        } catch {
            print("[Gemini] Serper.dev: error \(error.localizedDescription)")
            return nil
        }
    }

    private func crawlWebsite(
        startURL: URL,
        extractionQuery: String,
        maxCharsPerPage: Int,
        maxPages: Int,
        sameDomainOnly: Bool
    ) async -> [String: Any] {
        let tokens = queryTokens(from: extractionQuery)
        let normalizedStart = normalizeURLForCrawl(startURL)
        let rootHost = normalizedStart.host?.lowercased()

        var queue: [URL] = [normalizedStart]
        var queued: Set<String> = [normalizedStart.absoluteString]
        var visited: Set<String> = []
        var pageMatches: [[String: Any]] = []
        var pageErrors: [[String: Any]] = []

        while !queue.isEmpty && visited.count < maxPages {
            let nextURL = queue.removeFirst()
            queued.remove(nextURL.absoluteString)
            let normalizedNext = normalizeURLForCrawl(nextURL)
            let nextKey = normalizedNext.absoluteString
            if visited.contains(nextKey) {
                continue
            }
            visited.insert(nextKey)

            var request = URLRequest(url: normalizedNext)
            request.httpMethod = "GET"
            request.timeoutInterval = 20
            request.setValue("PocketPall/2.0 (+mobile tool fetch)", forHTTPHeaderField: "User-Agent")

            do {
                let (data, response) = try await URLSession.shared.data(for: request)
                guard let httpResponse = response as? HTTPURLResponse else {
                    pageErrors.append([
                        "url": normalizedNext.absoluteString,
                        "error": "No HTTP response"
                    ])
                    continue
                }

                let finalURL = normalizeURLForCrawl(httpResponse.url ?? normalizedNext)
                let status = httpResponse.statusCode
                let contentType = (httpResponse.value(forHTTPHeaderField: "Content-Type") ?? "").lowercased()

                guard (200...299).contains(status) else {
                    pageErrors.append([
                        "url": finalURL.absoluteString,
                        "status": status
                    ])
                    continue
                }

                if contentType.contains("application/json") {
                    let sourceText: String
                    if let obj = try? JSONSerialization.jsonObject(with: data),
                       let jsonData = try? JSONSerialization.data(withJSONObject: obj, options: [.prettyPrinted]),
                       let jsonText = String(data: jsonData, encoding: .utf8) {
                        sourceText = jsonText
                    } else {
                        sourceText = String(data: data, encoding: .utf8) ?? ""
                    }
                    let truncated = String(sourceText.prefix(maxCharsPerPage))
                    let snippets = extractRelevantSnippets(
                        from: truncated,
                        query: extractionQuery,
                        maxSnippets: 5
                    )
                    let score = scoreTextAgainstTokens(truncated, tokens: tokens)
                    pageMatches.append([
                        "url": finalURL.absoluteString,
                        "content_type": contentType,
                        "score": score,
                        "snippets": snippets
                    ])
                    continue
                }

                let html = String(data: data, encoding: .utf8)
                    ?? String(data: data, encoding: .isoLatin1)
                    ?? ""
                let title = extractHTMLTitle(fromHTML: html)
                let visibleText = extractVisibleText(fromHTML: html)
                let truncated = String(visibleText.prefix(maxCharsPerPage))
                let snippets = extractRelevantSnippets(
                    from: truncated,
                    query: extractionQuery,
                    maxSnippets: 5
                )
                let score = scoreTextAgainstTokens("\(title)\n\(truncated)", tokens: tokens)
                pageMatches.append([
                    "url": finalURL.absoluteString,
                    "title": title,
                    "content_type": contentType,
                    "score": score,
                    "snippets": snippets
                ])

                let rankedLinks = extractLinksFromHTML(
                    html: html,
                    baseURL: finalURL,
                    tokens: tokens,
                    rootHost: sameDomainOnly ? rootHost : nil
                )
                for link in rankedLinks {
                    let normalized = normalizeURLForCrawl(link)
                    let key = normalized.absoluteString
                    if visited.contains(key) || queued.contains(key) {
                        continue
                    }
                    queue.append(normalized)
                    queued.insert(key)
                    if queue.count >= maxPages * 6 {
                        break
                    }
                }
            } catch {
                pageErrors.append([
                    "url": normalizedNext.absoluteString,
                    "error": error.localizedDescription
                ])
            }
        }

        let sortedMatches = pageMatches.sorted { lhs, rhs in
            let lhsScore = lhs["score"] as? Int ?? 0
            let rhsScore = rhs["score"] as? Int ?? 0
            if lhsScore == rhsScore {
                return (lhs["url"] as? String ?? "") < (rhs["url"] as? String ?? "")
            }
            return lhsScore > rhsScore
        }

        return [
            "status": "success",
            "start_url": normalizedStart.absoluteString,
            "extraction_query": extractionQuery,
            "pages_scanned": visited.count,
            "max_pages": maxPages,
            "same_domain_only": sameDomainOnly,
            "matches": Array(sortedMatches.prefix(6)),
            "errors": Array(pageErrors.prefix(4)),
            "limitations": [
                "JavaScript-only, paywalled, or login-protected pages may not expose full text.",
                "Only a bounded number of pages are crawled for speed and stability."
            ]
        ]
    }

    private func parseDuckDuckGoResults(fromHTML html: String, limit: Int) -> [[String: Any]] {
        let pattern = "(?is)<a[^>]*class=[\"'][^\"']*result__a[^\"']*[\"'][^>]*href=[\"']([^\"']+)[\"'][^>]*>(.*?)</a>"
        guard let regex = try? NSRegularExpression(pattern: pattern) else {
            return []
        }

        let snippetPattern = "(?is)<(?:a|div)[^>]*class=[\"'][^\"']*result__snippet[^\"']*[\"'][^>]*>(.*?)</(?:a|div)>"
        let snippetRegex = try? NSRegularExpression(pattern: snippetPattern)
        let baseURL = URL(string: "https://duckduckgo.com")!
        let nsHTML = html as NSString
        let fullRange = NSRange(location: 0, length: nsHTML.length)
        let matches = regex.matches(in: html, options: [], range: fullRange)

        var results: [[String: Any]] = []
        var seenURLs: Set<String> = []

        for match in matches {
            if results.count >= limit { break }
            guard match.numberOfRanges >= 3 else { continue }
            let hrefRange = match.range(at: 1)
            let titleRange = match.range(at: 2)
            guard hrefRange.location != NSNotFound, titleRange.location != NSNotFound else { continue }

            let href = nsHTML.substring(with: hrefRange)
            let titleHTML = nsHTML.substring(with: titleRange)
            guard let resolvedURL = decodeSearchResultURL(href, baseURL: baseURL) else { continue }
            let urlString = resolvedURL.absoluteString
            if seenURLs.contains(urlString) { continue }
            seenURLs.insert(urlString)

            let title = extractVisibleText(fromHTML: titleHTML)
            var snippet = ""
            if let snippetRegex {
                let start = match.range.location + match.range.length
                let end = min(nsHTML.length, start + 1200)
                if start < end {
                    let snippetRange = NSRange(location: start, length: end - start)
                    if let snippetMatch = snippetRegex.firstMatch(in: html, options: [], range: snippetRange),
                       snippetMatch.numberOfRanges >= 2 {
                        let range = snippetMatch.range(at: 1)
                        if range.location != NSNotFound {
                            snippet = extractVisibleText(fromHTML: nsHTML.substring(with: range))
                        }
                    }
                }
            }

            results.append([
                "title": String(title.prefix(180)),
                "url": urlString,
                "snippet": String(snippet.prefix(260))
            ])
        }

        return results
    }

    private func decodeSearchResultURL(_ href: String, baseURL: URL) -> URL? {
        let trimmed = href.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty { return nil }

        if trimmed.hasPrefix("javascript:") ||
            trimmed.hasPrefix("mailto:") ||
            trimmed.hasPrefix("tel:") {
            return nil
        }

        let abs: String
        if trimmed.hasPrefix("http://") || trimmed.hasPrefix("https://") {
            abs = trimmed
        } else {
            abs = URL(string: trimmed, relativeTo: baseURL)?.absoluteURL.absoluteString ?? trimmed
        }

        if abs.contains("duckduckgo.com/l/?"),
           let components = URLComponents(string: abs),
           let uddg = components.queryItems?.first(where: { $0.name == "uddg" })?.value {
            let decoded = uddg.removingPercentEncoding ?? uddg
            if let target = URL(string: decoded) {
                return normalizeURLForCrawl(target)
            }
        }

        guard let url = URL(string: abs),
              let scheme = url.scheme?.lowercased(),
              scheme == "https" || scheme == "http" else {
            return nil
        }
        return normalizeURLForCrawl(url)
    }

    private func extractLinksFromHTML(
        html: String,
        baseURL: URL,
        tokens: [String],
        rootHost: String?
    ) -> [URL] {
        let pattern = "(?is)<a\\s+[^>]*href\\s*=\\s*[\"']([^\"']+)[\"'][^>]*>(.*?)</a>"
        guard let regex = try? NSRegularExpression(pattern: pattern) else {
            return []
        }

        let nsHTML = html as NSString
        let fullRange = NSRange(location: 0, length: nsHTML.length)
        let matches = regex.matches(in: html, options: [], range: fullRange)

        var bestScoreByURL: [String: Int] = [:]
        var urlByKey: [String: URL] = [:]

        for match in matches {
            if match.numberOfRanges < 3 { continue }
            let hrefRange = match.range(at: 1)
            let textRange = match.range(at: 2)
            guard hrefRange.location != NSNotFound, textRange.location != NSNotFound else { continue }

            let href = nsHTML.substring(with: hrefRange).trimmingCharacters(in: .whitespacesAndNewlines)
            if href.isEmpty || href.hasPrefix("#") { continue }
            if href.hasPrefix("javascript:") || href.hasPrefix("mailto:") || href.hasPrefix("tel:") { continue }

            guard let resolved = URL(string: href, relativeTo: baseURL)?.absoluteURL,
                  let scheme = resolved.scheme?.lowercased(),
                  scheme == "https" || scheme == "http" else {
                continue
            }

            let normalized = normalizeURLForCrawl(resolved)
            if let rootHost,
               let candidateHost = normalized.host?.lowercased(),
               !hostMatches(candidateHost, rootHost: rootHost) {
                continue
            }

            let anchorHTML = nsHTML.substring(with: textRange)
            let anchorText = extractVisibleText(fromHTML: anchorHTML)
            let score = scoreTextAgainstTokens("\(anchorText)\n\(normalized.absoluteString)", tokens: tokens)
            let key = normalized.absoluteString
            if score >= (bestScoreByURL[key] ?? Int.min) {
                bestScoreByURL[key] = score
                urlByKey[key] = normalized
            }
        }

        return bestScoreByURL
            .sorted { lhs, rhs in
                if lhs.value == rhs.value {
                    return lhs.key < rhs.key
                }
                return lhs.value > rhs.value
            }
            .prefix(16)
            .compactMap { urlByKey[$0.key] }
    }

    private func extractHTMLTitle(fromHTML html: String) -> String {
        let pattern = "(?is)<title[^>]*>(.*?)</title>"
        guard let regex = try? NSRegularExpression(pattern: pattern) else {
            return ""
        }
        let nsHTML = html as NSString
        let fullRange = NSRange(location: 0, length: nsHTML.length)
        guard let match = regex.firstMatch(in: html, options: [], range: fullRange),
              match.numberOfRanges >= 2 else {
            return ""
        }
        let titleRange = match.range(at: 1)
        guard titleRange.location != NSNotFound else {
            return ""
        }
        let titleHTML = nsHTML.substring(with: titleRange)
        return String(extractVisibleText(fromHTML: titleHTML).prefix(180))
    }

    private func normalizeURLForCrawl(_ url: URL) -> URL {
        guard var components = URLComponents(url: url, resolvingAgainstBaseURL: false) else {
            return url
        }
        components.fragment = nil
        return components.url ?? url
    }

    private func hostMatches(_ host: String, rootHost: String) -> Bool {
        if host == rootHost { return true }
        return host.hasSuffix(".\(rootHost)")
    }

    private func queryTokens(from query: String) -> [String] {
        let raw = query.lowercased()
            .components(separatedBy: CharacterSet.alphanumerics.inverted)
            .filter { $0.count >= 3 }
        return Array(Set(raw))
    }

    private func scoreTextAgainstTokens(_ text: String, tokens: [String]) -> Int {
        guard !tokens.isEmpty else { return 0 }
        let lower = text.lowercased()
        var score = 0
        for token in tokens {
            if lower.contains(token) {
                score += 1
                let occurrences = max(lower.components(separatedBy: token).count - 1, 1)
                score += min(occurrences, 4)
            }
        }
        return score
    }

    private func extractVisibleText(fromHTML html: String) -> String {
        let withoutScripts = html.replacingOccurrences(
            of: "(?is)<script[^>]*>.*?</script>|<style[^>]*>.*?</style>|<noscript[^>]*>.*?</noscript>",
            with: " ",
            options: .regularExpression
        )

        let strippedTags = withoutScripts.replacingOccurrences(
            of: "(?is)<[^>]+>",
            with: "\n",
            options: .regularExpression
        )

        let decodedEntities = decodeHTMLEntities(strippedTags)

        let compactLines = decodedEntities
            .components(separatedBy: .newlines)
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }

        return compactLines.joined(separator: "\n")
    }

    private func decodeHTMLEntities(_ text: String) -> String {
        text
            .replacingOccurrences(of: "&nbsp;", with: " ")
            .replacingOccurrences(of: "&amp;", with: "&")
            .replacingOccurrences(of: "&lt;", with: "<")
            .replacingOccurrences(of: "&gt;", with: ">")
            .replacingOccurrences(of: "&quot;", with: "\"")
            .replacingOccurrences(of: "&#39;", with: "'")
    }

    private func extractRelevantSnippets(from text: String, query: String, maxSnippets: Int) -> [String] {
        let lines = text.components(separatedBy: .newlines)
        let tokens = queryTokens(from: query)

        guard !tokens.isEmpty else {
            return Array(lines.prefix(maxSnippets)).map { String($0.prefix(220)) }
        }

        let scored: [(line: String, score: Int)] = lines.compactMap { line in
            let lower = line.lowercased()
            let score = tokens.reduce(0) { partial, token in
                partial + (lower.contains(token) ? 1 : 0)
            }
            guard score > 0 else { return nil }
            return (line, score)
        }

        if scored.isEmpty {
            return Array(lines.prefix(maxSnippets)).map { String($0.prefix(220)) }
        }

        return scored
            .sorted { lhs, rhs in
                if lhs.score == rhs.score {
                    return lhs.line.count < rhs.line.count
                }
                return lhs.score > rhs.score
            }
            .prefix(maxSnippets)
            .map { String($0.line.prefix(260)) }
    }

    /// Send a tool response back to the Gemini Live WebSocket so it can continue the conversation.
    private func sendToolResponse(callId: String, functionName: String, result: [String: Any]) async {
        guard let ws = webSocket else { return }

        let response: [String: Any] = [
            "toolResponse": [
                "functionResponses": [
                    [
                        "id": callId,
                        "name": functionName,
                        "response": result
                    ]
                ]
            ]
        ]

        do {
            let jsonString = try toJSONString(response)
            try await ws.send(.string(jsonString))
            print("[Gemini] Sent toolResponse for callId=\(callId)")
        } catch {
            print("[Gemini] Failed to send toolResponse: \(error)")
        }
    }

    /// Take a screenshot of a webpage using WKWebView and send it to the device display.
    private func takeWebpageScreenshot(urlString: String, callId: String) async {
        let trimmedURL = urlString.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let url = URL(string: trimmedURL),
              let scheme = url.scheme?.lowercased(),
              scheme == "https" || scheme == "http" else {
            await sendToolResponse(callId: callId, functionName: "get_webpage_screenshot", result: [
                "error": "Invalid URL. Please provide a full http/https URL."
            ])
            return
        }

        print("[Gemini] Taking screenshot of \(trimmedURL)")

        do {
            let (image, pageTitle) = try await loadWebpageAndCapture(url: url)

            // Send the image to device via the onImageGenerated callback
            onImageGenerated?(image)

            await sendToolResponse(callId: callId, functionName: "get_webpage_screenshot", result: [
                "status": "success",
                "url": trimmedURL,
                "title": pageTitle,
                "message": "Screenshot captured and sent to device display"
            ])
        } catch {
            print("[Gemini] Screenshot failed: \(error.localizedDescription)")
            await sendToolResponse(callId: callId, functionName: "get_webpage_screenshot", result: [
                "error": "Failed to capture screenshot: \(error.localizedDescription)"
            ])
        }
    }

    /// Get or create the persistent WKWebView for browser interactions.
    @MainActor
    private func getOrCreatePersistentWebView() -> (WKWebView, PersistentWebViewDelegate) {
        if let webView = persistentWebView, let delegate = persistentWebViewDelegate {
            return (webView, delegate)
        }

        let displayWidth = CGFloat(ImageProcessor.displayWidth)
        let displayHeight = CGFloat(ImageProcessor.displayHeight)

        let config = WKWebViewConfiguration()
        config.suppressesIncrementalRendering = true
        // Use default data store so cookies persist across navigations
        config.websiteDataStore = .default()

        let webView = WKWebView(
            frame: CGRect(x: 0, y: 0, width: displayWidth, height: displayHeight),
            configuration: config
        )
        webView.isOpaque = false
        webView.backgroundColor = .white
        webView.customUserAgent = "Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Mobile/15E148 Safari/604.1"

        let delegate = PersistentWebViewDelegate()
        webView.navigationDelegate = delegate

        persistentWebView = webView
        persistentWebViewDelegate = delegate

        print("[Gemini] Created persistent WKWebView (\(Int(displayWidth))x\(Int(displayHeight)))")
        return (webView, delegate)
    }

    /// Take a snapshot of the persistent WKWebView scaled to display dimensions.
    @MainActor
    private func captureSnapshot(of webView: WKWebView) async throws -> UIImage {
        let displayWidth = CGFloat(ImageProcessor.displayWidth)
        let displayHeight = CGFloat(ImageProcessor.displayHeight)

        let snapshotConfig = WKSnapshotConfiguration()
        snapshotConfig.rect = CGRect(x: 0, y: 0, width: displayWidth, height: displayHeight)

        let snapshot = try await webView.takeSnapshot(configuration: snapshotConfig)

        let renderer = UIGraphicsImageRenderer(size: CGSize(width: displayWidth, height: displayHeight))
        let scaledImage = renderer.image { _ in
            snapshot.draw(in: CGRect(x: 0, y: 0, width: displayWidth, height: displayHeight))
        }
        return scaledImage
    }

    /// Load a URL in the persistent WKWebView, auto-dismiss cookie banners, and capture a snapshot.
    @MainActor
    private func loadWebpageAndCapture(url: URL) async throws -> (UIImage, String) {
        let (webView, delegate) = getOrCreatePersistentWebView()
        currentBrowserURL = url

        let request = URLRequest(url: url, timeoutInterval: 20)
        webView.load(request)

        // Wait for page load via continuation
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            delegate.loadContinuation = continuation
        }

        // Allow rendering to complete
        try await Task.sleep(nanoseconds: 500_000_000) // 0.5s

        // Auto-dismiss cookie consent banners
        let dismissResult = try await webView.evaluateJavaScript(Self.cookieConsentDismissJS) as? String ?? "unknown"
        print("[Gemini] Cookie auto-dismiss result: \(dismissResult)")

        if dismissResult.hasPrefix("dismissed") {
            // Wait for dismiss animation to complete
            try await Task.sleep(nanoseconds: 400_000_000) // 0.4s
        }

        let pageTitle = webView.title ?? url.host ?? trimURL(url.absoluteString)
        let image = try await captureSnapshot(of: webView)

        return (image, pageTitle)
    }

    /// Click an element on the currently loaded page.
    private func clickElement(selector: String?, text: String?, callId: String) async {
        guard selector != nil || text != nil else {
            await sendToolResponse(callId: callId, functionName: "click_element", result: [
                "error": "Provide either 'selector' (CSS) or 'text' (visible text) to identify the element."
            ])
            return
        }

        do {
            let (image, info) = try await performClick(selector: selector, text: text)
            onImageGenerated?(image)
            await sendToolResponse(callId: callId, functionName: "click_element", result: info)
        } catch {
            print("[Gemini] click_element failed: \(error.localizedDescription)")
            await sendToolResponse(callId: callId, functionName: "click_element", result: [
                "error": "Click failed: \(error.localizedDescription)"
            ])
        }
    }

    @MainActor
    private func performClick(selector: String?, text: String?) async throws -> (UIImage, [String: Any]) {
        guard let webView = persistentWebView, let delegate = persistentWebViewDelegate else {
            throw NSError(domain: "GeminiProvider", code: 1, userInfo: [
                NSLocalizedDescriptionKey: "No browser page loaded. Use get_webpage_screenshot first."
            ])
        }

        // Build JavaScript to find and click the element
        let findAndClickJS: String
        if let selector = selector {
            let escapedSelector = selector.replacingOccurrences(of: "'", with: "\\'")
            findAndClickJS = """
            (function() {
                var el = document.querySelector('\(escapedSelector)');
                if (!el) return JSON.stringify({error: 'Element not found for selector: \(escapedSelector)'});
                el.scrollIntoView({block: 'center', behavior: 'instant'});
                el.click();
                return JSON.stringify({status: 'clicked', tag: el.tagName, text: (el.innerText || '').substring(0, 100)});
            })();
            """
        } else if let text = text {
            let escapedText = text.replacingOccurrences(of: "'", with: "\\'").replacingOccurrences(of: "\\", with: "\\\\")
            findAndClickJS = """
            (function() {
                var searchText = '\(escapedText)'.toLowerCase();
                var allElements = document.querySelectorAll('a, button, input[type="button"], input[type="submit"], [role="button"], [onclick], label, span, div[tabindex]');
                var best = null;
                var bestLen = Infinity;
                for (var i = 0; i < allElements.length; i++) {
                    var el = allElements[i];
                    if (!el.offsetParent && el.offsetWidth === 0 && el.offsetHeight === 0) continue;
                    var elText = (el.innerText || el.value || el.getAttribute('aria-label') || '').toLowerCase().trim();
                    if (elText.indexOf(searchText) !== -1 && elText.length < bestLen) {
                        best = el;
                        bestLen = elText.length;
                    }
                }
                if (!best) return JSON.stringify({error: 'No visible element found with text: \(escapedText)'});
                best.scrollIntoView({block: 'center', behavior: 'instant'});
                best.click();
                return JSON.stringify({status: 'clicked', tag: best.tagName, text: (best.innerText || '').substring(0, 100)});
            })();
            """
        } else {
            throw NSError(domain: "GeminiProvider", code: 2, userInfo: [
                NSLocalizedDescriptionKey: "No selector or text provided."
            ])
        }

        let resultJSON = try await webView.evaluateJavaScript(findAndClickJS) as? String ?? "{}"
        guard let resultData = resultJSON.data(using: .utf8),
              var clickResult = try JSONSerialization.jsonObject(with: resultData) as? [String: Any] else {
            throw NSError(domain: "GeminiProvider", code: 3, userInfo: [
                NSLocalizedDescriptionKey: "Failed to parse click result."
            ])
        }

        if let error = clickResult["error"] as? String {
            throw NSError(domain: "GeminiProvider", code: 4, userInfo: [
                NSLocalizedDescriptionKey: error
            ])
        }

        // Detect if click triggered a navigation (race: didFinish vs timeout)
        let navigationHappened = await withTaskGroup(of: Bool.self) { group in
            group.addTask { @MainActor in
                do {
                    try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                        delegate.loadContinuation = continuation
                    }
                    return true
                } catch {
                    return true // Navigation attempted but failed
                }
            }
            group.addTask {
                try? await Task.sleep(nanoseconds: 1_500_000_000) // 1.5s timeout
                return false
            }
            let result = await group.next() ?? false
            group.cancelAll()
            return result
        }

        if navigationHappened {
            print("[Gemini] Click triggered navigation, waiting for render...")
            try await Task.sleep(nanoseconds: 500_000_000) // 0.5s render time

            // Auto-dismiss cookie consent on new page
            let dismissResult = try await webView.evaluateJavaScript(Self.cookieConsentDismissJS) as? String ?? "unknown"
            if dismissResult.hasPrefix("dismissed") {
                try await Task.sleep(nanoseconds: 400_000_000)
            }
            clickResult["navigated"] = true
        } else {
            // Cancel the pending navigation continuation if no navigation happened
            delegate.loadContinuation = nil
            // Small delay for any dynamic content updates
            try await Task.sleep(nanoseconds: 300_000_000) // 0.3s
        }

        let pageTitle = webView.title ?? currentBrowserURL?.host ?? ""
        let currentURL = webView.url?.absoluteString ?? currentBrowserURL?.absoluteString ?? ""
        clickResult["page_title"] = pageTitle
        clickResult["current_url"] = currentURL
        clickResult["message"] = "Element clicked. Updated screenshot sent to device display."

        let image = try await captureSnapshot(of: webView)
        return (image, clickResult)
    }

    /// Scroll the currently loaded page.
    private func scrollPage(direction: String, amount: String, callId: String) async {
        do {
            let (image, info) = try await performScroll(direction: direction, amount: amount)
            onImageGenerated?(image)
            await sendToolResponse(callId: callId, functionName: "scroll_page", result: info)
        } catch {
            print("[Gemini] scroll_page failed: \(error.localizedDescription)")
            await sendToolResponse(callId: callId, functionName: "scroll_page", result: [
                "error": "Scroll failed: \(error.localizedDescription)"
            ])
        }
    }

    @MainActor
    private func performScroll(direction: String, amount: String) async throws -> (UIImage, [String: Any]) {
        guard let webView = persistentWebView else {
            throw NSError(domain: "GeminiProvider", code: 1, userInfo: [
                NSLocalizedDescriptionKey: "No browser page loaded. Use get_webpage_screenshot first."
            ])
        }

        let displayHeight = CGFloat(ImageProcessor.displayHeight)
        let scrollJS: String

        switch amount.lowercased() {
        case "top":
            scrollJS = "window.scrollTo(0, 0); JSON.stringify({scrollY: window.scrollY, scrollHeight: document.body.scrollHeight});"
        case "bottom":
            scrollJS = "window.scrollTo(0, document.body.scrollHeight); JSON.stringify({scrollY: window.scrollY, scrollHeight: document.body.scrollHeight});"
        case "half":
            let pixels = Int(displayHeight / 2) // 224px
            let sign = direction.lowercased() == "up" ? "-" : ""
            scrollJS = "window.scrollBy(0, \(sign)\(pixels)); JSON.stringify({scrollY: window.scrollY, scrollHeight: document.body.scrollHeight});"
        default: // "page"
            let pixels = Int(displayHeight) - 40 // 408px with overlap
            let sign = direction.lowercased() == "up" ? "-" : ""
            scrollJS = "window.scrollBy(0, \(sign)\(pixels)); JSON.stringify({scrollY: window.scrollY, scrollHeight: document.body.scrollHeight});"
        }

        let resultJSON = try await webView.evaluateJavaScript(scrollJS) as? String ?? "{}"
        var scrollInfo: [String: Any] = [:]
        if let data = resultJSON.data(using: .utf8),
           let parsed = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            scrollInfo = parsed
        }

        // Wait for any lazy-loaded content
        try await Task.sleep(nanoseconds: 300_000_000) // 0.3s

        let pageTitle = webView.title ?? currentBrowserURL?.host ?? ""
        let currentURL = webView.url?.absoluteString ?? currentBrowserURL?.absoluteString ?? ""
        scrollInfo["status"] = "scrolled"
        scrollInfo["direction"] = direction
        scrollInfo["amount"] = amount
        scrollInfo["page_title"] = pageTitle
        scrollInfo["current_url"] = currentURL
        scrollInfo["message"] = "Page scrolled. Updated screenshot sent to device display."

        let image = try await captureSnapshot(of: webView)
        return (image, scrollInfo)
    }

    private func trimURL(_ url: String) -> String {
        String(url.prefix(60))
    }

    // MARK: - Video Streaming

    private func handlePlayVideo(urlString: String, title: String?, callId: String) async {
        guard let transport = videoTransport else {
            print("[Gemini] play_video failed: no video transport")
            await sendToolResponse(callId: callId, functionName: "play_video", result: [
                "error": "Video transport not configured"
            ])
            return
        }

        guard let url = URL(string: urlString) else {
            print("[Gemini] play_video failed: invalid URL")
            await sendToolResponse(callId: callId, functionName: "play_video", result: [
                "error": "Invalid video URL"
            ])
            return
        }

        // Stop any existing video
        videoStreamer?.stop()

        let streamer = VideoStreamer(transport: transport)
        videoStreamer = streamer

        streamer.onVideoEnded = { [weak self] in
            print("[Gemini] Video ended naturally")
            self?.videoStreamer = nil
        }

        streamer.onError = { [weak self] error in
            print("[Gemini] Video error: \(error.localizedDescription)")
            self?.videoStreamer = nil
        }

        streamer.startPlaying(url: url)

        let displayTitle = title ?? url.lastPathComponent
        await sendToolResponse(callId: callId, functionName: "play_video", result: [
            "status": "playing",
            "title": displayTitle,
            "message": "Video is now playing on the device. Audio and video frames are streaming to the display."
        ])
    }

    private func handleStopVideo(callId: String) async {
        if let streamer = videoStreamer, streamer.isPlaying {
            streamer.stop()
            videoStreamer = nil
            await sendToolResponse(callId: callId, functionName: "stop_video", result: [
                "status": "stopped",
                "message": "Video playback stopped."
            ])
        } else {
            await sendToolResponse(callId: callId, functionName: "stop_video", result: [
                "status": "not_playing",
                "message": "No video was playing."
            ])
        }
    }

    /// Called when ESP32 sends a video_stop_request (e.g., user double-tapped)
    func stopVideoFromDevice() {
        guard let streamer = videoStreamer, streamer.isPlaying else { return }
        print("[Gemini] Stopping video (device request)")
        streamer.stop()
        videoStreamer = nil
    }
}

/// Persistent delegate for WKWebView that supports repeated navigations via a settable continuation.
private class PersistentWebViewDelegate: NSObject, WKNavigationDelegate {
    /// Set this before each navigation. It will be consumed (set to nil) when didFinish or didFail fires.
    var loadContinuation: CheckedContinuation<Void, Error>? {
        didSet {
            // If a new continuation is set, reset the completion guard
            if loadContinuation != nil {
                didResumeCurrent = false
            }
        }
    }
    private var didResumeCurrent = false

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        guard !didResumeCurrent, let continuation = loadContinuation else { return }
        didResumeCurrent = true
        loadContinuation = nil
        continuation.resume()
    }

    func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
        guard !didResumeCurrent, let continuation = loadContinuation else { return }
        didResumeCurrent = true
        loadContinuation = nil
        continuation.resume(throwing: error)
    }

    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) {
        guard !didResumeCurrent, let continuation = loadContinuation else { return }
        didResumeCurrent = true
        loadContinuation = nil
        continuation.resume(throwing: error)
    }
}

extension GeminiProvider: URLSessionWebSocketDelegate {
    func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask, didOpenWithProtocol protocol: String?) {
        socketDidOpen = true
        let proto = `protocol` ?? "none"
        print("[Gemini] WebSocket opened (protocol=\(proto))")
        resumeOpenContinuation(with: .success(()))
    }

    func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask, didCloseWith closeCode: URLSessionWebSocketTask.CloseCode, reason: Data?) {
        let reasonText: String
        if let reason, !reason.isEmpty, let utf8 = String(data: reason, encoding: .utf8) {
            reasonText = utf8
        } else {
            reasonText = "none"
        }

        print("[Gemini] WebSocket closed: code=\(closeCode.rawValue), reason=\(reasonText)")

        let closeError: Error = {
            let lowered = reasonText.lowercased()
            if closeCode == .policyViolation && lowered.contains("not found") && lowered.contains("models/") {
                return AIProviderError.apiError(
                    "Gemini model path is invalid or unavailable: \(modelPath)"
                )
            }
            return GeminiLiveError.closed(code: closeCode.rawValue, reason: reasonText)
        }()

        if isIntentionalClose {
            isIntentionalClose = false
            resumeOpenContinuation(with: .failure(closeError))
            resumeSetupContinuation(with: .failure(closeError))
            return
        }

        handleConnectionFailure(closeError)
    }
}
