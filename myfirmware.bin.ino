// ===================================================================
// --- LIBRARIES ---
// ===================================================================
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <WiFiManager.h>
#include <MikroTik.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ===================================================================
// --- SECURITY & HARDWARE CONFIGURATION ---
// ===================================================================
const char* AP_PASSWORD = "password123";
const char* ADMIN_USER = "admin";
const char* ADMIN_PASS = "admin_password";

#define COIN_PIN 14
#define PULSE_TIMEOUT 5000
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define API_PORT 8728

// ===================================================================
// --- GLOBAL OBJECTS & STATE VARIABLES ---
// ===================================================================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
ESP8266WebServer server(80);
WiFiClient client;
MikroTik api(client);
volatile int pulse_count = 0;
unsigned long last_pulse_time = 0;
bool is_counting_pulses = false;

struct Config {
  char mikrotik_host[40];
  char mikrotik_user[40];
  char mikrotik_pass[40];
  struct Rate { int pulses; char profile[40]; } rates[5];
};
Config config;

// ===================================================================
// --- UTILITY & CONFIG FUNCTIONS ---
// ===================================================================

void ICACHE_RAM_ATTR coinPulse() {
  pulse_count++;
  last_pulse_time = millis();
  is_counting_pulses = true;
}

void displayMessage(String text, int size, bool clear) {
  if (clear) display.clearDisplay();
  display.setTextSize(size);
  display.setTextColor(WHITE);
  display.setCursor(0, 10);
  display.println(text);
  display.display();
}

void saveConfig() {
  if (!SPIFFS.begin()) { Serial.println("FS mount fail for save"); return; }
  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) { Serial.println("Config open fail for write"); return; }
  StaticJsonDocument<1024> doc;
  doc["mikrotik_host"] = config.mikrotik_host;
  doc["mikrotik_user"] = config.mikrotik_user;
  doc["mikrotik_pass"] = config.mikrotik_pass;
  JsonArray ratesArray = doc.createNestedArray("rates");
  for (int i = 0; i < 5; i++) {
    if (config.rates[i].pulses > 0) {
      JsonObject rate = ratesArray.createNestedObject();
      rate["pulses"] = config.rates[i].pulses;
      rate["profile"] = config.rates[i].profile;
    }
  }
  if (serializeJson(doc, configFile) == 0) {
    Serial.println("Failed to write config");
  }
  configFile.close();
  SPIFFS.end();
  Serial.println("Config saved.");
}

void loadConfig() {
  if (!SPIFFS.begin()) { Serial.println("FS mount fail for load"); return; }
  if (SPIFFS.exists("/config.json")) {
    File configFile = SPIFFS.open("/config.json", "r");
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, configFile);
    if (error) { Serial.println("Failed to read config"); }
    strlcpy(config.mikrotik_host, doc["mikrotik_host"] | "192.168.88.1", sizeof(config.mikrotik_host));
    strlcpy(config.mikrotik_user, doc["mikrotik_user"] | "nodemcu-api", sizeof(config.mikrotik_user));
    strlcpy(config.mikrotik_pass, doc["mikrotik_pass"] | "", sizeof(config.mikrotik_pass));
    for (int i = 0; i < 5; i++) {
      config.rates[i].pulses = 0;
      strcpy(config.rates[i].profile, "");
    }
    JsonArray ratesArray = doc["rates"];
    int i = 0;
    for (JsonObject rate : ratesArray) {
      if (i < 5) {
        config.rates[i].pulses = rate["pulses"];
        strlcpy(config.rates[i].profile, rate["profile"], sizeof(config.rates[i].profile));
        i++;
      }
    }
    configFile.close();
  } else {
    config.rates[0].pulses = 1; strcpy(config.rates[0].profile, "1-hour");
    config.rates[1].pulses = 5; strcpy(config.rates[1].profile, "6-hours");
    saveConfig();
  }
  SPIFFS.end();
}

// ===================================================================
// --- NEW: SALES LOGGING FUNCTIONS (MOVED HERE) ---
// ===================================================================

void logSale(int pulses, String profile) {
  if (!SPIFFS.begin()) { Serial.println("FS mount fail for sale log"); return; }
  
  File salesFile = SPIFFS.open("/sales_log.csv", "a"); // "a" opens for appending
  if (!salesFile) {
    Serial.println("Failed to open sales log for writing");
    SPIFFS.end();
    return;
  }
  unsigned long seconds = millis() / 1000;
  unsigned long days = seconds / 86400;
  seconds %= 86400;
  int hours = seconds / 3600;
  seconds %= 3600;
  int minutes = seconds / 60;
  seconds %= 60;
  String timestamp = String(days) + "d-" + (hours < 10 ? "0" : "") + String(hours) + ":" + (minutes < 10 ? "0" : "") + String(minutes) + ":" + (seconds < 10 ? "0" : "") + String(seconds);
  salesFile.println(timestamp + "," + String(pulses) + "," + profile);
  salesFile.close();
  SPIFFS.end();
  Serial.println("Logged sale: " + String(pulses) + " pulses for profile " + profile);
}

