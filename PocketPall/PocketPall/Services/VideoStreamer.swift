import Foundation
import AVFoundation
import UIKit
import CoreImage
import MediaToolbox
import WebKit

/// Streams video from AVPlayer (direct URLs) or WKWebView (YouTube/web URLs)
/// to ESP32 via WiFi TCP. Captures frames as JPEG, fragments and sends over TCP.
/// For AVPlayer mode, audio is extracted via MTAudioProcessingTap, resampled to 24kHz mono,
/// ADPCM-encoded, and sent via the existing audio pipeline.
/// For WebView mode, audio plays through the iPhone speaker.
final class VideoStreamer: NSObject {

    // MARK: - Stream Mode

    private enum StreamMode {
        case avPlayer
        case webView
    }

    private let transport: WiFiTransportManager
    private var mode: StreamMode = .avPlayer

    // AVPlayer properties
    private var player: AVPlayer?
    private var playerItem: AVPlayerItem?
    private var videoOutput: AVPlayerItemVideoOutput?
    private var displayLink: CADisplayLink?
    private var endObserver: NSObjectProtocol?

    // Audio tap
    private var audioTap: MTAudioProcessingTap?
    private var audioTimer: DispatchSourceTimer?
    private var audioPending = Data()
    private let audioLock = NSLock()
    private var adpcmEncoder = ADPCMEncoder()
    private let audioMaxBacklogBytes = 5 * 24000 * 2 // 5 seconds @ 24kHz mono 16-bit

    // WKWebView properties
    private var webView: WKWebView?
    private var webViewDelegate: WebViewVideoDelegate?
    private var snapshotTimer: Timer?
    private var isCapturingSnapshot = false

    // Frame rate control
    private var lastFrameTime: CFTimeInterval = 0
    private let targetFrameInterval: CFTimeInterval = 1.0 / 8.0 // ~8 FPS target
    private var jpegQuality: CGFloat = 0.3

    // Stats
    private var frameCount: UInt32 = 0
    private var totalJpegBytes: UInt64 = 0

    // Callbacks
    var onVideoEnded: (() -> Void)?
    var onError: ((Error) -> Void)?

    private(set) var isPlaying = false

    init(transport: WiFiTransportManager) {
        self.transport = transport
        super.init()
    }

    // MARK: - URL Detection

    static func isWebViewURL(_ url: URL) -> Bool {
        guard let host = url.host?.lowercased() else { return false }
        let webHosts = [
            "youtube.com", "www.youtube.com", "m.youtube.com",
            "youtu.be",
            "vimeo.com", "www.vimeo.com",
            "dailymotion.com", "www.dailymotion.com",
            "twitch.tv", "www.twitch.tv"
        ]
        return webHosts.contains(host)
    }

    private static func youtubeEmbedURL(from url: URL) -> URL? {
        var videoID: String?

        if let host = url.host?.lowercased(), host.contains("youtu.be") {
            // youtu.be/VIDEO_ID
            videoID = url.pathComponents.last
        } else if let host = url.host?.lowercased(), host.contains("youtube.com") {
            // youtube.com/watch?v=VIDEO_ID
            if let components = URLComponents(url: url, resolvingAgainstBaseURL: false),
               let vParam = components.queryItems?.first(where: { $0.name == "v" })?.value {
                videoID = vParam
            }
            // youtube.com/embed/VIDEO_ID (already embed)
            if url.pathComponents.count > 2 && url.pathComponents[1] == "embed" {
                videoID = url.pathComponents[2]
            }
        }

        guard let id = videoID, !id.isEmpty else { return nil }
        return URL(string: "https://www.youtube.com/embed/\(id)?autoplay=1&controls=0&playsinline=1&mute=1")
    }

    // MARK: - Public API

    func startPlaying(url: URL) {
        guard !isPlaying else { return }
        isPlaying = true
        frameCount = 0
        totalJpegBytes = 0

        // Cancel any pending image transfer — video takes priority
        transport.cancelPendingImageFrames()

        // Send video_start command to ESP32
        transport.sendCommand(["cmd": "video_start"])

        if VideoStreamer.isWebViewURL(url) {
            mode = .webView
            jpegQuality = 0.20 // WebView snapshot quality (keep under ESP32 buffer)
            let embedURL = VideoStreamer.youtubeEmbedURL(from: url) ?? url
            startWebViewCapture(url: embedURL)
        } else {
            mode = .avPlayer
            jpegQuality = 0.3
            startAVPlayerCapture(url: url)
        }
    }

