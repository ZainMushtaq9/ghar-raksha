/*
  ============================================================================
  GHAR RAKSHA — Home Guard (ESP32 firmware)
  ----------------------------------------------------------------------------
  Kya karta hai:
  - 4 zones par nazar: Main Gate, Sehan, Kamra 1, Kamra 2
  - Sensors: PIR (HC-SR501), microwave radar (RCWL-0516),
    mmWave presence (HLK-LD2410, UART2), magnetic door sensor (MC-38)
  - Armed honay par trigger: siren ON + light ON (relay) + Telegram alert
  - Web server: dashboard is se /status mangta hai (har 1 second)
      GET  /status  -> JSON: {armed, zones:[{name,presence,motion,door_open}]}
      POST /arm     -> system armed
      POST /disarm  -> system disarmed
      (Browser se test ke liye /arm aur /disarm GET se bhi chalte hain)
  - Dashboard ko CORS allow hai, is liye file:// se khula dashboard bhi
    ESP32 se baat kar sakta hai.

  PEHLE YE 3 CHEEZEIN BHARO (neeche CONFIG section mein):
    1. WIFI_SSID / WIFI_PASSWORD — apne ghar ke WiFi ka naam + password
    2. TELEGRAM_BOT_TOKEN / TELEGRAM_CHAT_ID — alert ke liye
       (kaise banayein: neeche comment mein tareeqa likha hai)
    3. Jo sensor NAHI khareeda, us ka #define band kar do (// laga do)

  Sach baat: ek sensor sirf apna zone dekhta hai. Moti deewar ke peeche
  radar bharosa nahi karta — har kamray ko apna sensor chahiye.
  WiFi CSI is code ka hissa NAHI hai (wo Phase 2 experiment hai,
  firmware/wifi_csi_experiment/README.md dekho).
  ============================================================================
*/

#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>

// ============================================================================
// CONFIG — YAHAN APNI DETAILS BHARO
// ============================================================================
const char* WIFI_SSID     = "APNE_WIFI_KA_NAAM";      // <-- apna WiFi naam
const char* WIFI_PASSWORD = "APNA_WIFI_PASSWORD";     // <-- apna WiFi password

// --- Telegram alert (khali chhoro to alert band, baaki system chalta rahega) ---
// Token kaise lo:
//   1. Telegram mein @BotFather kholo -> /newbot -> naam do
//   2. Wo ek token dega, jaisa: 123456789:AAH.... (us ko neeche paste karo)
// Chat ID kaise lo:
//   1. Apne naye bot ko Telegram mein koi message bhejo (masalan "hi")
//   2. Browser mein kholo: https://api.telegram.org/bot<TOKEN>/getUpdates
//      (TOKEN ki jagah upar wala token)
//   3. Us mein "chat":{"id":123456789} nazar aayega — wo number neeche likho
const char* TELEGRAM_BOT_TOKEN = "APNA_BOT_TOKEN_YAHAN";   // <-- ya " " (khali) = band
const char* TELEGRAM_CHAT_ID   = "APNI_CHAT_ID_YAHAN";     // <-- ya " " (khali) = band

// --- Kaun se sensor lage hain? Jo nahi khareeda, us ki line par // laga do ---
#define USE_PIR      // PIR motion sensor HC-SR501
#define USE_RCWL     // Microwave radar RCWL-0516
#define USE_LD2410   // mmWave presence sensor HLK-LD2410 (UART2)
// Door sensors (MC-38) hamesha on hain — agar darwaza sensor nahi lagaya
// to neeche ZONES mein us zone ka door pin -1 kar do.

// --- Timing ---
const unsigned long SIREN_HOLD_MS    = 30000;  // trigger ke baad siren kitni der bajega
const unsigned long TELEGRAM_GAP_MS  = 30000;  // 2 alerts ke darmiyan kam se kam waqfa

// ============================================================================
// PINS — ESP32 DevKit V1 (30-pin). Wiring guide: docs/wiring-guide.md
// ============================================================================
#define SIREN_PIN   18   // Siren (12V siren relay/transistor ke zariye — direct 12V kabhi na lagao)
#define RELAY_PIN   19   // Relay module: bahar ki light on/off
#define BUZZER_PIN  21   // Chhota active buzzer (5V, direct pin se)

#ifdef USE_LD2410
  #define LD2410_RX_PIN 16   // ESP32 RX2 <- LD2410 TX
  #define LD2410_TX_PIN 17   // ESP32 TX2 -> LD2410 RX
  #define LD2410_ZONE   1    // LD2410 kis zone mein laga hai? 1 = Sehan
