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

#endif // CONFIG_HPP
