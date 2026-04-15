# Changelog

## BLE → WiFi transport migration

PocketPall originally used a Bluetooth LE L2CAP Connection-Oriented Channel between the ESP32-S3 and the iPhone. The L2CAP path delivered roughly 1–2 Mbit/s of throughput, which was enough for text and audio but tight for video and added latency under burst load. This release replaces it with a WiFi TCP socket running over the iPhone Personal Hotspot. Throughput is significantly higher, the ESP32 code is simpler, and the iOS side no longer depends on CoreBluetooth at all.

### Architecture

Before:

    ESP32-S3  <-- BLE L2CAP CoC -->  iPhone (CBPeripheral + L2CAPChannel)

After:

    ESP32-S3 (WiFi STA + TCP client)  <-- TCP port 5050 -->  iPhone (NWListener + Bonjour)
    Bonjour service: _pocketai._tcp

The wire-level frame protocol is unchanged: an 8-byte header `[Type:1][Flags:1][SeqNum:2 LE][Length:4 LE]` followed by the payload. The same frame types (0x01..0x0A) are now carried over a TCP byte stream instead of the L2CAP CoC byte stream.

### ESP32 side

Files added or rewritten:

| File | Change |
|---|---|
| `sdkconfig.defaults` | WiFi enabled, Bluetooth disabled, LWIP/TCP tuned for streaming |
| `main/CMakeLists.txt` | `l2cap_transport.c` replaced with `wifi_transport.c`; Bluetooth components dropped, `esp_wifi`, `esp_netif`, `lwip`, `mdns`, `nvs_flash` added |
| `main/app_config.h` | WiFi state values (`APP_STATE_WIFI_*`), event bits (`EVT_WIFI_*`), frame protocol constants |
| `main/wifi_transport.h` / `.c` | New public API: WiFi STA bring-up, TCP client, mDNS discovery, NVS credentials, auto-reconnect. Around 500 lines. |
| `main/ui_manager.h` / `.c` | New WiFi screens: setup, scanning, network list, password entry, connecting. WiFi status bar in the top bar. |
| `main/main.c` | All `l2cap_transport_*` calls replaced with `wifi_transport_*`, state machine renamed from `APP_STATE_BLE_*` to `APP_STATE_WIFI_*`, WiFi UI callbacks wired, auto-connect from NVS on boot |
| `main/l2cap_transport.h` / `.c` | Removed — WiFi transport replaces them |

The legacy `l2cap_transport.*` files are retained in the tree as empty stubs for historical reference but are no longer compiled.

### iOS side

Files added or rewritten:

| File | Change |
|---|---|
| `BLE/WiFiTransportManager.swift` | New file. TCP server via `NWListener`, `NWConnection` per client, `DeviceTransport` protocol, ping/pong, IO watchdog. Around 565 lines. |
| `BLE/L2CAPManager.swift` | Replaced with an empty stub so the Xcode project still compiles. Retained for historical reference; no longer referenced from app code. |
| `BLE/BLEConstants.swift` | Rewritten as `WiFiConstants` (port 5050, Bonjour type, device name) |
| `BLE/L2CAPProtocol.swift` | `static let psm` (BLE-specific) removed. The frame-building helpers are transport-agnostic and stay. |
| `PocketPallApp.swift` | `L2CAPManager` replaced with `WiFiTransportManager`. `.scanDevices` route removed. |
| `Views/ConnectView.swift` | New hotspot setup guide (three steps), blocks until the ESP32 connects |
| `Views/MainView.swift` | `l2capManager` property renamed to `wifiManager` |
| `Views/SetupView.swift` | Same rename |
| `Views/SavedDevicesView.swift` | Rewritten with WiFi icons; BLE connect actions dropped |
| `Views/ScanDevicesView.swift` | Replaced with a stub — the ESP32 scans for WiFi networks, not the iPhone |
| `Runtime/PocketPallRuntimeCoordinator.swift` | Large refactor: `l2capManager` → `wifiManager`, `CBManagerState` / `CBPeripheral` gone, BLE scanning/restoration dropped, `l2capOpen` → `tcpConnected`. Simplified to a TCP-listener model. |
| `Runtime/RuntimePermissionCoordinator.swift` | CoreLocation dropped (not needed for WiFi) — only notification permission is checked on launch |
| `Services/VideoStreamer.swift` | `L2CAPManager` → `WiFiTransportManager` |
| `Services/AudioBridge.swift` | Comments updated (L2CAP → WiFi TCP) |
| `Models/GeminiProvider.swift` | Video transport type updated |
| `Models/SavedDevice.swift` | UserDefaults key renamed `saved_ble_devices` → `saved_wifi_devices` |
| `Info.plist` | `bluetooth-central` background mode removed; Bluetooth and Location usage descriptions removed; the network usage description is now English |
| `PocketPall.xcodeproj/project.pbxproj` | Info.plist duplicate-build-error fixed with a `PBXFileSystemSynchronizedBuildFileExceptionSet` entry |

