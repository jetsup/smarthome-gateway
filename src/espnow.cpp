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
  while (uplinkTail != uplinkHead) {
    UplinkMessage& msg = uplinkQueue[uplinkTail];

    uint32_t deviceId = 0;
    uint8_t msgType = (msg.len >= 2) ? msg.data[1] : 0;
    if (msg.len >= 6) {
      deviceId = (uint32_t)msg.data[2] | ((uint32_t)msg.data[3] << 8) |
                 ((uint32_t)msg.data[4] << 16) | ((uint32_t)msg.data[5] << 24);
    }

    // Update local node registry (with mutex for thread safety)
    uint16_t val = (msg.len >= 8) ? ((uint16_t)msg.data[6] | ((uint16_t)msg.data[7] << 8)) : 0;
    if (msgType == MSG_TELEMETRY || msgType == MSG_DISCOVERY) {
      uint8_t devType = (msgType == MSG_DISCOVERY) ? (uint8_t)val : 0;
      if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
      addOrUpdateLocalNode(deviceId, devType, val);
      if (dataMutex) xSemaphoreGive(dataMutex);
    }

    // Forward to hub if connected
    if (tcpClient.connected()) {
      if (msgType == MSG_DISCOVERY) {
        tcpClient.write(msg.data, msg.len);
        Serial.printf("Uplink: deviceId=%u msgType=%u len=%d (discovery)\n",
                      deviceId, msgType, msg.len);

        if (scanning) {
          esp_now_send(broadcastMac, msg.data, msg.len);
        }
      } else if (msgType == MSG_SCAN_REQ) {
        esp_now_send(broadcastMac, msg.data, msg.len);
        Serial.printf("Uplink: deviceId=%u msgType=%u len=%d (scan rebroadcast)\n",
                      deviceId, msgType, msg.len);
      } else {
        tcpClient.write(msg.data, msg.len);
        Serial.printf("Uplink: deviceId=%u msgType=%u len=%d\n", deviceId,
                      msgType, msg.len);
      }
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
  } else if (lead == 'W') {
    String cmd = tcpClient.readStringUntil('\n');
    if (cmd.startsWith("WIFI:")) {
      handleWifiCommand(cmd);
    } else {
      Serial.printf("Unknown TCP data (W): %s\n", cmd.c_str());
    }
  } else {
    String unknown = tcpClient.readStringUntil('\n');
    Serial.printf("Unknown TCP data (lead=0x%02x): %s\n", lead,
                  unknown.c_str());
  }
}

