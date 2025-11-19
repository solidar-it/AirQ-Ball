#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <FastLED.h>
#include <ArduinoJson.h>
#include <WiFiClient.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>

// Version information
#define VERSION "1.01.11"
#define BUILD_DATE __DATE__

// LED Ring Configuration
#define LED_PIN     D4
#define NUM_LEDS    7
#define LED_TYPE    WS2812
#define COLOR_ORDER GRB

// Logo URLs
#define LOGO_URL "https://raw.githubusercontent.com/solidar-it/commonimages/refs/heads/main/solidarit_200x200.png"
#define AIRQ_LOGO_URL "https://raw.githubusercontent.com/solidar-it/commonimages/refs/heads/main/airq_with_slogan_200x200.png"

// Firmware Update URLs
#define FW_BASE_URL "https://github.com/solidar-it/AirQ-Ball/raw/main/AirQ-Ball/build/latest/"
#define FW_VERSION_URL "https://raw.githubusercontent.com/solidar-it/AirQ-Ball/main/AirQ-Ball/build/latest/version.txt"

// Sensor.community API
#define SENSOR_API "http://data.sensor.community/airrohr/v1/sensor/"
#define DEFAULT_SENSOR_ID "91200"

CRGB leds[NUM_LEDS];
ESP8266WebServer server(80);
DNSServer dnsServer;
WiFiClient client;

// Network settings
String chipId;
String apSSID;
String hostname;
bool apMode = true;
const byte DNS_PORT = 53;

// EEPROM addresses
#define EEPROM_SIZE 512
#define SSID_ADDR 0
#define PASS_ADDR 100
#define SENSOR_ID_ADDR 200
#define BRIGHTNESS_ADDR 250

// Current settings
int brightness = 25; // Default brightness changed to 25
int currentMode = 0; // 0: Solid, 1: Sparkle, 2: Breathing
CRGB currentColor = CRGB::White;
String sensorId = DEFAULT_SENSOR_ID;
float lastP2Value = 0.0;
String sensorCountry = "";
String sensorLatitude = "";
String sensorLongitude = "";
unsigned long lastSensorUpdate = 0;
const unsigned long SENSOR_UPDATE_INTERVAL = 60000; // 1 minute

// OTA Update variables
bool updateInProgress = false;
String latestVersion = "";
String updateStatus = "";
int updateProgress = 0;

// Debug timing
unsigned long lastDebugTime = 0;
const unsigned long DEBUG_INTERVAL = 60000; // 1 minute
unsigned long lastBreathTime = 0;

// Color reference points
#define COLOR_LIGHT_BLUE   CRGB(173, 216, 230)  // 5 μg/m³
#define COLOR_LIGHT_GREEN  CRGB(144, 238, 144)  // 15 μg/m³
#define COLOR_YELLOW       CRGB(255, 255, 0)    // 25 μg/m³
#define COLOR_ORANGE       CRGB(255, 165, 0)    // 40 μg/m³
#define COLOR_RED          CRGB(255, 0, 0)      // 55 μg/m³
#define COLOR_PURPLE       CRGB(128, 0, 128)    // 60+ μg/m³

void setup() {
  Serial.begin(115200);
  delay(100);
  
  // Initialize LEDs
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  
  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);
  
  // Try to read saved brightness
  int savedBrightness = EEPROM.read(BRIGHTNESS_ADDR);
  if (savedBrightness >= 1 && savedBrightness <= 255) {
    brightness = savedBrightness;
  }
  
  FastLED.setBrightness(brightness);
  
  // Show startup animation
  startupAnimation();
  
  // Get chip ID
  chipId = String(ESP.getChipId(), HEX);
  chipId.toUpperCase();
  apSSID = "AirQ-Ball-" + chipId;
  hostname = "AirQ-Ball-" + chipId;
  
  Serial.println("\nAirQ-Ball Starting...");
  Serial.println("Version: " + String(VERSION));
  Serial.println("Build Date: " + String(BUILD_DATE));
  Serial.println("Chip ID: " + chipId);
  
  // Try to read saved credentials and sensor ID
  String savedSSID = readStringFromEEPROM(SSID_ADDR);
  String savedPass = readStringFromEEPROM(PASS_ADDR);
  String savedSensorId = readStringFromEEPROM(SENSOR_ID_ADDR);
  
  if (savedSensorId.length() > 0) {
    sensorId = savedSensorId;
    Serial.println("Loaded Sensor ID: " + sensorId);
  }
  
  if (savedSSID.length() > 0) {
    Serial.println("Attempting to connect to saved WiFi: " + savedSSID);
    connectToWiFi(savedSSID, savedPass);
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    startAPMode();
  } else {
    apMode = false;
    setupWebServer();
    Serial.println("Connected! IP: " + WiFi.localIP().toString());
    connectedAnimation();
  }
  
  lastDebugTime = millis();
  lastSensorUpdate = millis() - SENSOR_UPDATE_INTERVAL; // Force immediate update
}

void loop() {
  if (apMode) {
    dnsServer.processNextRequest();
  }
  server.handleClient();
  
  // Update sensor data periodically
  if (!apMode && millis() - lastSensorUpdate >= SENSOR_UPDATE_INTERVAL) {
    updateSensorData();
    lastSensorUpdate = millis();
  }
  
  // Update LED colors based on air quality
  updateLEDColorFromAQI();
  
  // Update LED patterns based on mode
  switch(currentMode) {
    case 1: // Sparkle
      sparkle();
      break;
    case 2: // Breathing
      breathing();
      break;
    default: // Solid color (mode 0)
      fill_solid(leds, NUM_LEDS, currentColor);
      FastLED.show();
      delay(50);
      break;
  }
  
  // Serial debug output every minute
  if (millis() - lastDebugTime >= DEBUG_INTERVAL) {
    printDebugStatus();
    lastDebugTime = millis();
  }
}

CRGB interpolateColor(float value, float minVal, float maxVal, CRGB minColor, CRGB maxColor) {
  if (value <= minVal) return minColor;
  if (value >= maxVal) return maxColor;
  
  float ratio = (value - minVal) / (maxVal - minVal);
  CRGB result;
  result.r = minColor.r + (maxColor.r - minColor.r) * ratio;
  result.g = minColor.g + (maxColor.g - minColor.g) * ratio;
  result.b = minColor.b + (maxColor.b - minColor.b) * ratio;
  return result;
}

