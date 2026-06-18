#include "Gateway.h"

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
