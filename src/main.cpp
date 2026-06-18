#include "Gateway.h"

uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

State currentState = STATE_AP_SETUP;
unsigned long stateTimer = 0;

Preferences prefs;

WebServer httpServer(80);
DNSServer dnsServer;

String wifiSSID = "";
String wifiPass = "";

String apiKey = "";

WiFiClient tcpClient;
unsigned long lastTCPReconnect = 0;
bool pendingReboot = false;

UplinkMessage uplinkQueue[UPLINK_QUEUE_SIZE];
volatile int uplinkHead = 0;
volatile int uplinkTail = 0;

bool scanning = false;
unsigned long scanStartTime = 0;

void checkResetPin() {
  static unsigned long pressStart = 0;
  if (digitalRead(RESET_PIN) == LOW) {
    if (pressStart == 0) {
      pressStart = millis();
    } else if (millis() - pressStart >= RESET_HOLD_MS) {
      Serial.println("Factory reset via GPIO " + String(RESET_PIN));
      prefs.remove(NVS_KEY_SSID);
      prefs.remove(NVS_KEY_PASS);
      prefs.remove(NVS_KEY_APIKEY);
      prefs.end();
      delay(500);
      ESP.restart();
    }
  } else {
    pressStart = 0;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(RESET_PIN, INPUT_PULLUP);
  prefs.begin(NVS_NAMESPACE, false);

  String savedSSID = prefs.getString(NVS_KEY_SSID, "");
  String savedPass = prefs.getString(NVS_KEY_PASS, "");
  String savedKey = prefs.getString(NVS_KEY_APIKEY, "");

  if (savedSSID.length() > 0 && savedKey.length() > 0) {
    Serial.println("Full config found — entering mesh mode");
    wifiSSID = savedSSID;
    wifiPass = savedPass;
    apiKey = savedKey;
    currentState = STATE_CONNECTING;
    connectToWiFi(wifiSSID.c_str(), wifiPass.c_str());
  } else if (savedSSID.length() > 0) {
    Serial.println("WiFi saved but no API key — entering linking mode");
    wifiSSID = savedSSID;
    wifiPass = savedPass;
    currentState = STATE_CONNECTING;
    connectToWiFi(wifiSSID.c_str(), wifiPass.c_str());
  } else {
    Serial.println("No config — starting captive portal");
    startAPMode();
  }
}

void loop() {
  checkResetPin();
  switch (currentState) {
    case STATE_AP_SETUP:
      dnsServer.processNextRequest();
      httpServer.handleClient();
      break;

    case STATE_CONNECTING:
      if (WiFi.status() == WL_CONNECTED &&
          WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        Serial.print("WiFi connected. IP: ");
        Serial.println(WiFi.localIP());
        String key = prefs.getString(NVS_KEY_APIKEY, "");
        if (key.length() > 0) {
          apiKey = key;
          initESPNOW();
          currentState = STATE_MESH;
          tcpConnect();
        } else {
          startLinkingMode();
        }
      } else if (millis() - stateTimer > WIFI_TIMEOUT_MS) {
        Serial.println("WiFi connect timeout — restarting AP");
        startAPMode();
      }
      break;

    case STATE_LINKING:
      dnsServer.processNextRequest();
      httpServer.handleClient();
      if (pendingReboot) {
        delay(500);
        dnsServer.stop();
        ESP.restart();
      }
      break;

    case STATE_MESH:
      if (WiFi.status() != WL_CONNECTED) {
        attemptReconnect();
      }
      tcpLoop();
      handleESPNOW();
      handleDownlink();
      if (scanning && millis() - scanStartTime > SCAN_TIMEOUT_MS) {
        scanning = false;
        Serial.println("Scan timed out");
      }
      break;
  }
}
