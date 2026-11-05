# smarthome-gateway

ESP32 firmware for the SmartHome **gateway** — bridges ESP-NOW mesh network to the hub over TCP. Built with **PlatformIO**.

## Role

The gateway sits between the ESP32 sensor nodes and the Go backend hub:
- Receives ESP-NOW packets from nodes → forwards to hub via TCP
- Receives TCP commands from hub → broadcasts via ESP-NOW
- Hosts captive portal for first-time WiFi setup
- Hosts embedded linking page for API key assignment
- Proxies API requests from the linking page to the hub

## Hardware

- **ESP32** (any dev board with USB-serial)
- **GPIO 25** → GND (pull-up, 5-second hold for factory reset)
- **GPIO 2** (LED_BUILTIN, optional)

## Installation & first-time setup

Set up the hub first (`../smarthome-hub/README.md`) and note its LAN IP.

### 1. Prerequisites

- [PlatformIO](https://platformio.org/) CLI (`pip install platformio`) or the VS Code extension. Libraries and the ESP32 Arduino core (`espressif32@6.9.0`) are downloaded automatically on the first build.
- A USB cable to the ESP32; on Linux add yourself to the serial group: `sudo usermod -aG dialout $USER` (log out/in).

### 2. ⚠️ Set the hub address (required)

Edit `include/Config.hpp`, **or** pass it at build time in `platformio.ini` (preferred, keeps the source clean):

```ini
build_flags =
    -DHUB_ADDR=\"192.168.1.50\"    ; <-- YOUR hub's IP address
    ; -DHUB_WEB_PORT=9000          ; only if you changed SERVER_PORT on the hub
    ; -DHUB_TCP_PORT=9010          ; only if you changed TCP_PORT on the hub
    -DCONFIG_ASYNC_TCP_RUNNING_CORE=1
    -DASYNCWEBSERVER_REGEX=1
```

(Keep the two flags already in `platformio.ini`.) The default `192.168.100.100` is the author's network and will not work for you.

### 3. Build & flash

```bash
pio run                       # build
pio run -t upload             # flash (add --upload-port /dev/ttyUSB0 if several boards)
pio device monitor -b 115200  # serial log
```

### 4. Join Wi-Fi and link it to your account

**Easiest: the Android app.** Menu → **Add a gateway**: the gateway advertises over Bluetooth LE, the app gives it your Wi-Fi and the API key, and your phone never leaves its own network. Or do it by hand:

1. In the hub web UI (or app) **create a gateway** (Gateways → Add gateway).
2. Power the board. With no saved Wi-Fi it opens the access point **`SmartHome-Setup`**; connect a phone to it, the setup page opens by itself (or browse `http://192.168.4.1`). Enter your **2.4 GHz** Wi-Fi name and password.
3. The board reboots, joins Wi-Fi and, having no API key yet, opens **`SmartHome-Link`**. Connect to it, open `http://192.168.4.1`, sign in with your SmartHome account, pick the gateway you created and confirm. 
4. The gateway reboots into normal mode and shows **online** in the UI.

To start over, hold **GPIO 25 → GND for 5 s** (see Factory Reset).

## Finite State Machine

```
                    ┌──────────────┐
                    │  AP_SETUP    │  (no WiFi config in NVS)
                    │  AP "SmartHome-Setup"
                    └──────┬───────┘
                           │ WiFi credentials saved
                           ▼
                    ┌──────────────┐
                    │  CONNECTING  │  (joining WiFi)
                    └──────┬───────┘
                           │ WiFi OK
                           ▼
           ┌───────────────┴────────────────┐
           │                                │
           ▼                                ▼
   ┌──────────────┐                 ┌──────────────┐
   │   MESH       │                 │  LINKING     │
   │  (full ops)  │◄─────────────── │  AP "SmartHome-Link"
   └──────────────┘  API key saved  │  + embedded linking page
                                     └──────────────┘
```

### AP_SETUP
- ESP32 acts as WiFi AP "SmartHome-Setup"
- DNS captive portal redirects all requests to ESP32
- Serves HTML form for SSID + WiFi password
- On submit: saves to NVS, reboots into CONNECTING

### CONNECTING
- Attempts to join the home WiFi
- On success: checks NVS for API key
  - Has key → MESH mode
  - No key → LINKING mode
- On timeout (30s): reverts to AP_SETUP

### LINKING
- WiFi STA connected, AP "SmartHome-Link" active
- Serves embedded HTML/JS page at `http://192.168.4.1/`
- User logs in, selects gateway, retrieves API key
- `handleConfigure()` saves key to NVS, sets `pendingReboot` flag
- Reboot deferred to `loop()` to ensure HTTP response is flushed

### MESH
- TCP connection to the hub at `HUB_ADDR:HUB_TCP_PORT` (see Configuration; default `192.168.100.100:9010`)
- Authenticates with API key (newline-terminated)
- ESP-NOW receive interrupt → ring buffer → TCP send
- TCP receive → ESP-NOW broadcast
- Scan mode (30s timeout) for node discovery

## Configuration

All config in `include/Config.hpp` (only `HUB_ADDR` normally needs changing):

| Constant | Value | Description |
|---|---|---|
| `HUB_ADDR` | `192.168.100.100` | **Change:** hub IP address (or `-DHUB_ADDR` build flag) |
| `HUB_WEB_PORT` | `9000` | Hub HTTP API port |
| `HUB_TCP_PORT` | `9010` | Hub TCP mesh port |
| `AP_SSID_SETUP` | `SmartHome-Setup` | Initial AP SSID |
| `AP_SSID_LINK` | `SmartHome-Link` | Linking AP SSID |
| `WIFI_TIMEOUT_MS` | `30000` | WiFi connection timeout |
| `TCP_RECONNECT_MS` | `5000` | TCP reconnect interval |
| `RESET_PIN` | `25` | Factory reset GPIO |
| `RESET_HOLD_MS` | `5000` | Hold duration for reset |
| `SCAN_TIMEOUT_MS` | `30000` | Node discovery scan duration |

## Factory Reset

Hold GPIO 25 → GND for 5 seconds. Clears SSID, password, and API key from NVS, then reboots into AP_SETUP mode.