void updateLEDColorFromAQI() {
  if (lastP2Value <= 5.0) {
    currentColor = COLOR_LIGHT_BLUE;
    currentMode = 0; // Solid
  } else if (lastP2Value <= 15.0) {
    currentColor = interpolateColor(lastP2Value, 5.0, 15.0, COLOR_LIGHT_BLUE, COLOR_LIGHT_GREEN);
    currentMode = 0; // Solid
  } else if (lastP2Value <= 25.0) {
    currentColor = interpolateColor(lastP2Value, 15.0, 25.0, COLOR_LIGHT_GREEN, COLOR_YELLOW);
    currentMode = 0; // Solid
  } else if (lastP2Value <= 40.0) {
    currentColor = interpolateColor(lastP2Value, 25.0, 40.0, COLOR_YELLOW, COLOR_ORANGE);
    currentMode = 0; // Solid
  } else if (lastP2Value <= 55.0) {
    currentColor = interpolateColor(lastP2Value, 40.0, 55.0, COLOR_ORANGE, COLOR_RED);
    currentMode = 0; // Solid
  } else if (lastP2Value <= 60.0) {
    currentColor = COLOR_RED;
    currentMode = 2; // Breathing with slow cycle
  } else {
    currentColor = COLOR_PURPLE;
    currentMode = 2; // Breathing with slow cycle
  }
}

bool updateSensorData() {
  Serial.println("Updating sensor data from sensor.community...");
  
  String url = String(SENSOR_API) + sensorId + "/";
  
  // Use WiFiClient to make HTTP request
  WiFiClient client;
  if (!client.connect("data.sensor.community", 80)) {
    Serial.println("Connection to sensor.community failed");
    return false;
  }
  
  // Make HTTP request
  client.println("GET " + url + " HTTP/1.1");
  client.println("Host: data.sensor.community");
  client.println("Connection: close");
  client.println();
  
  // Wait for response
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 10000) {
      Serial.println("Client Timeout!");
      client.stop();
      return false;
    }
  }
  
  // Read response
  String response = "";
  while (client.available()) {
    response += client.readString();
  }
  client.stop();
  
  // Find JSON start (after headers)
  int jsonStart = response.indexOf('[');
  if (jsonStart == -1) {
    Serial.println("Invalid JSON response");
    return false;
  }
  
  String json = response.substring(jsonStart);
  
  // Parse JSON
  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, json);
  
  if (error) {
    Serial.print("JSON parsing failed: ");
    Serial.println(error.c_str());
    return false;
  }
  
  // Get the first P2 value and location data from the latest reading
  JsonArray sensors = doc.as<JsonArray>();
  if (sensors.size() > 0) {
    JsonObject latest = sensors[0];
    JsonArray values = latest["sensordatavalues"];
    
    // Get location data
    JsonObject location = latest["location"];
    sensorCountry = location["country"].as<String>();
    sensorLatitude = location["latitude"].as<String>();
    sensorLongitude = location["longitude"].as<String>();
    
    for (JsonObject value : values) {
      String valueType = value["value_type"].as<String>();
      if (valueType == "P2") {
        lastP2Value = value["value"].as<float>();
        Serial.println("Latest P2 value: " + String(lastP2Value) + " μg/m³");
        Serial.println("Sensor Location: " + sensorCountry + " - " + sensorLatitude + ", " + sensorLongitude);
        return true;
      }
    }
  }
  
  Serial.println("No P2 value found in response");
  return false;
}

// OTA Update Functions
String checkForUpdates() {
  Serial.println("Checking for firmware updates...");
  
  HTTPClient http;
  WiFiClient client;
  
  // Use the new API with WiFiClient
  http.begin(client, FW_VERSION_URL);
  http.setUserAgent("AirQ-Ball/" + String(VERSION));
  
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    latestVersion = http.getString();
    latestVersion.trim();
    Serial.println("Latest version available: " + latestVersion);
    
    if (latestVersion != VERSION) {
      return "Update available: " + latestVersion;
    } else {
      return "Firmware is up to date";
    }
  } else {
    Serial.println("Failed to check for updates. HTTP code: " + String(httpCode));
    return "Failed to check for updates";
  }
  
  http.end();
}

void performUpdate() {
  Serial.println("Starting firmware update...");
  updateInProgress = true;
  updateStatus = "Starting update...";
  updateProgress = 0;
  
  // Create firmware URL based on current date and version
  String fwURL = String(FW_BASE_URL) + "AirQ-Ball_v" + latestVersion + "_" + getCurrentDate() + ".bin";
  Serial.println("Downloading from: " + fwURL);
  
  updateStatus = "Downloading firmware...";
  
  // Show updating animation
  updatingAnimation();
  
  WiFiClient client;
  ESPhttpUpdate.onStart([]() {
    Serial.println("OTA update started");
    updateStatus = "Update started, please wait...";
    updateProgress = 20;
  });
  
  ESPhttpUpdate.onEnd([]() {
    Serial.println("OTA update finished");
    updateStatus = "Update finished! Rebooting...";
    updateProgress = 100;
  });
  
  ESPhttpUpdate.onProgress([](int cur, int total) {
    updateProgress = map(cur, 0, total, 20, 90);
    Serial.println("Update progress: " + String(updateProgress) + "%");
    updateStatus = "Updating: " + String(updateProgress) + "%";
  });
  
  ESPhttpUpdate.onError([](int err) {
    Serial.println("OTA update error: " + String(err));
    updateStatus = "Update failed with error: " + String(err);
    updateProgress = 0;
    updateInProgress = false;
  });
  
  // Use the new API with WiFiClient
  t_httpUpdate_return ret = ESPhttpUpdate.update(client, fwURL);
  
  switch(ret) {
    case HTTP_UPDATE_FAILED:
      Serial.println("Update failed");
      updateInProgress = false;
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("No updates available");
      updateStatus = "No updates available";
      updateInProgress = false;
      break;
    case HTTP_UPDATE_OK:
      Serial.println("Update OK");
      break;
  }
}

