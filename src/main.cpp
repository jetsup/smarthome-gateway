#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

// Universal packed C-struct matching the Go backend footprint (9 bytes total)
struct __attribute__((__packed__)) ESPNowMessage {
    uint8_t header;    // 0xAA
    uint8_t msgType;   // 1
    uint32_t deviceId; 
    uint16_t value;    
    uint8_t checksum;  // XOR of all previous bytes
}; // Total size is 9 bytes

// Broadcast MAC address to reach all nodes in range/mesh layer
uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Callback when data is received over the air from an End-Node
void onDataRecv(const uint8_t* mac_addr, const uint8_t* incomingData, int len) {
  if (len == sizeof(ESPNowMessage)) {
      
      Serial.printf(
        "Received command: Header=%d, Type=%d, DeviceID=%u, Value=%u, Checksum=%d\n",
        incomingData[0], incomingData[1], *((uint32_t*)(incomingData + 2)),
        *((uint16_t*)(incomingData + 6)), incomingData[8]);
    }
    
    // Direct, raw binary write straight to the USB Serial port
    // This is what Go's stream.Read() captures as a 9-byte chunk
    Serial.write(incomingData, sizeof(ESPNowMessage));
}

void setup() {
  Serial.begin(115200);

  // Set Wi-Fi to Station mode and disconnect to clear overhead
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    // Failed to initialize
    return;
  }

  // Register receive callback function
  esp_now_register_recv_cb(esp_now_recv_cb_t(onDataRecv));

  // Register the broadcast peer configuration
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastMac, 6);
  peerInfo.channel = 1;  // Must match your entire ecosystem's channel
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    // Failed to add broadcast peer
    return;
  }
}

void loop() {
  // Listen for incoming commands from the Go Master Server over USB
  if (Serial.available() >= sizeof(ESPNowMessage)) {
    ESPNowMessage outMsg;

    // Read raw bytes from Go directly into our struct memory area
    Serial.readBytes((uint8_t*)&outMsg, sizeof(ESPNowMessage));

    // Blast the command payload out to the entire mesh layer via broadcast
    esp_now_send(broadcastMac, (uint8_t*)&outMsg, sizeof(ESPNowMessage));

    Serial.printf("Sent command: Header=%d, Type=%d, DeviceID=%u, Value=%u, Checksum=%d\n",
                  outMsg.header, outMsg.msgType, outMsg.deviceId, outMsg.value, outMsg.checksum);
  }
}
