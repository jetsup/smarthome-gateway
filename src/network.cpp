#include <ArduinoJson.h>

#include "Gateway.h"

static const byte DNS_PORT = 53;

void startAPMode() {
  currentState = STATE_AP_SETUP;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID_SETUP);
  delay(200);

  Serial.print("AP started: ");
  Serial.println(AP_SSID_SETUP);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.println("DNS server started (captive portal)");
  bleSetupStart();  // also discoverable from the SmartHome app
}

void startLinkingMode() {
  currentState = STATE_LINKING;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID_LINK);
  delay(200);

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  Serial.println("Linking AP started: " + String(AP_SSID_LINK));
  Serial.println("AP IP: " + WiFi.softAPIP().toString());
  Serial.println("STA IP: " + WiFi.localIP().toString());
  bleSetupStart();
}

void connectToWiFi(const char* ssid, const char* pass) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  stateTimer = millis();
}

void attemptReconnect() {
  if (millis() - lastTCPReconnect > 10000) {
    lastTCPReconnect = millis();
    Serial.println("WiFi lost — reconnecting...");
    WiFi.reconnect();
  }
}

bool tcpConnect() {
  // Hub address handed over by the app during Bluetooth setup, else the
  // build-time default.
  String hubHost = prefs.getString(NVS_KEY_HUBHOST, HUB_ADDR);
  if (tcpClient.connect(hubHost.c_str(), HUB_TCP_PORT, 1200)) {  // short timeout: a down hub must not stall the mesh loop
    Serial.println("TCP connected to " + hubHost + ":" + HUB_TCP_PORT);
    tcpClient.write(apiKey.c_str(), apiKey.length());
    tcpClient.write('\n');
    // Send MAC-derived decimal announce ID for hub to map → hex DB ID
    uint64_t mac = ESP.getEfuseMac();
    uint32_t announceId = (uint32_t)(mac & 0xFFFFFFFF);
    tcpClient.printf("GWID:%u\n", announceId);
    tcpClient.printf("GWMAC:%02X:%02X:%02X:%02X:%02X:%02X\n", (uint8_t)mac, (uint8_t)(mac >> 8),
                     (uint8_t)(mac >> 16), (uint8_t)(mac >> 24), (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
    syncWifiCredentials();
    return true;
  }
  Serial.println("TCP connect failed");
  return false;
}

void tcpLoop() {
  if (!tcpClient.connected()) {
    if (millis() - lastTCPReconnect > TCP_RECONNECT_MS) {
      lastTCPReconnect = millis();
      tcpConnect();
    }
  }
}

// ── Multi-SSID WiFi credential management ──────────────────────────────────────

int getSavedWifiCount() {
  return prefs.getInt(NVS_WIFI_COUNT, 0);
}

bool getSavedWifi(int index, String& ssid, String& pass) {
  String sKey = String(NVS_WIFI_SSID_PREFIX) + index;
  String pKey = String(NVS_WIFI_PASS_PREFIX) + index;
  ssid = prefs.getString(sKey.c_str(), "");
  pass = prefs.getString(pKey.c_str(), "");
  return ssid.length() > 0;
}

void saveWifiCredential(const String& ssid, const String& pass) {
  // Check if SSID already exists, update it
  int count = getSavedWifiCount();
  for (int i = 0; i < count; i++) {
    String s, p;
    getSavedWifi(i, s, p);
    if (s == ssid) {
      String pKey = String(NVS_WIFI_PASS_PREFIX) + i;
      prefs.putString(pKey.c_str(), pass);
      Serial.printf("Updated WiFi credential: %s\n", ssid.c_str());
      prefs.end();
      prefs.begin(NVS_NAMESPACE, false);
      return;
    }
  }

  // New credential — add if space
  if (count < MAX_SAVED_WIFI) {
    String sKey = String(NVS_WIFI_SSID_PREFIX) + count;
    String pKey = String(NVS_WIFI_PASS_PREFIX) + count;
    prefs.putString(sKey.c_str(), ssid);
    prefs.putString(pKey.c_str(), pass);
    prefs.putInt(NVS_WIFI_COUNT, count + 1);
    Serial.printf("Saved WiFi credential %d: %s\n", count, ssid.c_str());
  } else {
    Serial.println("Max saved WiFi credentials reached — overwriting oldest");
    // Rotate: shift all down, put new at end
    String oldestSSID, oldestPass;
    getSavedWifi(0, oldestSSID, oldestPass);
    for (int i = 1; i < MAX_SAVED_WIFI; i++) {
      String s, p;
      getSavedWifi(i, s, p);
      String sKey = String(NVS_WIFI_SSID_PREFIX) + (i - 1);
      String pKey = String(NVS_WIFI_PASS_PREFIX) + (i - 1);
      prefs.putString(sKey.c_str(), s);
      prefs.putString(pKey.c_str(), p);
    }
    String sKey = String(NVS_WIFI_SSID_PREFIX) + (MAX_SAVED_WIFI - 1);
    String pKey = String(NVS_WIFI_PASS_PREFIX) + (MAX_SAVED_WIFI - 1);
    prefs.putString(sKey.c_str(), ssid);
    prefs.putString(pKey.c_str(), pass);
  }

  prefs.end();
  prefs.begin(NVS_NAMESPACE, false);
}

void removeAllWifiCredentials() {
  int count = getSavedWifiCount();
  for (int i = 0; i < count; i++) {
    String sKey = String(NVS_WIFI_SSID_PREFIX) + i;
    String pKey = String(NVS_WIFI_PASS_PREFIX) + i;
    prefs.remove(sKey.c_str());
    prefs.remove(pKey.c_str());
  }
  prefs.remove(NVS_WIFI_COUNT);
  prefs.end();
  prefs.begin(NVS_NAMESPACE, false);
  Serial.println("All saved WiFi credentials removed");
}

// Report the saved networks (in priority order) to the hub. JSON, so SSIDs and
// passwords containing ':' or other punctuation survive.
void syncWifiCredentials() {
  int count = getSavedWifiCount();
  if (count == 0) return;
  JsonDocument doc;
  JsonArray nets = doc["nets"].to<JsonArray>();
  for (int i = 0; i < count; i++) {
    String s, p;
    if (!getSavedWifi(i, s, p)) continue;
    JsonObject o = nets.add<JsonObject>();
    o["s"] = s;
    o["p"] = p;
  }
  String out;
  serializeJson(doc, out);
  tcpClient.println("CREDSJ:" + out);
  Serial.printf("Synced %d Wi-Fi credentials to hub\n", count);
}

// ── Priority-ordered network selection ────────────────────────────────────────
// Saved networks are kept in priority order (index 0 = most preferred). On boot
// we scan, keep those that are in range, and try them in that order. If one
// fails to connect, the next candidate is tried before falling back to setup
// mode.
static String candSsid[MAX_SAVED_WIFI];
static String candPass[MAX_SAVED_WIFI];
static int candCount = 0;
static int candPos = 0;

bool connectNextCandidate() {
  if (candPos >= candCount) return false;
  wifiSSID = candSsid[candPos];
  wifiPass = candPass[candPos];
  candPos++;
  Serial.printf("Trying saved network %d/%d: %s\n", candPos, candCount, wifiSSID.c_str());
  connectToWiFi(wifiSSID.c_str(), wifiPass.c_str());
  return true;
}

int candidateCount() { return candCount; }

bool scanAndConnect() {
  int count = getSavedWifiCount();
  if (count == 0) return false;

  Serial.println("Scanning WiFi networks...");
  int networks = WiFi.scanNetworks();
  if (networks == WIFI_SCAN_FAILED || networks <= 0) {
    Serial.println(networks == 0 ? "No networks found" : "WiFi scan failed");
    return false;
  }
  Serial.printf("Found %d networks\n", networks);

  candCount = 0;
  candPos = 0;
  for (int k = 0; k < count && candCount < MAX_SAVED_WIFI; k++) {
    String s, p;
    if (!getSavedWifi(k, s, p)) continue;
    bool visible = false;
    for (int i = 0; i < networks; i++) {
      if (WiFi.SSID(i) == s) { visible = true; break; }
    }
    if (visible) {
      candSsid[candCount] = s;
      candPass[candCount] = p;
      candCount++;
    }
  }
  WiFi.scanDelete();

  if (candCount == 0) {
    Serial.println("No saved networks in range");
    return false;
  }
  Serial.printf("%d saved network(s) in range, highest priority first\n", candCount);
  return connectNextCandidate();
}
