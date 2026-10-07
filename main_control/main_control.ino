/*
  ============================================================
   بورد 1 - المتحكم الرئيسي
   - Blynk IoT (التحكم من أي مكان في العالم)
   - MQTT (Home Assistant)
   - WiFi: AP دايماً + Station mode
   - واجهة ويب محلية
   - يستقبل بيانات الحساسات من بورد 2

  التغييرات:
   - لمبة 1 + لمبة 2 بحساس حركة واحد
   - مروحة 1 + مروحة 2 (RELAY4 بقى مروحة 2) بحساس حرارة واحد
   - تنبيه الغاز: قفل كامل دائم لحد Restart يدوي
   - تفعيل/تعطيل PIR من التطبيق (V11)

  المكتبات المطلوبة (Arduino Library Manager):
    - Blynk by Volodymyr Shymanskyy
    - PubSubClient by Nick O'Leary
    - ArduinoJson by Benoit Blanchon (نسخة 6.x)
    - LittleFS (مدمجة مع ESP8266)
  ============================================================
*/

// ==================== Blynk Config ====================
#define BLYNK_TEMPLATE_ID   "TMPL5a45qncLh"
#define BLYNK_TEMPLATE_NAME "smart lab"
#define BLYNK_AUTH_TOKEN    "5YvX43GqJOwkUYfrQWhptYIEsNWELGOH"

#define BLYNK_PRINT Serial

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <BlynkSimpleEsp8266.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

// ==================== Pins الريليهات ====================
#define RELAY1 5   // D1 - لمبة 1
#define RELAY2 4   // D2 - لمبة 2
#define RELAY3 0   // D3 - مروحة 1
#define RELAY4 14  // D5 - مروحة 2  ← (كان LED)
#define RELAY5 12  // D6 - داتا شو
#define RELAY6 13  // D7 - قفل الباب

const int RELAY_COUNT = 6;
const int relayPins[RELAY_COUNT] = {RELAY1, RELAY2, RELAY3, RELAY4, RELAY5, RELAY6};
bool relayState[RELAY_COUNT] = {false};

String relayNames[RELAY_COUNT] = {
  "لمبة 1", "لمبة 2", "مروحة 1", "مروحة 2", "داتا شو", "قفل الباب"
};

// ==================== Blynk Virtual Pins ====================
// V1→V6  : التحكم في الريليهات
// V7     : الحرارة
// V8     : الرطوبة
// V9     : الغاز
// V10    : الحركة (LED)
// V11    : تفعيل/تعطيل PIR
// V12    : تنبيه الغاز

// ==================== إعدادات الشبكة ====================
String apSSID     = "SmartLab";
String apPassword = "12345678";
String staSSID    = "";
String staPass    = "";

// ==================== مفاتيح الأمان ====================
const char* ACCESS_KEY = "lab2026key";
const char* SENSOR_KEY = "sensorNode2026";

// ==================== MQTT ====================
String mqttBroker   = "broker.hivemq.com";
String mqttUser     = "";
String mqttPassword = "";
int    mqttPort     = 1883;
String mqttPrefix   = "smartlab";

WiFiClient   espClient;
PubSubClient mqttClient(espClient);
unsigned long lastMqttReconnect = 0;
bool mqttEnabled = false;

// ==================== بيانات الحساسات ====================
float temperature    = 0;
float humidity       = 0;
int   gasValue       = 0;
bool  motionDetected = false;
bool  pirReady       = false;
bool  sensorBoardOnline = false;
unsigned long lastSensorUpdate = 0;
const unsigned long sensorTimeout = 8000;

// ==================== منطق الأتمتة ====================
// --- حركة ---
unsigned long lastMotionTime = 0;
const unsigned long motionTimeout = 15000;  // 15 ثانية بعد ما تختفي الحركة
bool lastMotionState = false;
bool pirEnabled = true;

// --- حرارة ---
const float tempThreshold = 30.0;  // فوق 30 → شغّل المروحتين

// --- غاز ---
int  gasThreshold = 500;
bool gasAlert     = false;          // لو true: قفل كامل دائم لحد Restart يدوي
int  gasHighCount = 0;
const int gasConfirmReadings = 3;

// ==================== Blynk Timer ====================
BlynkTimer blynkTimer;
ESP8266WebServer server(80);
bool staConnected = false;

// ==================== Relay Control ====================
void setRelay(int idx, bool state) {
  if (idx < 0 || idx >= RELAY_COUNT) return;
  relayState[idx] = state;
  digitalWrite(relayPins[idx], state ? LOW : HIGH);
  publishRelayStatus(idx);
  Blynk.virtualWrite(V1 + idx, state ? 1 : 0);
}

void allRelaysOff() {
  for (int i = 0; i < RELAY_COUNT; i++) setRelay(i, false);
}

// ==================== Blynk Handlers ====================
BLYNK_WRITE(V1) { if (!gasAlert) setRelay(0, param.asInt() == 1); }
BLYNK_WRITE(V2) { if (!gasAlert) setRelay(1, param.asInt() == 1); }
BLYNK_WRITE(V3) { if (!gasAlert) setRelay(2, param.asInt() == 1); }
BLYNK_WRITE(V4) { if (!gasAlert) setRelay(3, param.asInt() == 1); }
BLYNK_WRITE(V5) { if (!gasAlert) setRelay(4, param.asInt() == 1); }
BLYNK_WRITE(V6) { if (!gasAlert) setRelay(5, param.asInt() == 1); }

