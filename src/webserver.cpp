#include "Gateway.h"

// ── Globals ───────────────────────────────────────────────────────────────────
LocalNode localNodes[MAX_LOCAL_NODES];
int localNodeCount = 0;

QueueEntry opQueue[MAX_QUEUE_ENTRIES];
int opQueueCount = 0;

static unsigned long lastQueueFlush = 0;

// ── Local node registry helpers ──────────────────────────────────────────────
int findLocalNode(uint32_t deviceId) {
  for (int i = 0; i < localNodeCount; i++) {
    if (localNodes[i].deviceId == deviceId) return i;
  }
  return -1;
}

int addOrUpdateLocalNode(uint32_t deviceId, uint8_t deviceType, uint16_t value) {
  int idx = findLocalNode(deviceId);
  if (idx >= 0) {
    localNodes[idx].deviceType = deviceType;
    localNodes[idx].lastValue = value;
    localNodes[idx].lastSeen = millis();
    return idx;
  }
  if (localNodeCount >= MAX_LOCAL_NODES) return -1;
  idx = localNodeCount++;
  localNodes[idx].deviceId = deviceId;
  localNodes[idx].deviceType = deviceType;
  localNodes[idx].lastValue = value;
  localNodes[idx].lastSeen = millis();
  return idx;
}

// ── Queue helpers (NVS-backed) ───────────────────────────────────────────────
void loadQueue() {
  String raw = prefs.getString(QUEUE_NVS_KEY, "");
  opQueueCount = 0;
  if (raw.length() == 0) return;
  // Called once at startup — no mutex needed

  int start = 0;
  while (start < (int)raw.length() && opQueueCount < MAX_QUEUE_ENTRIES) {
    int nl = raw.indexOf('\n', start);
    if (nl < 0) nl = raw.length();
    String line = raw.substring(start, nl);
    start = nl + 1;
    if (line.length() == 0) continue;

    int c1 = line.indexOf(':');
    if (c1 < 0) continue;

    QueueEntry& e = opQueue[opQueueCount];
    memset(&e, 0, sizeof(e));
    String typeStr = line.substring(0, c1);
    typeStr.toCharArray(e.type, sizeof(e.type));
    String dataStr = line.substring(c1 + 1);
    dataStr.toCharArray(e.rawData, sizeof(e.rawData));
    opQueueCount++;
  }
}

void saveQueue() {
  // Caller must hold dataMutex
  String raw;
  for (int i = 0; i < opQueueCount; i++) {
    if (i > 0) raw += '\n';
    raw += String(opQueue[i].type) + ':' + String(opQueue[i].rawData);
  }
  prefs.putString(QUEUE_NVS_KEY, raw);
}

void tryFlushQueue() {
  static unsigned long lastFlush = 0;
  if (millis() - lastFlush < QUEUE_FLUSH_INTERVAL) return;
  lastFlush = millis();
  int flushed = flushQueue();
  if (flushed > 0) {
    Serial.printf("Queue flushed: %d operations sent\n", flushed);
  }
}

void enqueueOperation(const char* type, const char* rawData) {
  if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
  if (opQueueCount >= MAX_QUEUE_ENTRIES) {
    if (dataMutex) xSemaphoreGive(dataMutex);
    return;
  }
  QueueEntry& e = opQueue[opQueueCount++];
  memset(&e, 0, sizeof(e));
  strncpy(e.type, type, sizeof(e.type) - 1);
  if (rawData) strncpy(e.rawData, rawData, sizeof(e.rawData) - 1);
  saveQueue();
  if (dataMutex) xSemaphoreGive(dataMutex);
}

int flushQueue() {
  if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
  if (opQueueCount == 0) {
    if (dataMutex) xSemaphoreGive(dataMutex);
    return 0;
  }
  if (!tcpClient.connected()) {
    if (dataMutex) xSemaphoreGive(dataMutex);
    return -1;
  }

  int flushed = 0;
  int kept = 0;
  QueueEntry temp[MAX_QUEUE_ENTRIES];

  for (int i = 0; i < opQueueCount; i++) {
    QueueEntry& e = opQueue[i];
    if (strcmp(e.type, OP_PROVISION) == 0) {
      if (tcpClient.connected()) {
        tcpClient.printf("%s\n", e.rawData);
        // Extract deviceId from rawData for the ACK
        int c1 = 0, c2 = 0;
        // rawData = "BB:deviceId:..."
        c1 = String(e.rawData).indexOf(':');
        if (c1 >= 0) c2 = String(e.rawData).indexOf(':', c1 + 1);
        if (c2 >= 0) {
          uint32_t devId = parseUint32(String(e.rawData).substring(c1 + 1, c2));
          tcpClient.printf("ACK:provision:%u\n", devId);
          Serial.printf("Queue flush: provision node %u\n", devId);
        } else {
          Serial.printf("Queue flush: provision (raw=%s)\n", e.rawData);
        }
        flushed++;
      } else {
        temp[kept++] = e;
      }
    } else if (strcmp(e.type, OP_DISCONNECT) == 0) {
      if (tcpClient.connected()) {
        tcpClient.printf("%s\n", e.rawData);
        uint32_t devId = parseUint32(String(e.rawData));
        Serial.printf("Queue flush: disconnect node %u\n", devId);
        flushed++;
      } else {
        temp[kept++] = e;
      }
    }
  }

  memcpy(opQueue, temp, kept * sizeof(QueueEntry));
  opQueueCount = kept;
  saveQueue();
  if (dataMutex) xSemaphoreGive(dataMutex);
  return flushed;
}

