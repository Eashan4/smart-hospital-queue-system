/**
 * esp_code.ino
 * ESP8266 — AIoT Smart Hospital Queue & Crowd Monitor
 * ====================================================
 * FULLY STANDALONE + MQTT CLOUD PUBLISHING
 *
 * Architecture:
 *   1. All sensor logic runs 100% on ESP8266 (no backend)
 *   2. ESP8266 publishes JSON data to a free public MQTT broker
 *      (broker.emqx.io) every 2 seconds over TCP port 1883
 *   3. GitHub Pages / any deployed website subscribes to the
 *      same MQTT topic via WebSocket (WSS) for live updates
 *   4. Local web server still available on LAN (same network)
 *
 * TOKEN SYSTEM FLOW:
 *   1. Person walks through entry IR → Token PENDING (10s window)
 *   2. Person scans RFID within 10s  → Token VERIFIED (joins queue)
 *   3. If no RFID scan within 10s    → Token EXPIRED, count reverts
 *   4. Person walks through exit IR   → Oldest verified token removed
 *   5. Maximum 5 concurrent tokens
 *
 * IR EDGE DETECTION:
 *   - Only triggers on HIGH→LOW transition (falling edge)
 *   - Stationary objects do NOT re-trigger
 *   - Debounce protection (300ms)
 *
 * PINOUT (NodeMCU ESP8266):
 *   RFID RC522:
 *     SDA  → D8  (GPIO15)   SCK  → D5  (GPIO14)
 *     MOSI → D7  (GPIO13)   MISO → D6  (GPIO12)
 *     RST  → D4  (GPIO2)    3.3V → 3V3 / GND → GND
 *
 *   IR Sensor 1 (Entry): OUT → D2 (GPIO4), VCC → 3V3, GND → GND
 *   IR Sensor 2 (Exit):  OUT → D1 (GPIO5), VCC → 3V3, GND → GND
 *
 * Libraries required (Arduino Library Manager):
 *   - ESP8266WiFi          (bundled with ESP8266 board package)
 *   - ESP8266WebServer     (bundled)
 *   - SPI                  (bundled)
 *   - MFRC522              by miguelbalboa
 *   - ArduinoJson          by bblanchon (v6+)
 *   - PubSubClient         by Nick O'Leary   ← NEW for MQTT
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <MFRC522.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <LittleFS.h>
#include <Adafruit_NeoPixel.h>

// ═══════════════════════════════════════════════════════════════
//  CONFIGURATION (Overridable via .env Settings Page)
// ═══════════════════════════════════════════════════════════════
String wifiSSID = "ABCDE";
String wifiPassword = "password";

// MQTT Broker
String mqttServer = "broker.emqx.io";
int mqttPort = 1883;
String mqttTopic = "eashan4/hospital/queue/data";

// Publish interval in milliseconds (2 seconds)
const unsigned long MQTT_PUBLISH_INTERVAL = 2000;

// ═══════════════════════════════════════════════════════════════
//  PIN DEFINITIONS
// ═══════════════════════════════════════════════════════════════
#define RST_PIN D0  // RFID RST  → GPIO16
#define SS_PIN D8   // RFID SDA  → GPIO15
#define IR_ENTRY D2 // IR entry  → GPIO4  (active LOW)
#define IR_EXIT D1  // IR exit   → GPIO5  (active LOW)

// ═══════════════════════════════════════════════════════════════
//  WS2812B LED PINS
// ═══════════════════════════════════════════════════════════════
#define LED_PATIENT_PIN D3 // GPIO0   - 5 Patient LEDs
#define LED_RFID_PIN D4    // GPIO2   - 4 RFID LEDs
#define LED_IR_PIN 3       // GPIO3 (RX) - 2 Entry IR LEDs

// ═══════════════════════════════════════════════════════════════
//  CONSTANTS & THRESHOLDS
// ═══════════════════════════════════════════════════════════════
#define IR_DEBOUNCE_MS 300
#define HISTORY_SIZE 50
#define AVG_SERVICE_TIME_MIN 2.5
#define ANOMALY_Z_THRESHOLD 2.0
#define MAX_TOKENS 5
#define TOKEN_TIMEOUT_MS 10000 // 10s RFID window

// ═══════════════════════════════════════════════════════════════
//  TOKEN SYSTEM
// ═══════════════════════════════════════════════════════════════
struct Token {
  bool active;
  bool verified;
  unsigned long createdAt;
  String rfidUid;
  int tokenId;
};

Token tokens[MAX_TOKENS];
int nextTokenId = 1;

// ═══════════════════════════════════════════════════════════════
//  GLOBAL STATE
// ═══════════════════════════════════════════════════════════════
ESP8266WebServer server(80);
MFRC522 rfid(SS_PIN, RST_PIN);

// LED STRIPS
Adafruit_NeoPixel stripPatient(5, LED_PATIENT_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel stripRFID(4, LED_RFID_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel stripIR(2, LED_IR_PIN, NEO_GRB + NEO_KHZ800);

int ledBrightness = 255;
enum RfidLedState { RFID_NORMAL, RFID_FLASH_ACCEPT, RFID_FLASH_REJECT };
RfidLedState rfidLedState = RFID_NORMAL;
unsigned long rfidLedTimer = 0;
uint32_t patientColors[5];

// MQTT
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
unsigned long lastPublishTime = 0;
unsigned long lastMqttAttempt = 0;
bool mqttConnected = false;

String lastRfidTag = "NONE";
unsigned long bootTime = 0;

// Edge-detection state
bool prevEntryState = HIGH;
bool prevExitState = HIGH;
unsigned long lastEntryTime = 0;
unsigned long lastExitTime = 0;

// History ring buffer
struct DataPoint {
  int people;
  int queue;
  float waitTime;
};
DataPoint history[HISTORY_SIZE];
int historyIndex = 0;
int historyCount = 0;

// ML values
float predictedWait = 0.0;
bool isAnomaly = false;
float anomalyZScore = 0.0;
float rollingAvgPeople = 0.0;
float entryVelocity = 0.0;
float queuePressure = 0.0;

// Session statistics
unsigned long totalEntries = 0;
unsigned long totalExits = 0;
unsigned long totalRfidScans = 0;
unsigned long totalExpired = 0;

// ═══════════════════════════════════════════════════════════════
//  LED & ENV CONFIGURATION LOAD/SAVE
// ═══════════════════════════════════════════════════════════════
void loadEnv() {
  if (LittleFS.begin()) {
    if (LittleFS.exists("/env.json")) {
      File file = LittleFS.open("/env.json", "r");
      StaticJsonDocument<512> doc;
      if (!deserializeJson(doc, file)) {
        if (doc.containsKey("brightness")) ledBrightness = doc["brightness"];
        if (doc.containsKey("ssid")) wifiSSID = doc["ssid"].as<String>();
        if (doc.containsKey("pwd")) wifiPassword = doc["pwd"].as<String>();
        if (doc.containsKey("mqtt_srv")) mqttServer = doc["mqtt_srv"].as<String>();
        if (doc.containsKey("mqtt_prt")) mqttPort = doc["mqtt_prt"];
        if (doc.containsKey("mqtt_top")) mqttTopic = doc["mqtt_top"].as<String>();
      }
      file.close();
    }
  }
}

void saveEnv() {
  File file = LittleFS.open("/env.json", "w");
  StaticJsonDocument<512> doc;
  doc["brightness"] = ledBrightness;
  doc["ssid"] = wifiSSID;
  doc["pwd"] = wifiPassword;
  doc["mqtt_srv"] = mqttServer;
  doc["mqtt_prt"] = mqttPort;
  doc["mqtt_top"] = mqttTopic;
  serializeJson(doc, file);
  file.close();
}

void updatePatientLEDs() {
  unsigned long now = millis();
  for(int i = 0; i < MAX_TOKENS; i++) {
    if (tokens[i].active) {
      if (tokens[i].verified) {
        stripPatient.setPixelColor(i, patientColors[i]);
      } else { // Pending
        unsigned long elapsed = now - tokens[i].createdAt;
        unsigned long remaining = (elapsed < TOKEN_TIMEOUT_MS) ? (TOKEN_TIMEOUT_MS - elapsed) : 0;
        if (remaining > 6000) {
          stripPatient.setPixelColor(i, stripPatient.Color(255, 255, 0)); // Yellow
        } else if (remaining > 3000) {
          stripPatient.setPixelColor(i, stripPatient.Color(255, 128, 0)); // Orange
        } else {
          // Pulse Red
          if ((now / 200) % 2 == 0) stripPatient.setPixelColor(i, stripPatient.Color(255, 0, 0));
          else stripPatient.setPixelColor(i, stripPatient.Color(50, 0, 0));
        }
      }
    } else {
      stripPatient.setPixelColor(i, stripPatient.Color(0, 0, 0)); // Empty
    }
  }
}

void flagRfidAccept() {
  rfidLedState = RFID_FLASH_ACCEPT;
  rfidLedTimer = millis();
}

void flagRfidReject() {
  rfidLedState = RFID_FLASH_REJECT;
  rfidLedTimer = millis();
}

void processAnimations() {
  unsigned long now = millis();

  // 1. Patient LEDs
  updatePatientLEDs();
  stripPatient.show();

  // 2. IR Entry Continuous Feedback (Mirrors physical state)
  if (digitalRead(IR_ENTRY) == LOW) {
    for(int i = 0; i < 2; i++) stripIR.setPixelColor(i, stripIR.Color(200, 255, 255));
  } else {
    stripIR.clear();
  }
  stripIR.show();

  // 3. RFID Visual State Transitions
  if (rfidLedState == RFID_FLASH_ACCEPT) {
    if (now - rfidLedTimer > 1500) rfidLedState = RFID_NORMAL;
    else for(int i=0; i<4; i++) stripRFID.setPixelColor(i, stripRFID.Color(0, 255, 0));
  } else if (rfidLedState == RFID_FLASH_REJECT) {
    if (now - rfidLedTimer > 1500) rfidLedState = RFID_NORMAL;
    else for(int i=0; i<4; i++) stripRFID.setPixelColor(i, stripRFID.Color(255, 0, 0));
  } else {
    // Normal Mode -> Check pending count
    if (getPendingCount() > 0) {
      int glow = 100 + (int)(sin(now / 200.0) * 100);
      glow = constrain(glow, 0, 255);
      for(int i=0; i<4; i++) stripRFID.setPixelColor(i, stripRFID.Color(0, 0, glow)); // Pulse Blue
    } else {
      stripRFID.clear();
    }
  }
  stripRFID.show();
}

// ═══════════════════════════════════════════════════════════════
//  TOKEN MANAGEMENT
// ═══════════════════════════════════════════════════════════════
void initTokens() {
  for (int i = 0; i < MAX_TOKENS; i++) {
    tokens[i] = {false, false, 0, "", 0};
  }
}

int getActiveCount() {
  int c = 0;
  for (int i = 0; i < MAX_TOKENS; i++)
    if (tokens[i].active)
      c++;
  return c;
}
int getPendingCount() {
  int c = 0;
  for (int i = 0; i < MAX_TOKENS; i++)
    if (tokens[i].active && !tokens[i].verified)
      c++;
  return c;
}
int getVerifiedCount() {
  int c = 0;
  for (int i = 0; i < MAX_TOKENS; i++)
    if (tokens[i].active && tokens[i].verified)
      c++;
  return c;
}

int findFreeSlot() {
  for (int i = 0; i < MAX_TOKENS; i++)
    if (!tokens[i].active)
      return i;
  return -1;
}

int findOldestPending() {
  int oldest = -1;
  unsigned long t = 0xFFFFFFFF;
  for (int i = 0; i < MAX_TOKENS; i++)
    if (tokens[i].active && !tokens[i].verified && tokens[i].createdAt < t) {
      t = tokens[i].createdAt;
      oldest = i;
    }
  return oldest;
}

int findOldestVerified() {
  int oldest = -1;
  unsigned long t = 0xFFFFFFFF;
  for (int i = 0; i < MAX_TOKENS; i++)
    if (tokens[i].active && tokens[i].verified && tokens[i].createdAt < t) {
      t = tokens[i].createdAt;
      oldest = i;
    }
  return oldest;
}

bool createToken() {
  int slot = findFreeSlot();
  if (slot == -1)
    return false;
  tokens[slot] = {true, false, millis(), "", nextTokenId++};
  return true;
}

bool verifyOldestPending(const String &uid) {
  int idx = findOldestPending();
  if (idx == -1)
    return false;
  tokens[idx].verified = true;
  tokens[idx].rfidUid = uid;
  return true;
}

void removeToken(int idx) {
  if (idx < 0 || idx >= MAX_TOKENS)
    return;
  tokens[idx] = {false, false, 0, "", 0};
}

void checkTokenTimeouts() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_TOKENS; i++) {
    if (tokens[i].active && !tokens[i].verified &&
        (now - tokens[i].createdAt) > TOKEN_TIMEOUT_MS) {
      Serial.printf("[TOKEN] #%d EXPIRED\n", tokens[i].tokenId);
      removeToken(i);
      totalExpired++;
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  WiFi CONNECTION
// ═══════════════════════════════════════════════════════════════
void connectWiFi() {
  Serial.print("\n[WiFi] Connecting to: ");
  Serial.println(wifiSSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

  int attempts = 0;
  bool toggle = false;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    toggle = !toggle;
    if (toggle) {
        for(int i=0; i<5; i++) stripPatient.setPixelColor(i, stripPatient.Color(0,0,255));
        for(int i=0; i<4; i++) stripRFID.setPixelColor(i, stripRFID.Color(0,0,255));
        for(int i=0; i<2; i++) stripIR.setPixelColor(i, stripIR.Color(0,0,255));
    } else {
        stripPatient.clear(); stripRFID.clear(); stripIR.clear();
    }
    stripPatient.show(); stripRFID.show(); stripIR.show();

    Serial.print(".");
    if (++attempts > 40) {
      Serial.println("\n[WiFi] Timeout. Restarting...");
      delay(1000);
      ESP.restart();
    }
  }

  stripPatient.clear(); stripRFID.clear(); stripIR.clear();
  stripPatient.show(); stripRFID.show(); stripIR.show();

  Serial.println();
  Serial.println("+==========================================+");
  Serial.println("|  AIoT Smart Hospital — ESP8266 Ready     |");
  Serial.println("+==========================================+");
  Serial.print("|  Local IP:  http://");
  Serial.print(WiFi.localIP());
  Serial.println("   |");
  Serial.printf("|  MQTT Topic: %-28s|\n", mqttTopic.c_str());
  Serial.println("+==========================================+\n");
}

// ═══════════════════════════════════════════════════════════════
//  MQTT — CONNECT & RECONNECT
// ═══════════════════════════════════════════════════════════════
void connectMQTT() {
  // Throttle reconnect attempts to once every 5 seconds
  if (millis() - lastMqttAttempt < 5000)
    return;
  lastMqttAttempt = millis();

  // Generate unique client ID from chip ID
  String clientId = "esp8266hosp_" + String(ESP.getChipId(), HEX);

  Serial.printf("[MQTT] Connecting to %s:%d as %s...\n", mqttServer.c_str(), mqttPort,
                clientId.c_str());

  if (mqttClient.connect(clientId.c_str())) {
    mqttConnected = true;
    Serial.println("[MQTT] ✓ Connected!");
    Serial.printf("[MQTT] Publishing to topic: %s\n", mqttTopic.c_str());
  } else {
    mqttConnected = false;
    Serial.printf("[MQTT] ✗ Failed. State: %d — will retry in 5s\n",
                  mqttClient.state());
  }
}

// ═══════════════════════════════════════════════════════════════
//  RFID READER
// ═══════════════════════════════════════════════════════════════
String readRFID() {
  if (!rfid.PICC_IsNewCardPresent())
    return "";
  if (!rfid.PICC_ReadCardSerial())
    return "";

  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10)
      uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  Serial.print("[RFID] Tag: ");
  Serial.println(uid);
  return uid;
}

// ═══════════════════════════════════════════════════════════════
//  ML PREDICTION ENGINE (on-device)
// ═══════════════════════════════════════════════════════════════
void addToHistory(int people, int queue, float wait) {
  history[historyIndex] = {people, queue, wait};
  historyIndex = (historyIndex + 1) % HISTORY_SIZE;
  if (historyCount < HISTORY_SIZE)
    historyCount++;
}

void runPredictionEngine() {
  int currentPeople = getActiveCount();
  int currentQueue = getVerifiedCount();
  float rawWait = currentQueue * AVG_SERVICE_TIME_MIN;

  addToHistory(currentPeople, currentQueue, rawWait);

  // Rolling average (last 5)
  float sumP = 0;
  int win = min(historyCount, 5);
  for (int i = 0; i < win; i++)
    sumP +=
        history[(historyIndex - 1 - i + HISTORY_SIZE) % HISTORY_SIZE].people;
  rollingAvgPeople = (win > 0) ? sumP / win : (float)currentPeople;

  // Entry velocity
  if (historyCount >= 2) {
    int prev = (historyIndex - 2 + HISTORY_SIZE) % HISTORY_SIZE;
    entryVelocity = (float)(currentPeople - history[prev].people);
  } else
    entryVelocity = 0;

  // Queue pressure
  queuePressure = (float)currentQueue / (float)(currentPeople + 1);

  // Historical average wait
  float avgWait = 0;
  if (historyCount > 0) {
    float s = 0;
    for (int i = 0; i < historyCount; i++)
      s += history[i].waitTime;
    avgWait = s / historyCount;
  }

  // Weighted prediction
  predictedWait =
      0.6f * rawWait + 0.3f * avgWait + 0.1f * (queuePressure * 5.0f);
  if (entryVelocity > 0)
    predictedWait += entryVelocity * 0.5f;
  if (predictedWait < 0)
    predictedWait = 0;

  // Anomaly detection (Z-score)
  if (historyCount >= 5) {
    float mean = 0, sumSq = 0;
    for (int i = 0; i < historyCount; i++)
      mean += history[i].people;
    mean /= historyCount;
    for (int i = 0; i < historyCount; i++) {
      float d = history[i].people - mean;
      sumSq += d * d;
    }
    float std = sqrt(sumSq / historyCount);
    if (std > 0.01f) {
      anomalyZScore = fabs((float)currentPeople - mean) / std;
      isAnomaly = (anomalyZScore > ANOMALY_Z_THRESHOLD);
    } else {
      anomalyZScore = 0;
      isAnomaly = false;
    }
  } else {
    anomalyZScore = 0;
    isAnomaly = false;
  }
}

// ═══════════════════════════════════════════════════════════════
//  BUILD JSON PAYLOAD (shared by MQTT + API endpoint)
// ═══════════════════════════════════════════════════════════════
String buildJSON() {
  StaticJsonDocument<2048> doc;

  int people = getActiveCount();
  int queue = getVerifiedCount();
  int pending = getPendingCount();

  doc["people_count"] = people;
  doc["queue_length"] = queue;
  doc["pending_count"] = pending;
  doc["rfid_tag"] = lastRfidTag;
  doc["predicted_wait"] = round(predictedWait * 100.0) / 100.0;
  doc["anomaly"] = isAnomaly;
  doc["anomaly_z_score"] = round(anomalyZScore * 100.0) / 100.0;
  doc["rolling_avg_people"] = round(rollingAvgPeople * 100.0) / 100.0;
  doc["entry_velocity"] = entryVelocity;
  doc["queue_pressure"] = round(queuePressure * 100.0) / 100.0;
  doc["total_entries"] = totalEntries;
  doc["total_exits"] = totalExits;
  doc["total_rfid_scans"] = totalRfidScans;
  doc["total_expired"] = totalExpired;
  doc["history_count"] = historyCount;
  doc["uptime_sec"] = (millis() - bootTime) / 1000;
  doc["model_status"] = (historyCount >= 5) ? "ready" : "warming_up";

  JsonArray tokenArr = doc.createNestedArray("tokens");
  unsigned long now = millis();
  for (int i = 0; i < MAX_TOKENS; i++) {
    JsonObject t = tokenArr.createNestedObject();
    t["slot"] = i;
    t["active"] = tokens[i].active;
    t["verified"] = tokens[i].verified;
    t["token_id"] = tokens[i].tokenId;
    t["rfid"] = tokens[i].rfidUid;
    if (tokens[i].active && !tokens[i].verified) {
      unsigned long e = now - tokens[i].createdAt;
      t["remaining_ms"] =
          (e < TOKEN_TIMEOUT_MS) ? (int)(TOKEN_TIMEOUT_MS - e) : 0;
    } else
      t["remaining_ms"] = 0;
  }

  String out;
  serializeJson(doc, out);
  return out;
}

// ═══════════════════════════════════════════════════════════════
//  MQTT PUBLISH
// ═══════════════════════════════════════════════════════════════
void publishMQTT() {
  if (!mqttClient.connected())
    return;

  String payload = buildJSON();
  bool ok = mqttClient.publish(mqttTopic.c_str(), payload.c_str(), false);

  if (ok) {
    Serial.printf("[MQTT] ✓ Published %d bytes to %s\n", payload.length(),
                  mqttTopic.c_str());
  } else {
    Serial.println("[MQTT] ✗ Publish failed (payload may be too large)");
    // If payload too large, reduce MAX_TOKENS or HISTORY_SIZE
  }
}

// ═══════════════════════════════════════════════════════════════
//  WEB SERVER — LOCAL API (LAN access)
// ═══════════════════════════════════════════════════════════════
void handleAPI() {
  String json = buildJSON();
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

// ═══════════════════════════════════════════════════════════════
//  WEB SERVER — LOCAL DASHBOARD (redirects to GitHub Pages or
//  shows local info page if no network-wide access needed)
// ═══════════════════════════════════════════════════════════════
void handleRoot() {
  String html = R"(<!DOCTYPE html><html><head>
<meta charset='UTF-8'/>
<meta name='viewport' content='width=device-width,initial-scale=1'/>
<title>ESP8266 Hospital Monitor</title>
<style>
body{font-family:system-ui,sans-serif;background:#060910;color:#dce8ff;
  display:flex;flex-direction:column;align-items:center;justify-content:center;
  min-height:100vh;margin:0;text-align:center;padding:20px}
.box{background:#0f1623;border:1px solid rgba(255,255,255,0.08);border-radius:16px;
  padding:40px;max-width:500px;width:100%}
h1{font-size:1.4rem;margin-bottom:8px}
.hl{color:#4f8ef7}
p{color:#7d9abd;font-size:0.85rem;margin:8px 0}
.ip{font-family:monospace;background:#060910;padding:6px 14px;border-radius:8px;
  color:#00d4ff;font-size:1rem;display:inline-block;margin:12px 0}
.tag{background:rgba(79,142,247,0.1);color:#7eb3ff;border:1px solid rgba(79,142,247,0.2);
  border-radius:6px;padding:3px 10px;font-size:0.75rem;display:inline-block;margin:4px}
a{color:#4f8ef7;text-decoration:none}
a:hover{text-decoration:underline}
.mono{font-family:monospace;color:#00d97e;font-size:0.8rem}
</style></head><body>
<div class='box'>
  <h1>🏥 <span class='hl'>Smart Hospital</span><br/>ESP8266 Monitor</h1>
  <p>Device is online and publishing data via MQTT</p>
  <p><strong>Local IP:</strong></p>
  <div class='ip'>)";
  html += WiFi.localIP().toString();
  html += R"(</div>
  <p><strong>MQTT Topic:</strong></p>
  <p class='mono'>)";
  html += mqttTopic;
  html += R"(</p>
  <p style='margin-top:20px'>
    <span class='tag'>📡 MQTT Publishing</span>
    <span class='tag'>🌐 GitHub Pages</span>
    <span class='tag'>🤖 AI On-Device</span>
  </p>
  <p style='margin-top:20px;font-size:0.8rem'>
    View live dashboard on deployed GitHub Pages<br/>
    or access local API at:<br/>
    <a href='/api'>/api</a> — raw JSON data<br/>
    <a href='/settings'>/settings</a> — LED Config
  </p>
</div>
</body></html>)";

  server.send(200, "text/html", html);
}

void handleSettings() {
  String html = R"(<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>ESP8266 Settings</title><style>
body{font-family:system-ui,sans-serif;background:#060910;color:#dce8ff;display:flex;justify-content:center;padding:20px;}
.box{background:#0f1623;padding:30px;border-radius:12px;width:100%;max-width:450px;border:1px solid rgba(255,255,255,0.08);}
h2{margin-top:0;color:#4f8ef7;margin-bottom:20px;} 
label{font-size:0.85rem;color:#7d9abd;font-weight:600;display:block;margin-top:15px;}
input{width:100%;box-sizing:border-box;padding:12px;margin:8px 0;border-radius:8px;border:1px solid #1f2937;background:#0d1220;color:#fff;font-family:monospace;}
input:focus{outline:none;border-color:#4f8ef7;}
button{width:100%;padding:14px;background:#4f8ef7;color:#fff;font-weight:bold;font-size:1rem;border:none;border-radius:8px;cursor:pointer;margin-top:25px;transition:0.2s;}
button:hover{background:#3b7be8;}
a{color:#7d9abd;text-decoration:none;font-size:0.85rem;display:inline-block;margin-top:20px;}
.sect{border-bottom:1px solid #1f2937;padding-bottom:15px;margin-bottom:15px;}
.small{color:#7d9abd;font-size:0.75rem;display:block;margin-bottom:10px;}
</style></head><body><div class='box'>
<h2>⚙️ Global Configuration</h2>
<form action='/save_settings' method='GET'>

<div class='sect'>
<label>LED Brightness (0-255)</label>
<input type='number' name='b' min='0' max='255' value=')" + String(ledBrightness) + R"(' required/>
</div>

<div class='sect'>
<label>WiFi SSID</label>
<input type='text' name='ssid' value=')" + wifiSSID + R"(' required/>
<label>WiFi Password</label>
<input type='text' name='pwd' value=')" + wifiPassword + R"(' required/>
</div>

<div class='sect'>
<label>MQTT Broker</label>
<input type='text' name='mqtt_srv' value=')" + mqttServer + R"(' required/>
<label>MQTT Port</label>
<input type='number' name='mqtt_prt' value=')" + String(mqttPort) + R"(' required/>
<label>MQTT Topic</label>
<input type='text' name='mqtt_top' value=')" + mqttTopic + R"(' required/>
</div>

<span class='small'>Saving network changes will reboot the device.</span>
<button type='submit'>Save & Apply Settings</button>
</form>
<a href='/'>&larr; Back to Dashboard</a>
</div></body></html>)";

  server.send(200, "text/html", html);
}

void handleSaveSettings() {
  bool needsReboot = false;
  if (server.hasArg("b")) ledBrightness = server.arg("b").toInt();
  if (server.hasArg("ssid") && server.arg("ssid") != wifiSSID) { wifiSSID = server.arg("ssid"); needsReboot = true; }
  if (server.hasArg("pwd") && server.arg("pwd") != wifiPassword) { wifiPassword = server.arg("pwd"); needsReboot = true; }
  if (server.hasArg("mqtt_srv") && server.arg("mqtt_srv") != mqttServer) { mqttServer = server.arg("mqtt_srv"); needsReboot = true; }
  if (server.hasArg("mqtt_prt")) mqttPort = server.arg("mqtt_prt").toInt();
  if (server.hasArg("mqtt_top")) mqttTopic = server.arg("mqtt_top");

  saveEnv();
  
  stripPatient.setBrightness(ledBrightness);
  stripRFID.setBrightness(ledBrightness);
  stripIR.setBrightness(ledBrightness);

  if (needsReboot) {
    String html = R"(<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
    <title>Rebooting</title><style>
    body{font-family:system-ui;background:#060910;color:#00d97e;display:flex;align-items:center;justify-content:center;height:100vh;text-align:center;}
    .box{background:#0f1623;padding:40px;border-radius:12px;border:1px solid rgba(0,217,126,0.3);}
    </style></head><body><div class='box'>
    <h2>✅ Settings Saved</h2>
    <p style='color:#dce8ff;'>Applying network changes... Device is rebooting.</p>
    </div></body></html>)";
    server.send(200, "text/html", html);
    delay(1000);
    ESP.restart();
  } else {
    server.sendHeader("Location", "/settings");
    server.send(302, "text/plain", "");
  }
}

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY); // Enables GPIO3 (RX) for LED output
  delay(300);
  bootTime = millis();

  Serial.println("\n==========================================");
  Serial.println("  AIoT Smart Hospital — ESP8266");
  Serial.println("  MQTT Cloud Mode + Local Web Server");
  Serial.println("==========================================\n");

  initTokens();
  loadEnv();

  // Initialize WS2812B strips
  stripPatient.begin(); stripPatient.setBrightness(ledBrightness); stripPatient.show();
  stripRFID.begin();    stripRFID.setBrightness(ledBrightness);    stripRFID.show();
  stripIR.begin();      stripIR.setBrightness(ledBrightness);      stripIR.show();
  
  // Vibrant patient colors automatically assigned
  patientColors[0] = stripPatient.Color(0, 255, 255); // Cyan
  patientColors[1] = stripPatient.Color(0, 128, 255); // Blue
  patientColors[2] = stripPatient.Color(128, 0, 255); // Purple
  patientColors[3] = stripPatient.Color(255, 0, 128); // Magenta
  patientColors[4] = stripPatient.Color(0, 255, 128); // Green

  pinMode(IR_ENTRY, INPUT_PULLUP);
  pinMode(IR_EXIT, INPUT_PULLUP);
  prevEntryState = digitalRead(IR_ENTRY);
  prevExitState = digitalRead(IR_EXIT);

  SPI.begin();
  rfid.PCD_Init();
  delay(100);
  rfid.PCD_DumpVersionToSerial();

  connectWiFi();

  // Configure MQTT
  mqttClient.setServer(mqttServer.c_str(), mqttPort);
  // Set max packet size to handle our JSON payload
  mqttClient.setBufferSize(2048);

  // Initial MQTT connect
  connectMQTT();

  // Local web server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api", HTTP_GET, handleAPI);
  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/save_settings", HTTP_GET, handleSaveSettings);
  server.begin();

  Serial.println("[SERVER] Local web server started on port 80");
  Serial.printf("[MQTT]   Topic: %s\n", mqttTopic.c_str());
  Serial.println("[TOKEN]  Max tokens: 5  |  Timeout: 10s");
  Serial.println();
}

// ═══════════════════════════════════════════════════════════════
//  MAIN LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
  server.handleClient();
  unsigned long now = millis();

  // ── MQTT reconnect & loop ───────────────────────────────────
  if (!mqttClient.connected()) {
    mqttConnected = false;
    connectMQTT();
  }
  mqttClient.loop();

  // ── IR ENTRY — Falling-edge (HIGH → LOW) ───────────────────
  bool curEntry = digitalRead(IR_ENTRY);
  if (curEntry == LOW && prevEntryState == HIGH &&
      (now - lastEntryTime) > IR_DEBOUNCE_MS) {
    lastEntryTime = now;
    if (createToken()) {
      totalEntries++;
      Serial.printf("[IR] ENTRY → Token #%d | people=%d queue=%d pending=%d\n",
                    nextTokenId - 1, getActiveCount(), getVerifiedCount(),
                    getPendingCount());
      runPredictionEngine();
    } else {
      Serial.println("[IR] ENTRY: all 5 token slots full — ignored");
    }
  }
  prevEntryState = curEntry;

  // ── IR EXIT — Falling-edge (HIGH → LOW) ────────────────────
  bool curExit = digitalRead(IR_EXIT);
  if (curExit == LOW && prevExitState == HIGH &&
      (now - lastExitTime) > IR_DEBOUNCE_MS) {
    lastExitTime = now;
    int idx = findOldestVerified();
    if (idx == -1)
      idx = findOldestPending();
    if (idx != -1) {
      bool wasV = tokens[idx].verified;
      int tid = tokens[idx].tokenId;
      removeToken(idx);
      totalExits++;
      Serial.printf(
          "[IR] EXIT  → Removed token #%d (%s) | people=%d queue=%d\n", tid,
          wasV ? "verified" : "pending", getActiveCount(), getVerifiedCount());
      runPredictionEngine();
    } else {
      Serial.println("[IR] EXIT: no active tokens to remove");
    }
  }
  prevExitState = curExit;

  // ── Token timeouts ──────────────────────────────────────────
  checkTokenTimeouts();

  // ── RFID scan ───────────────────────────────────────────────
  String tag = readRFID();
  if (tag.length() > 0) {
    lastRfidTag = tag;
    totalRfidScans++;
    if (verifyOldestPending(tag)) {
      flagRfidAccept(); // Flash Green on verification success
      Serial.printf("[RFID] VERIFIED with %s | queue=%d\n", tag.c_str(),
                    getVerifiedCount());
    } else {
      flagRfidReject(); // Flash Red on validation failure (no tokens bounds)
      Serial.printf("[RFID] %s — no pending token to verify\n", tag.c_str());
    }
    runPredictionEngine();
  }

  // ── MQTT publish every 2 seconds ───────────────────────────
  if (mqttClient.connected() &&
      (now - lastPublishTime) >= MQTT_PUBLISH_INTERVAL) {
    lastPublishTime = now;
    publishMQTT();
  }

  // ── Global Synchronous LED Animations ───────────────────────
  processAnimations();

  yield(); // feed watchdog
}
