# PocketPall

> Wearable ESP32-S3 AI companion with a 4-inch AMOLED touchscreen, LVGL UI, real-time audio streaming over WiFi TCP, and pluggable AI providers (Gemini, ChatGPT, Claude). An iOS companion app acts as the WiFi hotspot gateway and provider relay.

## Overview

PocketPall is a single-board voice, text, and video assistant built around an ESP32-S3 (dual Xtensa LX7 @ 240 MHz, 16 MB flash, 8 MB octal PSRAM) and a Waveshare 4-inch AMOLED panel (368x448, QSPI via SH8601). Ten-plus peripherals share a single I²C bus at 100 kHz — capacitive touch (FT3168), stereo audio codec (ES8311), 6-axis IMU (QMI8658), PMIC (AXP2101), and an 8-bit I/O expander (TCA9554) — while QSPI, full-duplex I²S, and the WiFi stack compete for DMA channels, PSRAM bandwidth, and CPU cycles. The firmware is plain ESP-IDF 5.5 C with LVGL 8.4 on top; there is no Linux underneath.

The device runs in three modes. In **voice mode**, 16 kHz mono PCM is captured from the ES8311, gated by an energy-based VAD, compressed to IMA-ADPCM, and streamed in 8-byte-framed TCP chunks through an iPhone's Personal Hotspot to the chosen AI provider. In **text mode**, an LVGL on-screen keyboard feeds text to the provider and renders markdown responses with embedded LaTeX math through a custom 38 KB renderer that draws directly into the AMOLED's framebuffer. In **video mode**, the iOS app encodes a camera stream as MJPEG, the firmware decodes it in PSRAM, and a QMI8658-driven rotation task (EMA-filtered, 3-sample debounce) keeps the image upright regardless of how the device is held.

The iOS companion app is a SwiftUI application that acts as a TCP server on port 5050 with mDNS advertising under `_pocketai._tcp`. It brokers audio, text, and video frames between the device and the selected provider: Google Gemini (WebSocket Live API plus a persistent WKWebView for consent bypass), OpenAI (ChatGPT Realtime REST + WebSocket), and Anthropic Claude (stub, waiting on a first-party realtime audio API). API keys live in the iOS Keychain; the firmware never sees them.

## Hardware

| Component | Part | Bus / Purpose |
|---|---|---|
| MCU | ESP32-S3 (dual Xtensa LX7 @ 240 MHz, 16 MB flash, 8 MB octal PSRAM @ 80 MHz) | Host |
| Display | Waveshare SH8601 AMOLED (4-inch, 368x448, RGB565) | QSPI @ 30 MHz (SPI2_HOST, D0-D3 on GPIO4-7) |
| Touch | FT3168 capacitive (FT5x06 compatible) | I²C @ 0x38, INT on GPIO21, reset via TCA9554 |
| Audio codec | ES8311 stereo codec | I²S full-duplex + I²C @ 0x30, PA enable on GPIO46 |
| IMU | QMI8658 6-axis accelerometer + gyro | I²C @ 0x6B |
| PMIC | AXP2101 (LiPo charger, LDO rails, fuel gauge) | I²C @ 0x34 |
| I/O expander | TCA9554 (8-bit, used for touch reset, display reset, power button) | I²C @ 0x20 |
| Connectivity | WiFi STA 802.11 b/g/n, Bluetooth LE | On-SoC |

I²C bus is shared: SDA=GPIO15, SCL=GPIO14, 100 kHz master. `power_manager_init_i2c()` creates a single `i2c_master_bus_handle_t`, then each HAL registers its device via `i2c_master_bus_add_device()`. Any optional peripheral that fails probe (PMIC, IMU, I/O expander) returns `ESP_ERR_NOT_FOUND` and the boot continues with the dependent feature disabled.

Reference hardware module: see `HARDWARE.md`.

## Architecture

### Data flow