// ── CORS helper ──────────────────────────────────────────────────────────────
static void addCORS(AsyncWebServerResponse* resp) {
  resp->addHeader("Access-Control-Allow-Origin", "*");
  resp->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  resp->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
}

// ── JSON response helpers ────────────────────────────────────────────────────
static void sendJSON(AsyncWebServerRequest* req, int code, const String& body) {
  AsyncWebServerResponse* resp = req->beginResponse(code, "application/json", body);
  addCORS(resp);
  req->send(resp);
}

static void sendError(AsyncWebServerRequest* req, int code, const String& msg) {
  sendJSON(req, code, "{\"error\":\"" + msg + "\"}");
}

// ── REST API handlers ─────────────────────────────────────────────────────────

// GET /api/nodes
static void handleApiNodesList(AsyncWebServerRequest* req) {
  if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
  String json = "[";
  for (int i = 0; i < localNodeCount; i++) {
    if (i > 0) json += ',';
    json += "{\"deviceId\":" + String(localNodes[i].deviceId) +
            ",\"deviceType\":" + String(localNodes[i].deviceType) +
            ",\"lastValue\":" + String(localNodes[i].lastValue) +
            ",\"online\":" + (localNodes[i].isOnline() ? "true" : "false") +
            "}";
  }
  json += "]";
  if (dataMutex) xSemaphoreGive(dataMutex);
  sendJSON(req, 200, json);
}

// GET /api/nodes/{deviceId} — parse deviceId from URL
static void handleApiNodeGet(AsyncWebServerRequest* req) {
  String url = req->url();
  url.remove(0, 11); // remove "/api/nodes/"
  uint32_t deviceId = parseUint32(url);
  if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
  int idx = findLocalNode(deviceId);
  if (idx < 0) {
    if (dataMutex) xSemaphoreGive(dataMutex);
    sendError(req, 404, "Node not found");
    return;
  }
  String json = "{\"deviceId\":" + String(localNodes[idx].deviceId) +
                ",\"deviceType\":" + String(localNodes[idx].deviceType) +
                ",\"lastValue\":" + String(localNodes[idx].lastValue) +
                ",\"online\":" + (localNodes[idx].isOnline() ? "true" : "false") +
                "}";
  if (dataMutex) xSemaphoreGive(dataMutex);
  sendJSON(req, 200, json);
}

// POST /api/nodes/{deviceId}/command — parse deviceId from URL
static void handleApiNodeCommand(AsyncWebServerRequest* req) {
  if (currentState != STATE_MESH) {
    sendError(req, 503, "Gateway not in mesh mode");
    return;
  }
  String url = req->url();
  url.remove(0, 11); // remove "/api/nodes/"
  int slash = url.indexOf('/');
  if (slash < 0) {
    sendError(req, 400, "Invalid path");
    return;
  }
  uint32_t deviceId = parseUint32(url.substring(0, slash));

  // Read value from body
  if (req->contentLength() == 0) {
    sendError(req, 400, "Missing body");
    return;
  }

  String body = req->arg("plain");
  // Extract "value" field (simple JSON parse)
  int vPos = body.indexOf("\"value\"");
  if (vPos < 0) {
    sendError(req, 400, "Missing value field");
    return;
  }
  int colon = body.indexOf(':', vPos);
  if (colon < 0) {
    sendError(req, 400, "Bad JSON");
    return;
  }
  int numStart = colon + 1;
  while (numStart < (int)body.length() && (body[numStart] == ' ' || body[numStart] == '\t')) numStart++;
  int numEnd = numStart;
  while (numEnd < (int)body.length() && body[numEnd] >= '0' && body[numEnd] <= '9') numEnd++;
  uint16_t value = (uint16_t)body.substring(numStart, numEnd).toInt();

  // Send ESP-NOW command
  ESPNowMessage msg;
  msg.header = 0xAA;
  msg.msgType = MSG_COMMAND;
  msg.deviceId = deviceId;
  msg.value = value;
  uint8_t calc = 0;
  for (int i = 0; i < 8; i++) calc ^= ((uint8_t*)&msg)[i];
  msg.checksum = calc;

  esp_err_t result = esp_now_send(broadcastMac, (uint8_t*)&msg, sizeof(ESPNowMessage));
  if (result != ESP_OK) {
    sendError(req, 500, "ESP-NOW send failed");
    return;
  }

  Serial.printf("Local API: command sent to node %u value %u\n", deviceId, value);
  String json = "{\"status\":\"transmitted\",\"deviceId\":" + String(deviceId) +
                ",\"value\":" + String(value) + "}";
  sendJSON(req, 200, json);
}

