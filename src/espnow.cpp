#include <ArduinoJson.h>

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

// ── Shared clock ─────────────────────────────────────────────────────────────
uint32_t timeBaseEpoch = 0;
uint32_t timeBaseMillis = 0;
uint8_t timeSource = 0;  // 0 unknown, 1 rtc, 2 hub

void broadcastTime() {
  if (timeBaseEpoch == 0) return;
  uint32_t now = timeBaseEpoch + (millis() - timeBaseMillis) / 1000;
  uint8_t payload[5] = {(uint8_t)now, (uint8_t)(now >> 8), (uint8_t)(now >> 16), (uint8_t)(now >> 24), timeSource};
  uint8_t f[8 + 5];
  f[0] = 0xAA; f[1] = MSG_TIME; f[2] = f[3] = f[4] = f[5] = 0; f[6] = 5;
  memcpy(f + 7, payload, 5);
  uint8_t x = 0;
  for (int i = 0; i < 12; i++) x ^= f[i];
  f[12] = x;
  esp_now_send(broadcastMac, f, sizeof(f));
}

void sendGatewayAnnounce() {
  uint32_t gwId = prefs.getUInt("gateway_id", 0);
  String gwName;
  if (prefs.isKey("gateway_name")) {
    gwName = prefs.getString("gateway_name");
  } else {
    gwName = "SmartHome GW";
  }

  if (gwId == 0) {
    // Derive a stable ID from the MAC address last 4 bytes
    uint64_t mac = ESP.getEfuseMac();
    gwId = (uint32_t)(mac & 0xFFFFFFFF);
  }

  struct __attribute__((packed)) {
    uint8_t header;
    uint8_t msgType;
    uint32_t gatewayId;
    char name[32];
    uint8_t checksum;
  } pkt;

  pkt.header = 0xAA;
  pkt.msgType = MSG_GATEWAY_ANNOUNCE;
  pkt.gatewayId = gwId;

  memset(pkt.name, 0, sizeof(pkt.name));
  strncpy(pkt.name, gwName.c_str(), sizeof(pkt.name) - 1);

  uint8_t calc = 0;
  for (int i = 0; i < (int)sizeof(pkt) - 1; i++) {
    calc ^= ((uint8_t*)&pkt)[i];
  }
  pkt.checksum = calc;

  esp_now_send(broadcastMac, (uint8_t*)&pkt, sizeof(pkt));
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

    // Keep a timestamped copy of every reading (also while the hub is unreachable)
    dataLogFrame(msg.data, msg.len);

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

// ESP-NOW broadcasts have no acknowledgement or retry, so a single frame is
// easily lost (the node is also transmitting). Commands are idempotent ("set pin
// to value"), so send each one a few times a few milliseconds apart.
static void sendReliable(const uint8_t* data, size_t len, int times = 4, int gapMs = 15) {
  for (int i = 0; i < times; i++) {
    esp_now_send(broadcastMac, data, len);
    if (i + 1 < times) delay(gapMs);
  }
}

// Pairing code sent by the hub just before a BB: line (see PCODE: above).
static uint32_t pendingCodeDev = 0;
static char pendingCode[9] = {0};

void handleDownlink() {
  if (!tcpClient.available()) return;
  int avail = tcpClient.available();

  int lead = tcpClient.peek();
  if (lead < 0) return;

  if (lead == 0xAA) {
    // Classic 9-byte frames (types 1-7) or length-prefixed frames (type >= 8).
    if (avail < 2) return;
    uint8_t head[2];
    tcpClient.readBytes(head, 2);
    if (head[0] != 0xAA) return;

    if (head[1] >= 8) {
      uint8_t rest[5];  // deviceId[4] + len
      if (tcpClient.readBytes(rest, 5) != 5) return;
      uint8_t plen = rest[4];
      static uint8_t frame[2 + 5 + 255 + 1];
      memcpy(frame, head, 2);
      memcpy(frame + 2, rest, 5);
      if (tcpClient.readBytes(frame + 7, plen + 1) != plen + 1) return;
      uint8_t x = 0;
      for (int i = 0; i < 7 + plen; i++) x ^= frame[i];
      if (x != frame[7 + plen]) return;
      uint32_t devId = rest[0] | (rest[1] << 8) | (rest[2] << 16) | ((uint32_t)rest[3] << 24);
      Serial.printf("Downlink: deviceId=%u msgType=%u len=%u\n", devId, head[1], plen);
      sendReliable(frame, 8 + plen);
      tcpClient.printf("ACK:cmd:%u\n", devId);
      return;
    }

    ESPNowMessage msg;
    memcpy(&msg, head, 2);
    if (tcpClient.readBytes(((uint8_t*)&msg) + 2, sizeof(ESPNowMessage) - 2) != (int)sizeof(ESPNowMessage) - 2) return;

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

      default:
        sendReliable((uint8_t*)&msg, sizeof(ESPNowMessage));
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
    if (cmd.startsWith("WIFISET:")) {
      handleWifiSet(cmd);
    } else if (cmd.startsWith("WIFI:")) {
      handleWifiCommand(cmd);
    } else {
      Serial.printf("Unknown TCP data (W): %s\n", cmd.c_str());
    }
  } else if (lead == 'G') {
    String cmd = tcpClient.readStringUntil('\n');
    // GWNAME:<name> — the hub tells the gateway what the user called it, so it
    // announces the right name to HMIs over ESP-NOW.
    if (cmd.startsWith("GWNAME:")) {
      String name = cmd.substring(7);
      name.trim();
      if (name.length() > 0 && prefs.getString("gateway_name", "") != name) {
        prefs.putString("gateway_name", name);
        Serial.printf("Gateway name set by hub: %s\n", name.c_str());
      }
    } else {
      Serial.printf("Unknown TCP data (G): %s\n", cmd.c_str());
    }
  } else if (lead == 'T') {
    String cmd = tcpClient.readStringUntil('\n');
    // TIME:<epoch>:<source> — the hub's system clock (RTC module or hub).
    if (cmd.startsWith("TIME:")) {
      long epoch = cmd.substring(5).toInt();
      if (epoch > 1600000000L) {
        // The gateway's own RTC / internet time win; the hub's clock is the fallback.
        clockFromHub((uint32_t)epoch, cmd.indexOf(":rtc") > 0);
      }
    }
  } else if (lead == 'L') {
    String cmd = tcpClient.readStringUntil('\n');
    if (cmd.startsWith("LOGACK:")) {
      dataLogAck((uint32_t)strtoul(cmd.c_str() + 7, nullptr, 10));
    } else if (cmd.startsWith("LOGCFG:")) {
      // LOGCFG:<store 0 flash|1 sd>:<maxKB>:<sdCS>:<rtcSDA>:<rtcSCL>
      int v[5] = {0, 256, 5, 21, 22};
      int from = 7;
      for (int i = 0; i < 5 && from <= (int)cmd.length(); i++) {
        int c = cmd.indexOf(':', from);
        String part = c < 0 ? cmd.substring(from) : cmd.substring(from, c);
        if (part.length()) v[i] = part.toInt();
        if (c < 0) break;
        from = c + 1;
      }
      int oldSda = prefs.getInt("rtc_sda", 21), oldScl = prefs.getInt("rtc_scl", 22);
      int oldStore = prefs.getInt("log_store", 0), oldCs = prefs.getInt("sd_cs", 5);
      dataLogConfigure(v[0], v[1], v[2]);
      prefs.putInt("rtc_sda", v[3]);
      prefs.putInt("rtc_scl", v[4]);
      Serial.printf("Log settings from hub: store=%d max=%dKB sdcs=%d rtc=%d/%d\n", v[0], v[1], v[2], v[3], v[4]);
      if (v[3] != oldSda || v[4] != oldScl) clockConfigure(v[3], v[4]);
      if (v[0] != oldStore || v[2] != oldCs) pendingReboot = true;  // remount the storage
    } else {
      Serial.printf("Unknown TCP data (L): %s\n", cmd.c_str());
    }
  } else if (lead == 'P') {
    String cmd = tcpClient.readStringUntil('\n');
    // PAIR:CODE:XXXXXX — pairing code for HMI, log it for serial display
    if (cmd.startsWith("PCODE:")) {
      // PCODE:<deviceId>:<CODE> — the pairing code for the next BB: to that node
      int c = cmd.indexOf(':', 6);
      if (c > 0) {
        pendingCodeDev = parseUint32(cmd.substring(6, c));
        memset(pendingCode, 0, sizeof(pendingCode));
        cmd.substring(c + 1).toCharArray(pendingCode, sizeof(pendingCode));
      }
    } else if (cmd.startsWith("PAIR:CODE:")) {
      String code = cmd.substring(10);
      Serial.printf("HMI PAIRING CODE: %s\n", code.c_str());
      Serial.println("Enter this code on the HMI to complete pairing.");
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

  // Save gateway identity from first BB command (always MAC-derived, never hex DB ID)
  uint32_t knownId = prefs.getUInt("gateway_id", 0);
  if (knownId == 0) {
    uint64_t mac = ESP.getEfuseMac();
    uint32_t macId = (uint32_t)(mac & 0xFFFFFFFF);
    prefs.putUInt("gateway_id", macId);
    // NB: the gateway's *own* name comes from the hub (GWNAME:) or the app
    // (Bluetooth setup). It must never be taken from a node's name.
    Serial.printf("BB: saved gateway_id %u (MAC-derived)\n", macId);
  }

  // Parse capabilities
  uint8_t capCount = 0;
  CapabilitySlot caps[CAP_MAX_COUNT];
  memset(caps, 0, sizeof(caps));

  if (c6 > 0) {
    // After the name: capCount, then 7 fields per module:
    //   type:pin:extra:count:flags:pin2:label
    int pos = c6;
    int ccColon = cmd.indexOf(':', pos + 1);
    if (ccColon > 0) {
      capCount = (uint8_t)cmd.substring(pos + 1, ccColon).toInt();
      if (capCount > CAP_MAX_COUNT) capCount = CAP_MAX_COUNT;
      pos = ccColon;
      for (int i = 0; i < capCount; i++) {
        String f[7];
        int cur = pos;
        bool ok = true;
        for (int k = 0; k < 7; k++) {
          int nx = cmd.indexOf(':', cur + 1);
          if (k < 6) {
            if (nx < 0) { ok = false; break; }
            f[k] = cmd.substring(cur + 1, nx);
            cur = nx;
          } else {
            // label: up to the next ':' or end of line
            f[k] = (nx < 0) ? cmd.substring(cur + 1) : cmd.substring(cur + 1, nx);
            cur = (nx < 0) ? cmd.length() : nx;
          }
        }
        if (!ok) break;
        pos = cur;
        caps[i].type = (uint8_t)f[0].toInt();
        caps[i].pin = (uint8_t)f[1].toInt();
        caps[i].extra = (uint8_t)f[2].toInt();
        caps[i].count = (uint16_t)f[3].toInt();
        caps[i].flags = (uint8_t)f[4].toInt();
        caps[i].pin2 = (uint8_t)f[5].toInt();
        f[6].toCharArray(caps[i].label, CAP_LABEL_LEN);
        Serial.printf("  Cap %d: type=%d pin=%d extra=%d count=%d flags=%d pin2=%d label=%s\n",
          i, caps[i].type, caps[i].pin, caps[i].extra, caps[i].count, caps[i].flags, caps[i].pin2, caps[i].label);
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
  memset(provMsg.code, 0, sizeof(provMsg.code));
  if (pendingCodeDev == deviceId) memcpy(provMsg.code, pendingCode, sizeof(provMsg.code) - 1);
  pendingCodeDev = 0;

  uint8_t calc = 0;
  for (int i = 0; i < (int)sizeof(ESPNowProvisionMessage) - 1; i++) {
    calc ^= ((uint8_t*)&provMsg)[i];
  }
  provMsg.checksum = calc;

  esp_err_t result = esp_now_send(broadcastMac, (uint8_t*)&provMsg,
                                  sizeof(ESPNowProvisionMessage));
  for (int i = 0; i < 3; i++) {  // repeat: broadcasts are not acknowledged
    delay(25);
    esp_now_send(broadcastMac, (uint8_t*)&provMsg, sizeof(ESPNowProvisionMessage));
  }
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

// WIFISET:{"nets":[{"s":"ssid","p":"pass"},...]} — replace the saved networks
// with the hub's ordered list (index 0 = highest priority). We stay on the
// current network unless it was removed, so a reorder never drops the link.
void handleWifiSet(const String& cmd) {
  JsonDocument doc;
  if (deserializeJson(doc, cmd.substring(8)) != DeserializationError::Ok) {
    Serial.println("WIFISET: bad JSON");
    return;
  }
  JsonArray nets = doc["nets"].as<JsonArray>();

  String keepCurrent = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("");
  removeAllWifiCredentials();
  int saved = 0;
  bool currentStillSaved = false;
  for (JsonObject n : nets) {
    String s = n["s"] | "";
    String p = n["p"] | "";
    if (s.length() == 0 || saved >= MAX_SAVED_WIFI) continue;
    saveWifiCredential(s, p);
    saved++;
    if (s == keepCurrent) currentStillSaved = true;
  }
  Serial.printf("WIFISET: %d network(s) saved in hub order\n", saved);

  if (keepCurrent.length() > 0 && !currentStillSaved) {
    Serial.println("WIFISET: current network was removed — reselecting");
    scanAndConnect();
  }
  tcpClient.printf("ACK:wifiset:%d\n", saved);
}