// تفعيل/تعطيل PIR من التطبيق
BLYNK_WRITE(V11) {
  pirEnabled = (param.asInt() == 1);
  if (!pirEnabled) {
    setRelay(0, false);
    setRelay(1, false);
  }
}

BLYNK_CONNECTED() {
  Serial.println("✅ Blynk متصل!");
  for (int i = 0; i < RELAY_COUNT; i++) {
    Blynk.virtualWrite(V1 + i, relayState[i] ? 1 : 0);
  }
  Blynk.virtualWrite(V11, pirEnabled ? 1 : 0);
}

// ==================== بعت بيانات الحساسات لـ Blynk ====================
void sendSensorsToBlynk() {
  if (!Blynk.connected()) return;
  Blynk.virtualWrite(V7,  temperature);
  Blynk.virtualWrite(V8,  humidity);
  Blynk.virtualWrite(V9,  gasValue);
  Blynk.virtualWrite(V10, motionDetected ? 1 : 0);
  Blynk.virtualWrite(V12, gasAlert ? 1 : 0);
  if (gasAlert) {
    Blynk.logEvent("gas_alert", "🚨 تحذير! تم اكتشاف غاز — النظام مقفول! أعد التشغيل يدوياً");
  }
}

// ==================== MQTT ====================
String controlTopic(int idx) { return mqttPrefix + "/control/relay" + String(idx + 1); }
String statusTopic(int idx)  { return mqttPrefix + "/status/relay"  + String(idx + 1); }
String controlAllTopic()     { return mqttPrefix + "/control/all"; }
String statusAllTopic()      { return mqttPrefix + "/status/all"; }

void publishRelayStatus(int idx) {
  if (!mqttClient.connected()) return;
  mqttClient.publish(statusTopic(idx).c_str(), relayState[idx] ? "ON" : "OFF", true);
  String json = "{";
  for (int i = 0; i < RELAY_COUNT; i++) {
    json += "\"relay" + String(i+1) + "\":\"" + (relayState[i] ? "ON" : "OFF") + "\"";
    if (i < RELAY_COUNT-1) json += ",";
  }
  json += "}";
  mqttClient.publish(statusAllTopic().c_str(), json.c_str(), true);
}

void publishHADiscovery() {
  if (!mqttClient.connected()) return;
  for (int i = 0; i < RELAY_COUNT; i++) {
    String uid   = "smartlab_relay" + String(i+1);
    String topic = "homeassistant/switch/" + uid + "/config";
    StaticJsonDocument<512> doc;
    doc["name"]          = relayNames[i];
    doc["unique_id"]     = uid;
    doc["command_topic"] = controlTopic(i);
    doc["state_topic"]   = statusTopic(i);
    doc["payload_on"]    = "ON";
    doc["payload_off"]   = "OFF";
    doc["retain"]        = true;
    JsonObject dev = doc.createNestedObject("device");
    dev["identifiers"][0] = "smartlab_board1";
    dev["name"]           = "Smart Lab";
    dev["manufacturer"]   = "Sabry Garage";
    String payload; serializeJson(doc, payload);
    mqttClient.publish(topic.c_str(), payload.c_str(), true);
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t = String(topic);
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.toUpperCase();
  if (t == controlAllTopic()) { if (msg == "OFF") allRelaysOff(); return; }
  for (int i = 0; i < RELAY_COUNT; i++) {
    if (t == controlTopic(i) && !gasAlert) {
      if (msg == "ON")  setRelay(i, true);
      if (msg == "OFF") setRelay(i, false);
      return;
    }
  }
}

bool mqttConnect() {
  if (mqttBroker.length() < 3) return false;
  mqttClient.setServer(mqttBroker.c_str(), mqttPort);
  mqttClient.setCallback(mqttCallback);
  String clientId = "SmartLab-" + String(ESP.getChipId(), HEX);
  bool ok = mqttUser.length() > 0
    ? mqttClient.connect(clientId.c_str(), mqttUser.c_str(), mqttPassword.c_str())
    : mqttClient.connect(clientId.c_str());
  if (ok) {
    for (int i = 0; i < RELAY_COUNT; i++) mqttClient.subscribe(controlTopic(i).c_str());
    mqttClient.subscribe(controlAllTopic().c_str());
    publishHADiscovery();
    for (int i = 0; i < RELAY_COUNT; i++) publishRelayStatus(i);
    Serial.println("✅ MQTT متصل!");
  }
  return ok;
}

// ==================== LittleFS ====================
void saveSettings() {
  StaticJsonDocument<1024> doc;
  doc["staSSID"]    = staSSID;
  doc["staPass"]    = staPass;
  doc["apSSID"]     = apSSID;
  doc["apPass"]     = apPassword;
  doc["mqttBroker"] = mqttBroker;
  doc["mqttUser"]   = mqttUser;
  doc["mqttPass"]   = mqttPassword;
  doc["mqttPort"]   = mqttPort;
  doc["mqttPrefix"] = mqttPrefix;
  for (int i = 0; i < RELAY_COUNT; i++) doc["name" + String(i)] = relayNames[i];
  File f = LittleFS.open("/config.json", "w");
  if (f) { serializeJson(doc, f); f.close(); }
}

void loadSettings() {
  if (!LittleFS.exists("/config.json")) return;
  File f = LittleFS.open("/config.json", "r");
  if (!f) return;
  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    staSSID     = doc["staSSID"]    | "";
    staPass     = doc["staPass"]    | "";
    apSSID      = doc["apSSID"]     | "SmartLab";
    apPassword  = doc["apPass"]     | "12345678";
    mqttBroker  = doc["mqttBroker"] | "broker.hivemq.com";
    mqttUser    = doc["mqttUser"]   | "";
    mqttPassword= doc["mqttPass"]   | "";
    mqttPort    = doc["mqttPort"]   | 1883;
    mqttPrefix  = doc["mqttPrefix"] | "smartlab";
    for (int i = 0; i < RELAY_COUNT; i++) relayNames[i] = doc["name" + String(i)] | relayNames[i];
  }
  f.close();
}

