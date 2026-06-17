#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_now.h>

#include "Config.hpp"

// ── Packet ───────────────────────────────────────────────────────────────────
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
  uint8_t checksum;
};

uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── FSM ──────────────────────────────────────────────────────────────────────
enum State {
  STATE_AP_SETUP,    // Captive portal — no config at all
  STATE_CONNECTING,  // Joining WiFi after getting creds
  STATE_LINKING,     // WiFi OK, no API key → serve linking portal + proxy
  STATE_MESH         // Fully configured — ESP-NOW + TCP
};
State currentState = STATE_AP_SETUP;
unsigned long stateTimer = 0;

// ── NVS ──────────────────────────────────────────────────────────────────────
Preferences prefs;

// ── Servers ──────────────────────────────────────────────────────────────────
WebServer httpServer(80);
DNSServer dnsServer;
const byte DNS_PORT = 53;

// ── Credentials ──────────────────────────────────────────────────────────────
String wifiSSID = "";
String wifiPass = "";

// ── Hub config (from linking flow) ───────────────────────────────────────────
String apiKey = "";

// ── TCP (mesh mode) ──────────────────────────────────────────────────────────
WiFiClient tcpClient;
unsigned long lastTCPReconnect = 0;
bool pendingReboot = false;

// ── ESP-NOW Ring Buffer (variable-length)
// ─────────────────────────────────────
#define UPLINK_QUEUE_SIZE 32
#define MAX_MSG_SIZE 64

struct UplinkMessage {
  uint8_t data[MAX_MSG_SIZE];
  int len;
};

UplinkMessage uplinkQueue[UPLINK_QUEUE_SIZE];
volatile int uplinkHead = 0;
volatile int uplinkTail = 0;

// ── Scan state
// ─────────────────────────────────────────────────────────────────
bool scanning = false;
unsigned long scanStartTime = 0;

// ── Prototypes ───────────────────────────────────────────────────────────────
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

// ── Factory Reset via GPIO
// ─────────────────────────────────────────────────────
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

// ══════════════════════════════════════════════════════════════════════════════
// SETUP
// ══════════════════════════════════════════════════════════════════════════════
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

// ══════════════════════════════════════════════════════════════════════════════
// LOOP
// ══════════════════════════════════════════════════════════════════════════════
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
          // Have WiFi + API key → mesh mode
          apiKey = key;
          initESPNOW();
          currentState = STATE_MESH;
          tcpConnect();
        } else {
          // WiFi OK, no API key → enter linking mode
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
      // Scan timeout
      if (scanning && millis() - scanStartTime > SCAN_TIMEOUT_MS) {
        scanning = false;
        Serial.println("Scan timed out");
      }
      break;
  }
}

// ══════════════════════════════════════════════════════════════════════════════
// CAPTIVE PORTAL — AP Mode (first boot, no WiFi creds)
// ══════════════════════════════════════════════════════════════════════════════
void startAPMode() {
  currentState = STATE_AP_SETUP;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID_SETUP);
  delay(200);

  Serial.print("AP started: ");
  Serial.println(AP_SSID_SETUP);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  httpServer.on("/", handleCaptivePortal);
  httpServer.on("/wifi/connect", HTTP_POST, handleWiFiConfig);
  httpServer.onNotFound(handleCaptivePortal);
  httpServer.begin();
}

void handleCaptivePortal() {
  if (currentState == STATE_LINKING) {
    handleLinkingPage();
    return;
  }
  String html = R"rawliteral(
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
  httpServer.send(200, "text/html", html);
}

void handleWiFiConfig() {
  if (!httpServer.hasArg("ssid")) {
    httpServer.send(400, "text/plain", "Missing SSID");
    return;
  }

  wifiSSID = httpServer.arg("ssid");
  wifiPass = httpServer.arg("password");

  String page = R"rawliteral(
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
  httpServer.send(200, "text/html", page);

  httpServer.close();
  dnsServer.stop();

  prefs.putString(NVS_KEY_SSID, wifiSSID);
  prefs.putString(NVS_KEY_PASS, wifiPass);

  currentState = STATE_CONNECTING;
  stateTimer = millis();
  connectToWiFi(wifiSSID.c_str(), wifiPass.c_str());
}

