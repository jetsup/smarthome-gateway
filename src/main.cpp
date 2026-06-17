#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

// WiFi credentials
#define WIFI_SSID "Qt"
#define WIFI_PASS "QtNetwork."

// Hub TCP server (update this to your VPS IP when deployed)
#define HUB_HOST "192.168.100.100"
#define HUB_PORT 9010

// Universal packed C-struct matching the Go backend footprint (9 bytes total)
struct __attribute__((__packed__)) ESPNowMessage {
  uint8_t header;   // 0xAA
  uint8_t msgType;  // 1
  uint32_t deviceId;
  uint16_t value;
  uint8_t checksum;  // XOR of all previous bytes
};

// Broadcast MAC address
uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

WiFiClient tcpClient;

// Uplink ring buffer — decouples ESP-NOW callback from TCP writes
#define UPLINK_QUEUE_SIZE 32
ESPNowMessage uplinkQueue[UPLINK_QUEUE_SIZE];
volatile int uplinkHead = 0;
volatile int uplinkTail = 0;

// Callback when data is received over the air from an End-Node
// Runs in ESP-NOW context — must be fast, no blocking I/O
void onDataRecv(const uint8_t* mac_addr, const uint8_t* incomingData, int len) {
  if (len != sizeof(ESPNowMessage)) return;

  int next = (uplinkHead + 1) % UPLINK_QUEUE_SIZE;
  if (next == uplinkTail) return; // queue full, drop

  memcpy((void*)&uplinkQueue[uplinkHead], incomingData, sizeof(ESPNowMessage));
  uplinkHead = next;
}

void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected. IP: " + WiFi.localIP().toString());
}

bool connectTCP() {
  if (!tcpClient.connected()) {
    if (tcpClient.connect(HUB_HOST, HUB_PORT)) {
      Serial.println("Connected to hub at " + String(HUB_HOST) + ":" + String(HUB_PORT));
      return true;
    }
    Serial.println("TCP connection to hub failed");
    return false;
  }
  return true;
}

void ensureConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  if (!tcpClient.connected()) {
    connectTCP();
  }
}

void setup() {
  Serial.begin(115200);

  connectWiFi();

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(esp_now_recv_cb_t(onDataRecv));

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastMac, 6);
  peerInfo.channel = 0;  // 0 = use WiFi home channel
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add broadcast peer");
    return;
  }

  connectTCP();
}

void loop() {
  ensureConnection();

  // Uplink: drain ring buffer into TCP
  if (tcpClient.connected()) {
    while (uplinkTail != uplinkHead) {
      tcpClient.write((uint8_t*)&uplinkQueue[uplinkTail], sizeof(ESPNowMessage));
      uplinkTail = (uplinkTail + 1) % UPLINK_QUEUE_SIZE;
    }
  }

  // Downlink: read commands from hub and broadcast via ESP-NOW
  if (tcpClient.available() >= (int)sizeof(ESPNowMessage)) {
    ESPNowMessage outMsg;
    tcpClient.readBytes((uint8_t*)&outMsg, sizeof(ESPNowMessage));

    if (outMsg.header == 0xAA) {
      esp_now_send(broadcastMac, (uint8_t*)&outMsg, sizeof(ESPNowMessage));
    }
  }
}