String getCurrentDate() {
  // Extract date from build date (format: "MMM DD YYYY")
  String buildDate = String(BUILD_DATE);
  
  // Convert month name to number
  String month = buildDate.substring(0, 3);
  String day = buildDate.substring(4, 6);
  String year = buildDate.substring(7, 11);
  
  // Remove space from day if needed
  if (day.charAt(0) == ' ') {
    day = "0" + day.substring(1);
  }
  
  // Map month names to numbers
  String monthNum = "01";
  if (month == "Jan") monthNum = "01";
  else if (month == "Feb") monthNum = "02";
  else if (month == "Mar") monthNum = "03";
  else if (month == "Apr") monthNum = "04";
  else if (month == "May") monthNum = "05";
  else if (month == "Jun") monthNum = "06";
  else if (month == "Jul") monthNum = "07";
  else if (month == "Aug") monthNum = "08";
  else if (month == "Sep") monthNum = "09";
  else if (month == "Oct") monthNum = "10";
  else if (month == "Nov") monthNum = "11";
  else if (month == "Dec") monthNum = "12";
  
  return year + "-" + monthNum + "-" + day;
}

void printDebugStatus() {
  Serial.println("\n=== AirQ-Ball Debug Status ===");
  Serial.println("Version: " + String(VERSION));
  Serial.println("Device: " + hostname);
  Serial.println("Chip ID: " + chipId);
  Serial.println("AP Mode: " + String(apMode ? "Yes" : "No"));
  
  if (!apMode) {
    Serial.println("WiFi SSID: " + WiFi.SSID());
    Serial.println("IP Address: " + WiFi.localIP().toString());
    Serial.println("RSSI: " + String(WiFi.RSSI()) + " dBm");
    Serial.println("Sensor ID: " + sensorId);
    Serial.println("Latest P2: " + String(lastP2Value) + " μg/m³");
    Serial.println("Sensor Location: " + sensorCountry + " - " + sensorLatitude + ", " + sensorLongitude);
  } else {
    Serial.println("AP SSID: " + apSSID);
    Serial.println("AP IP: " + WiFi.softAPIP().toString());
  }
  
  Serial.println("LED Mode: " + String(currentMode == 0 ? "Solid" : currentMode == 1 ? "Sparkle" : "Breathing"));
  Serial.println("Brightness: " + String(brightness));
  Serial.println("Color: R=" + String(currentColor.r) + " G=" + String(currentColor.g) + " B=" + String(currentColor.b));
  Serial.println("Free Heap: " + String(ESP.getFreeHeap()) + " bytes");
  Serial.println("Uptime: " + String(millis() / 1000) + " seconds");
  Serial.println("=============================\n");
}

void startAPMode() {
  Serial.println("Starting AP Mode...");
  Serial.println("SSID: " + apSSID);
  Serial.println("Password: 12345678");
  
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSSID.c_str(), "12345678");
  
  delay(100);
  IPAddress IP = WiFi.softAPIP();
  Serial.println("AP IP: " + IP.toString());
  
  // Start DNS server for captive portal
  dnsServer.start(DNS_PORT, "*", IP);
  
  // Setup web server for configuration
  setupConfigServer();
  
  apMode = true;
  apModeAnimation();
}

void connectToWiFi(String ssid, String password) {
  WiFi.mode(WIFI_STA);
  WiFi.hostname(hostname);
  WiFi.begin(ssid.c_str(), password.c_str());
  
  Serial.print("Connecting");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();
}

void setupConfigServer() {
  server.onNotFound(handleCaptivePortal);
  server.on("/", handleConfigPage);
  server.on("/scan", handleScanNetworks);
  server.on("/save", handleSaveConfig);
  server.begin();
  Serial.println("Config server started");
}

void setupWebServer() {
  server.stop();
  server.on("/", handleRoot);
  server.on("/setColor", handleSetColor);
  server.on("/setBrightness", handleSetBrightness);
  server.on("/setMode", handleSetMode);
  server.on("/setSensor", handleSetSensor);
  server.on("/updateSensor", handleUpdateSensor);
  server.on("/off", handleOff);
  server.on("/reset", handleReset);
  server.on("/debug", handleDebugPage);
  
  // OTA Update endpoints
  server.on("/checkUpdate", handleCheckUpdate);
  server.on("/performUpdate", handlePerformUpdate);
  server.on("/updateStatus", handleUpdateStatus);
  
  server.begin();
  Serial.println("Web server started!");
}

void handleCaptivePortal() {
  handleConfigPage();
}

String scanNetworks() {
  String networks = "";
  
  // Scan for WiFi networks
  int numNetworks = WiFi.scanNetworks();
  Serial.println("Scanning networks... Found: " + String(numNetworks));
  
  if (numNetworks == 0) {
    networks = "<option value=''>No networks found</option>";
  } else {
    for (int i = 0; i < numNetworks; i++) {
      String ssid = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      int encryption = WiFi.encryptionType(i);
      
      // Skip empty SSIDs and our own AP
      if (ssid.length() == 0 || ssid == apSSID) {
        continue;
      }
      
      String security = "";
      if (encryption != ENC_TYPE_NONE) {
        security = "&#128274;"; // Lock emoji
      }
      
      // Convert RSSI to signal strength (0-4 bars)
      int strength = 0;
      if (rssi > -55) strength = 4;
      else if (rssi > -65) strength = 3;
      else if (rssi > -75) strength = 2;
      else if (rssi > -85) strength = 1;
      
      String strengthBars = "";
      for (int j = 0; j < strength; j++) {
        strengthBars += "&#9604;"; // Solid block
      }
      for (int j = strength; j < 4; j++) {
        strengthBars += "&#9617;"; // Light block
      }
      
      networks += "<option value='" + ssid + "'>" + security + " " + ssid + " (" + strengthBars + " " + String(rssi) + "dBm)</option>";
    }
  }
  
  return networks;
}

void handleScanNetworks() {
  String networks = scanNetworks();
  server.send(200, "application/json", "{\"networks\": \"" + networks + "\"}");
}