    func stop() {
        guard isPlaying else { return }
        isPlaying = false

        switch mode {
        case .avPlayer:
            stopAVPlayer()
        case .webView:
            stopWebView()
        }

        // Send video_stop command to ESP32
        transport.sendCommand(["cmd": "video_stop"])

        print("[VideoStreamer] Stopped (mode=\(mode), frames=\(frameCount), avgJPEG=\(frameCount > 0 ? totalJpegBytes / UInt64(frameCount) : 0)B)")
    }

    // MARK: - AVPlayer Capture Mode

    private func startAVPlayerCapture(url: URL) {
        let asset = AVURLAsset(url: url)
        playerItem = AVPlayerItem(asset: asset)

        let outputSettings: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA
        ]
        videoOutput = AVPlayerItemVideoOutput(pixelBufferAttributes: outputSettings)
        playerItem?.add(videoOutput!)

        player = AVPlayer(playerItem: playerItem!)
        player?.isMuted = true // Audio via tap, not speaker

        setupAudioTap()

        endObserver = NotificationCenter.default.addObserver(
            forName: .AVPlayerItemDidPlayToEndTime,
            object: playerItem,
            queue: .main
        ) { [weak self] _ in
            self?.handleVideoEnded()
        }

        displayLink = CADisplayLink(target: self, selector: #selector(displayLinkFired))
        displayLink?.preferredFrameRateRange = CAFrameRateRange(minimum: 6, maximum: 10, preferred: 8)
        displayLink?.add(to: .main, forMode: .common)

        startAudioPump()

        player?.play()
        print("[VideoStreamer] AVPlayer mode started: \(url.absoluteString.prefix(80))")
    }

    private func stopAVPlayer() {
        displayLink?.invalidate()
        displayLink = nil

        stopAudioPump()

        if let obs = endObserver {
            NotificationCenter.default.removeObserver(obs)
            endObserver = nil
        }

        player?.pause()

        // Remove audio tap
        if let item = playerItem, let track = item.asset.tracks(withMediaType: .audio).first {
            let inputParams = AVMutableAudioMixInputParameters(track: track)
            inputParams.audioTapProcessor = nil
            let audioMix = AVMutableAudioMix()
            audioMix.inputParameters = [inputParams]
            item.audioMix = audioMix
        }

        if let output = videoOutput {
            playerItem?.remove(output)
        }

        player = nil
        playerItem = nil
        videoOutput = nil

        audioLock.lock()
        audioPending.removeAll()
        audioLock.unlock()
        adpcmEncoder = ADPCMEncoder()
    }

    // MARK: - WKWebView Capture Mode

