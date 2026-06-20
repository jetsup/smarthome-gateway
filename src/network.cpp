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
  if (tcpClient.connect(HUB_ADDR, HUB_TCP_PORT)) {
    Serial.println("TCP connected to " + String(HUB_ADDR) + ":" + HUB_TCP_PORT);
    tcpClient.write(apiKey.c_str(), apiKey.length());
    tcpClient.write('\n');
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

void syncWifiCredentials() {
  int count = getSavedWifiCount();
  if (count == 0) return;
  String msg = "CREDS:" + String(count);
  for (int i = 0; i < count; i++) {
    String s, p;
    getSavedWifi(i, s, p);
    msg += ":" + s + ":" + p;
  }
  tcpClient.println(msg.c_str());
  Serial.printf("Synced %d Wi-Fi credentials to hub\n", count);
}

bool scanAndConnect() {
  int count = getSavedWifiCount();
  if (count == 0) return false;

  // Build list of known SSIDs
  String knownSSIDs[MAX_SAVED_WIFI];
  String knownPass[MAX_SAVED_WIFI];
  for (int i = 0; i < count; i++) {
    getSavedWifi(i, knownSSIDs[i], knownPass[i]);
  }

  Serial.println("Scanning WiFi networks...");
  int networks = WiFi.scanNetworks();
  if (networks == WIFI_SCAN_FAILED) {
    Serial.println("WiFi scan failed");
    return false;
  }
  if (networks == 0) {
    Serial.println("No networks found");
    return false;
  }

  Serial.printf("Found %d networks\n", networks);

  // Match known SSIDs and find the one with highest RSSI
  int bestIdx = -1;
  int bestRSSI = -1000;
  for (int i = 0; i < networks; i++) {
    String ssid = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);
    for (int k = 0; k < count; k++) {
      if (ssid == knownSSIDs[k] && rssi > bestRSSI) {
        bestRSSI = rssi;
        bestIdx = k;
      }
    }
  }

  WiFi.scanDelete();

  if (bestIdx < 0) {
    Serial.println("No known networks found in scan");
    return false;
  }

  Serial.printf("Connecting to best known network: %s (RSSI: %d)\n",
                knownSSIDs[bestIdx].c_str(), bestRSSI);

  wifiSSID = knownSSIDs[bestIdx];
  wifiPass = knownPass[bestIdx];
  connectToWiFi(wifiSSID.c_str(), wifiPass.c_str());
  return true;
}