// ══════════════════════════════════════════════════════════════════════════════
// LINKING MODE — WiFi connected, no API key → AP + embedded linking page + API
// proxy
// ══════════════════════════════════════════════════════════════════════════════
void startLinkingMode() {
  currentState = STATE_LINKING;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID_LINK);
  delay(200);

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  httpServer.stop();
  httpServer.on("/", handleLinkingPage);
  httpServer.on("/gateway/link", handleLinkingPage);
  httpServer.on("/gateway/configure", HTTP_POST, handleConfigure);
  httpServer.on("/generate_204", handleGen204);
  httpServer.on("/hotspot-detect.html", handleGen204);
  httpServer.onNotFound([]() {
    if (httpServer.uri().startsWith("/api/")) {
      handleApiProxy();
    } else {
      httpServer.sendHeader("Location",
                            "http://" + WiFi.softAPIP().toString() + "/");
      httpServer.send(302, "text/html", "");
    }
  });
  httpServer.begin();

  Serial.println("Linking AP started: " + String(AP_SSID_LINK));
  Serial.println("AP IP: " + WiFi.softAPIP().toString());
  Serial.println("STA IP: " + WiFi.localIP().toString());
}

// Captive portal detection → redirect to linking page so the OS opens the
// captive portal browser.
void handleGen204() {
  httpServer.sendHeader("Location",
                        "http://" + WiFi.softAPIP().toString() + "/");
  httpServer.send(302, "text/html", "");
}

void handleApiProxy() {
  String path = httpServer.uri();
  String methodName = (httpServer.method() == HTTP_POST) ? "POST" : "GET";
  String body = httpServer.hasArg("plain") ? httpServer.arg("plain") : "";
  String auth = httpServer.header("Authorization");

  WiFiClient client;
  if (!client.connect(HUB_ADDR, HUB_WEB_PORT)) {
    httpServer.send(502, "application/json", "{\"error\":\"Hub unreachable\"}");
    return;
  }

  client.setTimeout(3000);

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

  String response;
  unsigned long t = millis() + 5000;
  while (millis() < t) {
    if (client.available()) {
      response += (char)client.read();
      t = millis() + 200;
    }
  }
  client.stop();

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

  httpServer.send(statusCode, "application/json", resBody);
}

void handleLinkingPage() {
  String page = R"rawliteral(
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
  const res=await fetch('/api'+path,opts)
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
    const data=await api('GET','/gateways/'+gid+'/api-key')
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
  httpServer.send(200, "text/html", page);
}

void handleConfigure() {
  if (!httpServer.hasArg("plain")) {
    httpServer.send(400, "text/plain", "Missing body");
    return;
  }

  String body = httpServer.arg("plain");

  auto extractStr = [&](const String& key) -> String {
    int pos = body.indexOf("\"" + key + "\"");
    if (pos < 0) return "";
    int start = body.indexOf('"', pos + key.length() + 3);
    if (start < 0) return "";
    start++;
    int end = body.indexOf('"', start);
    if (end < 0) return "";
    return body.substring(start, end);
  };

  String newKey = extractStr("apiKey");

  if (newKey.length() == 0) {
    httpServer.send(400, "text/plain", "Missing apiKey");
    return;
  }

  prefs.putString(NVS_KEY_APIKEY, newKey);
  apiKey = newKey;

  httpServer.send(200, "application/json",
                  "{\"status\":\"configured\",\"message\":\"API key saved. "
                  "Gateway will reboot.\"}");

  pendingReboot = true;
}

// ══════════════════════════════════════════════════════════════════════════════
// WiFi Helpers
// ══════════════════════════════════════════════════════════════════════════════
void connectToWiFi(const char* ssid, const char* pass) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  stateTimer = millis();
}

void attemptReconnect() {
  if (millis() - lastTCPReconnect > 10000) {
    lastTCPReconnect = millis();
    Serial.println("WiFi lost — reconnecting...");
    WiFi.reconnect();
  }
}

// ══════════════════════════════════════════════════════════════════════════════
// ESP-NOW
// ══════════════════════════════════════════════════════════════════════════════
void onESPNOWRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (len < 1 || len > MAX_MSG_SIZE) return;
  int next = (uplinkHead + 1) % UPLINK_QUEUE_SIZE;
  if (next == uplinkTail) return;
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

    // Forward discovery packets (msgType=3) even to the hub for mesh relay
    if (msg.len >= 2 && msg.data[1] == MSG_DISCOVERY) {
      // Always forward discovery to the hub
      tcpClient.write(msg.data, msg.len);

      // If we're in scan mode, also rebroadcast to extend range
      if (scanning) {
        esp_now_send(broadcastMac, msg.data, msg.len);
      }
    } else if (msg.len >= 2 && msg.data[1] == MSG_SCAN_REQ) {
      // Re-broadcast scan requests to extend range (mesh forwarding)
      esp_now_send(broadcastMac, msg.data, msg.len);
    } else {
      // Normal telemetry or other messages → forward to hub
      tcpClient.write(msg.data, msg.len);
    }

    uplinkTail = (uplinkTail + 1) % UPLINK_QUEUE_SIZE;
  }
}