### Build verification

| Check | Result |
|---|---|
| ESP32 `idf.py build` | OK (1492/1492 steps, binary 1.3 MB) |
| ESP32 `CONFIG_ESP_WIFI_ENABLED=y` | OK |
| ESP32 `CONFIG_BT_ENABLED` unset | OK |
| iOS `xcodebuild` Debug build | BUILD SUCCEEDED |
| No active references to `import CoreBluetooth` | OK |
| No active references to `import CoreLocation` | OK |
| No active references to `L2CAPManager` / `l2capManager` | OK |
| No active references to `CBPeripheral` / `CBManagerState` | OK |

### Expected ESP32 flow

1. Boot, check NVS for saved WiFi credentials.
2. If credentials are present, auto-connect, then TCP-connect to the iPhone via mDNS (falling back to `172.20.10.1:5050`).
3. If no credentials, the display shows a "Find devices" screen with a single centred button.
4. User taps. The device runs a WiFi scan and shows a network list with signal strength and lock icons.
5. User selects a network, enters the password on the on-screen keyboard, and the device connects.
6. Credentials are written to NVS for the next boot.

### Expected iPhone flow

1. App opens and lands on `ConnectView`, which shows the three-step hotspot setup guide.
2. `NWListener` runs continuously on port 5050 with the `_pocketai._tcp` Bonjour advertisement.
3. The ESP32 connects. The `isConnected` signal flips to `true` and the app navigates to Setup or Main.
4. `BackgroundVoiceSessionKeeper` keeps the process alive in the background so the TCP connection survives the app being backgrounded.

### Fixes in this release

- **TCP reconnect loop.** Both the event handler and the reconnect task used to call `tcp_connect_to_server()` concurrently, which produced two sockets to the iPhone. The iPhone replaced the first with the second, tearing down the active voice session, and the cycle repeated every few seconds. The fix removes the call from the `IP_EVENT_STA_GOT_IP` handler and makes the reconnect task the only caller. The event handler now wakes the reconnect task via `xTaskNotifyGive` instead of racing it.
- **sys_evt stack overflow during WiFi scan.** A `ui_wifi_network_t ui_networks[20]` array was placed on the system-event task's stack. With 20 networks this exceeded the default 2304-byte stack. The fix heap-allocates the array with `calloc()` and raises `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` to 4096.
- **Info.plist duplicate-build error.** Two build-plist entries collided; a `PBXFileSystemSynchronizedBuildFileExceptionSet` line in `project.pbxproj` resolves it.

### Known gaps

- TCP stability under long sessions has not been exercised on real hardware yet.
- Audio streaming, video streaming (4096-byte chunks vs the old 496-byte L2CAP chunks), and reconnect after WiFi loss are not yet stress-tested.
- Background behaviour (app backgrounded + screen locked) is implemented but not end-to-end verified.
- The legacy `L2CAPManager.swift` and `ScanDevicesView.swift` stubs can be removed from the Xcode project entirely once nothing depends on them.