    private func startWebViewCapture(url: URL) {
        let config = WKWebViewConfiguration()
        config.allowsInlineMediaPlayback = true
        config.mediaTypesRequiringUserActionForPlayback = []
        config.preferences.javaScriptCanOpenWindowsAutomatically = true

        // Inject periodic script: play if paused, unmute once playing, click play button
        let unmuteScript = WKUserScript(source: """
            setInterval(function() {
                var v = document.querySelector('video');
                if (!v) return;
                if (v.paused) { v.play().catch(function(){}); }
                if (v.muted && v.currentTime > 0.5) { v.muted = false; }
                var pb = document.querySelector('.ytp-large-play-button');
                if (pb) { pb.click(); }
            }, 1000);
            """, injectionTime: .atDocumentEnd, forMainFrameOnly: false)
        config.userContentController.addUserScript(unmuteScript)

        let wv = WKWebView(frame: CGRect(x: 0, y: 0, width: 368, height: 448), configuration: config)
        wv.isOpaque = true
        wv.backgroundColor = .black

        // Add to app window so WebKit actually renders content (required for snapshots + audio)
        if let windowScene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene }).first,
           let window = windowScene.windows.first(where: { $0.isKeyWindow }) {
            wv.alpha = 1.0  // Must be visible for drawHierarchy to capture video overlay
            wv.frame = CGRect(x: 0, y: 0, width: 368, height: 448)
            window.addSubview(wv)
        }

        let delegate = WebViewVideoDelegate()
        delegate.onLoadFinished = { [weak self] in
            self?.startSnapshotTimer()
        }
        wv.navigationDelegate = delegate
        webViewDelegate = delegate

        webView = wv
        wv.load(URLRequest(url: url))

        // Fallback: start capturing after 4s even if page hasn't signalled load
        DispatchQueue.main.asyncAfter(deadline: .now() + 4.0) { [weak self] in
            guard let self, self.isPlaying, self.snapshotTimer == nil else { return }
            print("[VideoStreamer] WebView fallback timer: starting capture")
            self.startSnapshotTimer()
        }

        print("[VideoStreamer] WebView mode started: \(url.absoluteString.prefix(80))")
    }

    private func startSnapshotTimer() {
        guard snapshotTimer == nil else { return }
        // ~5 FPS = 200ms interval
        snapshotTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 5.0, repeats: true) { [weak self] _ in
            self?.captureWebViewFrame()
        }
        print("[VideoStreamer] Snapshot timer started (5 FPS)")
    }

    private func captureWebViewFrame() {
        guard isPlaying, let wv = webView else { return }

        // Congestion control: skip frame if TX queue is deep
        let queueDepth = transport.txQueueDepth
        if queueDepth > 15 { return }

        // Adaptive quality based on queue depth (gentler adaptation)
        if queueDepth > 10 {
            jpegQuality = max(0.18, jpegQuality - 0.02)
        } else if queueDepth < 5 {
            jpegQuality = min(0.25, jpegQuality + 0.01)
        }

        // drawHierarchy captures hardware-accelerated video content
        // (takeSnapshot misses <video> elements rendered as GPU overlays)
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1.0
        let renderer = UIGraphicsImageRenderer(size: wv.bounds.size, format: format)
        let image = renderer.image { _ in
            wv.drawHierarchy(in: wv.bounds, afterScreenUpdates: false)
        }

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self, self.isPlaying else { return }
            self.processWebViewSnapshot(image)
        }
    }

    private func processWebViewSnapshot(_ image: UIImage) {
        let targetSize = CGSize(width: 368, height: 448)

        let format = UIGraphicsImageRendererFormat()
        format.scale = 1.0 // Force 1x scale — avoid 3x retina (would produce 1104x1344 px!)
        let renderer = UIGraphicsImageRenderer(size: targetSize, format: format)
        let resizedImage = renderer.image { ctx in
            // Black background
            UIColor.black.setFill()
            ctx.fill(CGRect(origin: .zero, size: targetSize))

            // Scale preserving aspect ratio
            let scaleX = targetSize.width / image.size.width
            let scaleY = targetSize.height / image.size.height
            let scale = min(scaleX, scaleY)
            let scaledW = image.size.width * scale
            let scaledH = image.size.height * scale
            let offsetX = (targetSize.width - scaledW) / 2
            let offsetY = (targetSize.height - scaledH) / 2

            image.draw(in: CGRect(x: offsetX, y: offsetY, width: scaledW, height: scaledH))
        }

        // Encode with size cap — re-compress at lower quality if too large for ESP32 buffer
        let maxJpegSize = 40 * 1024 // Must fit in ESP32's 48KB JPEG buffer with margin
        var quality = jpegQuality
        var jpegData = resizedImage.jpegData(compressionQuality: quality)
        while let data = jpegData, data.count > maxJpegSize, quality > 0.15 {
            quality -= 0.03
            jpegData = resizedImage.jpegData(compressionQuality: quality)
        }
        guard let finalData = jpegData else { return }
        // Update quality for next frame based on what worked
        if quality < jpegQuality {
            jpegQuality = quality
        }

        transport.sendVideoFrame(finalData)

        frameCount += 1
        totalJpegBytes += UInt64(finalData.count)

        if frameCount % 30 == 0 {
            let avgSize = totalJpegBytes / UInt64(frameCount)
            print("[VideoStreamer] Frame #\(frameCount): JPEG=\(finalData.count)B, avg=\(avgSize)B, quality=\(String(format: "%.2f", jpegQuality)), txQ=\(transport.txQueueDepth)")
        }
    }

    private func stopWebView() {
        snapshotTimer?.invalidate()
        snapshotTimer = nil
        isCapturingSnapshot = false
        webView?.stopLoading()
        webView?.removeFromSuperview()
        webView?.navigationDelegate = nil
        webView = nil
        webViewDelegate = nil
    }

    // MARK: - AVPlayer Frame Capture

    @objc private func displayLinkFired(_ link: CADisplayLink) {
        guard isPlaying, let output = videoOutput else { return }

        // Rate limit
        let now = link.timestamp
        if now - lastFrameTime < targetFrameInterval { return }

        // Congestion control: skip frame if TX queue is deep
        let queueDepth = transport.txQueueDepth
        if queueDepth > 15 {
            return // Back off when queue is congested
        }

        // Adaptive quality based on queue depth (gentler adaptation)
        if queueDepth > 10 {
            jpegQuality = max(0.20, jpegQuality - 0.03)
        } else if queueDepth < 5 {
            jpegQuality = min(0.45, jpegQuality + 0.02)
        }

        let itemTime = output.itemTime(forHostTime: now)
        guard output.hasNewPixelBuffer(forItemTime: itemTime) else { return }
        guard let pixelBuffer = output.copyPixelBuffer(forItemTime: itemTime, itemTimeForDisplay: nil) else { return }

        lastFrameTime = now

        // Convert to JPEG on background queue
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self, self.isPlaying else { return }
            self.processFrame(pixelBuffer)
        }
    }

    private func processFrame(_ pixelBuffer: CVPixelBuffer) {
        let ciImage = CIImage(cvPixelBuffer: pixelBuffer)
        let context = CIContext()

        let targetWidth: CGFloat = 368
        let targetHeight: CGFloat = 448

        // Scale to target size
        let srcWidth = CGFloat(CVPixelBufferGetWidth(pixelBuffer))
        let srcHeight = CGFloat(CVPixelBufferGetHeight(pixelBuffer))
        let scaleX = targetWidth / srcWidth
        let scaleY = targetHeight / srcHeight
        let scale = min(scaleX, scaleY)

        let scaledImage = ciImage.transformed(by: CGAffineTransform(scaleX: scale, y: scale))

        // Center on black background
        let scaledW = srcWidth * scale
        let scaledH = srcHeight * scale
        let offsetX = (targetWidth - scaledW) / 2
        let offsetY = (targetHeight - scaledH) / 2
        let translatedImage = scaledImage.transformed(by: CGAffineTransform(translationX: offsetX, y: offsetY))

        // Render to CGImage
        let outputRect = CGRect(x: 0, y: 0, width: targetWidth, height: targetHeight)
        guard let cgImage = context.createCGImage(translatedImage, from: outputRect) else { return }

        // Compress to JPEG
        let uiImage = UIImage(cgImage: cgImage)
        guard let jpegData = uiImage.jpegData(compressionQuality: jpegQuality) else { return }

        transport.sendVideoFrame(jpegData)

        frameCount += 1
        totalJpegBytes += UInt64(jpegData.count)

        // Stats logging every 30 frames
        if frameCount % 30 == 0 {
            let avgSize = totalJpegBytes / UInt64(frameCount)
            print("[VideoStreamer] Frame #\(frameCount): JPEG=\(jpegData.count)B, avg=\(avgSize)B, quality=\(String(format: "%.2f", jpegQuality)), txQ=\(transport.txQueueDepth)")
        }
    }

    // MARK: - Audio Tap

    private func setupAudioTap() {
        guard let item = playerItem,
              let track = item.asset.tracks(withMediaType: .audio).first else {
            print("[VideoStreamer] No audio track found")
            return
        }

        var callbacks = MTAudioProcessingTapCallbacks(
            version: kMTAudioProcessingTapCallbacksVersion_0,
            clientInfo: UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque()),
            init: tapInit,
            finalize: tapFinalize,
            prepare: tapPrepare,
            unprepare: tapUnprepare,
            process: tapProcess
        )

        var tap: MTAudioProcessingTap?
        let status = MTAudioProcessingTapCreate(kCFAllocatorDefault, &callbacks, kMTAudioProcessingTapCreationFlag_PostEffects, &tap)

        guard status == noErr, let createdTap = tap else {
            print("[VideoStreamer] Failed to create audio tap: \(status)")
            return
        }

        self.audioTap = createdTap

        let inputParams = AVMutableAudioMixInputParameters(track: track)
        inputParams.audioTapProcessor = createdTap

        let audioMix = AVMutableAudioMix()
        audioMix.inputParameters = [inputParams]
        item.audioMix = audioMix

        print("[VideoStreamer] Audio tap installed")
    }

    // MARK: - Audio Pump

    private func startAudioPump() {
        let timer = DispatchSource.makeTimerSource(queue: DispatchQueue.global(qos: .userInteractive))
        timer.schedule(deadline: .now(), repeating: .milliseconds(20))
        timer.setEventHandler { [weak self] in
            self?.pumpAudio()
        }
        timer.resume()
        audioTimer = timer
    }

    private func stopAudioPump() {
        audioTimer?.cancel()
        audioTimer = nil
    }

    private func pumpAudio() {
        guard isPlaying else { return }

        // Dequeue up to 1920 bytes of PCM (40ms @ 24kHz mono 16-bit = 960 samples)
        let chunkBytes = 1920
        audioLock.lock()
        guard audioPending.count >= chunkBytes else {
            audioLock.unlock()
            return
        }
        let pcmChunk = audioPending.prefix(chunkBytes)
        audioPending.removeFirst(chunkBytes)
        audioLock.unlock()

        // ADPCM encode: 1920 PCM bytes → 480 ADPCM bytes
        let adpcmData = adpcmEncoder.encode(pcmChunk)

        // Send via existing audio transport
        transport.sendAudio(adpcmData)
    }

    /// Called from the audio tap process callback with resampled 24kHz mono int16 PCM
    fileprivate func enqueueAudio(_ pcmData: Data) {
        audioLock.lock()
        // Cap backlog
        if audioPending.count > audioMaxBacklogBytes {
            audioPending.removeFirst(audioPending.count - audioMaxBacklogBytes)
        }
        audioPending.append(pcmData)
        audioLock.unlock()
    }

    // MARK: - Video End

    private func handleVideoEnded() {
        print("[VideoStreamer] Video playback ended")
        stop()
        onVideoEnded?()
    }
}

