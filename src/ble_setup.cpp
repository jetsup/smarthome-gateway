#include <ArduinoJson.h>
#include <NimBLEDevice.h>

#include "Gateway.h"

// Bluetooth LE onboarding for the SmartHome app.
//
// The gateway has no screen, so it advertises itself as "SmartHome-GW-XXXX"
// while it is unconfigured (setup / linking). The app finds it, hands it a
// Wi-Fi network (tested before it is saved), then the hub-issued API key.
// This replaces joining the gateway's own hotspot; the hotspot portal stays
// available as a fallback.
//
// UUIDs must match smarthome-app/lib/services/gateway_provisioning_service.dart

namespace {
constexpr const char* kSvc = "4e5c0010-8d1f-4b6a-9c53-0a5b7a1e0001";
constexpr const char* kInfo = "4e5c0011-8d1f-4b6a-9c53-0a5b7a1e0001";  // read
constexpr const char* kWifi = "4e5c0012-8d1f-4b6a-9c53-0a5b7a1e0001";  // write
constexpr const char* kKey = "4e5c0013-8d1f-4b6a-9c53-0a5b7a1e0001";   // write
constexpr const char* kCtl = "4e5c0014-8d1f-4b6a-9c53-0a5b7a1e0001";   // write
constexpr const char* kStat = "4e5c0015-8d1f-4b6a-9c53-0a5b7a1e0001";  // read+notify

constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr int kLedPin = 2;  // onboard LED on most ESP32 dev boards

bool g_started = false;
NimBLECharacteristic* g_info = nullptr;
NimBLECharacteristic* g_stat = nullptr;

// Work posted by BLE callbacks (NimBLE task), executed from loop().
volatile bool g_wifiPending = false, g_keyPending = false, g_identPending = false;
String g_wifiSsid, g_wifiPass, g_keyVal, g_hubHost, g_gwName;

enum class Conn { Idle, Connecting, Ok, Failed };
Conn g_conn = Conn::Idle;
uint32_t g_connStart = 0;
String g_trySsid, g_tryPass;
String g_status = "idle";
bool g_statusDirty = true;
uint32_t g_ledUntil = 0;
uint32_t g_rebootAt = 0;

String macHex() {
  uint64_t m = ESP.getEfuseMac();
  char b[16];
  snprintf(b, sizeof(b), "%02X%02X%02X%02X%02X%02X", (uint8_t)(m), (uint8_t)(m >> 8),
           (uint8_t)(m >> 16), (uint8_t)(m >> 24), (uint8_t)(m >> 32), (uint8_t)(m >> 40));
  return String(b);
}

String bleName() {
  char b[24];
  snprintf(b, sizeof(b), "SmartHome-GW-%04X", (uint16_t)(ESP.getEfuseMac() >> 32));
  return String(b);
}

void setStatus(const String& s) {
  g_status = s;
  g_statusDirty = true;
}

void refreshInfo() {
  if (!g_info) return;
  JsonDocument d;
  d["id"] = macHex();
  d["linked"] = prefs.getString(NVS_KEY_APIKEY, "").length() > 0;
  d["wifiSaved"] = getSavedWifiCount() > 0;
  d["wifiUp"] = WiFi.status() == WL_CONNECTED;
  String out;
  serializeJson(d, out);
  g_info->setValue(std::string(out.c_str()));
}

class WriteCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string uuid = c->getUUID().toString();
    std::string raw = c->getValue();
    if (uuid == kWifi) {
      JsonDocument d;
      if (deserializeJson(d, raw) != DeserializationError::Ok) {
        setStatus("fail:bad request");
        return;
      }
      g_wifiSsid = String(d["s"] | "");
      g_wifiPass = String(d["p"] | "");
      g_wifiPending = true;
    } else if (uuid == kKey) {
      JsonDocument d;
      if (deserializeJson(d, raw) != DeserializationError::Ok) {
        setStatus("fail:bad request");
        return;
      }
      g_keyVal = String(d["k"] | "");
      g_hubHost = String(d["h"] | "");
      g_gwName = String(d["n"] | "");
      g_keyPending = true;
    } else if (uuid == kCtl) {
      if (raw == "identify") g_identPending = true;
    }
  }
};

