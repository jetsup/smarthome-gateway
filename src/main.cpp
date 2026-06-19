#include "Gateway.h"

uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

State currentState = STATE_AP_SETUP;
unsigned long stateTimer = 0;

Preferences prefs;

AsyncWebServer asyncServer(LOCAL_API_PORT);
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

SemaphoreHandle_t dataMutex = nullptr;

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

  // Create mutex for shared data access (web server + main loop)
  dataMutex = xSemaphoreCreateMutex();

  // Load offline operation queue
  loadQueue();

  // Initialize WiFi stack early so asyncServer.begin() doesn't crash
  WiFi.mode(WIFI_AP_STA);

  // Start web server once (handles all states)
  initWebServer();

  String savedKey = prefs.getString(NVS_KEY_APIKEY, "");

  // Try multi-SSID scan-and-connect first
  int savedWifiCount = getSavedWifiCount();
  bool connected = false;
  if (savedWifiCount > 0) {
    connected = scanAndConnect();
  }

  if (connected && savedKey.length() > 0) {
    Serial.println("Connected with multi-SSID — entering mesh mode");
    apiKey = savedKey;
    currentState = STATE_CONNECTING;
  } else if (connected) {
    Serial.println("Connected but no API key — entering linking mode");
    currentState = STATE_CONNECTING;
  } else {
    // Fallback: single SSID from old NVS key
    String savedSSID = prefs.getString(NVS_KEY_SSID, "");
    String savedPass = prefs.getString(NVS_KEY_PASS, "");
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
}

void loop() {
  checkResetPin();
  switch (currentState) {
    case STATE_AP_SETUP:
      dnsServer.processNextRequest();
      break;

    case STATE_CONNECTING:
      if (WiFi.status() == WL_CONNECTED &&
          WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        Serial.print("WiFi connected. IP: ");
        Serial.println(WiFi.localIP());
        // Start mDNS with unique hostname (last 4 hex of MAC)
        String macStr = WiFi.macAddress();
        macStr.replace(":", "");
        String mdnsHost = String(MDNS_NAME) + "-" + macStr.substring(macStr.length() - 4);
        if (MDNS.begin(mdnsHost.c_str())) {
          MDNS.addService(MDNS_SERVICE, MDNS_PROTO, LOCAL_API_PORT);
          MDNS.addServiceTxt(MDNS_SERVICE, MDNS_PROTO, "id", macStr.substring(macStr.length() - 8));
          Serial.printf("mDNS started: %s.local\n", mdnsHost.c_str());
        }
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

      // Periodically flush offline operation queue
      tryFlushQueue();
      break;
  }
}