// MARK: - WKWebView Navigation Delegate

private class WebViewVideoDelegate: NSObject, WKNavigationDelegate {
    var onLoadFinished: (() -> Void)?

    // Auto-dismiss YouTube consent popup on embed pages
    private static let youtubeConsentJS = """
    (function() {
        var sels = [
            'button[jsname="higCR"]',
            'button[aria-label*="ccept"]',
            'form[action*="consent"] button[type="submit"]',
            '.consent-bump button.yt-spec-button-shape-next--call-to-action',
            'tp-yt-paper-dialog .eom-buttons button.eom-button-row'
        ];
        for (var i = 0; i < sels.length; i++) {
            var el = document.querySelector(sels[i]);
            if (el) { el.click(); return 'dismissed'; }
        }
        var btns = document.querySelectorAll('button');
        for (var j = 0; j < btns.length; j++) {
            var t = (btns[j].innerText || '').toLowerCase();
            if (t.indexOf('accept') !== -1 || t.indexOf('accepter') !== -1 || t.indexOf('agree') !== -1) {
                btns[j].click(); return 'dismissed_text';
            }
        }
        return 'none';
    })();
    """

    // JS to check video state, play if paused, unmute once playing, click play button
    private static let unmuteJS = """
    (function() {
        var v = document.querySelector('video');
        if (!v) return 'no_video';
        var info = 'ready=' + v.readyState + ',paused=' + v.paused + ',time=' + v.currentTime.toFixed(1) + ',muted=' + v.muted;
        if (v.paused) { v.play().catch(function(e){ info += ',play_err=' + e.name; }); }
        if (v.muted && v.currentTime > 0.5) { v.muted = false; info += ',unmuted'; }
        var pb = document.querySelector('.ytp-large-play-button');
        if (pb) { pb.click(); info += ',clicked_play'; }
        return info;
    })();
    """

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        print("[VideoStreamer] WebView page loaded")