#endif

// Har zone: naam, PIR pin, RCWL pin, door pin (-1 = nahi laga)
struct ZoneCfg {
  const char* name;
  int pirPin;
  int rcwlPin;
  int doorPin;
};
const ZoneCfg ZONES[] = {
  { "Main Gate", 13, -1, 32 },   // Gate: PIR + darwaza sensor
  { "Sehan",     12, 26, -1 },   // Sehan: PIR + RCWL (+ LD2410 UART se)
  { "Kamra 1",   14, 25, 33 },   // Kamra 1: PIR + RCWL + darwaza sensor
  { "Kamra 2",   27, -1,  4 },   // Kamra 2: PIR + darwaza sensor
};
const int NUM_ZONES = sizeof(ZONES) / sizeof(ZONES[0]);

// ============================================================================
// STATE
// ============================================================================
struct ZoneState {
  bool presence = false;   // banda maujood (PIR ya RCWL ya LD2410)
  bool motion   = false;   // harkat (PIR ya RCWL)
  bool doorOpen = false;   // darwaza khula
};
ZoneState zoneState[4];
ZoneState prevState[4];

bool armed = false;
unsigned long sirenUntil = 0;       // siren kab tak bajega
unsigned long lastTelegramMs = 0;   // aakhri Telegram alert kab gaya
bool telegramConfigured = false;

WebServer server(80);

#ifdef USE_LD2410
HardwareSerial LDSerial(2);         // UART2
bool ld2410Present = false;         // LD2410 ne banda dekha?
int  ld2410DistanceCm = -1;
unsigned long ld2410LastSeen = 0;   // aakhri dafa frame kab aaya
#endif

// ============================================================================
// TELEGRAM
// ============================================================================
void sendTelegram(const String& text) {
  if (!telegramConfigured) return;
  if (millis() - lastTelegramMs < TELEGRAM_GAP_MS) return;  // spam rok
  lastTelegramMs = millis();

  WiFiClientSecure client;
  client.setInsecure();  // simple tareeqa; certificate check nahi (ghar ke use ke liye theek)
  if (!client.connect("api.telegram.org", 443)) {
    Serial.println("[Telegram] connect fail");
    return;
  }
  String body = "{\"chat_id\":\"" + String(TELEGRAM_CHAT_ID) +
                "\",\"text\":\"" + text + "\"}";
  String req = "POST /bot" + String(TELEGRAM_BOT_TOKEN) + "/sendMessage HTTP/1.1\r\n"
             + "Host: api.telegram.org\r\n"
             + "Content-Type: application/json\r\n"
             + "Content-Length: " + String(body.length()) + "\r\n"
             + "Connection: close\r\n\r\n" + body;
  client.print(req);
  // Jawab ka thora intezar (zaroori nahi, sirf debug ke liye)
  unsigned long t0 = millis();
  while (client.connected() && millis() - t0 < 3000) {
    if (client.available()) { Serial.write(client.read()); }
  }
  Serial.println("\n[Telegram] alert bheja: " + text);
}

// ============================================================================
// LD2410 PARSER (UART2, 256000 baud)
// Frame: F4 F3 F2 F1 | length(2, LE) | data | F8 F7 F6 F5
// Data (standard mode): [0]=target state (0 koi nahi, 1 moving, 2 static,
//   3 moving+static), [1..2]=moving distance cm, [3]=moving energy,
//   [4..5]=static distance cm, [6]=static energy, [7..8]=detection distance
// ============================================================================
#ifdef USE_LD2410
void parseLD2410() {
  // Poora frame buffer mein jama kar ke parse karo
  static uint8_t buf[64];
  static int len = 0;
  while (LDSerial.available()) {
    uint8_t b = LDSerial.read();
    if (len < (int)sizeof(buf)) buf[len++] = b; else len = 0;  // overflow: dobara shuru
    // Tail mil gaya? F8 F7 F6 F5
    if (len >= 4 && buf[len-4]==0xF8 && buf[len-3]==0xF7 && buf[len-2]==0xF6 && buf[len-1]==0xF5) {
      // Header dhoondo: F4 F3 F2 F1
      int h = -1;
      for (int i = 0; i + 3 < len; i++) {
        if (buf[i]==0xF4 && buf[i+1]==0xF3 && buf[i+2]==0xF2 && buf[i+3]==0xF1) { h = i; break; }
      }
      if (h >= 0 && h + 6 < len) {
        int dataLen = buf[h+4] | (buf[h+5] << 8);
        int d = h + 6;  // data shuru
        if (dataLen >= 9 && d + 9 <= len) {
          uint8_t state = buf[d];
          int movDist = buf[d+1] | (buf[d+2] << 8);
          int staDist = buf[d+4] | (buf[d+5] << 8);
          ld2410Present = (state >= 1 && state <= 3);
          ld2410DistanceCm = ld2410Present ? (state == 2 ? staDist : movDist) : -1;
          ld2410LastSeen = millis();
          Serial.printf("[LD2410] state=%d mov=%dcm sta=%dcm\n", state, movDist, staDist);
        }
      }
      len = 0;
    }
  }
  // 5 second se frame na aaye to sensor ko "nahi pata" maano (purana data na dikhao)
  if (millis() - ld2410LastSeen > 5000) ld2410Present = false;
}
#endif