// GET /api/status
static void handleApiStatus(AsyncWebServerRequest* req) {
  if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
  int nCount = localNodeCount;
  int qDepth = opQueueCount;
  if (dataMutex) xSemaphoreGive(dataMutex);
  String macStr = WiFi.macAddress();
  macStr.replace(":", "");
  String mdnsHost = String(MDNS_NAME) + "-" + macStr.substring(macStr.length() - 4);
  String json = "{\"gatewayId\":\"" + String(prefs.getString("gateway_id", "")) +
                "\",\"mac\":\"" + WiFi.macAddress() +
                "\",\"mdns\":\"" + mdnsHost + ".local" +
                "\",\"state\":\"" + String(currentState == STATE_MESH ? "mesh" :
                   currentState == STATE_LINKING ? "linking" :
                   currentState == STATE_CONNECTING ? "connecting" : "setup") +
                "\",\"wifiRSSI\":" + String(WiFi.RSSI()) +
                ",\"wifiIP\":\"" + WiFi.localIP().toString() +
                "\",\"hubConnected\":" + (tcpClient.connected() ? "true" : "false") +
                ",\"nodes\":" + String(nCount) +
                ",\"queueDepth\":" + String(qDepth) +
                ",\"uptime\":" + String(millis() / 1000) +
                "}";
  sendJSON(req, 200, json);
}

// POST /api/provision
static void handleApiProvision(AsyncWebServerRequest* req) {
  if (req->contentLength() == 0) {
    sendError(req, 400, "Missing body");
    return;
  }
  String body = req->arg("plain");

  // Extract deviceId
  int didPos = body.indexOf("\"deviceId\"");
  if (didPos < 0) {
    sendError(req, 400, "Missing deviceId");
    return;
  }
  int colon = body.indexOf(':', didPos);
  int numStart = colon + 1;
  while (numStart < (int)body.length() && (body[numStart] == ' ' || body[numStart] == '\t')) numStart++;
  int numEnd = numStart;
  while (numEnd < (int)body.length() && body[numEnd] >= '0' && body[numEnd] <= '9') numEnd++;
  uint32_t deviceId = parseUint32(body.substring(numStart, numEnd));

  // Extract apiKey
  String apiKeyStr;
  int akPos = body.indexOf("\"apiKey\"");
  if (akPos >= 0) {
    colon = body.indexOf(':', akPos);
    int qs = body.indexOf('"', colon + 1);
    int qe = body.indexOf('"', qs + 1);
    if (qs >= 0 && qe > qs) {
      apiKeyStr = body.substring(qs + 1, qe);
    }
  }

  // Extract gatewayId
  String gatewayIdStr;
  int gwPos = body.indexOf("\"gatewayId\"");
  if (gwPos >= 0) {
    colon = body.indexOf(':', gwPos);
    int qs = body.indexOf('"', colon + 1);
    int qe = body.indexOf('"', qs + 1);
    if (qs >= 0 && qe > qs) {
      gatewayIdStr = body.substring(qs + 1, qe);
    }
  }

  // Extract nodeName
  String nodeNameStr;
  int nnPos = body.indexOf("\"nodeName\"");
  if (nnPos >= 0) {
    colon = body.indexOf(':', nnPos);
    int qs = body.indexOf('"', colon + 1);
    int qe = body.indexOf('"', qs + 1);
    if (qs >= 0 && qe > qs) {
      nodeNameStr = body.substring(qs + 1, qe);
    }
  }

  // Extract deviceType
  uint8_t devType = 0;
  int dtPos = body.indexOf("\"deviceType\"");
  if (dtPos >= 0) {
    colon = body.indexOf(':', dtPos);
    int ns = colon + 1;
    while (ns < (int)body.length() && (body[ns] == ' ' || body[ns] == '\t')) ns++;
    int ne = ns;
    while (ne < (int)body.length() && body[ne] >= '0' && body[ne] <= '9') ne++;
    devType = (uint8_t)body.substring(ns, ne).toInt();
  }

  // Build the full BB: command (capabilities handled by hub, pass "0" for none here)
  // Format: BB:deviceId:apiKey:gatewayId:deviceType:nodeName:0
  String bbCmd = "BB:" + String(deviceId) + ":" + apiKeyStr + ":" +
                 gatewayIdStr + ":" + String(devType) + ":" + nodeNameStr + ":0";

  if (tcpClient.connected()) {
    // Send immediately via TCP
    tcpClient.printf("%s\n", bbCmd.c_str());
    tcpClient.printf("ACK:provision:%u\n", deviceId);
    Serial.printf("Provision sent immediately: node %u\n", deviceId);
    sendJSON(req, 200, "{\"status\":\"sent\"}");
  } else {
    // Queue for later
    enqueueOperation(OP_PROVISION, bbCmd.c_str());
    Serial.printf("Provision queued: node %u\n", deviceId);
    sendJSON(req, 200, "{\"status\":\"queued\"}");
  }
}

// POST /api/disconnect/{deviceId} — parse deviceId from URL
static void handleApiDisconnect(AsyncWebServerRequest* req) {
  String url = req->url();
  url.remove(0, 16); // remove "/api/disconnect/"
  uint32_t deviceId = parseUint32(url);
  String devIdStr = String(deviceId);

  if (tcpClient.connected()) {
    tcpClient.printf("DEL:%u:\n", deviceId);
    Serial.printf("Disconnect sent immediately: node %u\n", deviceId);
    sendJSON(req, 200, "{\"status\":\"sent\"}");
  } else {
    enqueueOperation(OP_DISCONNECT, devIdStr.c_str());
    Serial.printf("Disconnect queued: node %u\n", deviceId);
    sendJSON(req, 200, "{\"status\":\"queued\"}");
  }
}

// GET /api/queue
static void handleApiQueue(AsyncWebServerRequest* req) {
  if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
  String json = "[";
  for (int i = 0; i < opQueueCount; i++) {
    if (i > 0) json += ',';
    String escaped = String(opQueue[i].rawData);
    escaped.replace("\"", "\\\"");
    json += "{\"type\":\"" + String(opQueue[i].type) +
            "\",\"raw\":\"" + escaped + "\"}";
  }
  json += "]";
  if (dataMutex) xSemaphoreGive(dataMutex);
  sendJSON(req, 200, json);
}