void handleConfigPage() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta charset='UTF-8'>";
  html += "<title>AirQ-Ball Setup</title>";
  html += "<style>";
  html += "body { font-family: Arial; padding: 20px; background: #1a1a1a; color: white; max-width: 500px; margin: 0 auto; }";
  html += ".header { display: flex; align-items: center; justify-content: center; margin-bottom: 20px; }";
  html += ".header img { width: 100px; height: 100px; border-radius: 10px; cursor: pointer; }";
  html += ".header h1 { margin: 0 15px; color: #4CAF50; text-align: center; }";
  html += "select, input { width: 100%; padding: 12px; margin: 8px 0; box-sizing: border-box; background: #333; border: 2px solid #555; color: white; border-radius: 4px; }";
  html += "button { padding: 15px; background: #4CAF50; color: white; border: none; border-radius: 4px; font-size: 16px; cursor: pointer; margin-top: 10px; }";
  html += "button:hover { background: #45a049; }";
  html += ".info { background: #333; padding: 10px; border-radius: 4px; margin: 10px 0; }";
  html += ".scan-btn { width: 100%; background: #2196F3; }";
  html += ".connect-btn { width: 100%; }";
  html += ".refresh { float: right; background: #666; padding: 5px 10px; font-size: 12px; }";
  html += ".manual { margin-top: 20px; padding: 15px; background: #222; border-radius: 4px; }";
  html += ".manual-toggle { background: none; border: none; color: #4CAF50; text-decoration: underline; cursor: pointer; font-size: 14px; }";
  html += ".footer { text-align: center; margin-top: 30px; padding: 15px; background: #222; border-radius: 4px; font-size: 12px; color: #888; }";
  html += ".footer a { color: #4CAF50; text-decoration: none; }";
  html += ".footer a:hover { text-decoration: underline; }";
  html += ".version { margin-top: 10px; font-size: 10px; color: #666; }";
  html += "</style>";
  html += "</head><body>";
  
  // Header section with logos
  html += "<div class='header'>";
  html += "<a href='https://solidarit.gr' target='_blank'><img src='" + String(LOGO_URL) + "' alt='SolidarIT Logo'></a>";
  html += "<a href='/' style='text-decoration: none;'><h1>AirQ-Ball Setup</h1></a>";
  html += "<a href='https://airq.gr/airq-ball/' target='_blank'><img src='" + String(AIRQ_LOGO_URL) + "' alt='Air Quality Logo'></a>";
  html += "</div>";
  
  html += "<div class='info'><p><strong>Device:</strong> " + apSSID + "</p></div>";
  
  html += "<div id='scanSection'>";
  html += "<h2>Select WiFi Network</h2>";
  html += "<button class='scan-btn' onclick='scanNetworks()'>&#128269; Scan Networks</button>";
  html += "<div id='networksList' style='margin: 20px 0;'>";
  html += "<p style='text-align: center; color: #888;'>Click \"Scan Networks\" to find available WiFi networks</p>";
  html += "</div>";
  html += "<form id='wifiForm' action='/save' method='POST'>";
  html += "<label>Select Network:</label>";
  html += "<select id='ssid' name='ssid' required onchange='enablePassword()'>";
  html += "<option value=''>-- Select a network --</option>";
  html += "</select>";
  html += "<label>Password:</label>";
  html += "<input type='password' id='password' name='password' placeholder='Enter WiFi password' disabled>";
  html += "<button type='submit' class='connect-btn' id='connectBtn' disabled>Connect to WiFi</button>";
  html += "</form>";
  html += "</div>";
  
  html += "<div class='manual'>";
  html += "<button class='manual-toggle' onclick='toggleManual()'>&#128221; Or enter network manually</button>";
  html += "<div id='manualEntry' style='display: none; margin-top: 15px;'>";
  html += "<form action='/save' method='POST'>";
  html += "<label>Network Name (SSID):</label>";
  html += "<input type='text' name='ssid' placeholder='Enter WiFi SSID manually' required>";
  html += "<label>Password:</label>";
  html += "<input type='password' name='password' placeholder='Enter WiFi password'>";
  html += "<button type='submit' class='connect-btn'>Connect to WiFi</button>";
  html += "</form>";
  html += "</div>";
  html += "</div>";
  
  // Footer section
  html += "<div class='footer'>";
  html += "<p>Created by Art-Net | Hosted by EZHellas | Maintained by SolidarIT</p>";
  html += "<div class='version'>AirQ-Ball - " + String(VERSION) + " - " + String(BUILD_DATE) + "</div>";
  html += "</div>";
  
  html += "<p style='text-align: center; color: #888; margin-top: 20px;'>";
  html += "After connecting, find your AirQ-Ball at<br><strong>" + hostname + ".local</strong> or check your router";
  html += "</p>";
  
  html += "<script>";
  html += "function scanNetworks() {";
  html += "var scanBtn = event.target;";
  html += "var originalText = scanBtn.innerHTML;";
  html += "scanBtn.innerHTML = '&#128257; Scanning...';";
  html += "scanBtn.disabled = true;";
  html += "fetch('/scan')";
  html += ".then(response => response.json())";
  html += ".then(data => {";
  html += "var select = document.getElementById('ssid');";
  html += "select.innerHTML = '<option value=\"\">-- Select a network --</option>' + data.networks;";
  html += "scanBtn.innerHTML = '&#128269; Scan Again';";
  html += "scanBtn.disabled = false;";
  html += "})";
  html += ".catch(error => {";
  html += "console.error('Error:', error);";
  html += "scanBtn.innerHTML = '❌ Scan Failed - Retry';";
  html += "scanBtn.disabled = false;";
  html += "});";
  html += "}";
  
  html += "function enablePassword() {";
  html += "var ssid = document.getElementById('ssid').value;";
  html += "var password = document.getElementById('password');";
  html += "var connectBtn = document.getElementById('connectBtn');";
  html += "if (ssid) {";
  html += "password.disabled = false;";
  html += "connectBtn.disabled = false;";
  html += "} else {";
  html += "password.disabled = true;";
  html += "connectBtn.disabled = true;";
  html += "}";
  html += "}";
  
  html += "function toggleManual() {";
  html += "var manualEntry = document.getElementById('manualEntry');";
  html += "var toggleBtn = event.target;";
  html += "if (manualEntry.style.display === 'none') {";
  html += "manualEntry.style.display = 'block';";
  html += "toggleBtn.innerHTML = '&#128221; Hide manual entry';";
  html += "} else {";
  html += "manualEntry.style.display = 'none';";
  html += "toggleBtn.innerHTML = '&#128221; Or enter network manually';";
  html += "}";
  html += "}";
  
  html += "window.onload = function() {";
  html += "setTimeout(scanNetworks, 1000);";
  html += "};";
  html += "</script>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleSaveConfig() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  
  Serial.println("Saving WiFi credentials...");
  Serial.println("SSID: " + ssid);
  
  // Save to EEPROM
  writeStringToEEPROM(SSID_ADDR, ssid);
  writeStringToEEPROM(PASS_ADDR, password);
  EEPROM.commit();
  
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta charset='UTF-8'>";
  html += "<title>Connecting...</title>";
  html += "<style>";
  html += "body { font-family: Arial; padding: 20px; background: #1a1a1a; color: white; text-align: center; }";
  html += ".header { display: flex; align-items: center; justify-content: center; margin-bottom: 20px; }";
  html += ".header img { width: 100px; height: 100px; border-radius: 10px; cursor: pointer; }";
  html += ".header h1 { margin: 0 15px; color: #4CAF50; text-align: center; }";
  html += ".spinner { border: 8px solid #333; border-top: 8px solid #4CAF50; border-radius: 50%; width: 60px; height: 60px; animation: spin 1s linear infinite; margin: 20px auto; }";
  html += "@keyframes spin { 0% { transform: rotate(0deg); } 100% { transform: rotate(360deg); } }";
  html += ".footer { text-align: center; margin-top: 30px; padding: 15px; background: #222; border-radius: 4px; font-size: 12px; color: #888; }";
  html += ".version { margin-top: 10px; font-size: 10px; color: #666; }";
  html += "</style>";
  html += "</head><body>";
  
  // Header section with logos
  html += "<div class='header'>";
  html += "<a href='https://solidarit.gr' target='_blank'><img src='" + String(LOGO_URL) + "' alt='SolidarIT Logo'></a>";
  html += "<a href='/' style='text-decoration: none;'><h1>Connecting to WiFi...</h1></a>";
  html += "<a href='https://airq.gr/airq-ball/' target='_blank'><img src='" + String(AIRQ_LOGO_URL) + "' alt='Air Quality Logo'></a>";
  html += "</div>";
  
  html += "<div class='spinner'></div>";
  html += "<p>Your AirQ-Ball is connecting to: <strong>" + ssid + "</strong></p>";
  html += "<p>Look for <strong>" + hostname + ".local</strong> on your network in a moment!</p>";
  
  // Footer section
  html += "<div class='footer'>";
  html += "<p>Created by Art-Net | Hosted by EZHellas | Maintained by SolidarIT</p>";
  html += "<div class='version'>AirQ-Ball - " + String(VERSION) + " - " + String(BUILD_DATE) + "</div>";
  html += "</div>";
  
  html += "</body></html>";
  
  server.send(200, "text/html", html);
  
  delay(1000);
  Serial.println("Restarting...");
  ESP.restart();
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta charset='UTF-8'>";
  html += "<meta http-equiv='refresh' content='120'>"; // Refresh every 2 minutes
  html += "<title>AirQ-Ball Control</title>";
  html += "<style>";
  html += "body { font-family: Arial; text-align: center; padding: 20px; background: #1a1a1a; color: white; max-width: 500px; margin: 0 auto; }";
  html += ".header { display: flex; align-items: center; justify-content: center; margin-bottom: 20px; }";
  html += ".header img { width: 100px; height: 100px; border-radius: 10px; cursor: pointer; }";
  html += ".header h1 { margin: 0 15px; color: #4CAF50; text-align: center; }";
  html += ".control { margin: 20px auto; max-width: 400px; }";
  html += "button { padding: 12px 20px; margin: 5px; font-size: 14px; border: none; border-radius: 5px; cursor: pointer; transition: transform 0.1s; }";
  html += "button:active { transform: scale(0.95); }";
  html += ".color-btn { width: 60px; height: 60px; border-radius: 50%; }";
  html += "input[type=range] { width: 100%; height: 30px; }";
  html += ".mode-btn { background: #2196F3; color: white; width: 150px; }"; // Increased width
  html += ".off-btn { background: #f44336; color: white; width: 180px; }";
  html += ".reset-btn { background: #ff9800; color: white; width: 120px; font-size: 12px; }";
  html += ".debug-btn { background: #9C27B0; color: white; width: 180px; }";
  html += ".sensor-btn { background: #4CAF50; color: white; width: 220px; white-space: nowrap; }"; // No text wrap
  html += ".map-btn { background: #607D8B; color: white; width: 250px; font-size: 12px; padding: 10px 15px; }"; // Increased width
  html += ".update-btn { background: #FF5722; color: white; width: 220px; }";
  html += ".info { background: #2a2a2a; padding: 12px; border-radius: 6px; margin: 10px 0; font-size: 12px; border-left: 4px solid #4CAF50; }";
  html += ".location-info { background: #2a2a2a; padding: 10px; border-radius: 6px; margin: 8px 0; font-size: 11px; border-left: 4px solid #2196F3; }";
  html += ".aqi-info { background: linear-gradient(90deg, #ADD8E6, #90EE90, #FFFF00, #FFA500, #FF0000, #800080); padding: 6px; border-radius: 4px; margin: 8px 0; height: 20px; }";
  html += ".footer { text-align: center; margin-top: 25px; padding: 12px; background: #222; border-radius: 6px; font-size: 11px; color: #888; }";
  html += ".footer a { color: #4CAF50; text-decoration: none; }";
  html += ".footer a:hover { text-decoration: underline; }";
  html += ".sensor-input { display: flex; gap: 10px; align-items: center; }";
  html += ".sensor-input input { flex: 1; }";
  html += ".sensor-input button { flex: 0 0 auto; height: 44px; }";
  html += ".map-links { display: flex; gap: 10px; justify-content: center; margin: 5px 0; }";
  html += ".map-links a { background: #555; color: white; padding: 5px 10px; border-radius: 3px; text-decoration: none; font-size: 11px; }";
  html += ".map-links a:hover { background: #666; }";
  html += ".version { margin-top: 10px; font-size: 10px; color: #666; }";
  html += ".update-section { background: #2a2a2a; padding: 15px; border-radius: 6px; margin: 15px 0; border-left: 4px solid #FF5722; }";
  html += ".update-status { margin: 10px 0; padding: 10px; border-radius: 4px; background: #333; }";
  html += ".progress-bar { width: 100%; height: 20px; background: #555; border-radius: 10px; margin: 10px 0; }";
  html += ".progress { height: 100%; background: #4CAF50; border-radius: 10px; width: 0%; transition: width 0.3s; }";
  html += "</style>";
  html += "</head><body>";
  
  // Header section with logos
  html += "<div class='header'>";
  html += "<a href='https://solidarit.gr' target='_blank'><img src='" + String(LOGO_URL) + "' alt='SolidarIT Logo'></a>";
  html += "<a href='/' style='text-decoration: none;'><h1>AirQ-Ball Control</h1></a>";
  html += "<a href='https://airq.gr/airq-ball/' target='_blank'><img src='" + String(AIRQ_LOGO_URL) + "' alt='Air Quality Logo'></a>";
  html += "</div>";
  
  html += "<div class='info'>";
  html += "<p><strong>Device:</strong> " + hostname + " | <strong>IP:</strong> " + WiFi.localIP().toString() + "</p>";
  html += "<p><strong>Sensor ID:</strong> " + sensorId + " | <strong>Latest P2:</strong> " + String(lastP2Value) + " &mu;g/m&sup3;</p>";
  html += "</div>";
  
  if (sensorCountry.length() > 0 && sensorLatitude.length() > 0 && sensorLongitude.length() > 0) {
    html += "<div class='location-info'>";
    html += "<p><strong>Sensor Location:</strong> " + sensorCountry + " | " + sensorLatitude + ", " + sensorLongitude + "</p>";
    html += "<div class='map-links'>";
    html += "<a href='https://maps.sensor.community/#16/" + sensorLatitude + "/" + sensorLongitude + "' target='_blank'>Sensor Community Map</a>";
    html += "<a href='https://www.google.com/maps/place/" + sensorLatitude + "," + sensorLongitude + "/@" + sensorLatitude + "," + sensorLongitude + ",20z' target='_blank'>Google Maps</a>";
  html += "</div>";
    html += "</div>";
  }
  
  html += "<div class='aqi-info'></div>";
  
  // Firmware Update Section
  html += "<div class='update-section'>";
  html += "<h2>Firmware Update</h2>";
  html += "<p><strong>Current Version:</strong> " + String(VERSION) + "</p>";
  html += "<div class='update-status' id='updateStatus'>Click 'Check for Updates' to check firmware status</div>";
  html += "<div class='progress-bar' id='progressBar' style='display: none;'>";
  html += "<div class='progress' id='progress'></div>";
  html += "</div>";
  html += "<button class='update-btn' onclick='checkForUpdates()'>Check for Updates</button>";
  html += "<button class='update-btn' id='updateBtn' style='display: none;' onclick='performUpdate()'>Perform Update</button>";
  html += "<p style='font-size: 11px; color: #888; margin-top: 10px;'>Do not power off during update!</p>";
  html += "</div>";
  
  html += "<div class='control'><h2>Brightness</h2>";
  html += "<input type='range' min='1' max='255' value='" + String(brightness) + "' id='brightness' onchange='setBrightness(this.value)'>";
  html += "<p id='brightVal'>" + String(brightness) + "</p></div>";
  
  html += "<div class='control'>";
  html += "<button class='sensor-btn' onclick='updateSensorNow()'>Update Sensor Data Now</button>";
  html += "</div>";
  
  html += "<div class='control'>";
  html += "<h2>Sensor Configuration</h2>";
  html += "<button class='map-btn' onclick='window.open(\"https://maps.sensor.community/#11/40.4887/22.5865\", \"_blank\")'>Select sensor id from the sensor.community map</button>";
  html += "<div class='sensor-input' style='margin-top: 10px;'>";
  html += "<input type='text' id='sensorId' value='" + sensorId + "' placeholder='Sensor ID'>";
  html += "<button class='mode-btn' onclick='setSensorId()'>Set Sensor ID</button>";
  html += "</div>";
  html += "</div>";
  
  html += "<div class='control'>";
  html += "<button class='off-btn' onclick='turnOff()'>Turn OFF</button>";
  html += "</div>";
  
  html += "<div class='control'>";
  html += "<button class='debug-btn' onclick='window.location.href=\"/debug\"'>Debug Info</button>";
  html += "</div>";
  
  html += "<div class='control'>";
  html += "<button class='reset-btn' onclick='if(confirm(\"Reset WiFi settings?\")) resetWiFi()'>Reset WiFi</button>";
  html += "</div>";
  
  // Footer section
  html += "<div class='footer'>";
  html += "<p>Created by Art-Net | Hosted by EZHellas | Maintained by SolidarIT</p>";
  html += "<div class='version'>AirQ-Ball - " + String(VERSION) + " - " + String(BUILD_DATE) + "</div>";
  html += "</div>";
  
  html += "<script>";
  html += "function setBrightness(val) { document.getElementById('brightVal').innerText = val; fetch('/setBrightness?value='+val); }";
  html += "function turnOff() { fetch('/off'); }";
  html += "function resetWiFi() { fetch('/reset').then(() => alert('WiFi reset! Device will restart in AP mode.')); }";
  html += "function setSensorId() { var sensorId = document.getElementById('sensorId').value; fetch('/setSensor?id=' + sensorId).then(() => alert('Sensor ID updated!')); }";
  html += "function updateSensorNow() { fetch('/updateSensor').then(() => alert('Sensor data updated!')); }";
  
  // OTA Update functions
  html += "function checkForUpdates() {";
  html += "document.getElementById('updateStatus').innerHTML = 'Checking for updates...';";
  html += "fetch('/checkUpdate')";
  html += ".then(response => response.text())";
  html += ".then(data => {";
  html += "document.getElementById('updateStatus').innerHTML = data;";
  html += "if (data.includes('Update available')) {";
  html += "document.getElementById('updateBtn').style.display = 'inline-block';";
  html += "}";
  html += "})";
  html += ".catch(error => {";
  html += "document.getElementById('updateStatus').innerHTML = 'Error checking for updates';";
  html += "});";
  html += "}";
  
  html += "function performUpdate() {";
  html += "if (!confirm('Are you sure you want to update the firmware? Do not power off during update!')) return;";
  html += "document.getElementById('updateStatus').innerHTML = 'Starting update...';";
  html += "document.getElementById('progressBar').style.display = 'block';";
  html += "document.getElementById('updateBtn').disabled = true;";
  html += "fetch('/performUpdate');";
  html += "// Start polling for update status";
  html += "var progressInterval = setInterval(function() {";
  html += "fetch('/updateStatus')";
  html += ".then(response => response.json())";
  html += ".then(data => {";
  html += "document.getElementById('updateStatus').innerHTML = data.status;";
  html += "document.getElementById('progress').style.width = data.progress + '%';";
  html += "if (data.status.includes('Rebooting') || data.status.includes('failed') || data.status.includes('No updates')) {";
  html += "clearInterval(progressInterval);";
  html += "if (data.status.includes('Rebooting')) {";
  html += "setTimeout(function() { window.location.href = '/'; }, 10000);";
  html += "}";
  html += "}";
  html += "});";
  html += "}, 2000);";
  html += "}";
  
  html += "// Check for updates on page load";
  html += "window.onload = function() {";
  html += "setTimeout(checkForUpdates, 1000);";
  html += "};";
  html += "</script>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleDebugPage() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta charset='UTF-8'>";
  html += "<title>AirQ-Ball Debug</title>";
  html += "<meta http-equiv='refresh' content='60'>";
  html += "<style>";
  html += "body { font-family: Arial; padding: 20px; background: #1a1a1a; color: white; max-width: 600px; margin: 0 auto; }";
  html += ".header { display: flex; align-items: center; justify-content: center; margin-bottom: 20px; }";
  html += ".header img { width: 100px; height: 100px; border-radius: 10px; cursor: pointer; }";
  html += ".header h1 { margin: 0 15px; color: #4CAF50; text-align: center; }";
  html += ".debug-info { background: #2a2a2a; padding: 15px; border-radius: 6px; margin: 10px 0; font-family: monospace; font-size: 14px; border-left: 4px solid #4CAF50; }";
  html += ".debug-section { margin: 20px 0; }";
  html += ".debug-section h2 { color: #2196F3; border-bottom: 1px solid #444; padding-bottom: 5px; }";
  html += ".status-item { display: flex; justify-content: space-between; margin: 8px 0; }";
  html += ".status-label { font-weight: bold; color: #4CAF50; }";
  html += ".status-value { color: #FFC107; }";
  html += ".footer { text-align: center; margin-top: 25px; padding: 12px; background: #222; border-radius: 6px; font-size: 11px; color: #888; }";
  html += ".refresh-info { text-align: center; color: #888; font-size: 12px; margin: 10px 0; }";
  html += ".back-btn { background: #666; color: white; padding: 10px 20px; border: none; border-radius: 4px; cursor: pointer; margin: 10px; }";
  html += ".version { margin-top: 10px; font-size: 10px; color: #666; }";
  html += "</style>";
  html += "</head><body>";
  
  // Header section with logos
  html += "<div class='header'>";
  html += "<a href='https://solidarit.gr' target='_blank'><img src='" + String(LOGO_URL) + "' alt='SolidarIT Logo'></a>";
  html += "<a href='/' style='text-decoration: none;'><h1>AirQ-Ball Debug</h1></a>";
  html += "<a href='https://airq.gr/airq-ball/' target='_blank'><img src='" + String(AIRQ_LOGO_URL) + "' alt='Air Quality Logo'></a>";
  html += "</div>";
  
  html += "<div class='refresh-info'>Page auto-refreshes every 60 seconds</div>";
  
  html += "<div class='debug-section'>";
  html += "<h2>Device Information</h2>";
  html += "<div class='debug-info'>";
  html += "<div class='status-item'><span class='status-label'>Version:</span><span class='status-value'>" + String(VERSION) + "</span></div>";
  html += "<div class='status-item'><span class='status-label'>Device Name:</span><span class='status-value'>" + hostname + "</span></div>";
  html += "<div class='status-item'><span class='status-label'>Chip ID:</span><span class='status-value'>" + chipId + "</span></div>";
  html += "<div class='status-item'><span class='status-label'>AP Mode:</span><span class='status-value'>" + String(apMode ? "Yes" : "No") + "</span></div>";
  html += "<div class='status-item'><span class='status-label'>Free Heap:</span><span class='status-value'>" + String(ESP.getFreeHeap()) + " bytes</span></div>";
  html += "<div class='status-item'><span class='status-label'>Uptime:</span><span class='status-value'>" + String(millis() / 1000) + " seconds</span></div>";
  html += "</div>";
  html += "</div>";
  
  html += "<div class='debug-section'>";
  html += "<h2>Network Information</h2>";
  html += "<div class='debug-info'>";
  if (!apMode) {
    html += "<div class='status-item'><span class='status-label'>WiFi SSID:</span><span class='status-value'>" + WiFi.SSID() + "</span></div>";
    html += "<div class='status-item'><span class='status-label'>IP Address:</span><span class='status-value'>" + WiFi.localIP().toString() + "</span></div>";
    html += "<div class='status-item'><span class='status-label'>MAC Address:</span><span class='status-value'>" + WiFi.macAddress() + "</span></div>";
    html += "<div class='status-item'><span class='status-label'>Signal Strength:</span><span class='status-value'>" + String(WiFi.RSSI()) + " dBm</span></div>";
  } else {
    html += "<div class='status-item'><span class='status-label'>AP SSID:</span><span class='status-value'>" + apSSID + "</span></div>";
    html += "<div class='status-item'><span class='status-label'>AP IP:</span><span class='status-value'>" + WiFi.softAPIP().toString() + "</span></div>";
    html += "<div class='status-item'><span class='status-label'>AP MAC:</span><span class='status-value'>" + WiFi.softAPmacAddress() + "</span></div>";
  }
  html += "</div>";
  html += "</div>";
  
  html += "<div class='debug-section'>";
  html += "<h2>Air Quality Information</h2>";
  html += "<div class='debug-info'>";
  html += "<div class='status-item'><span class='status-label'>Sensor ID:</span><span class='status-value'>" + sensorId + "</span></div>";
  html += "<div class='status-item'><span class='status-label'>Latest P2 Value:</span><span class='status-value'>" + String(lastP2Value) + " &mu;g/m&sup3;</span></div>";
  html += "<div class='status-item'><span class='status-label'>Last Update:</span><span class='status-value'>" + String((millis() - lastSensorUpdate) / 1000) + " seconds ago</span></div>";
  if (sensorCountry.length() > 0 && sensorLatitude.length() > 0 && sensorLongitude.length() > 0) {
    html += "<div class='status-item'><span class='status-label'>Sensor Location:</span><span class='status-value'>" + sensorCountry + " - " + sensorLatitude + ", " + sensorLongitude + "</span></div>";
  }
  html += "<div class='status-item'><span class='status-label'>Air Quality Level:</span><span class='status-value'>";
  if (lastP2Value <= 5.0) html += "Good (0-5)";
  else if (lastP2Value <= 15.0) html += "Moderate (5-15)";
  else if (lastP2Value <= 25.0) html += "Poor (15-25)";
  else if (lastP2Value <= 40.0) html += "Unhealthy (25-40)";
  else if (lastP2Value <= 55.0) html += "Very Unhealthy (40-55)";
  else if (lastP2Value <= 60.0) html += "Hazardous (55-60)";
  else html += "Dangerous (60+)";
  html += "</span></div>";
  html += "</div>";
  html += "</div>";
  
  html += "<div class='debug-section'>";
  html += "<h2>LED Settings</h2>";
  html += "<div class='debug-info'>";
  html += "<div class='status-item'><span class='status-label'>Current Mode:</span><span class='status-value'>" + String(currentMode == 0 ? "Solid" : currentMode == 1 ? "Sparkle" : "Breathing") + "</span></div>";
  html += "<div class='status-item'><span class='status-label'>Brightness:</span><span class='status-value'>" + String(brightness) + "/255</span></div>";
  html += "<div class='status-item'><span class='status-label'>Color (RGB):</span><span class='status-value'>" + String(currentColor.r) + ", " + String(currentColor.g) + ", " + String(currentColor.b) + "</span></div>";
  html += "<div class='status-item'><span class='status-label'>LED Count:</span><span class='status-value'>" + String(NUM_LEDS) + "</span></div>";
  html += "</div>";
  html += "</div>";
  
  // Back button
  html += "<div style='text-align: center; margin-top: 20px;'>";
  html += "<button class='back-btn' onclick='window.location.href=\"/\"'>Back to Control</button>";
  html += "</div>";
  
  // Footer section
  html += "<div class='footer'>";
  html += "<p>Created by Art-Net | Hosted by EZHellas | Maintained by SolidarIT</p>";
  html += "<div class='version'>AirQ-Ball - " + String(VERSION) + " - " + String(BUILD_DATE) + "</div>";
  html += "</div>";
  
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

