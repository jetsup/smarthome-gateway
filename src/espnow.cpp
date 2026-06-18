#include "Gateway.h"

void onESPNOWRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (len < 1 || len > MAX_MSG_SIZE) return;

  uint32_t deviceId = 0;
  uint8_t msgType = (len >= 2) ? data[1] : 0;
  if (len >= 6) {
    deviceId = (uint32_t)data[2] | ((uint32_t)data[3] << 8) |
               ((uint32_t)data[4] << 16) | ((uint32_t)data[5] << 24);
  }

  Serial.printf("ESPNOW RX: deviceId=%u msgType=%u len=%d\n", deviceId,
                msgType, len);

  int next = (uplinkHead + 1) % UPLINK_QUEUE_SIZE;
  if (next == uplinkTail) {
    Serial.println("ESPNOW RX: queue full, dropping packet");
    return;
  }
  memcpy(uplinkQueue[uplinkHead].data, data, len);
  uplinkQueue[uplinkHead].len = len;
  uplinkHead = next;
}

void initESPNOW() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_register_recv_cb(esp_now_recv_cb_t(onESPNOWRecv));

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastMac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);
}

void handleESPNOW() {
  if (!tcpClient.connected()) return;

  while (uplinkTail != uplinkHead) {
    UplinkMessage& msg = uplinkQueue[uplinkTail];

    uint32_t deviceId = 0;
    uint8_t msgType = (msg.len >= 2) ? msg.data[1] : 0;
    if (msg.len >= 6) {
      deviceId = (uint32_t)msg.data[2] | ((uint32_t)msg.data[3] << 8) |
                 ((uint32_t)msg.data[4] << 16) | ((uint32_t)msg.data[5] << 24);
    }

    // Forward discovery packets (msgType=3) even to the hub for mesh relay
    if (msg.len >= 2 && msg.data[1] == MSG_DISCOVERY) {
      tcpClient.write(msg.data, msg.len);
      Serial.printf("Uplink: deviceId=%u msgType=%u len=%d (discovery)\n",
                    deviceId, msgType, msg.len);

      if (scanning) {
        esp_now_send(broadcastMac, msg.data, msg.len);
      }
    } else if (msg.len >= 2 && msg.data[1] == MSG_SCAN_REQ) {
      esp_now_send(broadcastMac, msg.data, msg.len);
      Serial.printf("Uplink: deviceId=%u msgType=%u len=%d (scan rebroadcast)\n",
                    deviceId, msgType, msg.len);
    } else {
      tcpClient.write(msg.data, msg.len);
      Serial.printf("Uplink: deviceId=%u msgType=%u len=%d\n", deviceId,
                    msgType, msg.len);
    }

    uplinkTail = (uplinkTail + 1) % UPLINK_QUEUE_SIZE;
  }
}

void handleDownlink() {
  if (!tcpClient.available()) return;
  int avail = tcpClient.available();

  int lead = tcpClient.peek();
  if (lead < 0) return;

  if (lead == 0xAA) {
    if (avail < (int)sizeof(ESPNowMessage)) return;

    ESPNowMessage msg;
    tcpClient.readBytes((uint8_t*)&msg, sizeof(ESPNowMessage));

    if (msg.header != 0xAA) return;

    uint8_t calc = 0;
    for (int i = 0; i < 8; i++) calc ^= ((uint8_t*)&msg)[i];
    if (calc != msg.checksum) return;

    Serial.printf("Downlink: deviceId=%u msgType=%u value=%u\n", msg.deviceId,
                  msg.msgType, msg.value);

    switch (msg.msgType) {
      case MSG_SCAN_REQ:
        scanning = true;
        scanStartTime = millis();
        Serial.println("Scan started by hub command");
        esp_now_send(broadcastMac, (uint8_t*)&msg, sizeof(ESPNowMessage));
        break;

      case MSG_PROVISION:
        esp_now_send(broadcastMac, (uint8_t*)&msg, sizeof(ESPNowMessage));
        break;

      default:
        esp_now_send(broadcastMac, (uint8_t*)&msg, sizeof(ESPNowMessage));
        break;
    }

    tcpClient.printf("ACK:cmd:%u\n", msg.deviceId);
  } else if (lead == 'B') {
    String cmd = tcpClient.readStringUntil('\n');
    if (cmd.startsWith("BB:")) {
      handleBBCommand(cmd);
    } else {
      Serial.printf("Unknown TCP data: %s\n", cmd.c_str());
    }
  } else {
    String unknown = tcpClient.readStringUntil('\n');
    Serial.printf("Unknown TCP data (lead=0x%02x): %s\n", lead,
                  unknown.c_str());
  }
}

void handleBBCommand(const String& cmd) {
  // Format: BB:deviceId:apiKey:gatewayId:deviceType
  int firstColon = cmd.indexOf(':');
  if (firstColon < 0) return;
  int secondColon = cmd.indexOf(':', firstColon + 1);
  if (secondColon < 0) return;
  int thirdColon = cmd.indexOf(':', secondColon + 1);
  if (thirdColon < 0) return;
  int fourthColon = cmd.indexOf(':', thirdColon + 1);

  String devIdStr = cmd.substring(firstColon + 1, secondColon);
  String apiKeyStr = cmd.substring(secondColon + 1, thirdColon);
  String gatewayIdStr = cmd.substring(thirdColon + 1, fourthColon > 0 ? fourthColon : cmd.length());

  uint32_t deviceId = (uint32_t)devIdStr.toInt();

  Serial.printf("BB: Provisioning node %u gateway=%s\n", deviceId, gatewayIdStr.c_str());

  ESPNowProvisionMessage provMsg;
  provMsg.header = 0xAA;
  provMsg.msgType = MSG_PROVISION;
  provMsg.deviceId = deviceId;
  memset(provMsg.apiKey, 0, sizeof(provMsg.apiKey));
  apiKeyStr.toCharArray(provMsg.apiKey, sizeof(provMsg.apiKey) - 1);
  memset(provMsg.gatewayId, 0, sizeof(provMsg.gatewayId));
  gatewayIdStr.toCharArray(provMsg.gatewayId, sizeof(provMsg.gatewayId) - 1);

  uint8_t calc = 0;
  for (int i = 0; i < (int)sizeof(ESPNowProvisionMessage) - 1; i++) {
    calc ^= ((uint8_t*)&provMsg)[i];
  }
  provMsg.checksum = calc;

  esp_err_t result =
      esp_now_send(broadcastMac, (uint8_t*)&provMsg,
                   sizeof(ESPNowProvisionMessage));
  Serial.printf("BB: ESP-NOW send result: %d\n", result);

  tcpClient.printf("ACK:provision:%u\n", deviceId);
}