- **Voice mode:** I²S mic (ES8311, 16 kHz, stereo) → averaged to mono → DC-removal IIR ($\alpha=0.995$) → pre-emphasis ($0.97$) → RMS energy VAD → 240-sample chunks → IMA-ADPCM encode → 8-byte framed TCP to iOS → AI provider (Gemini Live, OpenAI Realtime, Claude stub) → response frames back → IMA-ADPCM decode (89-entry step table) → 24 kHz playback ring buffer → I²S DAC → speaker. A 20-chunk pre-roll ring (~300 ms) catches the first phoneme before VAD opens.
- **Text mode:** LVGL keyboard → TCP text frame (UTF-8, optional `$$...$$` math blocks) → iOS → provider → markdown response → LVGL typewriter render, with the custom LaTeX renderer drawing fractions, square roots, Greek letters, and scaled delimiters directly into an LVGL label.
- **Video mode:** iOS camera → MJPEG encode → TCP video frame (4 KB chunks) → firmware `tjpgd_standalone.c` decode into a 322 KB RGB565 PSRAM framebuffer → byte-swapped (`__builtin_bswap16`) → SH8601 display. A parallel rotation task polls the QMI8658 every 100 ms, low-passes acceleration with an EMA ($\alpha=0.25$), requires 3 stable samples before committing a rotation, and fades through black (150 ms out, 200 ms in) so the UI never tears during the transition.

### Dual-core FreeRTOS layout

- **Core 0:** `app_task` (8 KB stack, priority 5) drives the enum state machine — `APP_STATE_INIT` → `WIFI_SETUP` → `WIFI_SCANNING` → `WIFI_CONNECTING` → `WIFI_CONNECTED` → `READY` → `LISTENING` → `PROCESSING` → `RESPONDING`. LVGL runs here via `esp_lvgl_port`, and so do the WiFi stack, mDNS, and TCP client. The event group `g_app_events` binds state transitions to the rest of the system.
- **Core 1:** `audio_hal` streaming loop (8 KB stack, priority 6) owns the I²S DMA, VAD, DSP filters, pre-roll ring, and the encoder — it is deliberately isolated from UI work so nothing in LVGL or WiFi can jitter the audio clock.

### Quantified constraints

| Subsystem | Budget | Notes |
|---|---|---|
| LVGL draw buffer | 45 lines × 368 × 2 B = ~33 KB | 16-line transfer granularity, double-buffered |
| LVGL refresh | 25 ms tick | Compatible with ES8311 DMA frame interval |
| Audio capture | 16 kHz, 240-sample chunks, 15 ms per chunk | Matches `WIFI_AUDIO_CHUNK_SIZE=480` bytes |
| Audio playback | 24 kHz PCM, 10-second ring buffer | Provider downlink decoded before ring push |
| VAD | Energy threshold 60, min speech 120 ms, silence timeout 1500 ms | Stream pre-roll 240 ms (up to 20 chunks) |
| Video framebuffer | 368 × 448 × 2 B ≈ 322 KB in PSRAM | Byte-swapped for `LV_COLOR_16_SWAP=y` |
| LaTeX render pool | 8 entries in PSRAM | Prevents internal-RAM pressure on deep trees |
| TCP ring | 16 KB TX / 16 KB RX, 32 KB reassembly | Single socket, multiplexed frames |

## Directory layout

