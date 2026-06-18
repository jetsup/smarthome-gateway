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
