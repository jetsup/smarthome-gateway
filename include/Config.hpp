#ifndef CONFIG_HPP
#define CONFIG_HPP

// ── Hardcoded Hub Address (captive portal linking) ─────
#define HUB_ADDR     "192.168.100.100"
#define HUB_WEB_PORT 9000
#define HUB_TCP_PORT 9010

// ── NVS key names ───────────────────────────────────────
#define NVS_NAMESPACE  "smarthome"
#define NVS_KEY_SSID   "wifi_ssid"
#define NVS_KEY_PASS   "wifi_pass"
#define NVS_KEY_APIKEY "api_key"

// ── Captive Portal ─────────────────────────────────────
#define AP_SSID_SETUP "SmartHome-Setup"
#define AP_SSID_LINK  "SmartHome-Link"

// ── Timing ──────────────────────────────────────────────
#define WIFI_TIMEOUT_MS  30000
#define TCP_RECONNECT_MS 5000

// ── Factory Reset (GPIO 25 → GND for 5s) ──────────────
#define RESET_PIN     25
#define RESET_HOLD_MS 5000

// ── ESP-NOW Message Types ──────────────────────────────
#define MSG_TELEMETRY   1
#define MSG_COMMAND     2
#define MSG_DISCOVERY   3
#define MSG_SCAN_REQ    4
#define MSG_PROVISION   5

// ── Scan timeout (ms) ──────────────────────────────────
#define SCAN_TIMEOUT_MS 30000

// ── mDNS ─────────────────────────────────────────────
#define MDNS_NAME "smarthome-gw"
#define MDNS_SERVICE "_http"
#define MDNS_PROTO   "_tcp"

// ── Local REST API ────────────────────────────────────
#define LOCAL_API_PORT 80
#define NODE_OFFLINE_MS 300000      // 5 min before local node shows offline
#define QUEUE_FLUSH_INTERVAL 10000    // ms between queue flush attempts
#define QUEUE_NVS_KEY    "op_queue"

// ── Offline queue operation types ─────────────────────
#define OP_PROVISION   "provision"
#define OP_DISCONNECT  "disconnect"

#endif // CONFIG_HPP
