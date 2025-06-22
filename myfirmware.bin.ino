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
// Password for the initial 'Coin-WiFi-Setup' Access Point
const char* AP_PASSWORD = "password123";  // <-- Set your desired AP password (min 8 chars)

// Username and Password for the ongoing Web Configuration Page
const char* ADMIN_USER = "admin";                 // <-- Set your web admin username
const char* ADMIN_PASS = "admin_password";        // <-- Set a STRONG web admin password

// Hardware Pins
#define COIN_PIN 14               // Raw GPIO number for D5
#define PULSE_TIMEOUT 5000        // 5 seconds to wait for more coins
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define API_PORT 8728             // Standard MikroTik API port

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

// ... (The rest of the functions like displayMessage, coinPulse, saveConfig, loadConfig, etc. are correct and don't need changes) ...
// ... The key changes are in the setup() function where the web server is configured ...

void ICACHE_RAM_ATTR coinPulse() { /* ... unchanged ... */
  pulse_count++;
  last_pulse_time = millis();
  is_counting_pulses = true;
}

void displayMessage(String text, int size, bool clear) { /* ... unchanged ... */
  if (clear) display.clearDisplay();
  display.setTextSize(size);
  display.setTextColor(WHITE);
  display.setCursor(0, 10);
  display.println(text);
  display.display();
}

struct Config {
  char mikrotik_host[40];
  char mikrotik_user[40];
  char mikrotik_pass[40];
  struct Rate { int pulses; char profile[40]; } rates[5];
};
Config config;

void saveConfig() { /* ... unchanged ... */
  if (!SPIFFS.begin()) { Serial.println("FS mount fail for save"); return; }
  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) { Serial.println("Config open fail for write"); return; }
  StaticJsonDocument<1024> doc;
  doc["mikrotik_host"] = config.mikrotik_host;
  doc["mikrotik_user"] = config.mikrotik_user;
  doc["mikrotik_pass"] = config.mikrotik_pass;
  JsonArray ratesArray = doc.createNestedArray("rates");
  for (int i = 0; i < 5; i++) { if (config.rates[i].pulses > 0) { JsonObject rate = ratesArray.createNestedObject(); rate["pulses"] = config.rates[i].pulses; rate["profile"] = config.rates[i].profile; } }
  if (serializeJson(doc, configFile) == 0) { Serial.println("Failed to write config"); }
  configFile.close(); SPIFFS.end(); Serial.println("Config saved.");
}

void loadConfig() { /* ... unchanged ... */
  if (!SPIFFS.begin()) { Serial.println("FS mount fail for load"); return; }
  if (SPIFFS.exists("/config.json")) {
    File configFile = SPIFFS.open("/config.json", "r");
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, configFile);
    if (error) { Serial.println("Failed to read config"); }
    strlcpy(config.mikrotik_host, doc["mikrotik_host"] | "192.168.88.1", sizeof(config.mikrotik_host));
    strlcpy(config.mikrotik_user, doc["mikrotik_user"] | "nodemcu-api", sizeof(config.mikrotik_user));
    strlcpy(config.mikrotik_pass, doc["mikrotik_pass"] | "", sizeof(config.mikrotik_pass));
    for (int i = 0; i < 5; i++) { config.rates[i].pulses = 0; strcpy(config.rates[i].profile, ""); }
    JsonArray ratesArray = doc["rates"];
    int i = 0;
    for (JsonObject rate : ratesArray) { if (i < 5) { config.rates[i].pulses = rate["pulses"]; strlcpy(config.rates[i].profile, rate["profile"], sizeof(config.rates[i].profile)); i++; } }
    configFile.close();
  } else {
    config.rates[0].pulses = 1; strcpy(config.rates[0].profile, "profile-1-pulse");
    config.rates[1].pulses = 5; strcpy(config.rates[1].profile, "profile-5-pulses");
    saveConfig();
  }
  SPIFFS.end();
}

