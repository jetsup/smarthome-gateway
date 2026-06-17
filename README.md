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

## Prerequisites

- [PlatformIO](https://platformio.org/) CLI or VS Code extension
- ESP32 Arduino core (installed by PlatformIO)

## Build & Flash

```bash
# Build
pio run

# Upload to ESP32
pio run --target upload

# Monitor serial output
pio device monitor -b 115200
```

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
- TCP connection to hub at `192.168.4.1:9010`
- Authenticates with API key (newline-terminated)
- ESP-NOW receive interrupt → ring buffer → TCP send
- TCP receive → ESP-NOW broadcast
- Scan mode (30s timeout) for node discovery

## Configuration

All config in `include/Config.hpp`:

| Constant | Value | Description |
|---|---|---|
| `HUB_ADDR` | `192.168.100.100` | Hub IP address (hardcoded) |
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