```
PocketPall/
├── Personlig_Ai/                  # ESP32-S3 firmware (ESP-IDF, C)
│   ├── main/
│   │   ├── main.c                 # Entry, app state machine, gesture dispatch
│   │   ├── app_config.h           # Pinout, clocks, buffers, state enums
│   │   ├── ui_manager.c           # LVGL screen management, typewriter, keyboard
│   │   ├── audio_hal.c            # I²S + ES8311 + VAD + IIR filters + pre-roll
│   │   ├── display_hal.c          # SH8601 QSPI bring-up
│   │   ├── touch_hal.c            # FT3168 I²C + INT
│   │   ├── imu_hal.c              # QMI8658 I²C, EMA-filtered rotation
│   │   ├── power_manager.c        # AXP2101 + TCA9554, shared I²C bus owner
│   │   ├── wifi_transport.c       # STA + TCP client + mDNS + NVS credentials
│   │   ├── latex_math.c           # Embedded LaTeX math renderer (~38 KB C)
│   │   ├── image_display.c        # Single-frame JPEG decode + LVGL canvas
│   │   ├── video_display.c        # MJPEG stream into PSRAM framebuffer
│   │   ├── tjpgd_standalone.c     # Standalone TJpgDec R0.03, JD_FORMAT=1 (RGB565)
│   │   ├── l2cap_transport.*      # Legacy BLE transport (retained, unused)
│   │   └── fonts/                 # LVGL font binaries (Montserrat + math subset)
│   ├── components/
│   │   └── waveshare__esp_lcd_sh8601/   # Vendored Waveshare SH8601 driver
│   ├── CMakeLists.txt
│   ├── partitions.csv
│   └── sdkconfig.defaults
├── PocketPall/                    # iOS companion (Swift, SwiftUI)
│   ├── PocketPall/
│   │   ├── PocketPallApp.swift
│   │   ├── Models/
│   │   │   ├── AIProvider.swift         # Provider protocol
│   │   │   ├── GeminiProvider.swift     # Gemini Live WebSocket + WKWebView consent
│   │   │   ├── OpenAIProvider.swift     # ChatGPT Realtime REST + WebSocket
│   │   │   ├── ClaudeProvider.swift     # Stub, pending Anthropic realtime audio
│   │   │   ├── RuntimeProfile.swift
│   │   │   └── SavedDevice.swift
│   │   ├── Views/
│   │   │   ├── SetupView.swift          # First-run AI + key entry
│   │   │   ├── MainView.swift           # Active-session UI
│   │   │   ├── ConnectView.swift
│   │   │   ├── ScanDevicesView.swift
│   │   │   └── SavedDevicesView.swift
│   │   ├── Services/
│   │   │   ├── AudioBridge.swift        # PCM ↔ provider audio routing
│   │   │   ├── VideoStreamer.swift      # Camera → MJPEG → TCP
│   │   │   ├── ImageProcessor.swift
│   │   │   └── KeychainService.swift    # API key storage
│   │   ├── Runtime/
│   │   │   ├── PocketPallRuntimeCoordinator.swift
│   │   │   ├── RuntimePermissionCoordinator.swift
│   │   │   ├── BackgroundVoiceSessionKeeper.swift
│   │   │   └── RuntimeNotifier.swift
│   │   ├── BLE/
│   │   │   ├── WiFiTransportManager.swift   # TCP server on :5050 (active path)
│   │   │   ├── L2CAPManager.swift           # Legacy BLE L2CAP (retained)
│   │   │   ├── L2CAPProtocol.swift
│   │   │   ├── BLEConstants.swift
│   │   │   └── DisplayCommand.swift
│   │   └── Info.plist
│   └── PocketPall.xcodeproj
├── HARDWARE.md
├── CHANGELOG.md                   # BLE L2CAP → WiFi TCP transport migration
└── README.md
```

## Build and run

### ESP32-S3 firmware

Requires **ESP-IDF 5.5** or newer.