void handleRoot() { /* ... unchanged ... */
  String html = "<html><head><title>Coin WiFi Config</title><style>body{font-family:sans-serif;background:#f4f4f4;color:#333;}div{background:white;padding:20px;margin:20px auto;max-width:600px;border-radius:8px;box-shadow:0 2px 5px rgba(0,0,0,0.1);}input{width:95%;padding:10px;margin:5px 0 15px 0;}button{padding:10px 20px;background:#007bff;color:white;border:none;cursor:pointer;}</style></head><body><div><h1>Coin WiFi Machine Config</h1><p>Device IP: " + WiFi.localIP().toString() + "</p><form action='/save' method='POST'><h2>MikroTik Settings</h2>IP Address: <input type='text' name='host' value='" + String(config.mikrotik_host) + "'><br>API User: <input type='text' name='user' value='" + String(config.mikrotik_user) + "'><br>API Password: <input type='password' name='pass' value=''><br><h2>Rate Settings (Pulses to Profile)</h2>";
  for (int i = 0; i < 5; i++) { html += "Rate " + String(i + 1) + ": <input type='number' name='p" + String(i) + "' placeholder='Pulses' value='" + (config.rates[i].pulses > 0 ? String(config.rates[i].pulses) : "") + "'><input type='text' name='n" + String(i) + "' placeholder='Profile Name' value='" + String(config.rates[i].profile) + "'><hr>"; }
  html += "<button type='submit'>Save & Reboot</button></form></div></body></html>";
  server.send(200, "text/html", html);
}

void handleSave() { /* ... unchanged ... */
  strlcpy(config.mikrotik_host, server.arg("host").c_str(), sizeof(config.mikrotik_host));
  strlcpy(config.mikrotik_user, server.arg("user").c_str(), sizeof(config.mikrotik_user));
  if (server.arg("pass").length() > 0) { strlcpy(config.mikrotik_pass, server.arg("pass").c_str(), sizeof(config.mikrotik_pass)); }
  for (int i = 0; i < 5; i++) { config.rates[i].pulses = server.arg("p" + String(i)).toInt(); strlcpy(config.rates[i].profile, server.arg("n" + String(i)).c_str(), sizeof(config.rates[i].profile)); }
  saveConfig();
  String html = "<html><head><title>Saved</title><meta http-equiv='refresh' content='3;url=/'></head><body><div><h1>Configuration Saved!</h1><p>Rebooting in 3 seconds...</p></div></body></html>";
  server.send(200, "text/html", html);
  delay(3000);
  ESP.restart();
}

void generateVoucherForPulses(int count) { /* ... unchanged ... */
  String profileName = "";
  for (int i = 0; i < 5; i++) { if (config.rates[i].pulses == count) { profileName = config.rates[i].profile; break; } }
  if (profileName.length() == 0) { displayMessage("INVALID COIN\nOR AMOUNT", 1, true); return; }
  if (api.connect(config.mikrotik_host, API_PORT)) {
    if (api.login(config.mikrotik_user, config.mikrotik_pass)) {
      int voucherCodeInt = random(1000, 9999); String voucherCodeStr = String(voucherCodeInt);
      String cmd = "/ip/hotspot/user/add?name=" + voucherCodeStr + "?profile=" + profileName;
      api.write(cmd); api.read(); api.disconnect();
      display.clearDisplay(); display.setTextSize(1); display.setCursor(0, 0); display.println("Your WiFi Code:");
      display.setTextSize(3); display.setCursor(25, 25); display.println(voucherCodeStr); display.display();
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

  // ============================================================
  // --- NEW: SECURE WEB SERVER SETUP ---
  // ============================================================
  // Handle the root page (GET request)
  server.on("/", HTTP_GET, []() {
    // First, check if the user is authenticated
    if (!server.authenticate(ADMIN_USER, ADMIN_PASS)) {
      // If not, demand authentication
      return server.requestAuthentication();
    }
    // If they are authenticated, show the config page
    handleRoot();
  });

  // Handle the save action (POST request)
  server.on("/save", HTTP_POST, []() {
    // Also protect the save action
    if (!server.authenticate(ADMIN_USER, ADMIN_PASS)) {
      return server.requestAuthentication();
    }
    handleSave();
  });
  // ============================================================
  
  server.begin();
  
  pinMode(COIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(COIN_PIN), coinPulse, FALLING);
  
  display.clearDisplay();
  display.println("Web Cfg Ready at:");
  display.println(WiFi.localIP());
  display.display();
  delay(5000);
  displayMessage("INSERT COIN", 2, true);
}

void loop() {
  server.handleClient();
  if (is_counting_pulses && (millis() - last_pulse_time > PULSE_TIMEOUT)) {
    int final_pulse_count = pulse_count;
    pulse_count = 0;
    is_counting_pulses = false;
    displayMessage("Pulses: " + String(final_pulse_count) + "\nProcessing...", 1, true);
    generateVoucherForPulses(final_pulse_count);
    delay(8000);
    displayMessage("INSERT COIN", 2, true);
  }
}