// ============================================================================
// SENSORS
// ============================================================================
void readSensors() {
  for (int i = 0; i < NUM_ZONES; i++) {
    bool motion = false;
    bool presence = false;

#ifdef USE_PIR
    if (ZONES[i].pirPin >= 0 && digitalRead(ZONES[i].pirPin) == HIGH) {
      motion = true; presence = true;
    }
#endif
#ifdef USE_RCWL
    if (ZONES[i].rcwlPin >= 0 && digitalRead(ZONES[i].rcwlPin) == HIGH) {
      motion = true; presence = true;
    }
#endif
#ifdef USE_LD2410
    if (i == LD2410_ZONE && ld2410Present) {
      presence = true;   // LD2410 khara banda bhi pakarta hai (motion zaroori nahi)
    }
#endif

    bool door = false;
    if (ZONES[i].doorPin >= 0) {
      // MC-38: darwaza band = circuit band (LOW, pull-up ki wajah se),
      // darwaza khula = HIGH. (Wiring guide mein pull-up samjhaya hai.)
      door = (digitalRead(ZONES[i].doorPin) == HIGH);
      if (door) presence = true;  // khula darwaza bhi trigger hai
    }

    zoneState[i].presence = presence;
    zoneState[i].motion   = motion;
    zoneState[i].doorOpen = door;
  }
}

// ============================================================================
// ALARM LOGIC
// ============================================================================
void handleAlarm() {
  bool anyTrigger = false;
  for (int i = 0; i < NUM_ZONES; i++) {
    bool rising = (zoneState[i].presence && !prevState[i].presence);
    if (zoneState[i].presence) anyTrigger = true;
    if (rising) {
      Serial.printf("[TRIGGER] %s — presence! armed=%d\n", ZONES[i].name, armed);
      if (armed) {
        sirenUntil = millis() + SIREN_HOLD_MS;
        sendTelegram(String("ALERT! Ghar Raksha: ") + ZONES[i].name +
                     " mein banda / darwaza khula. Waqt: " +
                     String(millis() / 1000) + "s (uptime)");
      } else {
        Serial.println("  (disarmed hai — siren nahi bajega)");
      }
    }
    prevState[i] = zoneState[i];
  }
  // Siren + light: trigger ke baad SIREN_HOLD_MS tak on rehte hain
  bool sirenActive = armed && (millis() < sirenUntil);
  digitalWrite(SIREN_PIN,  sirenActive ? HIGH : LOW);
  digitalWrite(RELAY_PIN,  sirenActive ? HIGH : LOW);
  digitalWrite(BUZZER_PIN, sirenActive ? HIGH : LOW);
}

// ============================================================================
// WEB SERVER
// ============================================================================
void sendCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "*");
}

String statusJson() {
  String j = "{\"armed\":";
  j += armed ? "true" : "false";
  j += ",\"zones\":[";
  for (int i = 0; i < NUM_ZONES; i++) {
    if (i) j += ",";
    j += "{\"name\":\"" + String(ZONES[i].name) + "\"";
    j += ",\"presence\":" + String(zoneState[i].presence ? "true" : "false");
    j += ",\"motion\":"   + String(zoneState[i].motion   ? "true" : "false");
    j += ",\"door_open\":" + String(zoneState[i].doorOpen ? "true" : "false");
    j += "}";
  }
  j += "]}";
  return j;
}

void handleStatus() {
  sendCors();
  server.send(200, "application/json", statusJson());
}