void handleBBCommand(const String& cmd) {
  // Format: BB:deviceId:apiKey:gatewayId:deviceType:nodeName:capCount:type1:pin1:extra1:label1:...
  int c1 = cmd.indexOf(':');
  if (c1 < 0) return;
  int c2 = cmd.indexOf(':', c1 + 1);
  if (c2 < 0) return;
  int c3 = cmd.indexOf(':', c2 + 1);
  if (c3 < 0) return;
  int c4 = cmd.indexOf(':', c3 + 1);
  if (c4 < 0) return;
  int c5 = cmd.indexOf(':', c4 + 1);
  if (c5 < 0) return;

  String devIdStr = cmd.substring(c1 + 1, c2);
  String apiKeyStr = cmd.substring(c2 + 1, c3);
  String gatewayIdStr = cmd.substring(c3 + 1, c4);
  String devTypeStr = cmd.substring(c4 + 1, c5);

  int c6 = cmd.indexOf(':', c5 + 1);
  String nodeNameStr;
  if (c6 < 0) {
    nodeNameStr = cmd.substring(c5 + 1);
  } else {
    nodeNameStr = cmd.substring(c5 + 1, c6);
  }

  uint32_t deviceId = parseUint32(devIdStr);

  Serial.printf("BB: Provisioning node %u gateway=%s name=%s\n",
    deviceId, gatewayIdStr.c_str(), nodeNameStr.c_str());

  // Parse capabilities
  uint8_t capCount = 0;
  CapabilitySlot caps[CAP_MAX_COUNT];
  memset(caps, 0, sizeof(caps));

  if (c6 > 0) {
    int pos = c6;
    int ccColon = cmd.indexOf(':', pos + 1);
    if (ccColon > 0) {
      capCount = (uint8_t)cmd.substring(pos + 1, ccColon).toInt();
      if (capCount > CAP_MAX_COUNT) capCount = CAP_MAX_COUNT;
      pos = ccColon;
      for (int i = 0; i < capCount; i++) {
        int tColon = cmd.indexOf(':', pos + 1);
        if (tColon < 0) break;
        int pColon = cmd.indexOf(':', tColon + 1);
        if (pColon < 0) break;
        int eColon = cmd.indexOf(':', pColon + 1);
        if (eColon < 0) break;
        int lColon = cmd.indexOf(':', eColon + 1);

        caps[i].type = (uint8_t)cmd.substring(pos + 1, tColon).toInt();
        caps[i].pin = (uint8_t)cmd.substring(tColon + 1, pColon).toInt();
        caps[i].extra = (uint8_t)cmd.substring(pColon + 1, eColon).toInt();

        String label;
        if (lColon < 0) {
          label = cmd.substring(eColon + 1);
        } else {
          label = cmd.substring(eColon + 1, lColon);
          pos = lColon;
        }
        label.toCharArray(caps[i].label, CAP_LABEL_LEN);

        Serial.printf("  Cap %d: type=%d pin=%d extra=%d label=%s\n",
          i, caps[i].type, caps[i].pin, caps[i].extra, caps[i].label);
      }
    }
  }

  ESPNowProvisionMessage provMsg;
  provMsg.header = 0xAA;
  provMsg.msgType = MSG_PROVISION;
  provMsg.deviceId = deviceId;
  memset(provMsg.apiKey, 0, sizeof(provMsg.apiKey));
  apiKeyStr.toCharArray(provMsg.apiKey, sizeof(provMsg.apiKey) - 1);
  memset(provMsg.gatewayId, 0, sizeof(provMsg.gatewayId));
  gatewayIdStr.toCharArray(provMsg.gatewayId, sizeof(provMsg.gatewayId) - 1);
  memset(provMsg.nodeName, 0, sizeof(provMsg.nodeName));
  nodeNameStr.toCharArray(provMsg.nodeName, sizeof(provMsg.nodeName) - 1);
  provMsg.capCount = capCount;
  memcpy(provMsg.caps, caps, sizeof(caps));

  uint8_t calc = 0;
  for (int i = 0; i < (int)sizeof(ESPNowProvisionMessage) - 1; i++) {
    calc ^= ((uint8_t*)&provMsg)[i];
  }
  provMsg.checksum = calc;

  esp_err_t result =
      esp_now_send(broadcastMac, (uint8_t*)&provMsg,
                   sizeof(ESPNowProvisionMessage));
  Serial.printf("BB: ESP-NOW send result: %d caps=%d msgSize=%d\n",
    result, capCount, sizeof(ESPNowProvisionMessage));

  tcpClient.printf("ACK:provision:%u\n", deviceId);
}

void handleWifiCommand(const String& cmd) {
  // Format: WIFI:ssid:password
  int firstColon = cmd.indexOf(':');
  if (firstColon < 0) return;
  int secondColon = cmd.indexOf(':', firstColon + 1);
  if (secondColon < 0) return;

  String ssid = cmd.substring(firstColon + 1, secondColon);
  String password = cmd.substring(secondColon + 1);
  password.trim();

  Serial.printf("WIFI: Configuring network SSID=%s len(pass)=%d\n",
                ssid.c_str(), password.length());

  // Save to credential list
  saveWifiCredential(ssid, password);

  // Also save as active SSID/pass for backward compat
  prefs.putString(NVS_KEY_SSID, ssid);
  prefs.putString(NVS_KEY_PASS, password);

  // Connect immediately
  wifiSSID = ssid;
  wifiPass = password;
  connectToWiFi(wifiSSID.c_str(), wifiPass.c_str());

  tcpClient.printf("ACK:wifi:%s\n", ssid.c_str());
}