// OTA Update handlers
void handleCheckUpdate() {
  String result = checkForUpdates();
  server.send(200, "text/plain", result);
}

void handlePerformUpdate() {
  server.send(200, "text/plain", "Update started");
  performUpdate();
}

void handleUpdateStatus() {
  String json = "{\"status\": \"" + updateStatus + "\", \"progress\": " + String(updateProgress) + "}";
  server.send(200, "application/json", json);
}

void handleSetColor() {
  int r = server.arg("r").toInt();
  int g = server.arg("g").toInt();
  int b = server.arg("b").toInt();
  currentColor = CRGB(r, g, b);
  currentMode = 0; // Switch to solid mode when color is set
  server.send(200, "text/plain", "OK");
}

void handleSetBrightness() {
  brightness = server.arg("value").toInt();
  FastLED.setBrightness(brightness);
  // Save brightness to EEPROM
  EEPROM.write(BRIGHTNESS_ADDR, brightness);
  EEPROM.commit();
  server.send(200, "text/plain", "OK");
}

void handleSetMode() {
  currentMode = server.arg("mode").toInt();
  server.send(200, "text/plain", "OK");
}

void handleSetSensor() {
  String newSensorId = server.arg("id");
  if (newSensorId.length() > 0) {
    sensorId = newSensorId;
    writeStringToEEPROM(SENSOR_ID_ADDR, sensorId);
    EEPROM.commit();
    Serial.println("Sensor ID updated to: " + sensorId);
    updateSensorData(); // Update immediately with new sensor
  }
  server.send(200, "text/plain", "OK");
}