// ===================================================================
// --- WEB SERVER HANDLERS ---
// ===================================================================

void handleRoot() {
  String html = "<html><head><title>Coin WiFi Config</title><style>body{font-family:sans-serif;background:#f4f4f4;color:#333;}div{background:white;padding:20px;margin:20px auto;max-width:800px;border-radius:8px;box-shadow:0 2px 5px rgba(0,0,0,0.1);}h1,h2{color:#0056b3;}input{width:95%;padding:10px;margin:5px 0 15px 0;border:1px solid #ccc;border-radius:4px;}button{padding:10px 20px;background:#007bff;color:white;border:none;border-radius:4px;cursor:pointer;} .btn-danger{background:#dc3545;} table{width:100%;border-collapse:collapse;margin-top:20px;} th,td{text-align:left;padding:8px;border-bottom:1px solid #ddd;} th{background:#e9ecef;} hr{border:0;border-top:1px solid #eee;margin:20px 0;}</style></head><body>";
  html += "<div><h2>Sales Monitoring</h2>";
  if (!SPIFFS.begin()) { Serial.println("FS mount fail for sales read"); } else {
    if (SPIFFS.exists("/sales_log.csv")) {
      File salesFile = SPIFFS.open("/sales_log.csv", "r");
      html += "<table><tr><th>Uptime</th><th>Pulses</th><th>Profile</th></tr>";
      while (salesFile.available()) {
        String line = salesFile.readStringUntil('\n');
        String timestamp, pulses, profile;
        int firstComma = line.indexOf(',');
        int secondComma = line.indexOf(',', firstComma + 1);
        timestamp = line.substring(0, firstComma);
        pulses = line.substring(firstComma + 1, secondComma);
        profile = line.substring(secondComma + 1);
        profile.trim();
        html += "<tr><td>" + timestamp + "</td><td>" + pulses + "</td><td>" + profile + "</td></tr>";
      }
      html += "</table>";
      salesFile.close();
    } else {
      html += "<p>No sales have been recorded yet.</p>";
    }
    SPIFFS.end();
  }
  html += "<br><form action='/clearlog' method='POST'><button type='submit' class='btn-danger'>Clear Sales Log</button></form></div>";
  html += "<div><h1>Coin WiFi Machine Config</h1><p>Device IP: " + WiFi.localIP().toString() + "</p><form action='/save' method='POST'><h2>MikroTik Settings</h2>IP Address: <input type='text' name='host' value='" + String(config.mikrotik_host) + "'><br>API User: <input type='text' name='user' value='" + String(config.mikrotik_user) + "'><br>API Password: <input type='password' name='pass' placeholder='Leave blank to keep current'><br><h2>Rate Settings (Pulses to Profile)</h2>";
  for (int i = 0; i < 5; i++) {
    html += "Rate " + String(i + 1) + ": <input type='number' name='p" + String(i) + "' placeholder='Pulses' value='" + (config.rates[i].pulses > 0 ? String(config.rates[i].pulses) : "") + "'><input type='text' name='n" + String(i) + "' placeholder='Profile Name' value='" + String(config.rates[i].profile) + "'><hr>";
  }
  html += "<button type='submit'>Save & Reboot</button></form></div></body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  strlcpy(config.mikrotik_host, server.arg("host").c_str(), sizeof(config.mikrotik_host));
  strlcpy(config.mikrotik_user, server.arg("user").c_str(), sizeof(config.mikrotik_user));
  if (server.arg("pass").length() > 0) {
    strlcpy(config.mikrotik_pass, server.arg("pass").c_str(), sizeof(config.mikrotik_pass));
  }
  for (int i = 0; i < 5; i++) {
    config.rates[i].pulses = server.arg("p" + String(i)).toInt();
    strlcpy(config.rates[i].profile, server.arg("n" + String(i)).c_str(), sizeof(config.rates[i].profile));
  }
  saveConfig();
  String html = "<html><head><title>Saved</title><meta http-equiv='refresh' content='3;url=/'></head><body><div style='font-family:sans-serif;text-align:center;'><h1>Configuration Saved!</h1><p>Rebooting in 3 seconds...</p></div></body></html>";
  server.send(200, "text/html", html);
  delay(3000);
  ESP.restart();
}