void handleDownlink() {
  if (!tcpClient.available()) return;
  int avail = tcpClient.available();

  // Peek at the first byte to determine message type
  int lead = tcpClient.peek();
  if (lead < 0) return;

  if (lead == 0xAA) {
    // Standard 9-byte ESP-NOW downlink packet
    if (avail < (int)sizeof(ESPNowMessage)) return;

    ESPNowMessage msg;
    tcpClient.readBytes((uint8_t*)&msg, sizeof(ESPNowMessage));

    if (msg.header != 0xAA) return;

    // Validate checksum
    uint8_t calc = 0;
    for (int i = 0; i < 8; i++) calc ^= ((uint8_t*)&msg)[i];
    if (calc != msg.checksum) return;

    switch (msg.msgType) {
      case MSG_SCAN_REQ:
        // Enter scan mode
        scanning = true;
        scanStartTime = millis();
        Serial.println("Scan started by hub command");
        // Re-broadcast scan request via ESP-NOW
        esp_now_send(broadcastMac, (uint8_t*)&msg, sizeof(ESPNowMessage));
        break;

      case MSG_PROVISION: {
        // Provision command — parse deviceId, send follow-up with API key
        // The hub sends the full API key via BB: command, not via 9-byte packet
        // So we just re-broadcast 9-byte provision signal
        esp_now_send(broadcastMac, (uint8_t*)&msg, sizeof(ESPNowMessage));
        break;
      }

      default:
        // Regular command → re-broadcast
        esp_now_send(broadcastMac, (uint8_t*)&msg, sizeof(ESPNowMessage));
        break;
    }
  } else if (lead == 0xBB) {
    // Variable-length BB: command — read until newline
    String cmd = tcpClient.readStringUntil('\n');
    handleBBCommand(cmd);
  }
}

void handleBBCommand(const String& cmd) {
  // Format: BB:<deviceId>:<apiKey>
  int firstColon = cmd.indexOf(':');
  if (firstColon < 0) return;
  int secondColon = cmd.indexOf(':', firstColon + 1);
  if (secondColon < 0) return;

  String devIdStr = cmd.substring(firstColon + 1, secondColon);
  String apiKey = cmd.substring(secondColon + 1);

  uint32_t deviceId = (uint32_t)devIdStr.toInt();

  Serial.printf("Provisioning node %u with API key\n", deviceId);

  // Build and send ESP-NOW provision message with full API key
  ESPNowProvisionMessage provMsg;
  provMsg.header = 0xAA;
  provMsg.msgType = MSG_PROVISION;
  provMsg.deviceId = deviceId;
  memset(provMsg.apiKey, 0, sizeof(provMsg.apiKey));
  apiKey.toCharArray(provMsg.apiKey, sizeof(provMsg.apiKey) - 1);

  // Compute checksum
  uint8_t calc = 0;
  for (int i = 0; i < (int)sizeof(ESPNowProvisionMessage) - 1; i++) {
    calc ^= ((uint8_t*)&provMsg)[i];
  }
  provMsg.checksum = calc;

  esp_now_send(broadcastMac, (uint8_t*)&provMsg,
               sizeof(ESPNowProvisionMessage));
  Serial.println("Provision message sent via ESP-NOW");
}

// ══════════════════════════════════════════════════════════════════════════════
// TCP
// ══════════════════════════════════════════════════════════════════════════════
bool tcpConnect() {
  if (tcpClient.connect(HUB_ADDR, HUB_TCP_PORT)) {
    Serial.println("TCP connected to " + String(HUB_ADDR) + ":" + HUB_TCP_PORT);
    tcpClient.write(apiKey.c_str(), apiKey.length());
    tcpClient.write('\n');
    return true;
  }
  Serial.println("TCP connect failed");
  return false;
}

void tcpLoop() {
  if (!tcpClient.connected()) {
    if (millis() - lastTCPReconnect > TCP_RECONNECT_MS) {
      lastTCPReconnect = millis();
      tcpConnect();
    }
  }
}