```bash
cd Personlig_Ai
source ~/esp/v5.5.1/esp-idf/export.sh    # or wherever your ESP-IDF lives
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

`sdkconfig.defaults` turns on 8 MB octal PSRAM @ 80 MHz, `LV_COLOR_16_SWAP=y`, and a custom partition table (`partitions.csv`) that reserves NVS for WiFi credentials plus a 4 MB application slot.

First-run WiFi provisioning:

1. Power on. The device boots through a fixed `[1/7]`..`[7/7]` HAL init sequence and lands on the "Find devices" screen (`APP_STATE_WIFI_SETUP`).
2. Tap → WiFi scan → select your iPhone Personal Hotspot (or any 2.4 GHz AP on the same subnet) → enter the password through the on-screen keyboard.
3. Credentials are written to NVS under namespace `wifi_creds`. Every subsequent boot auto-reconnects; mDNS discovers `_pocketai._tcp` on the LAN, falling back to gateway `172.20.10.1` when the host is an iPhone Personal Hotspot.

### iOS companion app

Requires **Xcode 15** or newer, iOS 15+ deployment target.

```bash
cd PocketPall
open PocketPall.xcodeproj
# In Xcode: set your Team ID under Signing & Capabilities, pick an iPhone, run.
```

`NSBonjourServices` in `Info.plist` already advertises `_pocketai._tcp`. `UIBackgroundModes = audio` keeps voice sessions alive when the phone is locked.

First-run AI provider configuration:

1. App shows "Choose your AI".
2. Pick Gemini, ChatGPT, or Claude.
3. Paste your API key. It is written to the iOS Keychain via `KeychainService.swift` and never touches disk, source, or the firmware.

## Configuration

Before building, fill in these placeholders:

| Placeholder | File | What it is |
|---|---|---|
| `YOUR_TEAM_ID` | `PocketPall/PocketPall.xcodeproj/project.pbxproj` | Apple developer Team ID (for code signing) |
| `YOUR_GOOGLE_OAUTH_CLIENT_ID` | `PocketPall/PocketPall/Info.plist` | Google Sign-In OAuth client ID from Google Cloud Console → APIs & Services → Credentials (appears in both `GIDClientID` and the reversed URL scheme) |

Runtime secrets that are **not** in source:

- Gemini / OpenAI / Anthropic API keys — entered in the app's setup flow, stored in iOS Keychain.
- ESP32-S3 WiFi SSID and password — entered through the on-device UI, stored in NVS.

## Wire protocol

TCP, port **5050**, mDNS service `_pocketai._tcp`. Every payload is wrapped in an 8-byte little-endian frame header:

```
offset  size  field
0       1     type      (1=audio, 2=text, 3=video, 4=control)
1       1     flags
2       2     sequence  (LE)
4       4     payload length (LE)
8       N     payload
```

| Type | Codec / format | Notes |
|---|---|---|
| Audio | IMA-ADPCM uplink, PCM 16-bit downlink | Uplink: 16 kHz mono, 240-sample chunks, 480-byte payload. Downlink: 24 kHz mono PCM decoded to the ring buffer. |
| Text | UTF-8 | Optional inline `$...$` / display `$$...$$` math blocks are rendered on-device by `latex_math.c`. |
| Video | MJPEG | 15 fps target, 4 KB chunks reassembled in a 32 KB RX buffer before being handed to `tjpgd_standalone.c`. |
| Control | JSON | State transitions, volume, mode switches, pings. |

The firmware's `wifi_transport.c` exposes four RX callbacks — `on_audio`, `on_command`, `on_image`, `on_video` — so modes plug into the same transport without touching LwIP directly. WiFi credentials persist in NVS (namespace `wifi_creds`); TX/RX buffers are 16 KB each.

## Design notes

Design decisions that are not obvious from the file list:

### Shared I²C bus, one owner

Every I²C device hangs off a single `i2c_master_bus_handle_t` created by `power_manager_init_i2c()`. HALs do not own the bus; they call `i2c_master_bus_add_device()` at their verified address (AXP2101 @ 0x34, TCA9554 @ 0x20, FT3168 @ 0x38, QMI8658 @ 0x6B, ES8311 @ 0x30) and that is the full extent of their bus knowledge. Probe failures return `ESP_ERR_NOT_FOUND` to the caller — the boot sequence logs the failure, skips the dependent feature, and continues. A missing IMU disables auto-rotation but does not block voice mode; a missing PMIC disables battery metering but does not block the display.

### Audio task isolation

LVGL, LwIP, and the TLS handshake on Core 0 can all produce long critical sections. The I²S DMA interrupt is cheap, but the VAD loop and the IMA-ADPCM encoder are not, so both live on Core 1, pinned, above LVGL priority, and share only the ring-buffered transport and the event group with Core 0. The audio clock then no longer jitters when the user opens a heavy LVGL screen.

### PSRAM pools, not malloc

Three subsystems that would otherwise chew internal DRAM live in PSRAM pools instead: the LVGL draw buffers, the 322 KB video framebuffer, and the LaTeX renderer's 8-entry layout pool. `sdkconfig.defaults` enables octal PSRAM at 80 MHz for exactly this reason. Internal DRAM is reserved for WiFi control blocks, the audio ring, and FreeRTOS stacks.

### IMU rotation is debounced, filtered, and faded

A raw accelerometer reading is too noisy to drive a UI rotation. The rotation task polls every 100 ms, feeds each sample through an EMA low-pass with $\alpha=0.25$, compares $|a_x|$, $|a_y|$, $|a_z|$ against `IMU_ROTATE_MIN_TILT_G=0.45` and `IMU_ROTATE_MAX_FLAT_Z_G=0.82`, requires three consecutive stable samples (`IMU_ROTATE_STABLE_SAMPLES=3`, ≈300 ms), and then fades through black — 150 ms out, 200 ms in — before committing the new orientation. The fade hides the single frame that would otherwise tear during the LVGL display rotation.

### Transport is layered, not tangled

`wifi_transport.c` sits directly on LwIP sockets. It does framing, reassembly, and the four typed RX callbacks. Modes (voice, text, video, control) register for their callback and never touch the socket. Re-targeting to a different transport is swapping one file; that is how the project migrated from BLE L2CAP to WiFi TCP (see `CHANGELOG.md`).

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Boot halts at `[1/7] Power` | AXP2101 not ACKing at 0x34 | Check SDA=GPIO15, SCL=GPIO14 pulls; verify LDO rails before the probe |
| Display stays black after boot | SH8601 reset line wired through TCA9554 | Verify TCA9554 probe succeeds; the display driver waits on the expander |
| Touch works but no INT | FT3168 INT on GPIO21 floating | Add a pull-up, or confirm the device tree configures it |
| Audio is clipped at the start of each phrase | VAD pre-roll too short | Raise `VAD_STREAM_PREROLL_MS` / `VAD_STREAM_PREROLL_MAX_CHUNKS` in `app_config.h` |
| iPhone does not see the device on mDNS | Local network permission not granted | iOS Settings → PocketPall → Local Network → on |
| WiFi reconnect loops every 3 s | Stale credentials in NVS | On device: WiFi setup → Forget, then re-provision. Or erase NVS: `idf.py erase-flash` |
| LaTeX math renders as literal text | Font binary missing | Confirm `main/fonts/lv_font_math_18.c` is in the `main/CMakeLists.txt` SRCS list |

## Status

- **Firmware:** stable on ESP-IDF 5.5 + LVGL 8.4 + vendored Waveshare SH8601 driver. Boot sequence tested cold-start to `APP_STATE_WIFI_SETUP` with all ten peripherals initialized; optional ones degrade gracefully.
- **iOS app:** builds on Xcode 15+, tested on iPhone 15 and iPad Pro. `WiFiTransportManager` is the active transport path.
- **AI providers:**
  - **Gemini** — full duplex over the Live API WebSocket, with a persistent WKWebView that holds a consent session across app launches so the user doesn't re-approve every session.
  - **OpenAI (ChatGPT)** — REST for text, Realtime WebSocket for voice.
  - **Anthropic (Claude)** — text path only. Realtime audio is stubbed pending a first-party streaming audio API.
- **Legacy BLE transport:** `Personlig_Ai/main/l2cap_transport.*` and `PocketPall/PocketPall/BLE/L2CAPManager.swift` are retained in-tree for historical reference. The active transport is WiFi TCP — see `CHANGELOG.md` for the BLE L2CAP → WiFi TCP migration notes.

## Known limitations

- **Single WiFi AP per profile.** The firmware stores one SSID/password in NVS at a time. Moving between a Personal Hotspot and a home AP means re-provisioning. A multi-profile NVS layout is sketched but not merged.
- **No on-device TLS.** The ESP32-S3 to iOS link is unencrypted TCP on the local subnet. The provider leg (iOS to Gemini/OpenAI/Anthropic) is HTTPS/WSS. Public-subnet deployments must gate access at the router.
- **Claude voice.** The Anthropic path is text-only until a first-party realtime audio streaming API is available; `ClaudeProvider.swift` is wired for text and returns a soft failure if asked to stream PCM.
- **LaTeX subset.** `latex_math.c` handles Greek letters, `\frac`, `\sqrt`, super/subscripts, scaled delimiters, and ~100 symbols — not the full AMS-LaTeX surface. Unknown commands render as literal text so the rest of the message still gets through.

## License

MIT — see `LICENSE`.