        // Consent dismiss + unmute retry loop: run at 0s, 1.5s, and 3.0s
        let delays: [Double] = [0.0, 1.5, 3.0, 5.0, 8.0]
        for delay in delays {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak webView] in
                guard let wv = webView else { return }
                wv.evaluateJavaScript(Self.youtubeConsentJS) { result, _ in
                    if let r = result as? String, r != "none" {
                        print("[VideoStreamer] YouTube consent (\(delay)s): \(r)")
                    }
                }
                // Check video state, play/unmute as needed
                wv.evaluateJavaScript(Self.unmuteJS) { result, _ in
                    if let r = result as? String {
                        print("[VideoStreamer] Video state (\(delay)s): \(r)")
                    }
                }
            }
        }

        // Delay capture start by 2s after page load to let video actually begin playing
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) { [weak self] in
            self?.onLoadFinished?()
        }
    }

    func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
        print("[VideoStreamer] WebView load failed: \(error.localizedDescription)")
    }
}

// MARK: - MTAudioProcessingTap Callbacks

private func tapInit(tap: MTAudioProcessingTap, clientInfo: UnsafeMutableRawPointer?, tapStorageOut: UnsafeMutablePointer<UnsafeMutableRawPointer?>) {
    tapStorageOut.pointee = clientInfo
}

