#ifndef GATEWAY_H
#define GATEWAY_H

#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_now.h>

#include "Config.hpp"

// ── Packet structs ────────────────────────────────────────────────────────────
struct __attribute__((__packed__)) ESPNowMessage {
  uint8_t header;
  uint8_t msgType;
  uint32_t deviceId;
  uint16_t value;
  uint8_t checksum;
};

struct __attribute__((__packed__)) ESPNowProvisionMessage {
  uint8_t header;
  uint8_t msgType;
  uint32_t deviceId;
  char apiKey[33];
  char gatewayId[17];
  uint8_t checksum;
};

extern uint8_t broadcastMac[];

// ── FSM ──────────────────────────────────────────────────────────────────────
enum State {
  STATE_AP_SETUP,
  STATE_CONNECTING,
  STATE_LINKING,
  STATE_MESH
};
extern State currentState;
extern unsigned long stateTimer;

// ── NVS ──────────────────────────────────────────────────────────────────────
extern Preferences prefs;

// ── Servers ──────────────────────────────────────────────────────────────────
extern WebServer httpServer;
extern DNSServer dnsServer;

// ── Credentials ──────────────────────────────────────────────────────────────
extern String wifiSSID;
extern String wifiPass;

// ── Hub config ───────────────────────────────────────────────────────────────
extern String apiKey;

// ── TCP ──────────────────────────────────────────────────────────────────────
extern WiFiClient tcpClient;
extern unsigned long lastTCPReconnect;
extern bool pendingReboot;

// ── ESP-NOW ring buffer ──────────────────────────────────────────────────────
#define UPLINK_QUEUE_SIZE 32
#define MAX_MSG_SIZE 64

struct UplinkMessage {
  uint8_t data[MAX_MSG_SIZE];
  int len;
};

extern UplinkMessage uplinkQueue[UPLINK_QUEUE_SIZE];
extern volatile int uplinkHead;
extern volatile int uplinkTail;

// ── Scan state ──────────────────────────────────────────────────────────────
extern bool scanning;
extern unsigned long scanStartTime;

// ── Function declarations ────────────────────────────────────────────────────
void startAPMode();
void startLinkingMode();
void connectToWiFi(const char* ssid, const char* pass);
void attemptReconnect();
void handleCaptivePortal();
void handleWiFiConfig();
void handleGen204();
void handleApiProxy();
void handleLinkingPage();
void handleConfigure();
void onESPNOWRecv(const uint8_t* mac, const uint8_t* data, int len);
void initESPNOW();
void handleESPNOW();
void handleDownlink();
void handleBBCommand(const String& cmd);
bool tcpConnect();
void tcpLoop();
void checkResetPin();

#endif // GATEWAY_H