// ── HTML pages ────────────────────────────────────────────────────────────────

static String SETUP_PAGE = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SmartHome Gateway Setup</title>
<style>
body{margin:0;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0d1117;color:#c9d1d9;display:flex;justify-content:center;align-items:center;min-height:100vh}
.card{background:#161b22;border:1px solid #30363d;border-radius:12px;padding:2.5rem;width:100%;max-width:400px;margin:1rem}
h1{color:#58a6ff;font-size:1.5rem;margin:0 0 0.25rem}
p{color:#8b949e;font-size:0.9rem;margin:0 0 1.5rem}
label{display:block;font-size:0.85rem;font-weight:600;margin:0 0 0.25rem;color:#c9d1d9}
input{width:100%;padding:0.6rem 0.75rem;border:1px solid #30363d;border-radius:8px;background:#0d1117;color:#c9d1d9;font-size:0.9rem;box-sizing:border-box;margin-bottom:1rem}
input:focus{outline:none;border-color:#58a6ff}
button{width:100%;padding:0.7rem;background:#238636;color:#fff;border:none;border-radius:8px;font-size:1rem;font-weight:600;cursor:pointer}
button:hover{background:#2ea043}
.pw-wrap{position:relative}
.pw-wrap input{padding-right:3.2rem;margin-bottom:0}
.pw-toggle{position:absolute;right:6px;top:50%;transform:translateY(-50%);background:none;border:none;color:#8b949e;cursor:pointer;font-size:.75rem;padding:4px 6px;width:auto;line-height:1;font-family:inherit;font-weight:500}
.pw-toggle:hover{color:#c9d1d9}
</style>
</head>
<body>
<div class="card">
<h1>SmartHome Gateway</h1>
<p>Connect this gateway to your WiFi network.</p>
<form method="POST" action="/wifi/connect" onsubmit="btn.disabled=true;btn.textContent='Connecting…'">
<label for="ssid">WiFi Name (SSID)</label>
<input id="ssid" name="ssid" required placeholder="Network name">
<label for="password">Password</label>
<div class="pw-wrap">
<input id="wifi-pass" name="password" type="password" placeholder="Network password">
<button type="button" class="pw-toggle" onclick="togglePw('wifi-pass',this)">Show</button>
</div>
<button type="submit" id="btn" style="margin-top:1rem">Connect</button>
</form>
</div>
<script>function togglePw(id,btn){const i=document.getElementById(id);if(i.type==='password'){i.type='text';btn.textContent='Hide'}else{i.type='password';btn.textContent='Show'}}</script>
</body>
</html>
)rawliteral";

static String LINKING_HTML = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SmartHome — Link Gateway</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0d1117;color:#c9d1d9;display:flex;justify-content:center;align-items:center;padding:1.5rem;min-height:100vh;-webkit-font-smoothing:antialiased}
.card{background:#161b22;border:1px solid #30363d;border-radius:12px;padding:2rem;width:100%;max-width:400px;box-shadow:0 1px 3px rgba(0,0,0,.3)}
h1{color:#58a6ff;font-size:1.3rem;font-weight:700;margin-bottom:.25rem}
p{color:#8b949e;font-size:.88rem;margin-bottom:1.25rem;line-height:1.5}
label{display:block;font-size:.82rem;font-weight:600;margin-bottom:.25rem;color:#c9d1d9}
input{width:100%;padding:.6rem .75rem;border:1px solid #30363d;border-radius:8px;background:#0d1117;color:#c9d1d9;font-size:.9rem;margin-bottom:.85rem;transition:border-color .2s;box-sizing:border-box}
input:focus{outline:none;border-color:#58a6ff;box-shadow:0 0 0 3px rgba(88,166,255,.15)}
button{width:100%;padding:.65rem;border:none;border-radius:8px;font-size:.9rem;font-weight:600;cursor:pointer;transition:background .2s;font-family:inherit}
.btn-primary{background:#238636;color:#fff;margin-top:1rem}
.btn-primary:hover{background:#2ea043}
.btn-primary:disabled{opacity:.6;cursor:default}
.btn-secondary{background:transparent;border:1px solid #30363d;color:#c9d1d9;margin-top:.5rem}
.btn-secondary:hover{background:#1c2128}
.error{color:#f85149;font-size:.82rem;margin-bottom:.5rem}
.pw-wrap{position:relative}
.pw-wrap input{padding-right:3.2rem;margin-bottom:0}
.pw-toggle{position:absolute;right:6px;top:50%;transform:translateY(-50%);background:none;border:none;color:#8b949e;cursor:pointer;font-size:.75rem;padding:4px 6px;width:auto;line-height:1;font-family:inherit;font-weight:500}
.pw-toggle:hover{color:#c9d1d9}
.gw-list{display:flex;flex-direction:column;gap:.5rem}
.gw-opt{display:flex;justify-content:space-between;align-items:center;padding:.65rem .85rem;border:1px solid #30363d;border-radius:8px;background:#0d1117;cursor:pointer;font-size:.85rem;text-align:left;width:100%;transition:border-color .2s}
.gw-opt:hover{border-color:#58a6ff;background:#1c2128}
.gw-opt strong{color:#58a6ff}
.gw-opt code{font-size:.7rem;color:#8b949e;background:#161b22;padding:.1rem .35rem;border-radius:4px}
.empty{text-align:center;padding:1rem 0}
.empty p{color:#8b949e;margin-bottom:.5rem}
.done-icon{font-size:2rem;text-align:center;margin-bottom:.5rem;color:#3fb950}
.key-box{background:#0d1117;border:1px solid #30363d;border-radius:6px;padding:.6rem;font-family:monospace;font-size:.78rem;word-break:break-all;text-align:center;margin-bottom:0}
.spinner{border:3px solid #30363d;border-top:3px solid #58a6ff;border-radius:50%;width:28px;height:28px;animation:spin .8s linear infinite;margin:1rem auto}
@keyframes spin{to{transform:rotate(360deg)}}
.hidden{display:none}
.form-group{margin-bottom:.85rem}
</style>
</head>
<body>
<div class="card" id="app">
  <div id="step-login">
    <h1>Link Gateway</h1>
    <p>Sign in to associate this gateway with your account.</p>
    <form id="login-form">
      <label for="email">Email</label>
      <input id="email" type="email" required placeholder="you@example.com">
      <label for="password">Password</label>
      <div class="pw-wrap">
        <input id="password" type="password" required placeholder="Password">
        <button type="button" class="pw-toggle" onclick="togglePw('password',this)">Show</button>
      </div>
      <p class="error hidden" id="login-err"></p>
      <button type="submit" class="btn-primary" id="login-btn">Sign In</button>
    </form>
  </div>

  <div id="step-select" class="hidden">
    <h1>Select Gateway</h1>
    <p>Choose which gateway to link to this device.</p>
    <div id="gw-list" class="gw-list"></div>
    <div id="gw-empty" class="empty hidden">
      <p>No gateways found on your account.</p>
      <p>Go to the dashboard at <strong>http://192.168.100.100:9000/gateways/create</strong> and create one first.</p>
      <button class="btn-secondary" onclick="fetchGateways()">Refresh</button>
    </div>
  </div>

  <div id="step-saving" class="hidden">
    <h1>Saving…</h1>
    <p>Applying the API key to the gateway.</p>
    <div class="spinner"></div>
  </div>

  <div id="step-done" class="hidden">
    <div class="done-icon">&#10003;</div>
    <h1>Gateway Linked</h1>
    <p>The API key has been saved. The gateway will reboot and connect securely.</p>
    <div class="key-box" id="key-display"></div>
  </div>

  <div id="step-error" class="hidden">
    <h1>Linking Failed</h1>
    <p id="err-msg"></p>
    <button class="btn-primary" onclick="showStep('login')">Try Again</button>
  </div>
</div>

<script>
let TOKEN = '';
function $(id){return document.getElementById(id)}
function togglePw(id,btn){const i=$(id);if(i.type==='password'){i.type='text';btn.textContent='Hide'}else{i.type='password';btn.textContent='Show'}}
function showStep(s){['login','select','saving','done','error'].forEach(id=>$('step-'+id).classList.toggle('hidden',id!==s))}
async function api(method,path,body){
  const opts={method,headers:{'Content-Type':'application/json'}}
  if(TOKEN)opts.headers.Authorization='Bearer '+TOKEN
  if(body)opts.body=JSON.stringify(body)
  const res=await fetch(path,opts)
  const data=await res.json()
  if(!res.ok)throw new Error(data.error||'Request failed')
  return data
}
document.getElementById('login-form').onsubmit=async function(e){
  e.preventDefault()
  const btn=$('login-btn'),err=$('login-err')
  btn.disabled=true;btn.textContent='Signing in…';err.classList.add('hidden')
  try{
    const data=await api('POST','/auth/login',{email:$('email').value,password:$('password').value})
    TOKEN=data.token
    fetchGateways()
  }catch(x){
    err.textContent=x.message;err.classList.remove('hidden');btn.disabled=false;btn.textContent='Sign In'
  }
}
async function fetchGateways(){
  $('login-btn').disabled=true;$('login-btn').textContent='Loading…'
  try{
    const gws=await api('GET','/gateways')
    const list=$('gw-list'),empty=$('gw-empty')
    list.innerHTML='';empty.classList.add('hidden')
    if(gws.length===0){
      empty.classList.remove('hidden');$('login-btn').disabled=false;$('login-btn').textContent='Sign In';showStep('select');return
    }
    gws.forEach(g=>{
      const b=document.createElement('button')
      b.className='gw-opt'
      b.innerHTML='<strong>'+g.name+'</strong> <code>'+g.id+'</code>'
      b.onclick=()=>selectGateway(g.id)
      list.appendChild(b)
    })
    showStep('select')
  }catch(x){
    $('login-err').textContent=x.message;$('login-err').classList.remove('hidden')
  }
  $('login-btn').disabled=false;$('login-btn').textContent='Sign In'
}
async function selectGateway(gid){
  showStep('saving')
  try{
    const data=await api('GET','/gateway/api-key?id='+gid)
    const res=await fetch('/gateway/configure',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({gatewayId:gid,apiKey:data.apiKey})})
    if(!res.ok)throw new Error('Gateway rejected the configuration')
    $('key-display').textContent=data.apiKey
    showStep('done')
  }catch(x){
    $('err-msg').textContent=x.message
    showStep('error')
  }
}
</script>
</body>
</html>
)rawliteral";

static String CONNECTING_PAGE = R"rawliteral(
<!DOCTYPE html>
<html><head><meta charset='UTF-8'>
<meta http-equiv='refresh' content='2;url=/status'>
<title>Connecting…</title>
<style>
body{background:#0d1117;color:#c9d1d9;font-family:sans-serif;display:flex;justify-content:center;align-items:center;min-height:100vh;text-align:center}
.spinner{border:3px solid #30363d;border-top:3px solid #58a6ff;border-radius:50%;width:40px;height:40px;animation:spin 1s linear infinite;margin:0 auto 1rem}
@keyframes spin{to{transform:rotate(360deg)}}
</style></head><body><div><div class='spinner'></div>
<h2>Connecting to WiFi…</h2></div></body></html>
)rawliteral";

// ── Handler: Root ─────────────────────────────────────────────────────────────
void handleRoot(AsyncWebServerRequest* req) {
  if (currentState == STATE_AP_SETUP) {
    AsyncWebServerResponse* resp = req->beginResponse(200, "text/html", SETUP_PAGE);
    addCORS(resp);
    req->send(resp);
  } else if (currentState == STATE_LINKING) {
    AsyncWebServerResponse* resp = req->beginResponse(200, "text/html", LINKING_HTML);
    addCORS(resp);
    req->send(resp);
  } else {
    // MESH — redirect to hub
    req->redirect("http://" + String(HUB_ADDR) + ":" + String(HUB_WEB_PORT));
  }
}

// ── Handler: WiFi config (AP_SETUP mode) ──────────────────────────────────────
void handleWiFiConfig(AsyncWebServerRequest* req) {
  if (!req->hasParam("ssid", true)) {
    sendError(req, 400, "Missing SSID");
    return;
  }

  wifiSSID = req->getParam("ssid", true)->value();
  wifiPass = req->getParam("password", true)->value();

  AsyncWebServerResponse* resp = req->beginResponse(200, "text/html", CONNECTING_PAGE);
  addCORS(resp);
  req->send(resp);

  // Save to multi-SSID credential list
  saveWifiCredential(wifiSSID, wifiPass);

  prefs.putString(NVS_KEY_SSID, wifiSSID);
  prefs.putString(NVS_KEY_PASS, wifiPass);

  currentState = STATE_CONNECTING;
  stateTimer = millis();
  connectToWiFi(wifiSSID.c_str(), wifiPass.c_str());
}

// ── Handler: Configure (LINKING mode) ────────────────────────────────────────
static void doApiConfigure(AsyncWebServerRequest* req, const String& body);

static String configureBody;
static bool configureBodyCollecting = false;

static void handleConfigureBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
  if (!configureBodyCollecting) return;
  Serial.printf("CONF: configure body chunk index=%u len=%u total=%u\n", index, len, total);
  configureBody.concat((char*)data, len);
  if (configureBody.length() >= total) {
    configureBodyCollecting = false;
    Serial.printf("CONF: configure body complete (%u bytes), calling doApiConfigure\n", configureBody.length());
    doApiConfigure(req, configureBody);
  }
}

static void doApiConfigure(AsyncWebServerRequest* req, const String& body) {
  Serial.printf("CONF: doApiConfigure called, body=%s\n", body.c_str());
  if (body.length() == 0) {
    Serial.println("CONF: body empty, returning 400");
    sendError(req, 400, "Missing body");
    return;
  }

  // Extract apiKey from JSON
  int akPos = body.indexOf("\"apiKey\"");
  if (akPos < 0) {
    Serial.println("CONF: apiKey not found in body, returning 400");
    sendError(req, 400, "Missing apiKey");
    return;
  }
  int colon = body.indexOf(':', akPos);
  int qs = body.indexOf('"', colon + 1);
  int qe = body.indexOf('"', qs + 1);
  if (qs < 0 || qe <= qs) {
    Serial.println("CONF: bad JSON format, returning 400");
    sendError(req, 400, "Bad JSON");
    return;
  }
  String newKey = body.substring(qs + 1, qe);

  if (newKey.length() == 0) {
    Serial.println("CONF: empty apiKey, returning 400");
    sendError(req, 400, "Missing apiKey");
    return;
  }

  Serial.printf("CONF: saving apiKey (len=%u), will reboot\n", newKey.length());
  prefs.putString(NVS_KEY_APIKEY, newKey);
  apiKey = newKey;

  String json = "{\"status\":\"configured\",\"message\":\"API key saved. Gateway will reboot.\"}";
  sendJSON(req, 200, json);

  pendingReboot = true;
}

void handleApiConfigure(AsyncWebServerRequest* req) {
  Serial.printf("CONF: POST /gateway/configure (content-length=%u)\n", req->contentLength());
  configureBodyCollecting = true;
  configureBody = "";
  // Response will be sent from handleConfigureBody once body is complete
}

// ── Handler: Gen204 / hotspot detection ──────────────────────────────────────
void handleRedirect(AsyncWebServerRequest* req) {
  req->redirect("http://" + WiFi.softAPIP().toString() + "/");
}

// ── API proxy helpers ─────────────────────────────────────────────────────────
static void doApiProxy(AsyncWebServerRequest* req, const String& path, const String& body, const String& auth);

static String proxyBody;
static bool proxyBodyCollecting = false;

static void handleProxyBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
  if (!proxyBodyCollecting) return;
  Serial.printf("LINK: proxy body chunk index=%u len=%u total=%u\n", index, len, total);
  proxyBody.concat((char*)data, len);
  if (proxyBody.length() >= total) {
    proxyBodyCollecting = false;
    Serial.printf("LINK: proxy body complete (%u bytes) — forwarding to hub\n", proxyBody.length());
    String auth = req->header("Authorization");
    doApiProxy(req, "/api/auth/login", proxyBody, auth);
  }
}

static void doApiProxy(AsyncWebServerRequest* req, const String& path, const String& body, const String& auth) {
  String methodName = (req->method() == HTTP_POST) ? "POST" : "GET";
  Serial.printf("LINK: doApiProxy %s %s (body=%u bytes)\n", methodName.c_str(), path.c_str(), body.length());

  WiFiClient client;
  if (!client.connect(HUB_ADDR, HUB_WEB_PORT)) {
    Serial.printf("LINK: FAILED to connect to hub %s:%u\n", HUB_ADDR, HUB_WEB_PORT);
    sendError(req, 502, "Hub unreachable");
    return;
  }
  Serial.printf("LINK: connected to hub %s:%u\n", HUB_ADDR, HUB_WEB_PORT);

  client.setTimeout(5000);

  client.println(methodName + " " + path + " HTTP/1.1");
  client.println("Host: " + String(HUB_ADDR) + ":" + String(HUB_WEB_PORT));
  client.println("Content-Type: application/json");
  if (auth.length() > 0) {
    client.println("Authorization: " + auth);
  }
  if (body.length() > 0) {
    client.print("Content-Length: ");
    client.println(body.length());
  }
  client.println("Connection: close");
  client.println();
  if (body.length() > 0) {
    client.print(body);
  }
  Serial.printf("LINK: request sent to hub, awaiting response\n");

  String response;
  unsigned long t = millis() + 5000;
  while (millis() < t) {
    if (client.available()) {
      response += (char)client.read();
      t = millis() + 200;
    }
  }
  client.stop();

  Serial.printf("LINK: hub response (%u bytes): %s\n", response.length(),
                response.substring(0, response.indexOf("\r\n")).c_str());

  int statusCode = 500;
  int nl = response.indexOf("\r\n");
  if (nl > 0) {
    String statusLine = response.substring(0, nl);
    int a = statusLine.indexOf(' ') + 1;
    int b = statusLine.indexOf(' ', a);
    if (a > 0 && b > a) statusCode = statusLine.substring(a, b).toInt();
  }

  int hEnd = response.indexOf("\r\n\r\n");
  String resBody = hEnd > 0 ? response.substring(hEnd + 4) : "";

  AsyncWebServerResponse* resp = req->beginResponse(statusCode, "application/json", resBody);
  addCORS(resp);
  req->send(resp);
}

// ── Proxied API handlers ──────────────────────────────────────────────────────
// POST /auth/login — proxy login to hub (body collected via callback)
static void handleProxyLogin(AsyncWebServerRequest* req) {
  Serial.println("LINK: POST /auth/login — collecting body");
  proxyBodyCollecting = true;
  proxyBody = "";
  // Response will be sent from handleProxyBody once body is complete
}

// GET /gateways — proxy gateway list to hub (no body needed)
static void handleProxyGateways(AsyncWebServerRequest* req) {
  Serial.println("LINK: GET /gateways — proxying to hub");
  String auth = req->header("Authorization");
  doApiProxy(req, "/api/gateways", "", auth);
}

// GET /gateway/api-key?id=xxx — proxy api-key fetch to hub (no path params to avoid regex issues)
static void handleProxyGatewayApiKey(AsyncWebServerRequest* req) {
  if (!req->hasParam("id")) {
    sendError(req, 400, "Missing id parameter");
    return;
  }
  String gid = req->getParam("id")->value();
  Serial.printf("LINK: GET /gateway/api-key?id=%s — proxying to hub\n", gid.c_str());
  String path = "/api/gateways/" + gid + "/api-key";
  String auth = req->header("Authorization");
  doApiProxy(req, path, "", auth);
}

// ── Handler: POST /api/wifi/configure (save WiFi credentials) ────────────────
static void handleApiWifiConfigure(AsyncWebServerRequest* req) {
  if (req->contentLength() == 0) {
    sendError(req, 400, "Missing body");
    return;
  }

  String body = req->arg("plain");

  int sPos = body.indexOf("\"ssid\"");
  if (sPos < 0) {
    sendError(req, 400, "Missing ssid");
    return;
  }
  int colon = body.indexOf(':', sPos);
  int qs = body.indexOf('"', colon + 1);
  int qe = body.indexOf('"', qs + 1);
  if (qs < 0 || qe <= qs) {
    sendError(req, 400, "Bad JSON ssid");
    return;
  }
  String ssid = body.substring(qs + 1, qe);

  String password;
  int pPos = body.indexOf("\"password\"");
  if (pPos >= 0) {
    colon = body.indexOf(':', pPos);
    qs = body.indexOf('"', colon + 1);
    qe = body.indexOf('"', qs + 1);
    if (qs >= 0 && qe > qs) {
      password = body.substring(qs + 1, qe);
    }
  }

  Serial.printf("WiFi configure via API: SSID=%s\n", ssid.c_str());

  // Save to credential list
  saveWifiCredential(ssid, password);

  // Also save as active for backward compat
  prefs.putString(NVS_KEY_SSID, ssid);
  prefs.putString(NVS_KEY_PASS, password);

  wifiSSID = ssid;
  wifiPass = password;

  sendJSON(req, 200, "{\"status\":\"saved\",\"ssid\":\"" + ssid + "\"}");

  // Connect immediately
  connectToWiFi(wifiSSID.c_str(), wifiPass.c_str());
}

// ── Handler: GET /api/wifi/networks (scan available networks) ─────────────────
static void handleApiWifiNetworks(AsyncWebServerRequest* req) {
  int networks = WiFi.scanComplete();
  if (networks == WIFI_SCAN_FAILED || networks == WIFI_SCAN_RUNNING) {
    // Start a new scan
    WiFi.scanNetworks(true);
    sendJSON(req, 200, "{\"status\":\"scanning\"}");
    return;
  }

  // Build known SSID set for isKnown check
  String knownSSIDs[MAX_SAVED_WIFI];
  int savedCount = getSavedWifiCount();
  for (int i = 0; i < savedCount; i++) {
    String pass;
    getSavedWifi(i, knownSSIDs[i], pass);
  }

  String json = "[";
  for (int i = 0; i < networks; i++) {
    if (i > 0) json += ',';
    String ssid = WiFi.SSID(i);
    bool isKnown = false;
    for (int k = 0; k < savedCount; k++) {
      if (ssid == knownSSIDs[k]) { isKnown = true; break; }
    }
    json += "{\"ssid\":\"" + ssid +
            "\",\"rssi\":" + String(WiFi.RSSI(i)) +
            ",\"encrypted\":" + (WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") +
            ",\"known\":" + (isKnown ? "true" : "false") +
            "}";
  }
  json += "]";

  WiFi.scanDelete();
  sendJSON(req, 200, json);
}

// ── Handler: GET /api/wifi/credentials (list saved credentials, passwords masked) ──
static void handleApiWifiCredentials(AsyncWebServerRequest* req) {
  int count = getSavedWifiCount();
  String json = "[";
  for (int i = 0; i < count; i++) {
    if (i > 0) json += ',';
    String ssid, pass;
    getSavedWifi(i, ssid, pass);
    String masked = pass.length() > 0 ? String(pass[0]) + "…" + String(pass[pass.length() > 1 ? pass.length() - 1 : 0]) : "";
    json += "{\"index\":" + String(i) +
            ",\"ssid\":\"" + ssid +
            "\",\"password\":\"" + masked +
            "\"}";
  }
  json += "]";
  sendJSON(req, 200, json);
}

// ── Handler: Not found (captive portal + fallback) ───────────────────────────
void handleNotFound(AsyncWebServerRequest* req) {
  // For OPTIONS (CORS preflight), respond OK
  if (req->method() == HTTP_OPTIONS) {
    AsyncWebServerResponse* resp = req->beginResponse(200);
    addCORS(resp);
    req->send(resp);
    return;
  }

  // Captive portal redirect in AP/LINKING modes
  if (currentState == STATE_AP_SETUP || currentState == STATE_LINKING) {
    req->redirect("http://" + WiFi.softAPIP().toString() + "/");
    return;
  }

  sendError(req, 404, "Not found");
}

// ── Initialization ───────────────────────────────────────────────────────────
void initWebServer() {
  // REST API — local routes (no path params to avoid ESP32 regex issues; parsed from URL manually)
  asyncServer.on("/api/nodes", HTTP_GET, handleApiNodesList);
  asyncServer.on("/api/nodes/*", HTTP_GET, handleApiNodeGet);
  asyncServer.on("/api/nodes/*", HTTP_POST, handleApiNodeCommand);
  asyncServer.on("/api/status", HTTP_GET, handleApiStatus);
  asyncServer.on("/api/provision", HTTP_POST, handleApiProvision);
  asyncServer.on("/api/disconnect/*", HTTP_POST, handleApiDisconnect);
  asyncServer.on("/api/queue", HTTP_GET, handleApiQueue);
  asyncServer.on("/api/wifi/configure", HTTP_POST, handleApiWifiConfigure);
  asyncServer.on("/api/wifi/networks", HTTP_GET, handleApiWifiNetworks);
  asyncServer.on("/api/wifi/credentials", HTTP_GET, handleApiWifiCredentials);

  // Proxy routes for linking page (body handlers for POST data)
  asyncServer.on("/auth/login", HTTP_POST, handleProxyLogin, nullptr, handleProxyBody);
  asyncServer.on("/gateways", HTTP_GET, handleProxyGateways);
  asyncServer.on("/gateway/api-key", HTTP_GET, handleProxyGatewayApiKey);

  // Captive portal / linking / setup pages
  asyncServer.on("/", HTTP_GET, handleRoot);
  asyncServer.on("/gateway/link", HTTP_GET, handleRoot);
  asyncServer.on("/gateway/configure", HTTP_POST, handleApiConfigure, nullptr, handleConfigureBody);
  asyncServer.on("/wifi/connect", HTTP_POST, handleWiFiConfig);
  asyncServer.on("/generate_204", HTTP_GET, handleRedirect);
  asyncServer.on("/hotspot-detect.html", HTTP_GET, handleRedirect);

  // Catch-all
  asyncServer.onNotFound(handleNotFound);

  asyncServer.begin();
  Serial.println("AsyncWebServer started on port " + String(LOCAL_API_PORT));
}