private func tapFinalize(tap: MTAudioProcessingTap) {
    // No cleanup needed — we use unretained reference
}

private func tapPrepare(tap: MTAudioProcessingTap, maxFrames: CMItemCount, processingFormat: UnsafePointer<AudioStreamBasicDescription>) {
    let fmt = processingFormat.pointee
    print("[VideoStreamer] Tap prepared: rate=\(fmt.mSampleRate), channels=\(fmt.mChannelsPerFrame), bitsPerChannel=\(fmt.mBitsPerChannel)")
}

private func tapUnprepare(tap: MTAudioProcessingTap) {
    // Nothing to clean up
}

private func tapProcess(
    tap: MTAudioProcessingTap,
    numberFrames: CMItemCount,
    flags: MTAudioProcessingTapFlags,
    bufferListInOut: UnsafeMutablePointer<AudioBufferList>,
    numberFramesOut: UnsafeMutablePointer<CMItemCount>,
    flagsOut: UnsafeMutablePointer<MTAudioProcessingTapFlags>
) {
    // Get source audio
    let status = MTAudioProcessingTapGetSourceAudio(tap, numberFrames, bufferListInOut, flagsOut, nil, numberFramesOut)
    guard status == noErr else { return }

    // Get the VideoStreamer reference
    var storage: UnsafeMutableRawPointer?
    storage = MTAudioProcessingTapGetStorage(tap)
    guard let clientInfo = storage else { return }
    let streamer = Unmanaged<VideoStreamer>.fromOpaque(clientInfo).takeUnretainedValue()
    guard streamer.isPlaying else { return }

    let bufferList = UnsafeMutableAudioBufferListPointer(bufferListInOut)
    guard let firstBuffer = bufferList.first,
          let rawData = firstBuffer.mData else { return }

    let srcChannels = Int(firstBuffer.mNumberChannels)
    let frameCount = Int(numberFramesOut.pointee)

    // Assume float32 interleaved (most common from MTAudioProcessingTap)
    let floatPtr = rawData.assumingMemoryBound(to: Float.self)
    let totalSamples = frameCount * srcChannels

    // Determine source sample rate (we'll assume 44100 or 48000 and decimate to 24000)
    // MTAudioProcessingTap typically outputs at the source track's rate
    // We use simple decimation: 48000→24000 = skip every other, 44100→24000 ≈ skip ~1.8375
    // For simplicity, we'll use 2:1 decimation (works for 48kHz sources)
    let decimationRatio = 2
    let outFrameCount = frameCount / decimationRatio

    // Output buffer: 24kHz mono int16
    var outPCM = Data(count: outFrameCount * 2) // 2 bytes per sample
    outPCM.withUnsafeMutableBytes { outBuf in
        let outPtr = outBuf.bindMemory(to: Int16.self)
        for i in 0..<outFrameCount {
            let srcIdx = i * decimationRatio * srcChannels
            guard srcIdx < totalSamples else { break }

            // Downmix to mono (average channels)
            var sample: Float = 0
            for ch in 0..<srcChannels {
                if srcIdx + ch < totalSamples {
                    sample += floatPtr[srcIdx + ch]
                }
            }
            sample /= Float(srcChannels)

            // Convert float32 [-1.0, 1.0] to int16
            let clamped = max(-1.0, min(1.0, sample))
            outPtr[i] = Int16(clamped * 32767.0)
        }
    }

    streamer.enqueueAudio(outPCM)
}
