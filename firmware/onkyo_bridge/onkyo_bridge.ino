/*
 * Onkyo TX-SR 806 — ESP32 RS232/eISCP Bridge
 *
 * Hardware:
 *   ESP32 UART2 (RX=GPIO16, TX=GPIO17) → MAX3232 → RS232 DB9 → Onkyo TX-SR 806
 *   RS232: 9600 baud, 8N1
 *
 * Features:
 *   - AP-Mode bei erstem Boot → WebGUI für WiFi-Setup (192.168.4.1)
 *   - eISCP Bridge auf TCP-Port 60128 (OpenHAB Onkyo Binding)
 *   - Kontroll-WebGUI: Power On/Off + Lautstärke in dB
 *   - Credentials persistent im Flash (Preferences)
 *   - mDNS: http://onkyo-bridge.local
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <sys/socket.h>
#include <netinet/tcp.h>

// ── Hardware-Pins ──────────────────────────────────────────────────
#define RS232_RX_PIN   16
#define RS232_TX_PIN   17
#define RS232_BAUD     9600

// ── Netzwerk-Ports ─────────────────────────────────────────────────
#define EISCP_PORT     60128
#define WEB_PORT       80

// ── Volume-Mapping TX-SR 806 ───────────────────────────────────────
// Hex 0x00 = Minimum, 0x50 (80) = 0 dB Referenz
// dB = hexVal - 80  →  Bereich: -80 dB bis 0 dB
#define VOL_MAX_HEX    80
#define VOL_DB_OFFSET  80

// ── Globals ────────────────────────────────────────────────────────
Preferences prefs;
WebServer   webServer(WEB_PORT);
WiFiServer  eiscpServer(EISCP_PORT);
WiFiClient  eiscpClient;

bool   apMode        = false;
String currentPower  = "UNKNOWN";
int    currentVol    = 40;   // AVR-Wert 0–80, wie auf Display
String currentInput  = "23"; // hex, default CD

unsigned long lastPollMs = 0;
int           pollState  = 0;   // 0=PWR, 1=MVL, 2=SLI


// ── eISCP Protokoll ────────────────────────────────────────────────
// Header: "ISCP" | hdrSize(4B BE) | dataSize(4B BE) | version(1B) | 0x000000
// Data  : "!1CMD...\r\n\x1a"
//
// WICHTIG: buildEiscp() als String ist KAPUTT — buf[13]=0x00 terminiert
// den String-Konstruktor vorzeitig → nur 13 Bytes werden gesendet.
// Stattdessen: sendEiscpToClient() schreibt Header + Daten separat.

void sendEiscpToClient(WiFiClient& client, const String& iscpMsg) {
  // iscpMsg enthält bereits "\r" am Ende (z.B. "!1PWR01\r")
  // eISCP-Standard-Terminator: \r\n\x1a — für maximale Binding-Kompatibilität
  String msg = iscpMsg;
  if (!msg.endsWith("\r")) msg += "\r";
  msg += "\n\x1a";
  uint32_t dataSize = msg.length();
  uint8_t hdr[16];
  hdr[0]  = 'I'; hdr[1]  = 'S'; hdr[2]  = 'C'; hdr[3]  = 'P';
  hdr[4]  = 0;   hdr[5]  = 0;   hdr[6]  = 0;   hdr[7]  = 16;
  hdr[8]  = (dataSize >> 24) & 0xFF;
  hdr[9]  = (dataSize >> 16) & 0xFF;
  hdr[10] = (dataSize >>  8) & 0xFF;
  hdr[11] =  dataSize        & 0xFF;
  hdr[12] = 0x01;
  hdr[13] = 0x00; hdr[14] = 0x00; hdr[15] = 0x00;
  client.write(hdr, 16);
  client.write((const uint8_t*)msg.c_str(), dataSize);
}

// Gibt den ISCP-Teil eines eISCP-Pakets zurück ("!1CMD...\r")
String parseEiscp(const uint8_t* buf, int len) {
  if (len < 16) return "";
  if (buf[0]!='I'||buf[1]!='S'||buf[2]!='C'||buf[3]!='P') return "";
  uint32_t dataSize = ((uint32_t)buf[8]<<24)|((uint32_t)buf[9]<<16)|
                      ((uint32_t)buf[10]<<8)| buf[11];
  if (len < 16 + (int)dataSize) return "";
  String iscp = "";
  for (uint32_t i = 0; i < dataSize; i++) {
    char c = (char)buf[16 + i];
    if (c != '\r' && c != '\n' && c != '\x1a') iscp += c;
  }
  return iscp;
}

// ── ISCP RS232 ─────────────────────────────────────────────────────
void sendIscp(const String& cmd) {
  Serial2.print(cmd);
  String dbg = cmd;
  dbg.replace("\r", "<CR>");
  Serial.println("[RS232 →] " + dbg);
}

// Nicht-blockierender Serial2-Reader — einzige Stelle die Serial2 liest.
// Parst vollständige ISCP-Nachrichten, aktualisiert Cache, leitet an eISCP weiter.
static String serial2Buf = "";
static unsigned long serial2LastByte = 0;

void readSerial2() {
  // Buffer nach 2s ohne Terminierung resetten (Garbage-Schutz)
  if (serial2Buf.length() > 0 && millis() - serial2LastByte > 500) {
    Serial.println("[RS232] Buffer-Reset (Timeout): " + serial2Buf);
    serial2Buf = "";
  }
  while (Serial2.available()) {
    char c = Serial2.read();
    serial2LastByte = millis();
    if (c == '\r' || c == '\n') {
      if (serial2Buf.length() >= 3) {
        Serial.println("[RS232 ←] " + serial2Buf);
        updateState(serial2Buf);
        if (eiscpClient && eiscpClient.connected()) {
          sendEiscpToClient(eiscpClient, serial2Buf + "\r");
        }
      }
      serial2Buf = "";
    } else if (c >= 0x20 && c <= 0x7E) {
      serial2Buf += c;
      if (serial2Buf.length() > 32) {
        // Statt komplett löschen: "!1"-Prefix suchen und behalten.
        // Verhindert dass z.B. "!1PW" durch Overflow verloren geht und
        // nur "R01" übrig bleibt (was dann nicht als "!1PWR01" erkannt wird).
        int idx = serial2Buf.lastIndexOf("!1");
        serial2Buf = (idx > 0) ? serial2Buf.substring(idx) : "";
      }
    }
  }
}

// Internen Status aus ISCP-Antwort aktualisieren
void updateState(const String& iscp) {
  if (iscp.indexOf("PWR01") >= 0) currentPower = "ON";
  else if (iscp.indexOf("PWR00") >= 0) currentPower = "OFF";
  else if (iscp.indexOf("PWRSTANDBY") >= 0) currentPower = "OFF";

  int idx = iscp.indexOf("MVL");
  if (idx >= 0 && (int)iscp.length() >= idx + 5) {
    String hexStr = iscp.substring(idx + 3, idx + 5);
    hexStr.trim();
    long v = strtol(hexStr.c_str(), nullptr, 16);
    if (v >= 0 && v <= 100) currentVol = (int)v;
  }

  idx = iscp.indexOf("SLI");
  if (idx >= 0 && (int)iscp.length() >= idx + 5) {
    String hexStr = iscp.substring(idx + 3, idx + 5);
    hexStr.trim();
    hexStr.toUpperCase();
    currentInput = hexStr;
  }
}

// ── WiFi ───────────────────────────────────────────────────────────
void startAP() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Onkyo-Bridge");
  Serial.println("AP: Onkyo-Bridge (offen)  IP: " + WiFi.softAPIP().toString());
}

bool connectWiFi(const String& ssid, const String& pass) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.print("WiFi " + ssid);
  for (int i = 0; i < 24; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println(" → " + WiFi.localIP().toString());
      return true;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println(" FAILED");
  return false;
}

// ── Web: AP-Seite (WiFi Setup) ─────────────────────────────────────
const char AP_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="de"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Onkyo Bridge – WiFi Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#0f0f1a;color:#e0e0e0;
     display:flex;justify-content:center;align-items:flex-start;min-height:100vh;padding:20px}
.card{background:#1a1a2e;border-radius:16px;padding:28px;width:100%;max-width:420px;
      box-shadow:0 8px 32px rgba(0,0,0,.5)}
h1{color:#e94560;font-size:1.4rem;margin-bottom:4px}
.sub{color:#666;font-size:.85rem;margin-bottom:24px}
h3{color:#aaa;font-size:.95rem;font-weight:600;margin-bottom:12px}
.btn{display:block;width:100%;padding:12px;border:none;border-radius:10px;
     font-size:1rem;cursor:pointer;transition:.15s}
.btn-scan{background:#0f3460;color:#ccc}
.btn-scan:hover{background:#1a4a80}
.btn-save{background:#e94560;color:#fff;margin-top:8px}
.btn-save:hover{background:#c73652}
.nets{margin:12px 0}
.net{padding:10px 14px;background:#0f3460;border-radius:8px;margin:4px 0;
     cursor:pointer;display:flex;justify-content:space-between;align-items:center;
     transition:.15s}
.net:hover{background:#1a4a80}
.rssi{font-size:.8rem;color:#888}
input{display:block;width:100%;padding:11px 14px;margin:6px 0;
      background:#0a0a1a;border:1px solid #333;border-radius:8px;color:#fff;font-size:1rem}
input:focus{outline:none;border-color:#e94560}
.msg{margin-top:14px;padding:10px 14px;border-radius:8px;font-size:.9rem;display:none}
.msg.ok{background:#1a3a1a;color:#4CAF50;display:block}
.msg.err{background:#3a1a1a;color:#f44336;display:block}
</style></head><body>
<div class="card">
  <h1>&#x1F4F6; Onkyo Bridge</h1>
  <div class="sub">ESP32 RS232 Bridge · WiFi Setup</div>
  <h3>WLAN-Netzwerk</h3>
  <button class="btn btn-scan" onclick="scan()">&#x1F50D; Netzwerke scannen</button>
  <div class="nets" id="nets"></div>
  <input id="ssid" placeholder="SSID (Netzwerkname)" autocomplete="off">
  <input id="pass" type="password" placeholder="Passwort" autocomplete="off">
  <button class="btn btn-save" onclick="connect()">&#x2705; Verbinden &amp; Speichern</button>
  <div class="msg" id="msg"></div>
</div>
<script>
async function scan(){
  document.getElementById('nets').innerHTML='<div class="net"><span>Suche...</span></div>';
  try{
    const d=await(await fetch('/scan')).json();
    if(!d.length){document.getElementById('nets').innerHTML='<div class="net"><span>Keine Netzwerke gefunden</span></div>';return;}
    document.getElementById('nets').innerHTML=d.map(n=>
      `<div class="net" onclick="document.getElementById('ssid').value='${n.ssid.replace(/'/g,"\\'")}'">`+
      `<span>${n.ssid}</span><span class="rssi">${n.rssi} dBm ${n.enc?'&#x1F512;':''}</span></div>`
    ).join('');
  }catch(e){document.getElementById('nets').innerHTML='<div class="net"><span>Fehler beim Scannen</span></div>';}
}
async function connect(){
  const ssid=document.getElementById('ssid').value.trim();
  const pass=document.getElementById('pass').value;
  const msg=document.getElementById('msg');
  if(!ssid){msg.className='msg err';msg.textContent='Bitte SSID eingeben!';return;}
  msg.className='msg ok';msg.textContent='Verbinde...';
  try{
    const d=await(await fetch('/connect',{method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({ssid,pass})})).json();
    if(d.ok){msg.textContent='&#x2705; Verbunden! ESP32 startet neu — bitte http://'+d.ip+' aufrufen.';}
    else{msg.className='msg err';msg.textContent='&#x274C; Verbindung fehlgeschlagen. Passwort korrekt?';}
  }catch(e){msg.className='msg err';msg.textContent='&#x274C; Fehler: '+e.message;}
}
</script></body></html>
)HTML";

// ── Web: Kontroll-Seite ────────────────────────────────────────────
const char CTRL_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="de"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Onkyo TX-SR 806</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#0f0f1a;color:#e0e0e0;
     display:flex;justify-content:center;padding:20px}
.card{background:#1a1a2e;border-radius:16px;padding:28px;width:100%;max-width:440px;
      box-shadow:0 8px 32px rgba(0,0,0,.5)}
h1{color:#e94560;font-size:1.4rem;margin-bottom:4px}
.sub{color:#555;font-size:.82rem;margin-bottom:28px}
.section{background:#0f0f24;border-radius:12px;padding:18px;margin-bottom:16px}
.row{display:flex;align-items:center;justify-content:space-between;gap:12px}
.label{font-size:.85rem;color:#888;font-weight:500;letter-spacing:.03em}
/* Power Button */
.pwr{width:72px;height:36px;border-radius:18px;border:2px solid #333;
     font-weight:700;font-size:.85rem;cursor:pointer;transition:.2s;
     background:#1a1a2e;color:#555}
.pwr.on{background:#1a3a1a;border-color:#4CAF50;color:#4CAF50}
.pwr.off{background:#3a1a1a;border-color:#f44336;color:#f44336}
/* Volume */
.vol-header{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:14px}
.vol-val{font-size:1.6rem;font-weight:700;color:#e94560}
.vol-unit{font-size:.85rem;color:#666;margin-left:4px}
input[type=range]{
  width:100%;height:6px;border-radius:3px;outline:none;border:none;
  -webkit-appearance:none;background:linear-gradient(to right,#e94560 var(--p,50%),#333 var(--p,50%));
  cursor:pointer}
input[type=range]::-webkit-slider-thumb{
  -webkit-appearance:none;width:20px;height:20px;border-radius:50%;
  background:#e94560;box-shadow:0 2px 6px rgba(233,69,96,.4)}
.scale{display:flex;justify-content:space-between;font-size:.72rem;color:#444;margin-top:6px}
select{width:100%;padding:10px 12px;background:#0a0a1a;border:1px solid #333;
       border-radius:8px;color:#fff;font-size:.95rem;cursor:pointer;
       -webkit-appearance:none;appearance:none;
       background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='12' viewBox='0 0 12 12'%3E%3Cpath fill='%23666' d='M6 8L1 3h10z'/%3E%3C/svg%3E");
       background-repeat:no-repeat;background-position:right 12px center}
select:focus{outline:none;border-color:#e94560}
/* Status */
.status{display:flex;align-items:center;gap:10px;font-size:.82rem;color:#666;
        padding:12px 16px;background:#0a0a14;border-radius:10px;margin-bottom:14px}
.dot{width:8px;height:8px;border-radius:50%;background:#4CAF50;flex-shrink:0;
     box-shadow:0 0 6px #4CAF50}
.dot.off{background:#555;box-shadow:none}
/* WiFi Button */
.wifi-btn{width:100%;padding:10px;background:transparent;border:1px solid #222;
          border-radius:8px;color:#444;font-size:.82rem;cursor:pointer;transition:.15s}
.wifi-btn:hover{border-color:#444;color:#888}
</style></head><body>
<div class="card">
  <h1>&#x1F3DB; Onkyo TX-SR 806</h1>
  <div class="sub">ESP32 RS232 Bridge</div>

  <div class="status">
    <div class="dot off" id="sDot"></div>
    <span id="sText">Verbinde...</span>
  </div>

  <!-- Power -->
  <div class="section">
    <div class="row">
      <span class="label">POWER</span>
      <div style="display:flex;gap:10px">
        <button class="pwr on" onclick="setPower(true)">ON</button>
        <button class="pwr off" onclick="setPower(false)">OFF</button>
      </div>
    </div>
  </div>

  <!-- Input -->
  <div class="section">
    <div class="row" style="margin-bottom:10px">
      <span class="label">EINGANG</span>
    </div>
    <select id="inputSel" onchange="setInput(this.value)">
      <option value="00">VIDEO 1</option>
      <option value="01">CBL / SAT</option>
      <option value="02">GAME</option>
      <option value="03">AUX</option>
      <option value="10">BD / DVD</option>
      <option value="20">TAPE</option>
      <option value="22">PHONO</option>
      <option value="23" selected>CD</option>
      <option value="24">TUNER FM</option>
      <option value="25">TUNER AM</option>
    </select>
  </div>

  <!-- Volume -->
  <div class="section">
    <div class="vol-header">
      <span class="label">LAUTST&#196;RKE</span>
      <div><span class="vol-val" id="volNum">--</span></div>
    </div>
    <input type="range" id="volSlider" min="0" max="80" value="40"
      oninput="onSlide(this.value)" onchange="sendVol(this.value)">
    <div class="scale"><span>0</span><span>40</span><span>80</span></div>
  </div>

  <button class="wifi-btn" onclick="location.href='/wifi'">&#x2699;&#xFE0F; WLAN-Einstellungen &#xe4ndern</button>
</div>
<script>
let power=false, pwrBusy=false, statusFails=0;

function setSliderGradient(v){
  const pct=(parseInt(v)/80*100).toFixed(1);
  document.getElementById('volSlider').style.setProperty('--p',pct+'%');
}

function applyPower(state){
  power = state==='ON';
  document.getElementById('sDot').className='dot'+(power?'':' off');
}

async function fetchStatus(){
  try{
    const d=await(await fetch('/status')).json();
    statusFails=0;
    applyPower(d.power);
    document.getElementById('volNum').textContent=d.volume;
    document.getElementById('volSlider').value=d.volume;
    setSliderGradient(d.volume);
    document.getElementById('inputSel').value=d.input;
    document.getElementById('sText').textContent='Verbunden · '+d.ip;
  }catch(e){
    statusFails++;
    if(statusFails>=3){
      document.getElementById('sText').textContent='Keine Verbindung zum Ger\xe4t';
      document.getElementById('sDot').className='dot off';
    }
  }
}

async function setPower(on){
  if(pwrBusy) return;
  pwrBusy=true;
  try{
    const d=await(await fetch('/power',{method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({on:on})})).json();
    applyPower(d.power);
  }catch(e){}
  setTimeout(()=>{pwrBusy=false;},1500);
}

let volTimer;
function onSlide(v){
  document.getElementById('volNum').textContent=v;
  setSliderGradient(v);
}
function sendVol(v){
  clearTimeout(volTimer);
  volTimer=setTimeout(async()=>{
    await fetch('/volume',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({vol:parseInt(v)})});
  },250);
}

async function setInput(hex){
  await fetch('/input',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({hex:hex})});
}

setInterval(fetchStatus,10000);
fetchStatus();
</script></body></html>
)HTML";

// ── Web-Handler ─────────────────────────────────────────────────────
void handleRoot() {
  webServer.sendHeader("Connection", "close");
  webServer.send_P(200, "text/html", apMode ? AP_HTML : CTRL_HTML);
}

void handleWifiPage() {
  webServer.sendHeader("Connection", "close");
  webServer.send_P(200, "text/html", AP_HTML);
}

void handleScan() {
  WiFi.scanDelete();
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n && i < 20; i++) {
    if (i > 0) json += ",";
    String ssid = WiFi.SSID(i);
    ssid.replace("\"", "\\\"");
    json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + WiFi.RSSI(i) +
            ",\"enc\":" + (WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  json += "]";
  WiFi.scanDelete();
  webServer.send(200, "application/json", json);
}

void handleConnect() {
  if (!webServer.hasArg("plain")) { webServer.send(400); return; }
  String body = webServer.arg("plain");

  String ssid = "", pass = "";
  int s = body.indexOf("\"ssid\":\"");
  if (s >= 0) { s += 8; ssid = body.substring(s, body.indexOf("\"", s)); }
  s = body.indexOf("\"pass\":\"");
  if (s >= 0) { s += 8; pass = body.substring(s, body.indexOf("\"", s)); }

  if (connectWiFi(ssid, pass)) {
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
    webServer.send(200, "application/json",
      "{\"ok\":true,\"ip\":\"" + WiFi.localIP().toString() + "\"}");
    delay(1500);
    ESP.restart();
  } else {
    webServer.send(200, "application/json", "{\"ok\":false}");
  }
}

void sendJson(int code, const String& body) {
  webServer.sendHeader("Connection", "close");
  webServer.sendHeader("Access-Control-Allow-Origin", "*");
  webServer.send(code, "application/json", body);
}

void handleStatus() {
  sendJson(200,
    "{\"power\":\"" + currentPower + "\","
    "\"volume\":"   + String(currentVol) + ","
    "\"input\":\""  + currentInput + "\","
    "\"ip\":\""     + WiFi.localIP().toString() + "\"}");
}

void handleInput() {
  if (!webServer.hasArg("plain")) { sendJson(400, "{}"); return; }
  String body = webServer.arg("plain");
  int pos = body.indexOf("\"hex\":\"");
  if (pos < 0) { sendJson(400, "{}"); return; }
  String hex = body.substring(pos + 7, pos + 9);
  hex.trim();
  hex.toUpperCase();
  char cmd[20];
  snprintf(cmd, sizeof(cmd), "!1SLI%s\r", hex.c_str());
  sendIscp(String(cmd));
  lastPollMs = millis();
  currentInput = hex;
  sendJson(200, "{\"ok\":true}");
}

void handlePower() {
  if (!webServer.hasArg("plain")) {
    Serial.println("[PWR] kein Body!");
    sendJson(400, "{}"); return;
  }
  String body = webServer.arg("plain");
  bool on = body.indexOf("\"on\":true") >= 0;
  Serial.println("[PWR] Body=" + body + "  on=" + String(on ? "ON" : "OFF"));
  // Doppelsend mit 300ms Pause: erster Versuch kann im AVR-Noise verschwinden,
  // zweiter Versuch trifft den sauberen Buffer — identischer Effekt wie "Volume vorher senden".
  String pwrCmd = on ? "!1PWR01\r" : "!1PWR00\r";
  sendIscp(pwrCmd);
  delay(300);
  sendIscp(pwrCmd);
  lastPollMs = millis();
  currentPower = on ? "ON" : "OFF";
  sendJson(200, "{\"ok\":true,\"power\":\"" + currentPower + "\"}");
}

void handleVolume() {
  if (!webServer.hasArg("plain")) { sendJson(400, "{}"); return; }
  String body = webServer.arg("plain");
  int pos = body.indexOf("\"vol\":");
  if (pos < 0) { sendJson(400, "{}"); return; }
  int vol = constrain(body.substring(pos + 6).toInt(), 0, VOL_MAX_HEX);
  char cmd[20];
  snprintf(cmd, sizeof(cmd), "!1MVL%02X\r", vol);
  sendIscp(String(cmd));
  lastPollMs = millis();
  currentVol = vol;
  sendJson(200, "{\"ok\":true}");
}

// ── eISCP Bridge (OpenHAB → RS232, nur senden) ─────────────────────
// RS232 → OpenHAB läuft über readSerial2() im Loop
unsigned long eiscpLastRxMs = 0;
#define EISCP_IDLE_TIMEOUT 300000   // 5 Minuten

void enableKeepalive(WiFiClient& client) {
  int fd = client.fd();
  if (fd < 0) return;
  int ka = 1, idle = 10, intvl = 5, cnt = 3;
  setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE,  &ka,    sizeof(ka));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));
}

// Aktuellen Cache-Status sofort an eISCP-Client schicken (damit OpenHAB ONLINE bleibt)
void sendCachedStatus() {
  if (!eiscpClient || !eiscpClient.connected()) return;
  // IMMER einen Power-Status senden — bei UNKNOWN nehmen wir OFF an (AVR startet in Standby).
  // Ohne Status bekommt OpenHAB keine Antwort → reconnect-Loop.
  sendEiscpToClient(eiscpClient, currentPower == "ON" ? "!1PWR01\r" : "!1PWR00\r");
  char volMsg[16];
  snprintf(volMsg, sizeof(volMsg), "!1MVL%02X\r", currentVol);
  sendEiscpToClient(eiscpClient, String(volMsg));
  sendEiscpToClient(eiscpClient, "!1SLI" + currentInput + "\r");
}

void handleEiscpBridge() {
  // IMMER auf neue Verbindungen prüfen — auch wenn eine bestehende existiert.
  // Verhindert "Connect timed out": wenn OpenHAB reconnectet, bevor der ESP32
  // die tote Verbindung erkennt, würde der Backlog volllaufen.
  if (eiscpServer.hasClient()) {
    WiFiClient newClient = eiscpServer.accept();
    if (newClient) {
      if (eiscpClient && eiscpClient.connected()) {
        Serial.println("[eISCP] Neue Verbindung verdrängt alte");
        eiscpClient.stop();
      }
      eiscpClient   = newClient;
      eiscpLastRxMs = millis();
      enableKeepalive(eiscpClient);
      Serial.println("[eISCP] Client: " + eiscpClient.remoteIP().toString());
      // Sofort gecachten Status senden → OpenHAB geht auf ONLINE
      // KEINE QSTN-Queries hier — der AVR-Flood war die Ursache des Reconnect-Loops
      sendCachedStatus();
    }
  }

  if (!eiscpClient || !eiscpClient.connected()) {
    eiscpClient.stop();
    return;
  }

  // Idle-Timeout: tote Verbindung erzwungen schliessen
  if (millis() - eiscpLastRxMs > EISCP_IDLE_TIMEOUT) {
    Serial.println("[eISCP] Idle-Timeout (5 min), Verbindung wird geschlossen");
    eiscpClient.stop();
    return;
  }

  // OpenHAB → AVR / Cache
  if (eiscpClient.available() > 0) {
    eiscpLastRxMs = millis();
    if (eiscpClient.available() >= 16) {
      uint8_t buf[512];
      int len = eiscpClient.read(buf, sizeof(buf));
      if (len > 0) {
        String iscp = parseEiscp(buf, len);
        if (iscp.length() >= 3) {
          Serial.println("[eISCP→RS232] " + iscp);
          // QSTN-Queries direkt aus dem Cache beantworten — NICHT an den AVR weiterleiten.
          // Verhindert den Reconnect-Loop: fragmentierte AVR-Antworten → OpenHAB timeout → reconnect → flood
          if (iscp == "!1PWRQSTN") {
            // Bei UNKNOWN OFF annehmen — AVR startet immer in Standby.
            // IMMER antworten, sonst reconnect-loop.
            sendEiscpToClient(eiscpClient, currentPower == "ON" ? "!1PWR01\r" : "!1PWR00\r");
          } else if (iscp == "!1MVLQSTN") {
            char m[16]; snprintf(m, sizeof(m), "!1MVL%02X\r", currentVol);
            sendEiscpToClient(eiscpClient, String(m));
          } else if (iscp == "!1SLIQSTN") {
            sendEiscpToClient(eiscpClient, "!1SLI" + currentInput + "\r");
          } else {
            // Alles andere (Kommandos: PWR01, PWRSTANDBY, MVLxx, SLIxx …) → AVR
            sendIscp(iscp + "\r");
          }
        }
      }
    }
  }
}

// ── Hintergrund-Poll (nur senden, readSerial2 liest die Antworten) ──
// Eine Query pro Intervall, rotierend — verhindert dass 3 gleichzeitige Queries
// die AVR-Antwort auf ein User-Kommando unterbrechen.
#define POLL_INTERVAL_MS 10000  // 10s pro Query → voller Zyklus alle 30s

void pollAvr() {
  if (apMode) return;
  unsigned long now = millis();
  if (now - lastPollMs < POLL_INTERVAL_MS) return;
  lastPollMs = now;
  switch (pollState) {
    case 0: sendIscp("!1PWRQSTN\r"); break;
    case 1: sendIscp("!1MVLQSTN\r"); break;
    case 2: sendIscp("!1SLIQSTN\r"); break;
  }
  pollState = (pollState + 1) % 3;
}

// ── Setup ───────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== Onkyo TX-SR 806 Bridge ===");

  Serial2.begin(RS232_BAUD, SERIAL_8N1, RS232_RX_PIN, RS232_TX_PIN);
  Serial.println("RS232 UART2 bereit (RX=" + String(RS232_RX_PIN) + " TX=" + String(RS232_TX_PIN) + ")");

  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();

  if (ssid.length() > 0 && connectWiFi(ssid, pass)) {
    apMode = false;

    // Startup-Poll: AVR-Status abfragen und 2s auf Antwort warten.
    // Cache muss gefüllt sein bevor OpenHAB sich verbindet, sonst Reconnect-Loop.
    Serial.println("Startup: AVR-Status abfragen...");
    sendIscp("!1PWRQSTN\r");
    sendIscp("!1MVLQSTN\r");
    sendIscp("!1SLIQSTN\r");
    for (int i = 0; i < 100; i++) { readSerial2(); delay(20); }
    Serial.println("Cache nach Startup: PWR=" + currentPower +
                   " VOL=" + String(currentVol) + " SLI=" + currentInput);

    MDNS.begin("onkyo-bridge");
    Serial.println("mDNS: http://onkyo-bridge.local");
    eiscpServer.begin();
    Serial.println("eISCP Server auf Port " + String(EISCP_PORT));
  } else {
    startAP();
  }

  webServer.on("/",        HTTP_GET,  handleRoot);
  webServer.on("/wifi",    HTTP_GET,  handleWifiPage);
  webServer.on("/scan",    HTTP_GET,  handleScan);
  webServer.on("/connect", HTTP_POST, handleConnect);
  webServer.on("/status",  HTTP_GET,  handleStatus);
  webServer.on("/power",   HTTP_POST, handlePower);
  webServer.on("/volume",  HTTP_POST, handleVolume);
  webServer.on("/input",   HTTP_POST, handleInput);
  webServer.begin();
  Serial.println("WebServer auf Port " + String(WEB_PORT));
}

// ── Loop ────────────────────────────────────────────────────────────
void loop() {
  webServer.handleClient();
  if (!apMode) {
    readSerial2();
    handleEiscpBridge();
    pollAvr();
  }
}