void handleClearLog() {
  if (!SPIFFS.begin()) { Serial.println("FS mount fail for clear log"); return; }
  SPIFFS.remove("/sales_log.csv");
  SPIFFS.end();
  Serial.println("Sales log cleared.");
  String html = "<html><head><title>Cleared</title><meta http-equiv='refresh' content='2;url=/'></head><body><div style='font-family:sans-serif;text-align:center;'><h1>Sales Log Cleared!</h1><p>Redirecting in 2 seconds...</p></div></body></html>";
  server.send(200, "text/html", html);
}

// ===================================================================
// --- CORE LOGIC ---
// ===================================================================

void generateVoucherForPulses(int count) {
  String profileName = "";
  for (int i = 0; i < 5; i++) { if (config.rates[i].pulses == count) { profileName = config.rates[i].profile; break; } }
  if (profileName.length() == 0) { displayMessage("INVALID COIN\nOR AMOUNT", 1, true); return; }
  
  if (api.connect(config.mikrotik_host, API_PORT)) {
    if (api.login(config.mikrotik_user, config.mikrotik_pass)) {
      int voucherCodeInt = random(1000, 9999);
      String voucherCodeStr = String(voucherCodeInt);
      String cmd = "/ip/hotspot/user/add?name=" + voucherCodeStr + "?profile=" + profileName + "?comment=CoinVoucher-" + voucherCodeStr;
      
      api.write(cmd);
      Rsp rsp = api.read();
      
      if(rsp.is("!done")){
        logSale(count, profileName); // This call now works because logSale is defined above
        display.clearDisplay(); display.setTextSize(1); display.setCursor(0, 0); display.println("Your WiFi Code:");
        display.setTextSize(3); display.setCursor(25, 25); display.println(voucherCodeStr); display.display();
      } else {
        displayMessage("VOUCHER CREATE\nFAIL", 1, true);
      }
      api.disconnect();
    } else { displayMessage("API LOGIN FAIL", 1, true); }
  } else { displayMessage("ROUTER CONNECT\nFAIL", 1, true); }
}

// ===================================================================
// --- MAIN SETUP & LOOP ---
// ===================================================================
void setup() {
  Serial.begin(115200);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  displayMessage("System Booting...", 1, true);
  
  loadConfig();
  
  WiFiManager wifiManager;
  wifiManager.setAPStaticIPConfig(IPAddress(192, 168, 5, 1), IPAddress(192, 168, 5, 1), IPAddress(255, 255, 255, 0));
  wifiManager.setAPCallback([](WiFiManager* myWiFiManager) {
    display.clearDisplay(); display.setTextSize(1);
    display.println("SETUP MODE"); display.println("Connect to:");
    display.println(myWiFiManager->getConfigPortalSSID());
    display.println("Password:"); display.println(AP_PASSWORD);
    display.display();
  });
  wifiManager.autoConnect("Coin-WiFi-Setup", AP_PASSWORD);
  
  displayMessage("WiFi Connected!", 1, true);

  server.on("/", HTTP_GET, []() { if (!server.authenticate(ADMIN_USER, ADMIN_PASS)) { return server.requestAuthentication(); } handleRoot(); });
  server.on("/save", HTTP_POST, []() { if (!server.authenticate(ADMIN_USER, ADMIN_PASS)) { return server.requestAuthentication(); } handleSave(); });
  server.on("/clearlog", HTTP_POST, []() { if (!server.authenticate(ADMIN_USER, ADMIN_PASS)) { return server.requestAuthentication(); } handleClearLog(); });
  
  server.begin();
  
  pinMode(COIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(COIN_PIN), coinPulse, FALLING);
  
  display.clearDisplay(); display.println("Web Cfg Ready at:"); display.println(WiFi.localIP()); display.display();
  delay(5000);
  displayMessage("INSERT COIN", 2, true);
}

void loop() {
  server.handleClient();
  
  if (is_counting_pulses && (millis() - last_pulse_time > PULSE_TIMEOUT)) {
    noInterrupts();
    int final_pulse_count = pulse_count;
    pulse_count = 0;
    is_counting_pulses = false;
    interrupts();
    
    displayMessage("Pulses: " + String(final_pulse_count) + "\nProcessing...", 1, true);
    generateVoucherForPulses(final_pulse_count);
    delay(8000);
    displayMessage("INSERT COIN", 2, true);
  }
}
