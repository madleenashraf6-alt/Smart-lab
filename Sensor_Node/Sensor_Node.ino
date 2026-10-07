/*
  ============================================================
   بورد 2 - بورد الحساسات
   - DHT11 (حرارة + رطوبة)
   - MQ-2  (غاز)
   - PIR   (حركة)
   - يتصل بشبكة بورد 1 كـ Station
   - يبعت البيانات لبورد 1 كل 2 ثانية عبر HTTP GET
  ============================================================

  المكتبات المطلوبة:
    - DHT sensor library by Adafruit
    - Adafruit Unified Sensor
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <DHT.h>

// ==================== بيانات شبكة بورد 1 ====================
// ملاحظة: لو غيّرت اسم AP من بورد 1، عدّله هنا كمان
const char* ssid           = "SmartLab";
const char* password       = "12345678";
const char* MAIN_BOARD_IP  = "192.168.4.1";
const char* SENSOR_KEY     = "sensorNode2026";

// ==================== Pins ====================
#define GAS_PIN  A0    // MQ-2 (Analog)
#define DHT_PIN  15    // D8 - DHT11
#define DHT_TYPE DHT11
#define PIR_PIN  2     // D4 - PIR

DHT dht(DHT_PIN, DHT_TYPE);

// ==================== متغيرات ====================
float temperature    = 0;
float humidity       = 0;
int   gasValue       = 0;
bool  motionDetected = false;

bool  pirReady       = false;
unsigned long pirStartTime  = 0;
const unsigned long pirWarmupTime = 30000; // 30 ثانية للاستقرار

unsigned long lastDHTRead   = 0;
unsigned long lastSendTime  = 0;
const unsigned long sendInterval = 2000;

int dhtFailCount = 0;

// ==================== WiFi ====================
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.println("🔌 بيتصل بـ SmartLab AP...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500); Serial.print("."); attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ اتصل! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n❌ فشل الاتصال - سيحاول تاني");
  }
}

// ==================== إرسال البيانات ====================
void sendDataToMain() {
  if (WiFi.status() != WL_CONNECTED) { connectWiFi(); return; }

  WiFiClient client;
  HTTPClient http;

  String url = "http://";
  url += MAIN_BOARD_IP;
  url += "/sensorUpdate?key=";
  url += SENSOR_KEY;
  url += "&temp=" + String(temperature, 1);
  url += "&hum="  + String(humidity, 1);
  url += "&gas="  + String(gasValue);
  url += "&motion=" + String(motionDetected ? "1" : "0");
  url += "&ready="  + String(pirReady ? "1" : "0");

  http.begin(client, url);
  int code = http.GET();
  if (code > 0) {
    Serial.println("📤 تم الإرسال - رد: " + String(code));
  } else {
    Serial.println("⚠️ فشل الإرسال: " + http.errorToString(code));
  }
  http.end();
}

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);
  Serial.println("\n⚡ بورد الحساسات يبدأ...");

  pinMode(PIR_PIN, INPUT);
  pirStartTime = millis();
  Serial.println("⏳ PIR بيستقر (30 ثانية)...");

  dht.begin();
  delay(2000);
  Serial.println("✅ DHT11 + MQ-2 جاهزين");

  connectWiFi();
}

// ==================== Loop ====================
void loop() {
  // إعادة الاتصال لو الشبكة قطعت
  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  // استقرار PIR
  if (!pirReady && millis() - pirStartTime >= pirWarmupTime) {
    pirReady = true;
    Serial.println("✅ PIR جاهز!");
  }

  // قراءة PIR
  motionDetected = pirReady && (digitalRead(PIR_PIN) == HIGH);

  // قراءة DHT11
  if (millis() - lastDHTRead >= 2000) {
    lastDHTRead = millis();
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      temperature = t;
      humidity    = h;
      dhtFailCount = 0;
      Serial.println("🌡️ " + String(temperature, 1) + "°C | 💧" + String(humidity, 1) + "%");
    } else {
      dhtFailCount++;
      if (dhtFailCount >= 5) Serial.println("⚠️ فشل متكرر في DHT11 - تحقق من التوصيل");
    }
  }

  // قراءة MQ-2
  gasValue = analogRead(GAS_PIN);

  // إرسال كل 2 ثانية
  if (millis() - lastSendTime >= sendInterval) {
    lastSendTime = millis();
    sendDataToMain();
  }

  delay(200);
}
