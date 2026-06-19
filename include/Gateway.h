#ifndef GATEWAY_H
#define GATEWAY_H

#include <Arduino.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
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
extern AsyncWebServer asyncServer;
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

// ── Local node registry ──────────────────────────────────────────────────────
struct LocalNode {
  uint32_t deviceId;
  uint8_t deviceType;
  uint16_t lastValue;
  unsigned long lastSeen;
  bool isOnline() const {
    return (millis() - lastSeen) <= NODE_OFFLINE_MS;
  }
};

// Simple array-based registry (max 64 nodes, no dynamic alloc)
#define MAX_LOCAL_NODES 64
extern LocalNode localNodes[MAX_LOCAL_NODES];
extern int localNodeCount;

int findLocalNode(uint32_t deviceId);
int addOrUpdateLocalNode(uint32_t deviceId, uint8_t deviceType, uint16_t value);

// ── Offline operation queue ──────────────────────────────────────────────────
#define MAX_QUEUE_ENTRIES 32

struct QueueEntry {
  char type[16];
  uint32_t deviceId;
  char apiKey[33];
};

extern QueueEntry opQueue[MAX_QUEUE_ENTRIES];
extern int opQueueCount;

extern SemaphoreHandle_t dataMutex;

void loadQueue();
void saveQueue();
void enqueueOperation(const char* type, uint32_t deviceId, const char* apiKey);
int flushQueue();
void tryFlushQueue();

// ── Web server ───────────────────────────────────────────────────────────────
void initWebServer();

// ── Function declarations ────────────────────────────────────────────────────
void startAPMode();
void startLinkingMode();
void connectToWiFi(const char* ssid, const char* pass);
void attemptReconnect();
void onESPNOWRecv(const uint8_t* mac, const uint8_t* data, int len);
void initESPNOW();
void handleESPNOW();
void handleDownlink();
void handleBBCommand(const String& cmd);
bool tcpConnect();
void tcpLoop();
void checkResetPin();

// ── WiFi credential management (multi-SSID) ─────────────
int  getSavedWifiCount();
bool getSavedWifi(int index, String& ssid, String& pass);
void saveWifiCredential(const String& ssid, const String& pass);
void removeAllWifiCredentials();
bool scanAndConnect();

void handleWifiCommand(const String& cmd);

#endif // GATEWAY_H