void handleUpdateSensor() {
  if (updateSensorData()) {
    server.send(200, "text/plain", "Sensor data updated: " + String(lastP2Value));
  } else {
    server.send(500, "text/plain", "Failed to update sensor data");
  }
}

void handleOff() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
  currentMode = -1;
  server.send(200, "text/plain", "OK");
}

void handleReset() {
  // Clear EEPROM
  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
  server.send(200, "text/plain", "OK");
  delay(1000);
  ESP.restart();
}

// EEPROM helper functions
void writeStringToEEPROM(int addr, String data) {
  int len = data.length();
  EEPROM.write(addr, len);
  for (int i = 0; i < len; i++) {
    EEPROM.write(addr + 1 + i, data[i]);
  }
}

String readStringFromEEPROM(int addr) {
  int len = EEPROM.read(addr);
  if (len == 0 || len > 99) return "";
  char data[len + 1];
  for (int i = 0; i < len; i++) {
    data[i] = EEPROM.read(addr + 1 + i);
  }
  data[len] = '\0';
  return String(data);
}

// Animation functions
void startupAnimation() {
  for(int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CRGB::Blue;
    FastLED.show();
    delay(100);
  }
  delay(200);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
}

void apModeAnimation() {
  for(int i = 0; i < 3; i++) {
    fill_solid(leds, NUM_LEDS, CRGB::Orange);
    FastLED.show();
    delay(200);
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
    delay(200);
  }
}