// ==================== فحص المفاتيح ====================
bool checkAccessKey() {
  if (!server.hasArg("key") || server.arg("key") != ACCESS_KEY) {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return false;
  }
  return true;
}
bool checkSensorKey() {
  if (!server.hasArg("key") || server.arg("key") != SENSOR_KEY) {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return false;
  }
  return true;
}

// ==================== HTML ====================
String buildMainHTML() {
  String html = R"rawhtml(
<!DOCTYPE html><html lang='ar' dir='rtl'>
<head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Smart Lab</title>
<style>
  *{margin:0;padding:0;box-sizing:border-box}
  body{font-family:'Segoe UI',sans-serif;background:linear-gradient(135deg,#0f2027,#203a43,#2c5364);min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:20px 15px}
  h1{color:#fff;font-size:1.9rem;margin-bottom:4px;text-shadow:0 0 20px #00d4ff}
  .subtitle{color:#00d4ff;font-size:.9rem;margin-bottom:10px;letter-spacing:2px}
  .nav{display:flex;gap:8px;margin-bottom:18px;flex-wrap:wrap;justify-content:center}
  .nav a{background:rgba(255,255,255,.1);color:#ccc;text-decoration:none;padding:7px 14px;border-radius:20px;font-size:.82rem;border:1px solid rgba(255,255,255,.15)}
  .nav a:hover{background:#00d4ff;color:#000}
  .offline-banner{display:none;background:#ff9900;color:#000;padding:10px 20px;border-radius:12px;font-size:.85rem;margin-bottom:12px;width:100%;max-width:620px;text-align:center;font-weight:bold}
  .offline-banner.show{display:block}
  .alert-banner{display:none;background:#ff4444;color:#fff;padding:12px 20px;border-radius:12px;font-size:1rem;margin-bottom:12px;width:100%;max-width:620px;text-align:center;font-weight:bold;animation:pulse 1s infinite}
  .alert-banner.show{display:block}
  @keyframes pulse{0%{box-shadow:0 0 0 #ff4444}50%{box-shadow:0 0 20px #ff4444}100%{box-shadow:0 0 0 #ff4444}}
  .sensor-row{display:flex;gap:12px;width:100%;max-width:620px;margin-bottom:14px}
  .sensor-card{flex:1;background:rgba(255,255,255,.07);border:1px solid rgba(255,255,255,.15);border-radius:18px;padding:16px 12px;text-align:center;backdrop-filter:blur(10px)}
  .s-icon{font-size:1.7rem;margin-bottom:6px}.s-label{color:#888;font-size:.75rem;margin-bottom:4px}
  .s-value{color:#00d4ff;font-size:1.4rem;font-weight:bold}.s-value.hot{color:#ff6644}
  .s-sub{color:#666;font-size:.72rem;margin-top:4px}
  .pir-card{background:rgba(255,255,255,.07);border:1px solid rgba(255,255,255,.15);border-radius:18px;padding:16px 22px;width:100%;max-width:620px;display:flex;justify-content:space-between;align-items:center;margin-bottom:14px;backdrop-filter:blur(10px)}
  .pir-title{color:#fff;font-size:.95rem;margin-bottom:4px}
  .motion-dot{width:12px;height:12px;border-radius:50%;background:#444;display:inline-block;margin-left:7px;vertical-align:middle}
  .motion-dot.active{background:#00ff88;box-shadow:0 0 10px #00ff88}
  .grid{display:grid;grid-template-columns:repeat(2,1fr);gap:16px;width:100%;max-width:620px}
  .card{background:rgba(255,255,255,.07);border:1px solid rgba(255,255,255,.15);border-radius:18px;padding:22px 12px;text-align:center;backdrop-filter:blur(10px);transition:transform .2s}
  .card:hover{transform:translateY(-3px)}
  .icon{font-size:2rem;margin-bottom:8px}.name{color:#ccc;font-size:.95rem;margin-bottom:14px}
  .toggle{width:62px;height:32px;background:#444;border-radius:32px;position:relative;cursor:pointer;margin:0 auto;border:none;transition:background .3s}
  .toggle.on{background:#00d4ff;box-shadow:0 0 12px #00d4ff88}
  .toggle.auto{background:#ff9900;box-shadow:0 0 12px #ff990088}
  .toggle::after{content:'';width:24px;height:24px;background:#fff;border-radius:50%;position:absolute;top:4px;left:4px;transition:left .3s}
  .toggle.on::after,.toggle.auto::after{left:34px}
  .toggle:disabled{opacity:.4;cursor:not-allowed}
  .status{color:#888;font-size:.78rem;margin-top:8px}.status.on{color:#00d4ff}.status.auto{color:#ff9900}
  .blynk-badge{background:rgba(0,212,100,.15);border:1px solid #00d464;border-radius:10px;padding:6px 14px;color:#00d464;font-size:.78rem;margin-bottom:14px;text-align:center;width:100%;max-width:620px}
  .footer{color:#555;margin-top:30px;font-size:.75rem;text-align:center}
</style></head><body>
<h1>⚡ Smart Lab</h1>
<div class='subtitle'>لوحة التحكم الذكية</div>
<div class='nav'>
  <a href='/'>🏠 الرئيسية</a>
  <a href='/wifi'>📶 WiFi</a>
  <a href='/mqtt'>📡 MQTT</a>
  <a href='/relaynames'>✏️ الأسماء</a>
  <a href='/restart'>🔄 إعادة تشغيل</a>
</div>
<div class='blynk-badge'>📱 متصل بـ Blynk IoT — تحكم من أي مكان في العالم</div>
<div class='offline-banner' id='offlineBanner'>⚠️ بورد الحساسات غير متصل</div>
<div class='alert-banner' id='alertBanner'>🚨 غاز مكتشف! النظام مقفول بالكامل — أعد التشغيل يدوياً من زر Restart</div>
<div class='sensor-row'>
  <div class='sensor-card'><div class='s-icon'>🌡️</div><div class='s-label'>الحرارة</div><div class='s-value' id='tempVal'>--</div><div class='s-sub'>حد: 30°C</div></div>
  <div class='sensor-card'><div class='s-icon'>💧</div><div class='s-label'>الرطوبة</div><div class='s-value' id='humVal'>--</div><div class='s-sub'>DHT11</div></div>
  <div class='sensor-card'><div class='s-icon'>💨</div><div class='s-label'>الغاز</div><div class='s-value' id='gasVal'>--</div><div class='s-sub'>MQ-2</div></div>
</div>
<div class='pir-card'>
  <div><div class='pir-title'>🚶 حساس الحركة PIR <span class='motion-dot' id='motionDot'></span></div>
  <span id='pirStatus' style='font-size:.82rem;color:#ccc'>جاري القراءة...</span></div>
  <button class='toggle )rawhtml";
  html += pirEnabled ? "on" : "";
  html += R"rawhtml(' id='pirToggle' onclick='togglePIR()'></button>
</div>
<div class='grid' id='grid'></div>
<div class='footer'>Smart Lab — ESP8266 + Blynk IoT | Sabry Garage</div>
<script>
const ACCESS_KEY=')rawhtml";
  html += ACCESS_KEY;
  html += R"rawhtml(';
const relays=[)rawhtml";
  for (int i = 0; i < RELAY_COUNT; i++) {
    html += "{id:" + String(i+1) + ",name:'" + relayNames[i] + "',state:" + (relayState[i]?"true":"false") + "}";
    if (i < RELAY_COUNT-1) html += ",";
  }
  html += R"rawhtml(];
const icons=['💡','💡','🌀','🌀','📽️','🔒'];
let fanAuto=false,gasAlertActive=false;
function buildCards(){
  const g=document.getElementById('grid');g.innerHTML='';
  relays.forEach((r,i)=>{
    // مروحة 1 (i=2) ومروحة 2 (i=3) كلتيهما تشتغل تلقائياً بالحرارة
    const isFan=(i===2||i===3);
    const cls=isFan&&fanAuto?'auto':(r.state?'on':'');
    const st=isFan&&fanAuto?'🌡️ تلقائي':(r.state?'✅ شغال':'⭕ واقف');
    const sc=isFan&&fanAuto?'auto':(r.state?'on':'');
    g.innerHTML+=`<div class='card'><div class='icon'>${icons[i]}</div><div class='name'>${r.name}</div>
      <button class='toggle ${cls}' onclick='toggle(${r.id})' ${gasAlertActive?"disabled":""}></button>
      <div class='status ${sc}'>${st}</div></div>`;
  });
}
function toggle(id){
  if(gasAlertActive)return;
  fetch('/toggle?relay='+id+'&key='+ACCESS_KEY).then(r=>r.json()).then(d=>{
    if(!d.error){relays[id-1].state=d.state;buildCards();}
  });
}
function togglePIR(){
  if(gasAlertActive)return;
  fetch('/pirToggle?key='+ACCESS_KEY).then(r=>r.json()).then(d=>{
    if(!d.error){document.getElementById('pirToggle').className='toggle '+(d.enabled?'on':'');}
  });
}
setInterval(()=>{
  fetch('/status').then(r=>r.json()).then(d=>{
    document.getElementById('offlineBanner').className=d.sensorOnline?'offline-banner':'offline-banner show';
    gasAlertActive=d.gasAlert;
    document.getElementById('alertBanner').className=d.gasAlert?'alert-banner show':'alert-banner';
    const te=document.getElementById('tempVal'),he=document.getElementById('humVal'),ge=document.getElementById('gasVal');
    te.textContent=d.temp+'°C';he.textContent=d.hum+'%';ge.textContent=d.gasValue;
    te.className=d.temp>=30?'s-value hot':'s-value';
    ge.className=d.gasAlert?'s-value hot':'s-value';
    fanAuto=d.fanAuto;
    const dot=document.getElementById('motionDot'),ps=document.getElementById('pirStatus');
    document.getElementById('pirToggle').disabled=gasAlertActive;
    if(!d.pirEnabled){dot.className='motion-dot';ps.textContent='🚫 معطّل';}
    else if(!d.pirReady){dot.className='motion-dot';ps.textContent='⏳ الحساس بيستقر...';}
    else{dot.className='motion-dot '+(d.motion?'active':'');ps.textContent=d.motion?'⚡ حركة مكتشفة!':'😴 لا توجد حركة';}
    d.relays.forEach((s,i)=>{relays[i].state=s;});
    buildCards();
  });
},1000);
buildCards();
</script></body></html>)rawhtml";
  return html;
}

String buildWifiHTML() {
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  String html = "<!DOCTYPE html><html lang='ar' dir='rtl'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>WiFi</title>";
  html += "<style>*{margin:0;padding:0;box-sizing:border-box}body{font-family:'Segoe UI',sans-serif;background:linear-gradient(135deg,#0f2027,#203a43,#2c5364);min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:25px 15px;color:#fff}h2{font-size:1.5rem;margin-bottom:5px}.nav{display:flex;gap:8px;margin:12px 0 20px;flex-wrap:wrap;justify-content:center}.nav a{background:rgba(255,255,255,.1);color:#ccc;text-decoration:none;padding:7px 14px;border-radius:20px;font-size:.82rem;border:1px solid rgba(255,255,255,.15)}.nav a:hover{background:#00d4ff;color:#000}.card{background:rgba(255,255,255,.07);border:1px solid rgba(255,255,255,.15);border-radius:18px;padding:22px;width:100%;max-width:500px;margin-bottom:18px;backdrop-filter:blur(10px)}.card h3{color:#00d4ff;margin-bottom:14px;font-size:1rem}.badge{padding:3px 10px;border-radius:10px;font-size:.78rem}.badge.ok{background:#00c853;color:#000}.badge.fail{background:#ff4444;color:#fff}.row{display:flex;justify-content:space-between;margin-bottom:7px;font-size:.85rem;color:#ccc}.row span:last-child{color:#fff;font-weight:bold}input{width:100%;background:rgba(255,255,255,.1);border:1px solid rgba(255,255,255,.2);border-radius:10px;padding:11px 14px;color:#fff;font-size:.9rem;margin-bottom:10px}input::placeholder{color:#666}.btn{width:100%;padding:13px;border:none;border-radius:12px;font-size:.95rem;cursor:pointer;font-weight:bold;margin-top:4px}.g{background:#00c853;color:#000}.r{background:#ff4444;color:#fff}.b{background:#2196F3;color:#fff}.o{background:#ff9900;color:#000}.note{color:#888;font-size:.78rem;margin-top:8px;text-align:center}</style></head><body>";
  html += "<h2>📶 WiFi Settings</h2><div class='nav'><a href='/'>🏠</a><a href='/wifi'>📶 WiFi</a><a href='/mqtt'>📡 MQTT</a><a href='/relaynames'>✏️ الأسماء</a></div>";
  html += "<div class='card'><h3>🔌 Current Status</h3>";
  html += "<div class='row'><span>Status:</span><span>" + String(isConnected ? "<span class='badge ok'>✅ Connected</span>" : "<span class='badge fail'>❌ AP Mode Only</span>") + "</span></div>";
  html += "<div class='row'><span>Network:</span><span>" + String(isConnected ? WiFi.SSID() : "-") + "</span></div>";
  html += "<div class='row'><span>STA IP:</span><span>" + String(isConnected ? WiFi.localIP().toString() : "-") + "</span></div></div>";
  html += "<div class='card'><h3>📡 AP (Always Active)</h3><div class='row'><span>SSID:</span><span>" + apSSID + "</span></div><div class='row'><span>Password:</span><span>" + apPassword + "</span></div><div class='row'><span>IP:</span><span>192.168.4.1</span></div></div>";
  html += "<div class='card'><h3>✏️ Connect to WiFi</h3><input type='text' id='ssid' placeholder='Network name (SSID)'><input type='password' id='pass' placeholder='Password'><button class='btn g' onclick='saveWifi()'>💾 Save and restart</button><p class='note'>بعد الحفظ، Blynk هيشتغل تلقائياً</p></div>";
  html += "<div class='card'><h3>⚙️ AP Settings</h3><input type='text' id='apN' placeholder='AP Name' value='" + apSSID + "'><input type='password' id='apP' placeholder='AP Password' value='" + apPassword + "'><button class='btn o' onclick='saveAP()'>💾 Save AP</button><button class='btn r' style='margin-top:8px' onclick='clearS()'>🗑️ Clear settings</button><button class='btn b' style='margin-top:8px' onclick='location.href=\"/restart\"'>🔄 Restart</button></div>";
  html += "<script>function saveWifi(){const s=document.getElementById('ssid').value,p=document.getElementById('pass').value;if(!s){alert('أدخل SSID');return;}fetch('/saveWifi?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)+'&key=" + String(ACCESS_KEY) + "').then(r=>r.json()).then(d=>alert(d.msg));}";
  html += "function saveAP(){const n=document.getElementById('apN').value,p=document.getElementById('apP').value;if(!n||p.length<8){alert('8 أحرف على الأقل');return;}fetch('/saveAP?name='+encodeURIComponent(n)+'&pass='+encodeURIComponent(p)+'&key=" + String(ACCESS_KEY) + "').then(r=>r.json()).then(d=>alert(d.msg));}";
  html += "function clearS(){if(!confirm('هتمسح الإعدادات؟'))return;fetch('/clearSettings?key=" + String(ACCESS_KEY) + "').then(r=>r.json()).then(d=>alert(d.msg));}</script></body></html>";
  return html;
}

String buildMqttHTML() {
  String html = "<!DOCTYPE html><html lang='ar' dir='rtl'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>MQTT</title>";
  html += "<style>*{margin:0;padding:0;box-sizing:border-box}body{font-family:'Segoe UI',sans-serif;background:linear-gradient(135deg,#0f2027,#203a43,#2c5364);min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:25px 15px;color:#fff}h2{font-size:1.5rem;margin-bottom:5px}.nav{display:flex;gap:8px;margin:12px 0 20px;flex-wrap:wrap;justify-content:center}.nav a{background:rgba(255,255,255,.1);color:#ccc;text-decoration:none;padding:7px 14px;border-radius:20px;font-size:.82rem;border:1px solid rgba(255,255,255,.15)}.nav a:hover{background:#00d4ff;color:#000}.card{background:rgba(255,255,255,.07);border:1px solid rgba(255,255,255,.15);border-radius:18px;padding:22px;width:100%;max-width:500px;margin-bottom:18px;backdrop-filter:blur(10px)}.card h3{color:#00d4ff;margin-bottom:14px;font-size:1rem}input{width:100%;background:rgba(255,255,255,.1);border:1px solid rgba(255,255,255,.2);border-radius:10px;padding:11px 14px;color:#fff;font-size:.9rem;margin-bottom:10px}input::placeholder{color:#666}label{color:#888;font-size:.82rem;display:block;margin-bottom:4px}.topics{background:rgba(0,0,0,.3);border-radius:10px;padding:12px;font-size:.78rem;color:#ccc;line-height:1.9}.topics span{color:#00d4ff}.btn{width:100%;padding:13px;border:none;border-radius:12px;font-size:.95rem;cursor:pointer;font-weight:bold;background:#00c853;color:#000}</style></head><body>";
  html += "<h2>📡 MQTT Settings</h2><div class='nav'><a href='/'>🏠</a><a href='/wifi'>📶 WiFi</a><a href='/mqtt'>📡 MQTT</a><a href='/relaynames'>✏️ الأسماء</a></div>";
  html += "<div class='card'><h3>🛰️ Configure MQTT Broker</h3>";
  html += "<label>Broker</label><input type='text' id='b' value='" + mqttBroker + "' placeholder='broker.hivemq.com'>";
  html += "<label>Username</label><input type='text' id='u' value='" + mqttUser + "' placeholder='اتركه فاضي لو مفيش'>";
  html += "<label>Port</label><input type='number' id='p' value='" + String(mqttPort) + "'>";
  html += "<label>Prefix</label><input type='text' id='pr' value='" + mqttPrefix + "' placeholder='smartlab'>";
  html += "<div class='topics'>Control: <span>" + mqttPrefix + "/control/relay1</span><br>Status: <span>" + mqttPrefix + "/status/relay1</span><br>Control All: <span>" + mqttPrefix + "/control/all</span><br>HA Discovery: <span>homeassistant/switch/smartlab_relay1/config</span></div>";
  html += "<button class='btn' onclick='saveMqtt()' style='margin-top:14px'>💾 Save MQTT Settings</button></div>";
  html += "<script>function saveMqtt(){const b=document.getElementById('b').value,u=document.getElementById('u').value,p=document.getElementById('p').value,pr=document.getElementById('pr').value;fetch('/saveMqtt?broker='+encodeURIComponent(b)+'&user='+encodeURIComponent(u)+'&port='+p+'&prefix='+encodeURIComponent(pr)+'&key=" + String(ACCESS_KEY) + "').then(r=>r.json()).then(d=>alert(d.msg));}</script></body></html>";
  return html;
}

String buildNamesHTML() {
  String html = "<!DOCTYPE html><html lang='ar' dir='rtl'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Names</title>";
  html += "<style>*{margin:0;padding:0;box-sizing:border-box}body{font-family:'Segoe UI',sans-serif;background:linear-gradient(135deg,#0f2027,#203a43,#2c5364);min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:25px 15px;color:#fff}h2{font-size:1.5rem;margin-bottom:5px}.nav{display:flex;gap:8px;margin:12px 0 20px;flex-wrap:wrap;justify-content:center}.nav a{background:rgba(255,255,255,.1);color:#ccc;text-decoration:none;padding:7px 14px;border-radius:20px;font-size:.82rem;border:1px solid rgba(255,255,255,.15)}.nav a:hover{background:#00d4ff;color:#000}.card{background:rgba(255,255,255,.07);border:1px solid rgba(255,255,255,.15);border-radius:18px;padding:22px;width:100%;max-width:500px;backdrop-filter:blur(10px)}.card h3{color:#00d4ff;margin-bottom:14px;font-size:1rem}.lbl{color:#00d4ff;font-size:.82rem;margin-bottom:5px;margin-top:10px}input{width:100%;background:rgba(255,255,255,.1);border:1px solid rgba(255,255,255,.2);border-radius:10px;padding:11px 14px;color:#fff;font-size:.9rem}.btn{width:100%;padding:13px;border:none;border-radius:12px;font-size:.95rem;cursor:pointer;font-weight:bold;background:#00c853;color:#000;margin-top:16px}</style></head><body>";
  html += "<h2>✏️ Edit Relay Names</h2><div class='nav'><a href='/'>🏠</a><a href='/wifi'>📶 WiFi</a><a href='/mqtt'>📡 MQTT</a><a href='/relaynames'>✏️ الأسماء</a></div>";
  html += "<div class='card'><h3>🔧 Change Device Names</h3>";
  for (int i = 0; i < RELAY_COUNT; i++) {
    html += "<div class='lbl'>• Relay " + String(i+1) + ":</div><input type='text' id='r" + String(i) + "' value='" + relayNames[i] + "'>";
  }
  html += "<button class='btn' onclick='saveN()'>💾 Save New Names</button></div>";
  html += "<script>function saveN(){let url='/saveNames?key=" + String(ACCESS_KEY) + "';for(let i=0;i<6;i++)url+='&n'+i+'='+encodeURIComponent(document.getElementById('r'+i).value);fetch(url).then(r=>r.json()).then(d=>alert(d.msg));}</script></body></html>";
  return html;
}

// ==================== Routes ====================
void handleRoot()   { server.send(200, "text/html; charset=UTF-8", buildMainHTML()); }
void handleWifi()   { server.send(200, "text/html; charset=UTF-8", buildWifiHTML()); }
void handleMqtt()   { server.send(200, "text/html; charset=UTF-8", buildMqttHTML()); }
void handleNames()  { server.send(200, "text/html; charset=UTF-8", buildNamesHTML()); }

void handleRestart() {
  server.send(200, "text/html; charset=UTF-8",
    "<html><body style='background:#0f2027;color:#fff;text-align:center;padding:50px;font-family:sans-serif'>"
    "<h2>🔄 جاري إعادة التشغيل...</h2></body></html>");
  delay(1500); ESP.restart();
}

void handleToggle() {
  if (!checkAccessKey()) return;
  if (gasAlert) { server.send(403, "application/json", "{\"error\":\"gas alert - restart required\"}"); return; }
  if (server.hasArg("relay")) {
    int idx = server.arg("relay").toInt() - 1;
    if (idx >= 0 && idx < RELAY_COUNT) {
      setRelay(idx, !relayState[idx]);
      server.send(200, "application/json",
        "{\"relay\":" + String(idx+1) + ",\"state\":" + (relayState[idx]?"true":"false") + "}");
      return;
    }
  }
  server.send(400, "application/json", "{\"error\":\"invalid\"}");
}

void handlePIRToggle() {
  if (!checkAccessKey()) return;
  pirEnabled = !pirEnabled;
  if (!pirEnabled) {
    setRelay(0, false);
    setRelay(1, false);
  }
  Blynk.virtualWrite(V11, pirEnabled ? 1 : 0);
  server.send(200, "application/json", "{\"enabled\":" + String(pirEnabled?"true":"false") + "}");
}

void handleSensorUpdate() {
  if (!checkSensorKey()) return;
  if (server.hasArg("temp"))   temperature    = server.arg("temp").toFloat();
  if (server.hasArg("hum"))    humidity       = server.arg("hum").toFloat();
  if (server.hasArg("gas"))    gasValue       = server.arg("gas").toInt();
  if (server.hasArg("motion")) motionDetected = (server.arg("motion") == "1");
  if (server.hasArg("ready"))  pirReady       = (server.arg("ready") == "1");
  lastSensorUpdate  = millis();
  sensorBoardOnline = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleStatus() {
  bool fanAuto = (temperature >= tempThreshold);
  String json = "{\"gasValue\":" + String(gasValue) +
    ",\"gasAlert\":"     + (gasAlert?"true":"false") +
    ",\"temp\":"         + String(temperature, 1) +
    ",\"hum\":"          + String(humidity, 1) +
    ",\"fanAuto\":"      + (fanAuto?"true":"false") +
    ",\"motion\":"       + (motionDetected?"true":"false") +
    ",\"pirEnabled\":"   + (pirEnabled?"true":"false") +
    ",\"pirReady\":"     + (pirReady?"true":"false") +
    ",\"sensorOnline\":" + (sensorBoardOnline?"true":"false") +
    ",\"relays\":[";
  for (int i = 0; i < RELAY_COUNT; i++) {
    json += (relayState[i]?"true":"false");
    if (i < RELAY_COUNT-1) json += ",";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleSaveWifi() {
  if (!checkAccessKey()) return;
  staSSID = server.arg("ssid"); staPass = server.arg("pass");
  saveSettings();
  server.send(200, "application/json", "{\"msg\":\"تم الحفظ - جاري إعادة التشغيل\"}");
  delay(1500); ESP.restart();
}
void handleSaveAP() {
  if (!checkAccessKey()) return;
  apSSID = server.arg("name"); apPassword = server.arg("pass");
  saveSettings();
  server.send(200, "application/json", "{\"msg\":\"تم حفظ AP\"}");
}
void handleSaveMqtt() {
  if (!checkAccessKey()) return;
  mqttBroker   = server.arg("broker");
  mqttUser     = server.arg("user");
  mqttPort     = server.arg("port").toInt() ?: 1883;
  mqttPrefix   = server.arg("prefix");
  saveSettings();
  server.send(200, "application/json", "{\"msg\":\"تم حفظ MQTT - أعد التشغيل\"}");
}
void handleSaveNames() {
  if (!checkAccessKey()) return;
  for (int i = 0; i < RELAY_COUNT; i++) {
    String k = "n" + String(i);
    if (server.hasArg(k) && server.arg(k).length() > 0) relayNames[i] = server.arg(k);
  }
  saveSettings();
  server.send(200, "application/json", "{\"msg\":\"تم حفظ الأسماء\"}");
}
void handleClearSettings() {
  if (!checkAccessKey()) return;
  LittleFS.remove("/config.json");
  server.send(200, "application/json", "{\"msg\":\"تم المسح - أعد التشغيل\"}");
}

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);
  Serial.println("\n⚡ Smart Lab + Blynk IoT Starting...");

  if (!LittleFS.begin()) {
    Serial.println("❌ LittleFS فشل");
  } else {
    loadSettings();
    Serial.println("✅ الإعدادات اتحملت");
  }

  for (int i = 0; i < RELAY_COUNT; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], HIGH);
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSSID.c_str(), apPassword.c_str());
  Serial.println("📡 AP: " + apSSID + " | IP: 192.168.4.1");

  if (staSSID.length() > 0) {
    Serial.println("🔌 بيتصل بـ: " + staSSID);
    WiFi.begin(staSSID.c_str(), staPass.c_str());
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 20) {
      delay(500); Serial.print("."); tries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      staConnected = true;
      Serial.println("\n✅ WiFi متصل: " + WiFi.localIP().toString());
      mqttEnabled = (mqttBroker.length() > 3);
      Blynk.config(BLYNK_AUTH_TOKEN);
      Blynk.connect(3000);
    } else {
      Serial.println("\n⚠️ WiFi فشل — AP mode فقط");
    }
  } else {
    Serial.println("⚠️ مفيش WiFi محفوظ — اتصل بـ SmartLab وافتح 192.168.4.1/wifi");
  }

  if (mqttEnabled) mqttConnect();

  blynkTimer.setInterval(3000L, sendSensorsToBlynk);

  server.on("/",             handleRoot);
  server.on("/wifi",         handleWifi);
  server.on("/mqtt",         handleMqtt);
  server.on("/relaynames",   handleNames);
  server.on("/restart",      handleRestart);
  server.on("/toggle",       handleToggle);
  server.on("/pirToggle",    handlePIRToggle);
  server.on("/sensorUpdate", handleSensorUpdate);
  server.on("/status",       handleStatus);
  server.on("/saveWifi",     handleSaveWifi);
  server.on("/saveAP",       handleSaveAP);
  server.on("/saveMqtt",     handleSaveMqtt);
  server.on("/saveNames",    handleSaveNames);
  server.on("/clearSettings",handleClearSettings);
  server.begin();
  Serial.println("🌐 Web Server جاهز");
}

// ==================== Loop ====================
void loop() {
  if (staConnected) Blynk.run();
  blynkTimer.run();
  server.handleClient();

  // MQTT reconnect
  if (mqttEnabled && !mqttClient.connected()) {
    if (millis() - lastMqttReconnect > 5000) {
      lastMqttReconnect = millis();
      mqttConnect();
    }
  }
  if (mqttClient.connected()) mqttClient.loop();

  // Sensor board timeout
  if (sensorBoardOnline && millis() - lastSensorUpdate > sensorTimeout) {
    sensorBoardOnline = false;
    Serial.println("⚠️ بورد الحساسات offline");
  }

  // ==================== غاز → قفل كامل دائم ====================
  // لو الغاز اتكشف، فصّل كل حاجة ومنعها نهائياً لحد Restart يدوي
  if (!gasAlert) {
    if (gasValue >= gasThreshold) {
      if (++gasHighCount >= gasConfirmReadings) {
        gasAlert = true;
        allRelaysOff();
        Serial.println("🚨 غاز مكتشف! النظام مقفول بالكامل - أعد التشغيل يدوياً لإرجاعه");
        Blynk.logEvent("gas_alert", "🚨 غاز مكتشف! النظام مقفول - أعد التشغيل يدوياً");
      }
    } else {
      gasHighCount = 0;
    }
  } else {
    // النظام مقفول: امنع أي تشغيل، فضّل القفل ثابت
    allRelaysOff();
  }

  // ==================== حركة → لمبة 1 + لمبة 2 ====================
  if (!gasAlert && pirEnabled && pirReady) {
    if (motionDetected && !lastMotionState) {
      // حركة جديدة → شغّل اللمبتين
      lastMotionTime = millis();
      if (!relayState[0]) { setRelay(0, true); Serial.println("🚶 حركة! لمبة 1 اشتغلت"); }
      if (!relayState[1]) { setRelay(1, true); Serial.println("🚶 حركة! لمبة 2 اشتغلت"); }
    }
    if (!motionDetected && lastMotionState) {
      // الحركة اختفت → ابدأ عدّ التايم أوت
      lastMotionTime = millis();
    }
    // لو مفيش حركة وعدى التايم أوت → طفّي اللمبتين
    if (!motionDetected && millis() - lastMotionTime > motionTimeout) {
      if (relayState[0]) { setRelay(0, false); Serial.println("😴 لمبة 1 اتطفت"); }
      if (relayState[1]) { setRelay(1, false); Serial.println("😴 لمبة 2 اتطفت"); }
    }
    lastMotionState = motionDetected;
  }

  // ==================== حرارة → مروحة 1 + مروحة 2 ====================
  if (!gasAlert) {
    if (temperature >= tempThreshold) {
      if (!relayState[2]) { setRelay(2, true); Serial.println("🌀 مروحة 1 اشتغلت"); }
      if (!relayState[3]) { setRelay(3, true); Serial.println("🌀 مروحة 2 اشتغلت"); }
    } else if (temperature < tempThreshold - 2) {
      if (relayState[2]) { setRelay(2, false); Serial.println("🌀 مروحة 1 اتطفت"); }
      if (relayState[3]) { setRelay(3, false); Serial.println("🌀 مروحة 2 اتطفت"); }
    }
  }

  delay(100);
}