void handleArm() {
  armed = true;
  Serial.println("[WEB] ARMED (dashboard/app se)");
  sendCors();
  server.send(200, "application/json", "{\"armed\":true}");
}

void handleDisarm() {
  armed = false;
  sirenUntil = 0;
  digitalWrite(SIREN_PIN, LOW);
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  Serial.println("[WEB] DISARMED");
  sendCors();
  server.send(200, "application/json", "{\"armed\":false}");
}

void handleRoot() {
  sendCors();
  server.send(200, "text/plain",
    "Ghar Raksha Home Guard chal raha hai.\n"
    "Endpoints: /status (GET), /arm (POST), /disarm (POST)\n"
    "Dashboard: dashboard/index.html kholo aur LIVE MODE dabao.");
}

void handleOptions() {  // browser ka preflight (CORS)
  sendCors();
  server.send(204);
}

// ============================================================================
// SETUP / LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== GHAR RAKSHA — Home Guard start ===");

  // Pins
  for (int i = 0; i < NUM_ZONES; i++) {
    if (ZONES[i].pirPin  >= 0) pinMode(ZONES[i].pirPin, INPUT);
    if (ZONES[i].rcwlPin >= 0) pinMode(ZONES[i].rcwlPin, INPUT);
    // Door sensor: INPUT_PULLUP — MC-38 ka aik taar pin par, doosra GND par.
    // Darwaza band = LOW, khula = HIGH (pull-up).
    if (ZONES[i].doorPin >= 0) pinMode(ZONES[i].doorPin, INPUT_PULLUP);
  }
  pinMode(SIREN_PIN, OUTPUT);  digitalWrite(SIREN_PIN, LOW);
  pinMode(RELAY_PIN, OUTPUT);  digitalWrite(RELAY_PIN, LOW);
  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);

#ifdef USE_LD2410
  LDSerial.begin(256000, SERIAL_8N1, LD2410_RX_PIN, LD2410_TX_PIN);
  Serial.println("[LD2410] UART2 256000 baud shuru (Sehan zone)");
#endif

  telegramConfigured =
    (String(TELEGRAM_BOT_TOKEN).length() > 10 &&
     String(TELEGRAM_BOT_TOKEN) != "APNA_BOT_TOKEN_YAHAN" &&
     String(TELEGRAM_CHAT_ID) != "APNI_CHAT_ID_YAHAN");
  if (!telegramConfigured) {
    Serial.println("[Telegram] token/chat ID nahi bhara — Telegram alert BAND (baaki sab chalega)");
  }

  // WiFi connect
  Serial.print("[WiFi] Connect ho raha hai: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {  // ~20 second
    delay(500); Serial.print("."); tries++;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected! IP: ");
    Serial.println(WiFi.localIP());
    Serial.println("[WiFi] Ye IP dashboard mein daalo (LIVE MODE).");
  } else {
    Serial.println("[WiFi] CONNECT NAHI HUA — SSID/password check karo. System phir bhi chalega, siren local bajega, lekin dashboard + Telegram nahi chalenge.");
  }

  // Web server
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/arm", HTTP_POST, handleArm);
  server.on("/disarm", HTTP_POST, handleDisarm);
  server.on("/arm", HTTP_GET, handleArm);       // browser se asaan test
  server.on("/disarm", HTTP_GET, handleDisarm);
  server.on("/status", HTTP_OPTIONS, handleOptions);
  server.on("/arm", HTTP_OPTIONS, handleOptions);
  server.on("/disarm", HTTP_OPTIONS, handleOptions);
  server.onNotFound([]() { sendCors(); server.send(404, "text/plain", "Nahi mila"); });
  server.begin();
  Serial.println("[WEB] Server port 80 par chal raha hai.");

  // PIR sensors ko warm-up chahiye (~30-60 sec). Is doran ghalat trigger aa sakta hai.
  Serial.println("[PIR] Warm-up: 30 second ruk jao, phir test karo.");
  delay(1000);
  Serial.println("=== Tayyar. Dashboard kholo: dashboard/index.html ===");
}

void loop() {
  server.handleClient();
#ifdef USE_LD2410
  parseLD2410();
#endif
  readSensors();
  handleAlarm();

  // WiFi toot gaya to dobara connect ki koshish (har loop mein halki check)
  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 10000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Dobara connect ki koshish...");
      WiFi.reconnect();
    }
  }

  delay(50);  // sensors ko thora waqt, CPU ko araam
}