void connectedAnimation() {
  for(int i = 0; i < 3; i++) {
    fill_solid(leds, NUM_LEDS, CRGB::Green);
    FastLED.show();
    delay(200);
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
    delay(200);
  }
}

void updatingAnimation() {
  for(int i = 0; i < 5; i++) {
    fill_solid(leds, NUM_LEDS, CRGB::Yellow);
    FastLED.show();
    delay(300);
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
    delay(300);
  }
}

void sparkle() {
  static unsigned long lastSparkle = 0;
  if (millis() - lastSparkle > 100) {
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    leds[random(NUM_LEDS)] = currentColor;
    FastLED.show();
    lastSparkle = millis();
  }
  delay(10);
}

void breathing() {
  static int breathBrightness = 0;
  static int breathDirection = 1;
  static unsigned long lastBreathTime = 0;
  
  // Slower breathing for red and purple (deeper and slower)
  unsigned long breathInterval = (lastP2Value > 60.0) ? 800 : 2000; // 1.6sec for >60, 4sec for 55-60
  
  if (millis() - lastBreathTime >= 80) { // Slower update for deeper breathing
    fill_solid(leds, NUM_LEDS, currentColor);
    
    // Slower brightness change for deeper effect
    breathBrightness += breathDirection * 2;
    if(breathBrightness >= brightness) {
      breathBrightness = brightness;
      breathDirection = -1;
    }
    if(breathBrightness <= 5) { // Deeper minimum for more dramatic effect
      breathBrightness = 5;
      breathDirection = 1;
    }
    
    FastLED.setBrightness(breathBrightness);
    FastLED.show();
    lastBreathTime = millis();
  }
  delay(20); // Increased delay for slower overall loop
}