void startConnectTest(const String& ssid, const String& pass) {
  g_trySsid = ssid;
  g_tryPass = pass;
  g_conn = Conn::Connecting;
  g_connStart = millis();
  setStatus("connecting");
  if (WiFi.getMode() == WIFI_AP) WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect(false);
  delay(50);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("[BLE] Trying Wi-Fi '%s'\n", ssid.c_str());
}

void failConnect(const char* why) {
  g_conn = Conn::Failed;
  WiFi.disconnect(false);
  setStatus(String("fail:") + why);
  Serial.printf("[BLE] Wi-Fi failed: %s\n", why);
}
}  // namespace

void bleSetupStart() {
  if (g_started) return;
  g_started = true;
  g_conn = Conn::Idle;
  setStatus("idle");

  NimBLEDevice::init(bleName().c_str());
  NimBLEServer* srv = NimBLEDevice::createServer();
  NimBLEService* svc = srv->createService(kSvc);
  g_info = svc->createCharacteristic(kInfo, NIMBLE_PROPERTY::READ);
  auto* cb = new WriteCb();
  svc->createCharacteristic(kWifi, NIMBLE_PROPERTY::WRITE)->setCallbacks(cb);
  svc->createCharacteristic(kKey, NIMBLE_PROPERTY::WRITE)->setCallbacks(cb);
  svc->createCharacteristic(kCtl, NIMBLE_PROPERTY::WRITE)->setCallbacks(cb);
  g_stat = svc->createCharacteristic(kStat, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  g_stat->setValue(std::string("idle"));
  refreshInfo();
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(kSvc);
  adv->setScanResponse(true);
  adv->start();
  pinMode(kLedPin, OUTPUT);
  Serial.printf("[BLE] Advertising as %s\n", bleName().c_str());
}

void bleSetupStop() {
  if (!g_started) return;
  g_started = false;
  g_info = g_stat = nullptr;
  NimBLEDevice::deinit(true);
  digitalWrite(kLedPin, LOW);
  Serial.println("[BLE] Stopped");
}

void bleSetupLoop() {
  if (!g_started) return;

  if (g_identPending) {
    g_identPending = false;
    g_ledUntil = millis() + 5000;
  }
  if (g_ledUntil) {
    if (millis() > g_ledUntil) {
      g_ledUntil = 0;
      digitalWrite(kLedPin, LOW);
    } else {
      digitalWrite(kLedPin, (millis() / 150) % 2);
    }
  }

  if (g_wifiPending) {
    g_wifiPending = false;
    if (g_conn != Conn::Connecting) startConnectTest(g_wifiSsid, g_wifiPass);
  }

  if (g_conn == Conn::Connecting) {
    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
      g_conn = Conn::Ok;
      saveWifiCredential(g_trySsid, g_tryPass);
      Serial.printf("[BLE] Wi-Fi OK (%s), credentials saved\n", WiFi.localIP().toString().c_str());
      refreshInfo();
      setStatus("wifi_ok:" + WiFi.localIP().toString());
    } else if (st == WL_NO_SSID_AVAIL && millis() - g_connStart > 6000) {
      failConnect("network not found");
    } else if (st == WL_CONNECT_FAILED) {
      failConnect("wrong password");
    } else if (millis() - g_connStart > kConnectTimeoutMs) {
      failConnect("timed out - check the password");
    }
  }

  if (g_keyPending) {
    g_keyPending = false;
    if (!g_keyVal.startsWith("gwy_") || g_keyVal.length() < 20) {
      setStatus("fail:invalid key");
    } else if (getSavedWifiCount() == 0) {
      setStatus("fail:set up Wi-Fi first");
    } else {
      prefs.putString(NVS_KEY_APIKEY, g_keyVal);
      if (g_hubHost.length() > 0) prefs.putString(NVS_KEY_HUBHOST, g_hubHost);
      if (g_gwName.length() > 0) prefs.putString("gateway_name", g_gwName);
      Serial.println("[BLE] API key saved — restarting to join the hub");
      setStatus("linked");
      g_rebootAt = millis() + 2500;  // give the app time to read "linked"
    }
  }

  if (g_statusDirty && g_stat) {
    g_statusDirty = false;
    g_stat->setValue(std::string(g_status.c_str()));
    g_stat->notify();
  }

  if (g_rebootAt && millis() > g_rebootAt) ESP.restart();
}
