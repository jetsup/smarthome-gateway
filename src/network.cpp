#include "Gateway.h"

static const byte DNS_PORT = 53;

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
