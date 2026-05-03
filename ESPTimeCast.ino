#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <ESP8266mDNS.h>  // Add mDNS library
#include <sntp.h>
#include <time.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>

// Forward declarations
bool shouldHandleWebRequest();
void processFetchStationsData();
void processProxyRequest();
bool refreshNetatmoToken();
void extractDeviceInfo();
void fetchStationsDataFallback();
void processSaveCredentials();
void setupOptimizedHandlers();
void saveTokensToConfig();

#include "mfactoryfont.h"  // Replace with your font, or comment/remove if not using custom
#include "tz_lookup.h" // Timezone lookup, do not duplicate mapping here!

// Function prototypes
String formatTemperature(float temperature, bool roundToInteger = false);
void fetchNetatmoIndoorTemperature();
String getNetatmoToken();
void forceNetatmoTokenRefresh();
bool parseNetatmoJson(String &payload, JsonDocument &doc);
String fetchNetatmoDevices();
void createDefaultConfig();
void setupNetatmoHandler();
void setupSaveSettingsHandler();
void processTokenExchange();
void processFetchDevices();
void triggerNetatmoDevicesFetch();
void processSettingsSave();
bool isNetatmoTokenValid();
bool checkNetworkConnectivity();
bool writeChunkedResponseToFile(WiFiClient* stream, File& file);
bool isChunkedResponse(HTTPClient& http);
String urlEncode(const char* input);
void exchangeAuthCode(const String &code);
void handleBlockedRequest(String errorPayload);
void setupCrashHandler();
String getCrashInfoHtml();
bool checkAndRepairConfig();
void checkNetatmoConfig();
String getExceptionDetails(rst_info *resetInfo);
void printMemoryStats();
void defragmentHeap();
void forceGarbageCollection();
extern bool apiCallInProgress;
bool shouldDefragment();
bool isInvalidTokenError(const String &errorPayload);
void enhanceApiResponse(AsyncWebServerRequest *request, const char* contentType, const String &payload);

// Function to create default config.json
void createDefaultConfig() {
  // External variables used in this function
  extern int brightness;
  extern float tempAdjust;
  extern bool flipDisplay;
  extern bool twelveHourToggle;
  extern bool showDayOfWeek;
  extern bool showIndoorTemp;
  extern bool showOutdoorTemp;
  extern bool useNetatmoOutdoor;
  extern bool prioritizeNetatmoIndoor;
  extern char ntpServer1[64];
  extern char ntpServer2[64];
  
  Serial.println(F("[CONFIG] Creating default config.json"));
  
  // Make sure LittleFS is mounted
  if (!LittleFS.begin()) {
    Serial.println(F("[CONFIG] ERROR: LittleFS not mounted, cannot create config"));
    return;
  }
  
  // Create a minimal config first
  JsonDocument doc; // Increased size to accommodate all fields
  doc[F("ssid")] = "";
  doc[F("password")] = "";
  doc[F("mdnsHostname")] = "esptime";
  doc[F("weatherUnits")] = "metric";
  doc[F("timeZone")] = "";
  doc[F("brightness")] = brightness;
  doc[F("flipDisplay")] = flipDisplay;
  doc[F("twelveHourToggle")] = twelveHourToggle;
  doc[F("showDayOfWeek")] = showDayOfWeek;
  doc[F("showIndoorTemp")] = showIndoorTemp;
  doc[F("showOutdoorTemp")] = showOutdoorTemp;
  doc[F("clockDuration")] = 10000;
  doc[F("weatherDuration")] = 5000;
  doc[F("tempAdjust")] = tempAdjust;
  doc[F("ntpServer1")] = ntpServer1;
  doc[F("ntpServer2")] = ntpServer2;
  
  // Netatmo fields (excluding tokens)
  doc[F("netatmoClientId")] = "";
  doc[F("netatmoClientSecret")] = "";
  doc[F("netatmoDeviceId")] = "";
  doc[F("netatmoModuleId")] = "";
  doc[F("netatmoIndoorModuleId")] = "";
  doc[F("useNetatmoOutdoor")] = useNetatmoOutdoor;
  doc[F("prioritizeNetatmoIndoor")] = prioritizeNetatmoIndoor;
  
  // Write the minimal config
  File f = LittleFS.open("/config.json", "w");
  if (!f) {
    Serial.println(F("[CONFIG] ERROR: Failed to open config.json for writing"));
    return;
  }
  
  if (serializeJsonPretty(doc, f) == 0) {
    Serial.println(F("[CONFIG] ERROR: Failed to write to config.json"));
    f.close();
    return;
  }
  
  f.close();
  Serial.println(F("[CONFIG] Default config.json created successfully"));
  
  // Create default tokens.json
  JsonDocument tokenDoc;
  tokenDoc[F("netatmoAccessToken")] = "";
  tokenDoc[F("netatmoRefreshToken")] = "";
  
  File tokenFile = LittleFS.open("/tokens.json", "w");
  if (!tokenFile) {
    Serial.println(F("[CONFIG] ERROR: Failed to open tokens.json for writing"));
    return;
  }
  
  if (serializeJsonPretty(tokenDoc, tokenFile) == 0) {
    Serial.println(F("[CONFIG] ERROR: Failed to write to tokens.json"));
    tokenFile.close();
    return;
  }
  
  tokenFile.close();
  Serial.println(F("[CONFIG] Default tokens.json created successfully"));
  
  // Verify the files were created correctly
  f = LittleFS.open("/config.json", "r");
  if (!f) {
    Serial.println(F("[CONFIG] ERROR: Failed to open config.json for verification"));
    return;
  }
  
  size_t fileSize = f.size();
  Serial.print(F("[CONFIG] config.json size: "));
  Serial.println(fileSize);
  
  if (fileSize == 0) {
    Serial.println(F("[CONFIG] ERROR: config.json is empty after creation"));
    f.close();
    return;
  }
  
  f.close();
  
  tokenFile = LittleFS.open("/tokens.json", "r");
  if (!tokenFile) {
    Serial.println(F("[CONFIG] ERROR: Failed to open tokens.json for verification"));
    return;
  }
  
  fileSize = tokenFile.size();
  Serial.print(F("[CONFIG] tokens.json size: "));
  Serial.println(fileSize);
  
  if (fileSize == 0) {
    Serial.println(F("[CONFIG] ERROR: tokens.json is empty after creation"));
    tokenFile.close();
    return;
  }
  
  tokenFile.close();
}

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 4
#define CLK_PIN   12
#define DATA_PIN  15
#define CS_PIN    13

// DS18B20 Configuration
#define ONE_WIRE_BUS 4  // GPIO4 (D2) for DS18B20 data
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
MD_Parola P = MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);

AsyncWebServer server(80);

char ssid[32] = "";
char password[32] = "";
// Netatmo API credentials and tokens
char netatmoClientId[64] = "";
char netatmoClientSecret[64] = "";
char netatmoAccessToken[256] = "";
char netatmoRefreshToken[256] = "";
char netatmoDeviceId[64] = "";
char netatmoStationId[64] = ""; // Station ID for Netatmo
char netatmoModuleId[64] = "";
char netatmoIndoorModuleId[64] = ""; // New: Module ID for indoor temperature from Netatmo
char mdnsHostname[32] = "esptime"; // Default mDNS hostname
char weatherUnits[12] = "metric";
char timeZone[64] = "";

// Reboot control variables
bool rebootPending = false;
unsigned long rebootTime = 0;
unsigned long clockDuration = 10000;
unsigned long weatherDuration = 5000;
int tempSource = 0; // 0: Local sensor primary, 1: Netatmo primary, 2: Netatmo only

// ADVANCED SETTINGS
int brightness = 7;
float tempAdjust = 0.0;
bool flipDisplay = false;
bool twelveHourToggle = false;
bool showDayOfWeek = true;
bool showIndoorTemp = true;
bool showOutdoorTemp = true;
bool useNetatmoOutdoor = false;
bool prioritizeNetatmoIndoor = false;
char ntpServer1[64] = "pool.ntp.org";
char ntpServer2[64] = "time.nist.gov";

WiFiClient client;
const byte DNS_PORT = 53;
DNSServer dnsServer;

String indoorTemp = "";
String outdoorTemp = "";
bool indoorTempAvailable = false;
bool outdoorTempAvailable = false;
float netatmoIndoorTemp = 0;
bool netatmoIndoorTempAvailable = false;
bool weatherFetched = false;
bool weatherFetchInitiated = false;
bool isAPMode = false;
char tempSymbol = 'C';

unsigned long lastSwitch = 0;
unsigned long lastColonBlink = 0;
int displayMode = 0;
bool indicatorVisible = true; // Shared visibility state for colon and temperature indicators
bool needTokenRefresh = false; // Flag to indicate we need to refresh the token soon
unsigned long lastBlockedRequest = 0; // Timestamp of the last blocked request

bool ntpSyncSuccessful = false;

char daysOfTheWeek[7][12] = {"&", "*", "/", "?", "@", "=", "$"};
char indoorSymbol[12] = {"^"};  // Custom symbol for indoor temperature (IN)
char outdoorSymbol[12] = {"~"}; // Custom symbol for outdoor temperature (OUT)

// Netatmo status for display indicators
enum NetatmoStatus {
  NETATMO_OK,        // Working fine
  NETATMO_NO_TOKEN,  // No token, needs OAuth2 auth -> "auth"
  NETATMO_AUTH_FAIL, // Token refresh failed          -> "auth"
  NETATMO_CONN_FAIL, // Connection/network error      -> "conn"
  NETATMO_NO_CONFIG, // Missing device/module IDs     -> "conf"
  NETATMO_API_ERROR  // API returned an error         -> "err"
};
NetatmoStatus netatmoStatus = NETATMO_OK;
int lastNetatmoHttpCode = 0;



// NTP Synchronization State Machine
enum NtpState {
  NTP_IDLE,
  NTP_SYNCING,
  NTP_SUCCESS,
  NTP_FAILED
};

NtpState ntpState = NTP_IDLE;
unsigned long ntpStartTime = 0;
const int ntpTimeout = 30000; // 30 seconds
const int maxNtpRetries = 30;
int ntpRetryCount = 0;

void printConfigToSerial() {
  Serial.println(F("========= Loaded Configuration ========="));
  Serial.print(F("WiFi SSID: ")); Serial.println(ssid);
  Serial.print(F("WiFi Password: ")); Serial.println(password);
  Serial.print(F("Netatmo Client ID: ")); Serial.println(netatmoClientId);
  Serial.print(F("Netatmo Client Secret: ")); Serial.println(netatmoClientSecret);
  Serial.print(F("Netatmo Device ID: ")); Serial.println(netatmoDeviceId);
  Serial.print(F("Netatmo Module ID: ")); Serial.println(netatmoModuleId);
  Serial.print(F("Temperature Unit: ")); Serial.println(weatherUnits);
  Serial.print(F("Clock duration: ")); Serial.println(clockDuration);
  Serial.print(F("Weather duration: ")); Serial.println(weatherDuration);
  Serial.print(F("TimeZone (IANA): ")); Serial.println(timeZone);
  Serial.print(F("Brightness: ")); Serial.println(brightness);
  Serial.print(F("Temperature Adjustment: ")); Serial.println(tempAdjust);
  Serial.print(F("Flip Display: ")); Serial.println(flipDisplay ? "Yes" : "No");
  Serial.print(F("Show 12h Clock: ")); Serial.println(twelveHourToggle ? "Yes" : "No");
  Serial.print(F("Show Day of the Week: ")); Serial.println(showDayOfWeek ? "Yes" : "No");
  Serial.print(F("Show Indoor Temp: ")); Serial.println(showIndoorTemp ? "Yes" : "No");
  Serial.print(F("Show Outdoor Temp: ")); Serial.println(showOutdoorTemp ? "Yes" : "No");
  Serial.print(F("NTP Server 1: ")); Serial.println(ntpServer1);
  Serial.print(F("NTP Server 2: ")); Serial.println(ntpServer2);
  Serial.println(F("========================================"));
  Serial.println();
}

// Function to format LittleFS if needed
bool formatLittleFS() {
  Serial.println(F("[CONFIG] Formatting LittleFS..."));
  bool success = LittleFS.format();
  if (success) {
    Serial.println(F("[CONFIG] LittleFS formatted successfully"));
  } else {
    Serial.println(F("[CONFIG] ERROR: Failed to format LittleFS"));
  }
  return success;
}

void loadConfig() {
  Serial.println(F("[CONFIG] Loading configuration..."));
  
  // Try to mount LittleFS
  if (!LittleFS.begin()) {
    Serial.println(F("[CONFIG] ERROR: LittleFS mount failed, trying to format"));
    
    // Try to format the file system
    if (formatLittleFS()) {
      // Try to mount again
      if (!LittleFS.begin()) {
        Serial.println(F("[CONFIG] ERROR: LittleFS mount failed even after formatting"));
        return;
      }
    } else {
      Serial.println(F("[CONFIG] ERROR: LittleFS format failed"));
      return;
    }
  }

  // Load main configuration
  if (!LittleFS.exists("/config.json")) {
    Serial.println(F("[CONFIG] config.json not found, creating with defaults..."));
    createDefaultConfig();
  }
  
  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println(F("[CONFIG] ERROR: Failed to open config.json"));
    
    // Try to recreate the config file
    Serial.println(F("[CONFIG] Trying to recreate config.json"));
    createDefaultConfig();
    
    // Try to open again
    configFile = LittleFS.open("/config.json", "r");
    if (!configFile) {
      Serial.println(F("[CONFIG] ERROR: Failed to open config.json after recreation"));
      return;
    }
  }
  
  size_t size = configFile.size();
  if (size == 0 || size > 2048) {
    Serial.println(F("[ERROR] Invalid config file size"));
    configFile.close();
    return;
  }
  
  String jsonString = configFile.readString();
  configFile.close();
  
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, jsonString);
  if (error) {
    Serial.print(F("[ERROR] JSON parse failed: "));
    Serial.println(error.f_str());
    return;
  }
  
  // Load main configuration settings
  if (doc["ssid"].is<JsonVariant>()) strlcpy(ssid, doc["ssid"], sizeof(ssid));
  if (doc["password"].is<JsonVariant>()) strlcpy(password, doc["password"], sizeof(password));
  if (doc["netatmoClientId"].is<JsonVariant>()) strlcpy(netatmoClientId, doc["netatmoClientId"], sizeof(netatmoClientId));
  if (doc["netatmoClientSecret"].is<JsonVariant>()) strlcpy(netatmoClientSecret, doc["netatmoClientSecret"], sizeof(netatmoClientSecret));
  
  // Load Netatmo settings from main config
  if (doc["netatmoDeviceId"].is<JsonVariant>()) strlcpy(netatmoDeviceId, doc["netatmoDeviceId"], sizeof(netatmoDeviceId));
  if (doc["netatmoModuleId"].is<JsonVariant>()) strlcpy(netatmoModuleId, doc["netatmoModuleId"], sizeof(netatmoModuleId));
  if (doc["netatmoIndoorModuleId"].is<JsonVariant>()) strlcpy(netatmoIndoorModuleId, doc["netatmoIndoorModuleId"], sizeof(netatmoIndoorModuleId));
  if (doc["netatmoStationId"].is<JsonVariant>()) strlcpy(netatmoStationId, doc["netatmoStationId"], sizeof(netatmoStationId));
  if (doc["useNetatmoOutdoor"].is<JsonVariant>()) useNetatmoOutdoor = doc["useNetatmoOutdoor"];
  if (doc["prioritizeNetatmoIndoor"].is<JsonVariant>()) {
    prioritizeNetatmoIndoor = doc["prioritizeNetatmoIndoor"];
    // Set tempSource based on prioritizeNetatmoIndoor setting
    tempSource = prioritizeNetatmoIndoor ? 1 : 0;
  }
  
  // Now load tokens from tokens.json if it exists
  if (LittleFS.exists("/tokens.json")) {
    Serial.println(F("[CONFIG] Loading tokens from tokens.json"));
    File tokenFile = LittleFS.open("/tokens.json", "r");
    if (tokenFile) {
      JsonDocument tokenDoc;
      DeserializationError tokenError = deserializeJson(tokenDoc, tokenFile);
      tokenFile.close();
      
      if (!tokenError) {
        if (tokenDoc["netatmoAccessToken"].is<JsonVariant>()) {
          strlcpy(netatmoAccessToken, tokenDoc["netatmoAccessToken"], sizeof(netatmoAccessToken));
          Serial.println(F("[CONFIG] Loaded access token from tokens.json"));
        }
        
        if (tokenDoc["netatmoRefreshToken"].is<JsonVariant>()) {
          strlcpy(netatmoRefreshToken, tokenDoc["netatmoRefreshToken"], sizeof(netatmoRefreshToken));
          Serial.println(F("[CONFIG] Loaded refresh token from tokens.json"));
        }
      } else {
        Serial.print(F("[CONFIG] Error parsing tokens.json: "));
        Serial.println(tokenError.c_str());
      }
    } else {
      Serial.println(F("[CONFIG] Failed to open tokens.json"));
    }
  } else {
    Serial.println(F("[CONFIG] tokens.json not found, checking for tokens in config.json"));
    
    // For backward compatibility, check if tokens are in config.json
    if (doc["netatmoAccessToken"].is<JsonVariant>()) {
      strlcpy(netatmoAccessToken, doc["netatmoAccessToken"], sizeof(netatmoAccessToken));
      Serial.println(F("[CONFIG] Loaded access token from config.json"));
    }
    
    if (doc["netatmoRefreshToken"].is<JsonVariant>()) {
      strlcpy(netatmoRefreshToken, doc["netatmoRefreshToken"], sizeof(netatmoRefreshToken));
      Serial.println(F("[CONFIG] Loaded refresh token from config.json"));
      
      // If we found tokens in config.json, save them to tokens.json for future use
      if (strlen(netatmoAccessToken) > 0 || strlen(netatmoRefreshToken) > 0) {
        Serial.println(F("[CONFIG] Migrating tokens to tokens.json"));
        saveTokensToConfig();
      }
    }
  }
  
  // Debug output for raw JSON content
  Serial.println(F("[CONFIG] Raw JSON content from config.json:"));
  String jsonDebug;
  serializeJson(doc, jsonDebug);
  Serial.println(jsonDebug);
  
  // Check if Netatmo fields exist in the JSON
  Serial.println(F("[CONFIG] Checking for Netatmo fields in config.json:"));
  Serial.print(F("[CONFIG] netatmoDeviceId exists: "));
  Serial.println(doc["netatmoDeviceId"].is<JsonVariant>() ? "Yes" : "No");
  Serial.print(F("[CONFIG] netatmoModuleId exists: "));
  Serial.println(doc["netatmoModuleId"].is<JsonVariant>() ? "Yes" : "No");
  Serial.print(F("[CONFIG] netatmoStationId exists: "));
  Serial.println(doc["netatmoStationId"].is<JsonVariant>() ? "Yes" : "No");
  
  // Debug output for Netatmo settings
  Serial.println(F("[CONFIG] Netatmo settings loaded:"));
  Serial.print(F("[CONFIG] Device ID: '"));
  Serial.print(netatmoDeviceId);
  Serial.println(F("'"));
  Serial.print(F("[CONFIG] Station ID: '"));
  Serial.print(netatmoStationId);
  Serial.println(F("'"));
  Serial.print(F("[CONFIG] Module ID: '"));
  Serial.print(netatmoModuleId);
  Serial.println(F("'"));
  Serial.print(F("[CONFIG] Indoor Module ID: '"));
  Serial.print(netatmoIndoorModuleId);
  Serial.println(F("'"));
  if (doc["tempSource"].is<JsonVariant>()) tempSource = doc["tempSource"];
  if (doc["mdnsHostname"].is<JsonVariant>()) strlcpy(mdnsHostname, doc["mdnsHostname"], sizeof(mdnsHostname));
  if (doc["weatherUnits"].is<JsonVariant>()) strlcpy(weatherUnits, doc["weatherUnits"], sizeof(weatherUnits));
  if (doc["clockDuration"].is<JsonVariant>()) clockDuration = doc["clockDuration"];
  if (doc["weatherDuration"].is<JsonVariant>()) weatherDuration = doc["weatherDuration"];
  if (doc["timeZone"].is<JsonVariant>()) strlcpy(timeZone, doc["timeZone"], sizeof(timeZone));
  if (doc["brightness"].is<JsonVariant>()) brightness = doc["brightness"];
  if (doc["tempAdjust"].is<JsonVariant>()) tempAdjust = doc["tempAdjust"];
  if (doc["flipDisplay"].is<JsonVariant>()) flipDisplay = doc["flipDisplay"];
  if (doc["twelveHourToggle"].is<JsonVariant>()) twelveHourToggle = doc["twelveHourToggle"];
  if (doc["showDayOfWeek"].is<JsonVariant>()) showDayOfWeek = doc["showDayOfWeek"];
  if (doc["showIndoorTemp"].is<JsonVariant>()) showIndoorTemp = doc["showIndoorTemp"]; else showIndoorTemp = true;
  if (doc["showOutdoorTemp"].is<JsonVariant>()) showOutdoorTemp = doc["showOutdoorTemp"]; else showOutdoorTemp = true;
  if (doc["ntpServer1"].is<JsonVariant>()) strlcpy(ntpServer1, doc["ntpServer1"], sizeof(ntpServer1));
  if (doc["ntpServer2"].is<JsonVariant>()) strlcpy(ntpServer2, doc["ntpServer2"], sizeof(ntpServer2));
  
  if (strcmp(weatherUnits, "imperial") == 0)
    tempSymbol = 'F';
  else if (strcmp(weatherUnits, "standard") == 0)
    tempSymbol = 'K';
  else
    tempSymbol = 'C';
    
  Serial.println(F("[CONFIG] Configuration loaded."));
}

void connectWiFi() {
  Serial.println(F("[WIFI] Connecting to WiFi..."));
  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(ssid, password);
  unsigned long startAttemptTime = millis();
  const unsigned long timeout = 15000;
  unsigned long animTimer = 0;
  int animFrame = 0;
  bool animating = true;
  while (animating) {
    unsigned long now = millis();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println(F("[WiFi] Connected: ") + WiFi.localIP().toString());
      isAPMode = false;
      animating = false;
      
      // Initialize mDNS
      if (MDNS.begin(mdnsHostname)) {
        Serial.print(F("[mDNS] Responder started: "));
        Serial.print(mdnsHostname);
        Serial.println(F(".local"));
        
        // Add service to mDNS
        MDNS.addService("http", "tcp", 80);
      } else {
        Serial.println(F("[mDNS] Failed to start responder"));
      }
      
      break;
    } else if (now - startAttemptTime >= timeout) {
      Serial.println(F("\r\n[WiFi] Failed. Starting AP mode..."));
      WiFi.softAP("ESPTimeCast", "12345678");
      Serial.print(F("AP IP address: "));
      Serial.println(WiFi.softAPIP());
      dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
      isAPMode = true;
      animating = false;
      Serial.println(F("[WIFI] AP Mode Started"));
      break;
    }
    if (now - animTimer > 750) {
      animTimer = now;
      P.setTextAlignment(PA_CENTER);
      switch (animFrame % 3) {
        case 0: P.print("WiFi©"); break;
        case 1: P.print("WiFiª"); break;
        case 2: P.print("WiFi«"); break;
      }
      animFrame++;
    }
    yield();
  }
}

void setupTime() {
  sntp_stop();
  Serial.println(F("[TIME] Starting NTP sync..."));
  configTime(0, 0, ntpServer1, ntpServer2); // Use custom NTP servers
  setenv("TZ", ianaToPosix(timeZone), 1);
  tzset();
  ntpState = NTP_SYNCING;
  ntpStartTime = millis();
  ntpRetryCount = 0;
  ntpSyncSuccessful = false; // Reset the flag
}

void setupWebServer() {
  Serial.println(F("[WEBSERVER] Setting up web server..."));
  
  server.on("/config.json", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println(F("[WEBSERVER] Request: /config.json"));
    
    // Ensure LittleFS is mounted
    if (!LittleFS.begin()) {
      Serial.println(F("[WEBSERVER] ERROR: LittleFS not mounted"));
      request->send(500, "application/json", "{\"error\":\"LittleFS not mounted\"}");
      return;
    }
    
    // Check if config.json exists, create if not
    if (!LittleFS.exists("/config.json")) {
      Serial.println(F("[WEBSERVER] config.json not found, creating default"));
      createDefaultConfig();
      
      // Verify the file was created
      if (!LittleFS.exists("/config.json")) {
        Serial.println(F("[WEBSERVER] ERROR: Failed to create config.json"));
        request->send(500, "application/json", "{\"error\":\"Failed to create config.json\"}");
        return;
      }
    }
    
    // Print file info
    File infoFile = LittleFS.open("/config.json", "r");
    if (infoFile) {
      Serial.print(F("[WEBSERVER] config.json size: "));
      Serial.println(infoFile.size());
      infoFile.close();
    }
    
    File f = LittleFS.open("/config.json", "r");
    if (!f) {
      Serial.println(F("[WEBSERVER] Error opening /config.json"));
      request->send(500, "application/json", "{\"error\":\"Failed to open config.json\"}");
      return;
    }
    
    // Check file size
    size_t fileSize = f.size();
    if (fileSize == 0) {
      Serial.println(F("[WEBSERVER] ERROR: config.json is empty"));
      f.close();
      
      // Try to recreate the file
      Serial.println(F("[WEBSERVER] Attempting to recreate config.json"));
      createDefaultConfig();
      
      // Try to open again
      f = LittleFS.open("/config.json", "r");
      if (!f || f.size() == 0) {
        Serial.println(F("[WEBSERVER] ERROR: Failed to recreate config.json"));
        request->send(500, "application/json", "{\"error\":\"config.json is empty\"}");
        return;
      }
    }
    
    // Read the first few bytes to check content
    Serial.print(F("[WEBSERVER] First bytes of config.json: "));
    for (int i = 0; i < min(20, (int)fileSize); i++) {
      Serial.print((char)f.read());
    }
    Serial.println();
    
    // Reset file position
    f.seek(0);
    
    // Read the file content for debugging
    String fileContent = f.readString();
    Serial.print(F("[WEBSERVER] config.json content: "));
    Serial.println(fileContent);
    
    // Reset file position again
    f.close();
    f = LittleFS.open("/config.json", "r");
    if (!f) {
      Serial.println(F("[WEBSERVER] Error reopening /config.json"));
      request->send(500, "application/json", "{\"error\":\"Failed to reopen config.json\"}");
      return;
    }
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
      Serial.print(F("[WEBSERVER] Error parsing /config.json: "));
      Serial.println(err.f_str());
      
      // Try to read the file content for debugging
      File debugFile = LittleFS.open("/config.json", "r");
      if (debugFile) {
        String content = debugFile.readString();
        Serial.print(F("[WEBSERVER] config.json content: "));
        Serial.println(content);
        debugFile.close();
      }
      
      request->send(500, "application/json", "{\"error\":\"Failed to parse config.json\"}");
      return;
    }
    
    // Debug: Print the values of netatmoClientId and netatmoClientSecret
    Serial.print(F("[WEBSERVER] netatmoClientId in config: "));
    if (doc["netatmoClientId"].is<JsonVariant>()) {
      Serial.println(doc["netatmoClientId"].as<String>());
    } else {
      Serial.println(F("(not found)"));
    }
    
    Serial.print(F("[WEBSERVER] netatmoClientSecret in config: "));
    if (doc["netatmoClientSecret"].is<JsonVariant>()) {
      Serial.println(F("(present but not shown)"));
    } else {
      Serial.println(F("(not found)"));
    }
    
    doc[F("mode")] = isAPMode ? "ap" : "sta";
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });
  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
    Serial.println(F("[WEBSERVER] Request: /save"));    
    
    // First, read the existing config file to preserve all settings
    JsonDocument doc;
    bool configExists = false;
    
    if (LittleFS.exists("/config.json")) {
      File existingConfig = LittleFS.open("/config.json", "r");
      if (existingConfig) {
        DeserializationError error = deserializeJson(doc, existingConfig);
        existingConfig.close();
        
        if (!error) {
          configExists = true;
          Serial.println(F("[WEBSERVER] Successfully read existing config"));
          
          // Debug: Print Netatmo settings before update
          Serial.println(F("[WEBSERVER] Netatmo settings before update:"));
          if (doc["netatmoDeviceId"].is<JsonVariant>()) {
            Serial.print(F("[WEBSERVER] netatmoDeviceId: "));
            Serial.println(doc["netatmoDeviceId"].as<String>());
          }
          if (doc["netatmoModuleId"].is<JsonVariant>()) {
            Serial.print(F("[WEBSERVER] netatmoModuleId: "));
            Serial.println(doc["netatmoModuleId"].as<String>());
          }
        } else {
          Serial.print(F("[WEBSERVER] Error parsing config file: "));
          Serial.println(error.c_str());
          // Continue with empty doc if parsing fails
        }
      }
    }
    
    // Now update only the fields that were changed in the form
    for (int i = 0; i < request->params(); i++) {
      const AsyncWebParameter* p = request->getParam(i);
      String n = p->name();
      String v = p->value();
      if (n == "brightness") doc[n] = v.toInt();
      else if (n == "flipDisplay") doc[n] = (v == "true" || v == "on" || v == "1");
      else if (n == "twelveHourToggle") doc[n] = (v == "true" || v == "on" || v == "1");
      else if (n == "showDayOfWeek") doc[n] = (v == "true" || v == "on" || v == "1"); 
      else if (n == "showIndoorTemp") doc[n] = (v == "true" || v == "on" || v == "1");
      else if (n == "showOutdoorTemp") doc[n] = (v == "true" || v == "on" || v == "1");
      else if (n == "useNetatmoOutdoor") doc[n] = (v == "true" || v == "on" || v == "1");
      else if (n == "prioritizeNetatmoIndoor") doc[n] = (v == "true" || v == "on" || v == "1");
      else if (n == "tempSource") doc[n] = v.toInt();
      else doc[n] = v;
    }
    
    // Create a backup of the existing config if it exists
    if (LittleFS.exists("/config.json")) {
      LittleFS.rename("/config.json", "/config.bak");
    }
    
    // Write the updated config
    File f = LittleFS.open("/config.json", "w");
    if (!f) {
      Serial.println(F("[WEBSERVER] Failed to open /config.json for writing"));
      JsonDocument errorDoc;
      errorDoc[F("error")] = "Failed to write config";
      String response;
      serializeJson(errorDoc, response);
      request->send(500, "application/json", response);
      return;
    }
    
    // Debug: Print Netatmo settings after update
    Serial.println(F("[WEBSERVER] Netatmo settings after update:"));
    if (doc["netatmoDeviceId"].is<JsonVariant>()) {
      Serial.print(F("[WEBSERVER] netatmoDeviceId: "));
      Serial.println(doc["netatmoDeviceId"].as<String>());
    }
    if (doc["netatmoModuleId"].is<JsonVariant>()) {
      Serial.print(F("[WEBSERVER] netatmoModuleId: "));
      Serial.println(doc["netatmoModuleId"].as<String>());
    }
    
    serializeJson(doc, f);
    f.close();
    
    // Create a backup of the configuration
    if (LittleFS.exists("/config.bak")) {
      LittleFS.remove("/config.bak");
    }
    
    if (LittleFS.exists("/config.json")) {
      File src = LittleFS.open("/config.json", "r");
      if (src) {
        File dst = LittleFS.open("/config.bak", "w");
        if (dst) {
          // Copy file contents
          static const size_t BUFFER_SIZE = 256;
          uint8_t buffer[BUFFER_SIZE];
          size_t bytesRead;
          
          while ((bytesRead = src.read(buffer, BUFFER_SIZE)) > 0) {
            dst.write(buffer, bytesRead);
          }
          
          dst.close();
        }
        src.close();
      }
    }
    
    File verify = LittleFS.open("/config.json", "r");
    JsonDocument test;
    DeserializationError err = deserializeJson(test, verify);
    verify.close();
    if (err) {
      Serial.print(F("[WEBSERVER] Config corrupted after save: "));
      Serial.println(err.f_str());
      JsonDocument errorDoc;
      errorDoc[F("error")] = "Config corrupted. Reboot cancelled.";
      String response;
      serializeJson(errorDoc, response);
      request->send(500, "application/json", response);
      return;
    }
    JsonDocument okDoc;
    okDoc[F("message")] = "Saved successfully. Rebooting...";
    String response;
    serializeJson(okDoc, response);
    request->send(200, "application/json", response);
    Serial.println(F("[WEBSERVER] Rebooting..."));
    request->onDisconnect([]() {
      // Ensure all file operations are complete before rebooting
      Serial.println(F("[WEBSERVER] Flushing file system before reboot..."));
      LittleFS.end();  // Properly close the file system
      delay(500);      // Short delay to ensure flash operations complete
      Serial.println(F("[WEBSERVER] Rebooting..."));
      ESP.restart();
    });
  });

  server.on("/restore", HTTP_POST, [](AsyncWebServerRequest *request){
    Serial.println(F("[WEBSERVER] Request: /restore"));
    if (LittleFS.exists("/config.bak")) {
      File src = LittleFS.open("/config.bak", "r");
      if (!src) {
        Serial.println(F("[WEBSERVER] Failed to open /config.bak"));
        JsonDocument errorDoc;
        errorDoc[F("error")] = "Failed to open backup file.";
        String response;
        serializeJson(errorDoc, response);
        request->send(500, "application/json", response);
        return;
      }
      File dst = LittleFS.open("/config.json", "w");
      if (!dst) {
        src.close();
        Serial.println(F("[WEBSERVER] Failed to open /config.json for writing"));
        JsonDocument errorDoc;
        errorDoc[F("error")] = "Failed to open config for writing.";
        String response;
        serializeJson(errorDoc, response);
        request->send(500, "application/json", response);
        return;
      }
      // Copy contents
      while (src.available()) {
        dst.write(src.read());
      }
      src.close();
      dst.close();

      JsonDocument okDoc;
      okDoc[F("message")] = "✅ Backup restored! Device will now reboot.";
      String response;
      serializeJson(okDoc, response);
      request->send(200, "application/json", response);
      request->onDisconnect([]() {
        // Ensure all file operations are complete before rebooting
        Serial.println(F("[WEBSERVER] Flushing file system before reboot..."));
        LittleFS.end();  // Properly close the file system
        delay(500);      // Short delay to ensure flash operations complete
        Serial.println(F("[WEBSERVER] Rebooting after restore..."));
        ESP.restart();
      });

    } else {
      Serial.println(F("[WEBSERVER] No backup found"));
      JsonDocument errorDoc;
      errorDoc[F("error")] = "No backup found.";
      String response;
      serializeJson(errorDoc, response);
      request->send(404, "application/json", response);
    }
  });

  server.on("/ap_status", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.print(F("[WEBSERVER] Request: /ap_status. isAPMode = "));
    Serial.println(isAPMode);
    String json = "{\"isAP\": ";
    json += (isAPMode) ? "true" : "false";
    json += "}";
    request->send(200, "application/json", json);
  });
  
  server.on("/refresh_netatmo_token", HTTP_POST, [](AsyncWebServerRequest *request){
    Serial.println(F("[WEBSERVER] Request: /refresh_netatmo_token"));
    forceNetatmoTokenRefresh();
    String json = "{\"success\": true, \"message\": \"Token refresh initiated\"}";
    request->send(200, "application/json", json);
  });
  
  // Add restart endpoint
  server.on("/restart", HTTP_POST, [](AsyncWebServerRequest *request){
    Serial.println(F("[WEBSERVER] Request: /restart"));
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Restarting device...\"}");
    // Ensure all file operations are complete before rebooting
    Serial.println(F("[WEBSERVER] Flushing file system before reboot..."));
    LittleFS.end();  // Properly close the file system
    delay(1000);     // Delay to ensure flash operations complete and response is sent
    ESP.restart();
  });
  
  // Add new API endpoint to fetch Netatmo devices
  server.on("/api/netatmo/devices", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println(F("[WEBSERVER] Request: /api/netatmo/devices"));
    String devices = fetchNetatmoDevices();
    request->send(200, "application/json", devices);
  });
  
  server.on("/debug", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println(F("[WEBSERVER] Request: /debug"));
    
    String html = "<html><body>";
    html += "<h1>ESPTimeCast Debug Information</h1>";
    
    // Add crash information
    html += getCrashInfoHtml();
    
    // Add memory information
    html += "<h2>Memory Information</h2>";
    html += "<p>Free Heap: " + String(ESP.getFreeHeap()) + " bytes</p>";
    html += "<p>Heap Fragmentation: " + String(ESP.getHeapFragmentation()) + "%</p>";
    html += "<p>Free Stack: " + String(ESP.getFreeContStack()) + " bytes</p>";
    
    // Add system information
    html += "<h2>System Information</h2>";
    html += "<p>Chip ID: " + String(ESP.getChipId(), HEX) + "</p>";
    html += "<p>Core Version: " + String(ESP.getCoreVersion()) + "</p>";
    html += "<p>SDK Version: " + String(ESP.getSdkVersion()) + "</p>";
    html += "<p>CPU Frequency: " + String(ESP.getCpuFreqMHz()) + " MHz</p>";
    html += "<p>Flash Chip ID: " + String(ESP.getFlashChipId(), HEX) + "</p>";
    html += "<p>Flash Chip Size: " + String(ESP.getFlashChipSize() / 1024) + " KB</p>";
    html += "<p>Flash Chip Real Size: " + String(ESP.getFlashChipRealSize() / 1024) + " KB</p>";
    html += "<p>Flash Chip Speed: " + String(ESP.getFlashChipSpeed() / 1000000) + " MHz</p>";
    html += "<p>Sketch Size: " + String(ESP.getSketchSize() / 1024) + " KB</p>";
    html += "<p>Free Sketch Space: " + String(ESP.getFreeSketchSpace() / 1024) + " KB</p>";
    
    // Add WiFi information
    html += "<h2>WiFi Information</h2>";
    html += "<p>WiFi Mode: " + String(WiFi.getMode()) + "</p>";
    html += "<p>MAC Address: " + WiFi.macAddress() + "</p>";
    html += "<p>SSID: " + WiFi.SSID() + "</p>";
    html += "<p>RSSI: " + String(WiFi.RSSI()) + " dBm</p>";
    html += "<p>IP Address: " + WiFi.localIP().toString() + "</p>";
    html += "<p>Subnet Mask: " + WiFi.subnetMask().toString() + "</p>";
    html += "<p>Gateway IP: " + WiFi.gatewayIP().toString() + "</p>";
    html += "<p>DNS IP: " + WiFi.dnsIP().toString() + "</p>";
    
    // Add file system information
    html += "<h2>File System Information</h2>";
    
    // Check LittleFS
    if (LittleFS.begin()) {
      FSInfo fs_info;
      LittleFS.info(fs_info);
      html += "<p>Total Bytes: " + String(fs_info.totalBytes) + "</p>";
      html += "<p>Used Bytes: " + String(fs_info.usedBytes) + "</p>";
      html += "<p>Block Size: " + String(fs_info.blockSize) + "</p>";
      html += "<p>Page Size: " + String(fs_info.pageSize) + "</p>";
      html += "<p>Max Open Files: " + String(fs_info.maxOpenFiles) + "</p>";
      html += "<p>Max Path Length: " + String(fs_info.maxPathLength) + "</p>";
      
      // List files
      html += "<h3>Files:</h3>";
      html += "<ul>";
      
      // List files
      Dir dir = LittleFS.openDir("/");
      while (dir.next()) {
        html += "<li>" + dir.fileName() + " - " + String(dir.fileSize()) + " bytes</li>";
      }
      html += "</ul>";
      
      // Check config.json
      bool configExists = LittleFS.exists("/config.json");
      html += "<h3>Configuration File:</h3>";
      
      if (configExists) {
        File f = LittleFS.open("/config.json", "r");
        if (f) {
          html += "<p>Size: " + String(f.size()) + " bytes</p>";
          
          if (f.size() > 0) {
            String configContent = f.readString();
            html += "<p>Content (first 100 chars): <pre>" + configContent.substring(0, 100) + "...</pre></p>";
          } else {
            html += "<p>File is empty</p>";
          }
          
          f.close();
        } else {
          html += "<p>Failed to open config file</p>";
        }
      } else {
        html += "<p>Config file does not exist</p>";
      }
    } else {
      html += "<p>Failed to mount file system</p>";
    }
    
    html += "<p><a href='/'>Return to configuration</a></p>";
    html += "</body></html>";
    
    request->send(200, "text/html", html);
  });
  
  server.on("/format", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println(F("[WEBSERVER] Request: /format"));
    
    bool success = LittleFS.format();
    
    JsonDocument doc;
    doc[F("format_success")] = success;
    
    if (success) {
      Serial.println(F("[WEBSERVER] LittleFS formatted successfully"));
      
      // Create default config
      if (LittleFS.begin()) {
        JsonDocument configDoc;
        configDoc[F("ssid")] = "";
        configDoc[F("password")] = "";
        configDoc[F("mdnsHostname")] = "esptime";
        configDoc[F("weatherUnits")] = "metric";
        
        File f = LittleFS.open("/config.json", "w");
        if (f) {
          serializeJsonPretty(configDoc, f);
          f.close();
          doc[F("config_created")] = true;
        } else {
          doc[F("config_created")] = false;
        }
      } else {
        doc[F("mount_after_format")] = false;
      }
    } else {
      Serial.println(F("[WEBSERVER] LittleFS format failed"));
    }
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });
  
  // Add OAuth2 endpoints
  // Setup Netatmo OAuth handler
  setupNetatmoHandler();
  
  // Setup Netatmo save settings handler
  setupSaveSettingsHandler();
  
  // Add a generic static file handler for any other files
  // Set up optimized handlers for memory-intensive requests
  setupOptimizedHandlers();
  
  server.serveStatic("/", LittleFS, "/", "max-age=86400");
  
  server.begin();
  Serial.println(F("[WEBSERVER] Web server started"));
}

// Function to get Netatmo OAuth token
// Function to save tokens to config.json
void saveTokensToConfig();

String getNetatmoToken() {
  Serial.println(F("[NETATMO] Getting OAuth token..."));
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[NETATMO] Skipped: WiFi not connected"));
    return "";
  }
  
  // Check if we have an access token already
  if (strlen(netatmoAccessToken) > 0) {
    Serial.println(F("[NETATMO] Using existing access token"));
    return netatmoAccessToken;
  }
  
  // Check if we have a refresh token
  if (strlen(netatmoRefreshToken) == 0) {
    Serial.println(F("[NETATMO] Skipped: No refresh token available (use OAuth2 flow to authenticate)"));
    return "";
  }
  
  // Use the same memory preparation function as successful calls
  prepareForAPICall();
  
  // Create a new client for the API call using the optimization function
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  optimizeBearSSLClient(client.get()); // Use our optimization function
  
  HTTPClient https;
  optimizeHTTPClient(https);
  String token = "";
  
  // Add DNS resolution test
  Serial.println(F("[NETATMO-DEBUG] Testing DNS resolution..."));
  IPAddress ip;
  if (WiFi.hostByName("api.netatmo.com", ip)) {
    Serial.print(F("[NETATMO-DEBUG] api.netatmo.com resolved to: "));
    Serial.println(ip.toString());
  } else {
    Serial.println(F("[NETATMO-DEBUG] DNS resolution failed!"));
  }
  
  if (https.begin(*client, "https://api.netatmo.com/oauth2/token")) {
    https.addHeader("Content-Type", "application/x-www-form-urlencoded");
    https.addHeader("Accept", "application/json");
    https.addHeader("User-Agent", "ESPTimeCast/1.0");
    
    String postData;
    
    // Use refresh token flow
    postData = "grant_type=refresh_token&refresh_token=";
    postData += netatmoRefreshToken;
    postData += "&client_id=";
    postData += netatmoClientId;
    postData += "&client_secret=";
    postData += netatmoClientSecret;
    
    Serial.print(F("[NETATMO] POST data: "));
    // Print a redacted version of the POST data for debugging
    String redactedPostData = postData;
    if (redactedPostData.indexOf("client_secret=") >= 0) {
      int start = redactedPostData.indexOf("client_secret=") + 14;
      int end = redactedPostData.indexOf("&", start);
      if (end < 0) end = redactedPostData.length();
      redactedPostData = redactedPostData.substring(0, start) + "REDACTED" + redactedPostData.substring(end);
    }
    if (redactedPostData.indexOf("password=") >= 0) {
      int start = redactedPostData.indexOf("password=") + 9;
      int end = redactedPostData.indexOf("&", start);
      if (end < 0) end = redactedPostData.length();
      redactedPostData = redactedPostData.substring(0, start) + "REDACTED" + redactedPostData.substring(end);
    }
    if (redactedPostData.indexOf("refresh_token=") >= 0) {
      int start = redactedPostData.indexOf("refresh_token=") + 14;
      int end = redactedPostData.indexOf("&", start);
      if (end < 0) end = redactedPostData.length();
      redactedPostData = redactedPostData.substring(0, start) + "REDACTED" + redactedPostData.substring(end);
    }
    Serial.println(redactedPostData);
    
    // DEBUG: Log the complete request details
    Serial.println(F("\n========== COMPLETE TOKEN REQUEST =========="));
    Serial.println(F("POST https://api.netatmo.com/oauth2/token HTTP/1.1"));
    Serial.println(F("Content-Type: application/x-www-form-urlencoded"));
    Serial.println(F("Accept: application/json"));
    Serial.println(F("User-Agent: ESPTimeCast/1.0"));
    Serial.println();
    Serial.println(redactedPostData); // Use redacted version for security
    Serial.println(F("===========================================\n"));
    
    // Defragment heap right before making the request - like successful API calls
    defragmentHeap();
    yield(); // Allow the watchdog to be fed
    
    Serial.println(F("[NETATMO] Sending token request..."));
    int httpCode = https.POST(postData);
    Serial.print(F("[NETATMO] HTTP response code: "));
    Serial.println(httpCode);
    
    // Add SSL error diagnostics for connection errors
    if (httpCode <= 0) {
      int sslError = client->getLastSSLError();
      char errorMsg[100];
      client->getLastSSLError(errorMsg, sizeof(errorMsg));
      
      Serial.print(F("[NETATMO-DEBUG] SSL Error Code: "));
      Serial.println(sslError);
      Serial.print(F("[NETATMO-DEBUG] SSL Error Message: "));
      Serial.println(errorMsg);
    }
    
    if (httpCode == HTTP_CODE_OK) {
      String payload = https.getString();
      
      // DEBUG: Log the complete response
      Serial.println(F("\n========== COMPLETE TOKEN RESPONSE =========="));
      Serial.print(F("HTTP/1.1 "));
      Serial.print(httpCode);
      Serial.println(F(" OK"));
      
      // Log response headers
      for (int i = 0; i < https.headers(); i++) {
        Serial.print(https.headerName(i));
        Serial.print(F(": "));
        Serial.println(https.header(i));
      }
      Serial.println();
      Serial.println(payload);
      Serial.println(F("============================================\n"));
      
      Serial.println(F("[NETATMO] Response received:"));
      Serial.println(payload);
      
      JsonDocument doc; // Increased from 1024 to 2048
      DeserializationError error = deserializeJson(doc, payload);
      
      if (!error) {
        if (doc["access_token"].is<JsonVariant>()) {
          token = doc["access_token"].as<String>();
          
          // Store the tokens for future use
          strlcpy(netatmoAccessToken, token.c_str(), sizeof(netatmoAccessToken));
          
          if (doc["refresh_token"].is<JsonVariant>()) {
            String refreshToken = doc["refresh_token"].as<String>();
            strlcpy(netatmoRefreshToken, refreshToken.c_str(), sizeof(netatmoRefreshToken));
            
            // Save the tokens to config.json
            saveTokensToConfig();
          }
          
          // Reset the auth failure flag since we got a valid token
          
          Serial.println(F("[NETATMO] Token obtained successfully"));
        } else {
          Serial.println(F("[NETATMO] Error: No access token in response"));
        }
      } else {
        Serial.print(F("[NETATMO] JSON parse error: "));
        Serial.println(error.c_str());
      }
    } else {
      Serial.print(F("[NETATMO] HTTP error: "));
      Serial.println(httpCode);
      
      // Get the error response
      String errorPayload = https.getString();
      
      // DEBUG: Log the complete error response
      Serial.println(F("\n========== COMPLETE TOKEN ERROR RESPONSE =========="));
      Serial.print(F("HTTP/1.1 "));
      Serial.print(httpCode);
      Serial.println(F(" Error"));
      
      // Log response headers
      for (int i = 0; i < https.headers(); i++) {
        Serial.print(https.headerName(i));
        Serial.print(F(": "));
        Serial.println(https.header(i));
      }
      Serial.println();
      Serial.println(errorPayload);
      Serial.println(F("==================================================\n"));
      
      Serial.println(F("[NETATMO] Error response:"));
      Serial.println(errorPayload);
      
      // Try to parse the error message
      JsonDocument errorDoc; // Increased from 1024 to 2048
      DeserializationError errorParseError = deserializeJson(errorDoc, errorPayload);
      if (!errorParseError && errorDoc["error"].is<JsonVariant>()) {
        String errorType = errorDoc["error"].as<String>();
        String errorMessage = errorDoc["error_description"].is<JsonVariant>() ? 
                             errorDoc["error_description"].as<String>() : "Unknown error";
        
        Serial.print(F("[NETATMO] Error type: "));
        Serial.println(errorType);
        Serial.print(F("[NETATMO] Error message: "));
        Serial.println(errorMessage);
        
        // Handle specific error types
        if (errorType == "invalid_grant" || errorType == "invalid_client") {
          Serial.println(F("[NETATMO] Invalid credentials or refresh token, clearing tokens"));
          netatmoRefreshToken[0] = '\0';
          netatmoAccessToken[0] = '\0';
          saveTokensToConfig();
        }
      }
      
      // Check for "request is blocked" HTML response
      if (errorPayload.indexOf("The request is blocked") > 0) {
        handleBlockedRequest(errorPayload);
      }
      
      // Handle refresh token failure
      // Check if it's a connection error (httpCode <= 0)
      if (httpCode <= 0) {
        Serial.println(F("[NETATMO] Connection error, not clearing refresh token"));
        netatmoStatus = NETATMO_CONN_FAIL;
      } else {
        Serial.println(F("[NETATMO] Refresh token might be expired, clearing it"));
        netatmoRefreshToken[0] = '\0';
        netatmoAccessToken[0] = '\0';
        saveTokensToConfig();
        netatmoStatus = NETATMO_AUTH_FAIL;
      }
    }
    
    https.end();
  } else {
    Serial.println(F("[NETATMO] Unable to connect to Netatmo API"));
  }
  
  return token;
}

// Function to read indoor temperature from DS18B20
void readIndoorTemperature(bool roundToInteger = false) {
  Serial.println(F("[DS18B20] Reading indoor temperature..."));
  
  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);
  
  if (tempC != DEVICE_DISCONNECTED_C) {
    Serial.print(F("[DS18B20] Raw temperature: "));
    Serial.print(tempC);
    Serial.println(F("°C"));
    
    // Format the temperature using our helper function
    indoorTemp = formatTemperature(tempC, roundToInteger);
    indoorTemp += "º";
    
    indoorTempAvailable = true;
    Serial.print(F("[DS18B20] Formatted indoor temperature: "));
    Serial.println(indoorTemp);
  } else {
    Serial.println(F("[DS18B20] Error: Could not read temperature data"));
    indoorTempAvailable = false;
  }
}

// Function to force Netatmo token refresh
void forceNetatmoTokenRefresh() {
  Serial.println(F("[NETATMO] Forcing token refresh"));
  netatmoAccessToken[0] = '\0';  // Clear access token
  needTokenRefresh = true;       // Set flag to refresh token
}

// Function to save tokens to config.json
void saveTokensToConfig() {
  Serial.println(F("[CONFIG] Saving tokens to tokens.json"));
  
  // Add memory reporting

  
  // Defragment heap before saving tokens
  defragmentHeap();
  
  yield(); // Feed the watchdog
  
  if (!LittleFS.begin()) {
    Serial.println(F("[CONFIG] Failed to mount file system"));
    return;
  }
  
  yield(); // Feed the watchdog
  
  // Create a smaller JSON document just for tokens
  JsonDocument tokenDoc;
  
  // Update only the Netatmo tokens in the JSON document
  tokenDoc["netatmoAccessToken"] = netatmoAccessToken;
  tokenDoc["netatmoRefreshToken"] = netatmoRefreshToken;
  
  yield(); // Feed the watchdog
  
  // Create a backup of the current tokens file if it exists
  if (LittleFS.exists("/tokens.json")) {
    if (LittleFS.exists("/tokens.json.bak")) {
      LittleFS.remove("/tokens.json.bak");
    }
    
    yield(); // Feed the watchdog
    
    if (!LittleFS.rename("/tokens.json", "/tokens.json.bak")) {
      Serial.println(F("[CONFIG] Failed to create tokens backup"));
    }
  }
  
  yield(); // Feed the watchdog
  
  // Create the tokens file
  File tokenFile = LittleFS.open("/tokens.json", "w");
  if (!tokenFile) {
    Serial.println(F("[CONFIG] Failed to open tokens file for writing"));
    return;
  }
  
  yield(); // Feed the watchdog
  
  // Serialize the tokens JSON document to the file
  if (serializeJson(tokenDoc, tokenFile) == 0) {
    Serial.println(F("[CONFIG] Failed to write to tokens file"));
    tokenFile.close();
    return;
  }
  
  tokenFile.close();
  Serial.println(F("[CONFIG] Tokens saved successfully"));
  
  yield(); // Feed the watchdog
  
  // Also update the main config with other Netatmo settings (not tokens)
  // First, read the existing config file to preserve all settings
  JsonDocument doc;
  bool configExists = false;
  
  if (LittleFS.exists("/config.json")) {
    File configFile = LittleFS.open("/config.json", "r");
    if (configFile) {
      yield(); // Feed the watchdog
      DeserializationError error = deserializeJson(doc, configFile);
      configFile.close();
      
      yield(); // Feed the watchdog
      
      if (!error) {
        configExists = true;
        Serial.println(F("[CONFIG] Successfully read existing config"));
      } else {
        Serial.print(F("[CONFIG] Error parsing config file: "));
        Serial.println(error.c_str());
        // Continue with empty doc if parsing fails
      }
    }
  }
  
  yield(); // Feed the watchdog
  
  // Update Netatmo configuration (but not tokens)
  doc["netatmoClientId"] = netatmoClientId;
  doc["netatmoClientSecret"] = netatmoClientSecret;
  doc["netatmoStationId"] = netatmoStationId;
  doc["netatmoModuleId"] = netatmoModuleId;
  doc["netatmoIndoorModuleId"] = netatmoIndoorModuleId;
  
  // Remove tokens from config.json if they exist
  doc.remove("netatmoAccessToken");
  doc.remove("netatmoRefreshToken");
  
  yield(); // Feed the watchdog
  
  // Create a new config file
  File outFile = LittleFS.open("/config.json.new", "w");
  if (!outFile) {
    Serial.println(F("[CONFIG] Failed to open temp config file for writing"));
    return;
  }
  
  yield(); // Feed the watchdog
  
  // Serialize the updated JSON document to the file
  if (serializeJson(doc, outFile) == 0) {
    Serial.println(F("[CONFIG] Failed to write to config file"));
    outFile.close();
    return;
  }
  
  outFile.close();
  
  yield(); // Feed the watchdog
  
  // Replace the old config file with the new one
  if (LittleFS.exists("/config.json")) {
    LittleFS.remove("/config.json");
  }
  
  yield(); // Feed the watchdog
  
  if (LittleFS.rename("/config.json.new", "/config.json")) {
    Serial.println(F("[CONFIG] Config updated successfully"));
  } else {
    Serial.println(F("[CONFIG] Failed to rename config file"));
  }
  
  yield(); // Feed the watchdog
  
  printMemoryStats();
}

// Function to fetch outdoor temperature from Netatmo
void fetchOutdoorTemperature(bool roundToInteger = true) {
  Serial.println(F("\n[NETATMO] Fetching outdoor temperature..."));
  
  
  if (shouldDefragment()) {
    Serial.println(F("[NETATMO] Memory fragmentation detected, defragmenting..."));
    defragmentHeap();
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[NETATMO] Skipped: WiFi not connected"));
    outdoorTempAvailable = false;
    netatmoStatus = NETATMO_CONN_FAIL;
    return;
  }
  
  if (strlen(netatmoDeviceId) == 0 || strlen(netatmoModuleId) == 0) {
    Serial.println(F("[NETATMO] Skipped: Missing device or module ID"));
    Serial.print(F("[NETATMO] Device ID: '"));
    Serial.print(netatmoDeviceId);
    Serial.println(F("'"));
    Serial.print(F("[NETATMO] Module ID: '"));
    Serial.print(netatmoModuleId);
    Serial.println(F("'"));
    outdoorTempAvailable = false;
    netatmoStatus = NETATMO_NO_CONFIG;
    return;
  }
  
  // Get a fresh token directly from the function
  String token = getNetatmoToken();
  if (token.length() == 0) {
    Serial.println(F("[NETATMO] Skipped: No access token available"));
    outdoorTempAvailable = false;
    netatmoStatus = NETATMO_NO_TOKEN;
    return;
  }
  
  
  // Create a smaller buffer size for the SSL client
  // Use static to avoid stack allocation — BearSSL::WiFiClientSecure is large
  static BearSSL::WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(512, 512);
  
  static HTTPClient https;
  https.setTimeout(10000);
  https.useHTTP10(true); // HTTP/1.0 avoids chunked encoding, saves memory
  
  String url = "https://api.netatmo.com/api/getstationsdata?device_id=";
  url += netatmoDeviceId;
  
  Serial.print(F("[NETATMO] GET "));
  Serial.println(url);
  Serial.print(F("[NETATMO] Using token: "));
  Serial.println(token.substring(0, 10) + "...");
  
  
  if (https.begin(client, url)) {
    https.addHeader("Authorization", "Bearer " + token);
    https.addHeader("Accept", "application/json");
    https.addHeader("User-Agent", "ESPTimeCast/1.0");
    
    Serial.println(F("[NETATMO] Sending request..."));
    int httpCode = https.GET();
    Serial.print(F("[NETATMO] HTTP response code: "));
    Serial.println(httpCode);
    
    if (httpCode == HTTP_CODE_OK) {
      String payload = https.getString();
      https.end(); // Free BearSSL memory before JSON parsing
      Serial.println(F("[NETATMO] Response received"));
      
      JsonDocument doc;
      
      if (parseNetatmoJson(payload, doc)) {
        // Navigate through the JSON to find the outdoor module
        JsonArray devices = doc["body"]["devices"];
        
        Serial.print(F("[NETATMO] Number of devices found: "));
        Serial.println(devices.size());
        
        bool deviceFound = false;
        bool moduleFound = false;
        
        for (JsonObject device : devices) {
          String deviceId = device["_id"].as<String>();
          Serial.print(F("[NETATMO] Found device ID: "));
          Serial.println(deviceId);
          
          if (deviceId == String(netatmoDeviceId)) {
            deviceFound = true;
            Serial.println(F("[NETATMO] Device ID matched!"));
            
            // Check if the indoor module ID matches the main device ID
            if (String(netatmoIndoorModuleId) == deviceId) {
              Serial.println(F("[NETATMO] Indoor module is the main device"));
              
              // Get indoor temperature from the main device
              JsonObject device_dashboard_data = device["dashboard_data"];
              
              if (device_dashboard_data["Temperature"].is<JsonVariant>()) {
                float indoorTempValue = device_dashboard_data["Temperature"];
                
                Serial.print(F("[NETATMO] Indoor temperature from main device: "));
                Serial.print(indoorTempValue);
                Serial.println(F("°C"));
                
                netatmoIndoorTemp = indoorTempValue;
                netatmoIndoorTempAvailable = true;
                
                // Format the indoor temperature
                indoorTemp = formatTemperature(indoorTempValue, roundToInteger) + "º";
                indoorTempAvailable = true;
              }
            }
            
            JsonArray modules = device["modules"];
            Serial.print(F("[NETATMO] Number of modules found: "));
            Serial.println(modules.size());
            
            for (JsonObject module : modules) {
              String moduleId = module["_id"].as<String>();
              String moduleName = module["module_name"].as<String>();
              Serial.print(F("[NETATMO] Found module: "));
              Serial.print(moduleId);
              Serial.print(F(" ("));
              Serial.print(moduleName);
              Serial.println(F(")"));
              
              if (moduleId == String(netatmoModuleId)) {
                moduleFound = true;
                Serial.println(F("[NETATMO] Module ID matched!"));
                
                JsonObject dashboard_data = module["dashboard_data"];
                
                if (dashboard_data["Temperature"].is<JsonVariant>()) {
                  float temp = dashboard_data["Temperature"];
                  
                  // Debug the raw temperature value
                  Serial.print(F("[NETATMO] Raw temperature from API: "));
                  Serial.print(temp);
                  Serial.println(F("°C"));
                  
                  // Note: formatTemperature already handles unit conversion based on weatherUnits
                  // so we don't need to convert here
                  outdoorTemp = formatTemperature(temp, roundToInteger) + "º";
                  Serial.print(F("[NETATMO] Outdoor temperature: "));
                  Serial.println(outdoorTemp);
                  outdoorTempAvailable = true;
                  netatmoStatus = NETATMO_OK;
                } else {
                  Serial.println(F("[NETATMO] Temperature data not found in dashboard_data"));
                  Serial.println(F("[NETATMO] Available dashboard_data fields:"));
                  for (JsonPair kv : dashboard_data) {
                    Serial.print(kv.key().c_str());
                    Serial.print(F(": "));
                    Serial.println(kv.value().as<String>());
                  }
                  outdoorTempAvailable = false;
                }
              }
              
              // Check if this module is the indoor module
              if (moduleId == String(netatmoIndoorModuleId)) {
                Serial.println(F("[NETATMO] Indoor module ID matched!"));
                
                JsonObject dashboard_data = module["dashboard_data"];
                
                if (dashboard_data["Temperature"].is<JsonVariant>()) {
                  float indoorTempValue = dashboard_data["Temperature"];
                  
                  Serial.print(F("[NETATMO] Indoor temperature from module: "));
                  Serial.print(indoorTempValue);
                  Serial.println(F("°C"));
                  
                  netatmoIndoorTemp = indoorTempValue;
                  netatmoIndoorTempAvailable = true;
                  
                  // Format the indoor temperature if we're using Netatmo as primary
                  if (tempSource == 1 || tempSource == 2) {
                    indoorTemp = formatTemperature(indoorTempValue, roundToInteger) + "º";
                    indoorTempAvailable = true;
                  }
                }
                break;
              }
            }
            
            if (!moduleFound) {
              Serial.println(F("[NETATMO] Error: Module ID not found in the device's modules"));
              outdoorTempAvailable = false;
            }
            break;
          }
        }
        
        if (!deviceFound) {
          Serial.println(F("[NETATMO] Error: Device ID not found in the response"));
          outdoorTempAvailable = false;
        }
      } else {
        Serial.println(F("[NETATMO] JSON parsing failed"));
        outdoorTempAvailable = false;
      }
    } else {
      Serial.print(F("[NETATMO] HTTP error: "));
      Serial.println(httpCode);
      
      // Get the error response
      // Debug: Print more information about the error
      if (httpCode == -1) {
        Serial.println(F("[NETATMO] Connection failed or timed out"));
        Serial.print(F("[NETATMO] Last error: "));
        Serial.println(https.errorToString(httpCode));
      }
      
      String errorPayload = https.getString();
      Serial.println(F("[NETATMO] Error response:"));
      Serial.println(errorPayload);
      
      // Try to parse the error message
      JsonDocument errorDoc;
      DeserializationError errorParseError = deserializeJson(errorDoc, errorPayload);
      if (!errorParseError && errorDoc["error"].is<JsonVariant>()) {
        String errorType = errorDoc["error"].as<String>();
        String errorMessage = errorDoc["error_description"].is<JsonVariant>() ? 
                             errorDoc["error_description"].as<String>() : "Unknown error";
        
        Serial.print(F("[NETATMO] Error type: "));
        Serial.println(errorType);
        Serial.print(F("[NETATMO] Error message: "));
        Serial.println(errorMessage);
        
        // Handle specific error types
        if (errorType == "access_denied" || errorMessage.indexOf("blocked") >= 0) {
          Serial.println(F("[NETATMO] Request blocked - check your Netatmo app permissions"));
        }
      }
      
      // Check for "request is blocked" HTML response
      if (errorPayload.indexOf("The request is blocked") > 0) {
        handleBlockedRequest(errorPayload);
      }
      // If we get a 403 error (Forbidden), the token is expired or invalid
      if (httpCode == 403 || httpCode == 401) {
        // Check if the error payload indicates an invalid token
        if (isInvalidTokenError(errorPayload)) {
          Serial.println(F("[NETATMO] 403/401 error - invalid access token, need new API keys"));
          // Don't try to refresh, need new API keys
        } else {
          Serial.println(F("[NETATMO] 403/401 error - token expired"));
          forceNetatmoTokenRefresh();  // Force token refresh on next API call
        }
        netatmoStatus = NETATMO_AUTH_FAIL;
      } else if (httpCode <= 0) {
        netatmoStatus = NETATMO_CONN_FAIL;
      } else {
        netatmoStatus = NETATMO_API_ERROR;
        lastNetatmoHttpCode = httpCode;
      }
      
      outdoorTempAvailable = false;
    }
    
    https.end();
  } else {
    Serial.println(F("[NETATMO] Unable to connect to Netatmo API"));
    outdoorTempAvailable = false;
    netatmoStatus = NETATMO_CONN_FAIL;
  }
}

// Function to update both indoor and outdoor temperatures
void updateTemperatures() {
  Serial.println(F("[TEMP] Updating temperatures..."));
  
  // Debug output for temperature source
  Serial.print(F("[TEMP] Temperature source: "));
  Serial.print(tempSource);
  Serial.println(tempSource == 0 ? " (Local sensor primary)" : 
                (tempSource == 1 ? " (Netatmo primary)" : " (Netatmo only)"));
  
  Serial.print(F("[TEMP] prioritizeNetatmoIndoor: "));
  Serial.println(prioritizeNetatmoIndoor ? "true" : "false");
  
  // Determine if we need to round temperatures to integers
  // Only round to integers when both indoor and outdoor temps will be displayed
  bool shouldRound = showIndoorTemp && showOutdoorTemp && 
                    (strlen(netatmoDeviceId) > 0 && strlen(netatmoModuleId) > 0);
  
  Serial.print(F("[TEMP] Rounding to integers: "));
  Serial.println(shouldRound ? "Yes" : "No");
  
  // Reset Netatmo temperature availability
  netatmoIndoorTempAvailable = false;
  
  // Fetch outdoor temperature from Netatmo (this will also fetch indoor temperature if available)
  fetchOutdoorTemperature(shouldRound);
  
  // Handle indoor temperature based on source setting
  if (tempSource == 0) { // Local sensor primary
    // Read indoor temperature from DS18B20
    readIndoorTemperature(shouldRound);
    
    // If local sensor failed and Netatmo indoor temperature is available, use that as fallback
    if (!indoorTempAvailable && netatmoIndoorTempAvailable) {
      // Convert Netatmo temperature to the same format as local sensor
      indoorTemp = formatTemperature(netatmoIndoorTemp, shouldRound) + "º";
      indoorTempAvailable = true;
      Serial.println(F("[TEMP] Using Netatmo as fallback for indoor temperature"));
    }
  } else if (tempSource == 1) { // Netatmo primary, no local fallback
    if (!netatmoIndoorTempAvailable) {
      indoorTempAvailable = false;
    }
  } else if (tempSource == 2) { // Netatmo only
    // Only use Netatmo for indoor temperature
    if (!netatmoIndoorTempAvailable) {
      indoorTempAvailable = false;
    }
  }
  
  // Set weatherFetched flag if at least one temperature is available
  weatherFetched = indoorTempAvailable || outdoorTempAvailable;
}

void setup() {
  Serial.begin(115200);
  delay(500); // Give serial monitor time to connect
  Serial.println();
  Serial.println(F("[SETUP] Starting setup..."));
  
  // Initialize crash handler
  setupCrashHandler();
  
  P.begin();
  P.setFont(mFactory); // Custom font
  
  // Check and repair configuration if needed
  if (!checkAndRepairConfig()) {
    Serial.println(F("[SETUP] WARNING: Configuration check failed"));
  }
  
  loadConfig(); // Load config before setting intensity & flip
  P.setIntensity(brightness);
  P.setZoneEffect(0, flipDisplay, PA_FLIP_UD);
  P.setZoneEffect(0, flipDisplay, PA_FLIP_LR);
  Serial.println(F("[SETUP] Parola (LED Matrix) initialized"));
  
  // Check Netatmo configuration
  checkNetatmoConfig();
  
  // Initialize DS18B20 temperature sensor
  sensors.begin();
  Serial.println(F("[SETUP] DS18B20 sensor initialized"));
  
  connectWiFi();
  Serial.println(F("[SETUP] Wifi connected"));  
  setupWebServer();
  Serial.println(F("[SETUP] Webserver setup complete"));  
  Serial.println(F("[SETUP] Setup complete"));
  Serial.println();
  printConfigToSerial();
  setupTime(); // Start NTP sync process
  displayMode = 0;
  lastSwitch = millis();
  lastColonBlink = millis();
  indicatorVisible = true;
}

void loop() {
  // Check if a reboot is pending
  if (rebootPending && millis() > rebootTime) {
    Serial.println(F("[SYSTEM] Executing scheduled reboot..."));
    // Ensure all file operations are complete before rebooting
    Serial.println(F("[SYSTEM] Flushing file system before reboot..."));
    LittleFS.end();  // Properly close the file system
    delay(100); // Short delay to allow serial output to complete
    ESP.restart();
    return; // Just in case restart doesn't happen immediately
  }
  
  // WiFi reconnection check (skip in AP mode)
  static unsigned long lastWiFiReconnectAttempt = 0;
  if (!isAPMode && WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWiFiReconnectAttempt > 30000) {
      lastWiFiReconnectAttempt = millis();
      Serial.println(F("[WIFI] Connection lost, attempting reconnect..."));
      WiFi.reconnect();
    }
  }

  // Update mDNS responder
  if (WiFi.status() == WL_CONNECTED && !isAPMode) {
    MDNS.update();
  }
  
  // Process any pending Netatmo token exchanges
  processTokenExchange();
  
  // Process any pending Netatmo device fetches
  processFetchDevices();
  
  // Process any pending Netatmo stations data fetches
  processFetchStationsData();
  
  // Process any pending Netatmo proxy requests
  processProxyRequest();
  
  // Process any pending settings saves
  // Process any pending settings save
  processSettingsSave();
  
  // Process any pending credential saves
  processSaveCredentials();

  // --- AP Mode Animation ---
  static unsigned long apAnimTimer = 0;
  static int apAnimFrame = 0;
  if (isAPMode) {
    unsigned long now = millis();
    if (now - apAnimTimer > 750) {
      apAnimTimer = now;
      apAnimFrame++;
    }
    P.setTextAlignment(PA_CENTER);
    switch (apAnimFrame % 3) {
      case 0: P.print("AP©"); break;
      case 1: P.print("APª"); break;
      case 2: P.print("AP«"); break;
    }
    yield(); // Let system do background work
    return;  // Don't run normal display logic
  }

  static bool colonVisible = true;
  const unsigned long colonBlinkInterval = 800;
  if (millis() - lastColonBlink > colonBlinkInterval) {
    colonVisible = !colonVisible;
    indicatorVisible = colonVisible; // Sync temperature indicators with colon
    lastColonBlink = millis();
  }

  static unsigned long ntpAnimTimer = 0;
  static int ntpAnimFrame = 0;
  static bool tzSetAfterSync = false;
  static time_t lastPrint = 0;

  // TEMPERATURE UPDATE
  static unsigned long lastTempUpdate = 0;
  const unsigned long tempUpdateInterval = 300000; // 5 minutes

  // NTP state machine
  switch (ntpState) {
    case NTP_IDLE:
      break;
    case NTP_SYNCING: {
      time_t now = time(nullptr);
      if (now > 1000) {
        Serial.println(F("\n[TIME] NTP sync successful."));
        ntpSyncSuccessful = true;
        ntpState = NTP_SUCCESS;
      } else if (millis() - ntpStartTime > ntpTimeout || ntpRetryCount > maxNtpRetries) {
        Serial.println(F("\n[TIME] NTP sync failed."));
        ntpSyncSuccessful = false;
        ntpState = NTP_FAILED;
      } else {
        if (millis() - ntpStartTime > (ntpRetryCount * 1000)) {
          Serial.print(".");
          ntpRetryCount++;
        }
      }
      break;
    }
    case NTP_SUCCESS:
      if (!tzSetAfterSync) {
        const char* posixTz = ianaToPosix(timeZone);
        setenv("TZ", posixTz, 1);
        tzset();
        tzSetAfterSync = true;
      }
      ntpAnimTimer = 0;
      ntpAnimFrame = 0;
      break;
    case NTP_FAILED:
      ntpAnimTimer = 0;
      ntpAnimFrame = 0;
      break;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    if (!weatherFetchInitiated) {
        weatherFetchInitiated = true;
        updateTemperatures();
        lastTempUpdate = millis();
    }
    
    // Check if we need to refresh the token sooner due to an expired token
    if (needTokenRefresh) {
        // If we had a blocked request, wait longer (2 minutes)
        if (lastBlockedRequest > 0) {
            if (millis() - lastBlockedRequest > 120000) { // 2 minutes after blocked request
                Serial.println(F("[LOOP] Retry after blocked request, updating temperatures..."));
                weatherFetched = false;
                updateTemperatures();
                lastTempUpdate = millis();
                needTokenRefresh = false; // Reset the flag
                lastBlockedRequest = 0;   // Reset the blocked request timestamp
            }
        } 
        // Otherwise, just wait 30 seconds
        else if (millis() - lastTempUpdate > 30000) { // 30 seconds after last attempt
            Serial.println(F("[LOOP] Token refresh needed, updating temperatures..."));
            weatherFetched = false;
            updateTemperatures();
            lastTempUpdate = millis();
            needTokenRefresh = false; // Reset the flag
        }
    }
    // Regular temperature update interval
    else if (millis() - lastTempUpdate > tempUpdateInterval) {
        Serial.println(F("[LOOP] Updating temperatures..."));
        weatherFetched = false;
        updateTemperatures();
        lastTempUpdate = millis();
    }
  } else {
    weatherFetchInitiated = false;
  }

  // Time display logic
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  int dayOfWeek = timeinfo.tm_wday;
  char* daySymbol = daysOfTheWeek[dayOfWeek];

  char timeStr[9]; // enough for "12:34 AM"
  if (twelveHourToggle) {
    int hour12 = timeinfo.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    sprintf(timeStr, "%d:%02d", hour12, timeinfo.tm_min);
  } else {
    sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  }

  // Only prepend day symbol if showDayOfWeek is true
  String formattedTime;
  if (showDayOfWeek) {
    formattedTime = String(daySymbol) + " " + String(timeStr);
  } else {
    formattedTime = String(timeStr);
  }

  unsigned long displayDuration = (displayMode == 0) ? clockDuration : weatherDuration;
  if (millis() - lastSwitch > displayDuration) {
    displayMode = (displayMode + 1) % 2;
    lastSwitch = millis();
    Serial.printf("[LOOP] Switching to display mode: %s\n", displayMode == 0 ? "CLOCK" : "TEMPERATURE");
  }

  P.setTextAlignment(PA_CENTER);
  static bool tempWasAvailable = false;

  if (displayMode == 0) { // Clock
    if (ntpState == NTP_SYNCING) {
      if (millis() - ntpAnimTimer > 750) {
        ntpAnimTimer = millis();
        switch (ntpAnimFrame % 3) {
          case 0: P.print("sync®"); break;
          case 1: P.print("sync¯"); break;
          case 2: P.print("sync°"); break;
        }
        ntpAnimFrame++;
      }
    } else if (!ntpSyncSuccessful) {
      P.print("no ntp");
    } else {
      String timeString = formattedTime;
      if (!colonVisible) timeString.replace(":", " ");
      P.print(timeString);
    }
  } else { // Temperature mode
    if (indoorTempAvailable || outdoorTempAvailable) {
      // --- Temperature display string ---
      String tempDisplay;
      
      if (showIndoorTemp && indoorTempAvailable && showOutdoorTemp && outdoorTempAvailable) {
        tempDisplay = indoorTemp + (indicatorVisible ? "|" : "}") + outdoorTemp;
      } else if (showIndoorTemp && indoorTempAvailable) {
        tempDisplay = (indicatorVisible ? "i " : "  ") + indoorTemp + tempSymbol;
      } else if (showOutdoorTemp && outdoorTempAvailable) {
        tempDisplay = (indicatorVisible ? "o " : "  ") + outdoorTemp + tempSymbol;
      } else {
        String timeString = formattedTime;
        if (!colonVisible) timeString.replace(":", " ");
        P.print(timeString);
        return;
      }
      
      P.print(tempDisplay.c_str());
      tempWasAvailable = true;
    } else if (netatmoStatus != NETATMO_OK) {
      // Show short status code when Netatmo has issues and no temp data
      switch (netatmoStatus) {
        case NETATMO_NO_TOKEN:
        case NETATMO_AUTH_FAIL: P.print("auth"); break;
        case NETATMO_CONN_FAIL: P.print("conn"); break;
        case NETATMO_NO_CONFIG: P.print("conf"); break;
        case NETATMO_API_ERROR: P.print(String(lastNetatmoHttpCode).c_str()); break;
        default: break;
      }
    } else {
      if (tempWasAvailable) {
        Serial.println(F("[DISPLAY] Temperatures not available, showing clock..."));
        tempWasAvailable = false;
      }
      if (ntpSyncSuccessful) {
        String timeString = formattedTime;
        if (!colonVisible) timeString.replace(":", " ");
        P.print(timeString);
      } else {
        P.print("no temp");
      }
    }
  }

  static unsigned long lastDisplayUpdate = 0;
  const unsigned long displayUpdateInterval = 50;
  if (millis() - lastDisplayUpdate >= displayUpdateInterval) {
    lastDisplayUpdate = millis();
  }

  yield();
}

// External declarations for variables defined in other files

// Function to check if we should handle web requests
bool shouldHandleWebRequest() {
  // If an API call is in progress, we should defer web requests
  if (apiCallInProgress) {
    Serial.println(F("[WEBSERVER] API call in progress, deferring web request"));
    return false;
  }
  return true;
}
// Function declarations

// External variable declarations


// Function to log detailed API request and response information - simplified version


// Function to check Netatmo configuration
void checkNetatmoConfig() {
  Serial.println(F("\n[NETATMO] Checking Netatmo configuration..."));
  
  bool configOK = true;
  
  // Check client ID and secret
  if (strlen(netatmoClientId) == 0) {
    Serial.println(F("[NETATMO] ERROR: Client ID is missing"));
    configOK = false;
  } else {
    Serial.print(F("[NETATMO] Client ID: "));
    Serial.println(netatmoClientId);
  }
  
  if (strlen(netatmoClientSecret) == 0) {
    Serial.println(F("[NETATMO] ERROR: Client Secret is missing"));
    configOK = false;
  } else {
    Serial.println(F("[NETATMO] Client Secret is set"));
  }
  
  // Check device and module IDs
  if (strlen(netatmoDeviceId) == 0) {
    Serial.println(F("[NETATMO] ERROR: Device ID is missing"));
    configOK = false;
  } else {
    Serial.print(F("[NETATMO] Device ID: "));
    Serial.println(netatmoDeviceId);
  }
  
  if (strlen(netatmoModuleId) == 0) {
    Serial.println(F("[NETATMO] ERROR: Module ID is missing"));
    configOK = false;
  } else {
    Serial.print(F("[NETATMO] Module ID: "));
    Serial.println(netatmoModuleId);
  }
  
  // Check indoor module ID if set
  if (strlen(netatmoIndoorModuleId) > 0) {
    Serial.print(F("[NETATMO] Indoor Module ID: "));
    Serial.println(netatmoIndoorModuleId);
  } else {
    Serial.println(F("[NETATMO] Indoor Module ID is not set"));
  }
  
  // Check tokens
  if (strlen(netatmoAccessToken) > 0) {
    Serial.println(F("[NETATMO] Access token is set"));
  } else {
    Serial.println(F("[NETATMO] Access token is not set (will be obtained through OAuth)"));
  }
  
  if (strlen(netatmoRefreshToken) > 0) {
    Serial.println(F("[NETATMO] Refresh token is set"));
  } else {
    Serial.println(F("[NETATMO] Refresh token is not set (will be obtained through OAuth)"));
  }
  
  // Check temperature settings
  Serial.print(F("[NETATMO] Use Netatmo for outdoor temperature: "));
  Serial.println(useNetatmoOutdoor ? "Yes" : "No");
  
  Serial.print(F("[NETATMO] Prioritize Netatmo for indoor temperature: "));
  Serial.println(prioritizeNetatmoIndoor ? "Yes" : "No");
  
  if (configOK) {
    Serial.println(F("[NETATMO] Configuration looks good!"));
  } else {
    Serial.println(F("[NETATMO] Configuration is incomplete - outdoor temperature will not be available"));
  }
  Serial.println();
}


// Functions for handling chunked transfer encoding

// Function to check if a response is chunked
bool isChunkedResponse(HTTPClient& http) {
  // Check if the Transfer-Encoding header contains "chunked"
  String transferEncoding = http.header("Transfer-Encoding");
  bool isChunked = transferEncoding.indexOf("chunked") >= 0;
  
  Serial.print(F("[CHUNKED] Transfer-Encoding header: "));
  Serial.println(transferEncoding);
  
  // If Content-Length is -1, it's likely chunked
  int contentLength = http.getSize();
  Serial.print(F("[CHUNKED] Content-Length: "));
  Serial.println(contentLength);
  
  if (contentLength == -1) {
    Serial.println(F("[CHUNKED] Content-Length is -1, likely chunked encoding"));
    isChunked = true;
  }
  
  // If header doesn't indicate chunking but we see a chunk size at the start of the stream,
  // we should still treat it as chunked
  if (!isChunked) {
    WiFiClient* stream = http.getStreamPtr();
    if (stream && stream->available() >= 5) { // Need at least a few bytes to check
      // Peek at the first few bytes without consuming them
      char peek[6];
      int i = 0;
      while (i < 5 && stream->available()) {
        peek[i] = stream->peek();
        i++;
      }
      peek[i] = '\0';
      
      Serial.print(F("[CHUNKED] First bytes of stream: "));
      for (int j = 0; j < i; j++) {
        Serial.print((char)peek[j]);
      }
      Serial.println();
      
      // Check if it looks like a hex chunk size followed by CRLF
      bool looksLikeChunk = false;
      for (int j = 0; j < i; j++) {
        if ((peek[j] >= '0' && peek[j] <= '9') || 
            (peek[j] >= 'a' && peek[j] <= 'f') || 
            (peek[j] >= 'A' && peek[j] <= 'F')) {
          // This is a hex digit, continue
          continue;
        } else if (peek[j] == '\r' || peek[j] == '\n') {
          // Found CR or LF after hex digits, likely a chunk
          looksLikeChunk = true;
          break;
        } else {
          // Not a hex digit or CRLF, not a chunk
          break;
        }
      }
      
      if (looksLikeChunk) {
        Serial.println(F("[CHUNKED] Stream appears to be chunked based on content inspection"));
        isChunked = true;
      }
    }
  }
  
  Serial.print(F("[CHUNKED] Is chunked response: "));
  Serial.println(isChunked ? "Yes" : "No");
  
  return isChunked;
}

// Completely rewritten memory-efficient chunked encoding handler
bool writeChunkedResponseToFile(WiFiClient* stream, File& file) {
  Serial.println(F("[CHUNKED] Starting chunked response processing"));
  
  int totalBytes = 0;
  unsigned long startTime = millis();
  const unsigned long timeout = 10000; // 10 second timeout
  
  // Check if we've already read some bytes (the chunk size line)
  // If so, we need to handle the first chunk differently
  bool firstChunkHandled = false;
  
  // Process chunks until we get a zero-length chunk
  while (true) {
    // Check for timeout
    unsigned long currentTime = millis();
    if (currentTime - startTime > timeout) {
      Serial.println(F("[CHUNKED] Timeout reading chunked response"));
      return false;
    }
    
    // Log progress every 2 seconds
    if (currentTime % 2000 < 10) {
      Serial.print(F("[CHUNKED] Still processing after "));
      Serial.print((currentTime - startTime) / 1000);
      Serial.println(F(" seconds"));
    }
    
    // For the first chunk, we may have already read the chunk size line
    // and possibly some of the chunk data
    if (!firstChunkHandled) {
      Serial.println(F("[CHUNKED] First chunk may have been partially read"));
      firstChunkHandled = true;
      
      // We'll just continue and read the next chunk
      // The data we've already read has been written to the file
      // in setupNetatmoHandler.ino
    }
    
    // Read the chunk size (hex digits)
    int chunkSize = 0;
    String chunkSizeStr = ""; // For logging only
    
    // Wait for data to be available
    unsigned long waitStart = millis();
    while (!stream->available()) {
      yield();
      if (millis() - waitStart > 1000) {
        Serial.print(F("[CHUNKED] Waiting for chunk size data... "));
        Serial.print(millis() - waitStart);
        Serial.println(F(" ms"));
        waitStart = millis(); // Reset to only log once per second
      }
      if (millis() - startTime > timeout) {
        Serial.println(F("[CHUNKED] Timeout waiting for chunk size"));
        return false;
      }
    }
    
    // Read hex digits until CR
    bool foundCR = false;
    while (!foundCR) {
      if (stream->available()) {
        char c = stream->read();
        
        if (c == '\r') {
          foundCR = true;
          // Skip the LF that should follow
          while (!stream->available()) {
            yield();
            if (millis() - startTime > timeout) {
              Serial.println(F("[CHUNKED] Timeout waiting for LF after CR"));
              return false;
            }
          }
          char lf = stream->read();
          if (lf != '\n') {
            Serial.println(F("[CHUNKED] Warning: Expected LF after CR"));
          }
        } else if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
          // For logging
          chunkSizeStr += c;
          
          // Convert hex digit to integer
          if (c >= '0' && c <= '9') {
            chunkSize = (chunkSize << 4) | (c - '0');
          } else if (c >= 'a' && c <= 'f') {
            chunkSize = (chunkSize << 4) | (c - 'a' + 10);
          } else { // c >= 'A' && c <= 'F'
            chunkSize = (chunkSize << 4) | (c - 'A' + 10);
          }
        }
        // Ignore any other characters (like chunk extensions)
      } else {
        yield();
        if (millis() - startTime > timeout) {
          Serial.println(F("[CHUNKED] Timeout reading chunk size"));
          return false;
        }
      }
    }
    
    Serial.print(F("[CHUNKED] Chunk size: 0x"));
    Serial.print(chunkSizeStr);
    Serial.print(F(" ("));
    Serial.print(chunkSize);
    Serial.println(F(" bytes)"));
    
    // A chunk size of 0 means we're done
    if (chunkSize == 0) {
      Serial.println(F("[CHUNKED] Found end chunk (size 0), transfer complete"));
      
      // Read the final CRLF
      while (!stream->available()) {
        yield();
        if (millis() - startTime > timeout) {
          Serial.println(F("[CHUNKED] Timeout waiting for final CRLF"));
          break;
        }
      }
      
      // Read and discard the final CRLF
      if (stream->available()) {
        char cr = stream->read();
        if (cr != '\r') {
          Serial.println(F("[CHUNKED] Warning: Expected CR at end of transfer"));
        }
        
        while (!stream->available()) {
          yield();
          if (millis() - startTime > timeout) break;
        }
        
        if (stream->available()) {
          char lf = stream->read();
          if (lf != '\n') {
            Serial.println(F("[CHUNKED] Warning: Expected LF at end of transfer"));
          }
        }
      }
      
      // We're done!
      break;
    }
    
    // Read the chunk data
    int bytesRemaining = chunkSize;
    const size_t bufSize = 64; // Small buffer size to save memory
    uint8_t buf[bufSize];
    
    while (bytesRemaining > 0) {
      // Check for timeout
      if (millis() - startTime > timeout) {
        Serial.println(F("[CHUNKED] Timeout reading chunk data"));
        return false;
      }
      
      // Wait for data to be available
      unsigned long dataWaitStart = millis();
      while (!stream->available()) {
        yield();
        if (millis() - dataWaitStart > 1000) {
          Serial.print(F("[CHUNKED] Waiting for chunk data... "));
          Serial.print(millis() - dataWaitStart);
          Serial.println(F(" ms"));
          dataWaitStart = millis(); // Reset to only log once per second
        }
        if (millis() - startTime > timeout) {
          Serial.println(F("[CHUNKED] Timeout waiting for chunk data"));
          return false;
        }
      }
      
      // Reset timeout when data is available
      startTime = millis();
      
      // Read up to buffer size or remaining bytes in chunk
      size_t available = stream->available();
      size_t readBytes = (available < bufSize) ? available : bufSize;
      readBytes = (readBytes < (size_t)bytesRemaining) ? readBytes : (size_t)bytesRemaining;
      int bytesRead = stream->readBytes(buf, readBytes);
      
      if (bytesRead > 0) {
        // Write to file
        file.write(buf, bytesRead);
        totalBytes += bytesRead;
        bytesRemaining -= bytesRead;
        
        // Print progress every 1KB
        if (totalBytes % 1024 == 0) {
          Serial.print(F("[CHUNKED] Read "));
          Serial.print(totalBytes);
          Serial.println(F(" bytes"));
        }
      }
      
      yield(); // Allow the watchdog to be fed
    }
    
    // Read the CRLF at the end of the chunk
    unsigned long crlfWaitStart = millis();
    while (!stream->available()) {
      yield();
      if (millis() - crlfWaitStart > 1000) {
        Serial.print(F("[CHUNKED] Waiting for CRLF after chunk... "));
        Serial.print(millis() - crlfWaitStart);
        Serial.println(F(" ms"));
        crlfWaitStart = millis(); // Reset to only log once per second
      }
      if (millis() - startTime > timeout) {
        Serial.println(F("[CHUNKED] Timeout waiting for CRLF after chunk"));
        break;
      }
    }
    
    if (stream->available()) {
      char cr = stream->read();
      if (cr != '\r') {
        Serial.print(F("[CHUNKED] Warning: Expected CR after chunk, got: "));
        Serial.println(cr);
      }
      
      while (!stream->available()) {
        yield();
        if (millis() - startTime > timeout) break;
      }
      
      if (stream->available()) {
        char lf = stream->read();
        if (lf != '\n') {
          Serial.print(F("[CHUNKED] Warning: Expected LF after chunk, got: "));
          Serial.println(lf);
        }
      }
    }
  }
  
  unsigned long duration = millis() - startTime;
  Serial.print(F("[CHUNKED] Finished processing chunked response: "));
  Serial.print(totalBytes);
  Serial.print(F(" bytes in "));
  Serial.print(duration);
  Serial.println(F(" ms"));
  
  return true;
}


// Function to write clean JSON to a file
void writeCleanJsonToFile(const String &jsonString, const char* filePath) {
  // Open the file for writing
  File file = LittleFS.open(filePath, "w");
  if (!file) {
    Serial.print(F("[JSON] Failed to open file for writing: "));
    Serial.println(filePath);
    return;
  }
  
  // Write only valid JSON characters to the file
  for (size_t i = 0; i < jsonString.length(); i++) {
    char ch = jsonString.charAt(i);
    // Only write valid JSON characters
    if (ch == '{' || ch == '}' || ch == '[' || ch == ']' || 
        ch == ',' || ch == ':' || ch == '"' || ch == '\'' || 
        ch == '.' || ch == '-' || ch == '+' || 
        (ch >= '0' && ch <= '9') || 
        (ch >= 'a' && ch <= 'z') || 
        (ch >= 'A' && ch <= 'Z') || 
        ch == '_' || ch == ' ' || ch == '\t' || 
        ch == '\r' || ch == '\n') {
      file.write(ch);
    }
  }
  
  // Close the file
  file.close();
  
  Serial.print(F("[JSON] Clean JSON written to file: "));
  Serial.println(filePath);
}

// Global flag to indicate when a chunked transfer is complete
bool chunkedTransferComplete = false;

// Function to write clean JSON to a file from a buffer
bool writeCleanJsonFromBuffer(const uint8_t* buffer, size_t bufferSize, File &file) {
  // Debug: Print the first few bytes of the buffer to help diagnose chunked encoding
  static bool firstChunk = true;
  static bool isChunkedEncoding = false;
  static bool inChunkHeader = false;
  static bool skipCRLF = false;
  static int chunkSize = 0;
  static int bytesReadInChunk = 0;
  
  if (firstChunk && bufferSize > 0) {
    Serial.print(F("[JSON] First bytes of buffer: "));
    for (size_t i = 0; i < min(bufferSize, (size_t)16); i++) {
      char ch = (char)buffer[i];
      if (ch >= 32 && ch <= 126) { // Printable ASCII
        Serial.print(ch);
      } else {
        Serial.print(F("\\x"));
        Serial.print(buffer[i] < 16 ? "0" : "");
        Serial.print(buffer[i], HEX);
      }
    }
    Serial.println();
    
    // Check if this looks like a chunked response
    // Chunked responses start with a hex number followed by CR LF
    if (bufferSize >= 3) {
      char ch1 = (char)buffer[0];
      char ch2 = (char)buffer[1];
      char ch3 = (char)buffer[2];
      
      if (((ch1 >= '0' && ch1 <= '9') || (ch1 >= 'a' && ch1 <= 'f') || (ch1 >= 'A' && ch1 <= 'F')) &&
          ((ch2 >= '0' && ch2 <= '9') || (ch2 >= 'a' && ch2 <= 'f') || (ch2 >= 'A' && ch2 <= 'F')) &&
          ((ch3 >= '0' && ch3 <= '9') || (ch3 >= 'a' && ch3 <= 'f') || (ch3 >= 'A' && ch3 <= 'F'))) {
        isChunkedEncoding = true;
        inChunkHeader = true;
        chunkSize = 0;
        bytesReadInChunk = 0;
        chunkedTransferComplete = false; // Reset the flag
        Serial.println(F("[JSON] Detected chunked encoding in first chunk"));
      }
    }
    
    firstChunk = false;
  }

  // Process the buffer
  for (size_t i = 0; i < bufferSize; i++) {
    char ch = (char)buffer[i];
    
    // Handle chunked encoding
    if (isChunkedEncoding) {
      // Skip chunk headers and chunk boundaries
      if (inChunkHeader) {
        // Parse the chunk size
        if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) {
          // Convert hex digit to integer
          if (ch >= '0' && ch <= '9') {
            chunkSize = (chunkSize << 4) | (ch - '0');
          } else if (ch >= 'a' && ch <= 'f') {
            chunkSize = (chunkSize << 4) | (ch - 'a' + 10);
          } else { // ch >= 'A' && ch <= 'F'
            chunkSize = (chunkSize << 4) | (ch - 'A' + 10);
          }
        } else if (ch == '\r') {
          skipCRLF = true;
        } else if (ch == '\n' && skipCRLF) {
          inChunkHeader = false;
          skipCRLF = false;
          
          // If chunk size is 0, we're done
          if (chunkSize == 0) {
            Serial.println(F("[JSON] Found end chunk (size 0)"));
            chunkedTransferComplete = true; // Set the flag
            return true; // Return true to indicate transfer is complete
          }
          
          Serial.print(F("[JSON] Chunk size: "));
          Serial.println(chunkSize);
        }
        continue;
      }
      
      // Check if we've read the entire chunk
      if (bytesReadInChunk >= chunkSize) {
        // We've read the entire chunk, look for the CRLF
        if (ch == '\r') {
          skipCRLF = true;
          continue;
        } else if (ch == '\n' && skipCRLF) {
          // Start of a new chunk
          inChunkHeader = true;
          chunkSize = 0;
          bytesReadInChunk = 0;
          skipCRLF = false;
          continue;
        }
      }
      
      // Count bytes read in the current chunk
      bytesReadInChunk++;
    }
    
    // Only write valid JSON characters to the file
    if (ch == '{' || ch == '}' || ch == '[' || ch == ']' || 
        ch == ',' || ch == ':' || ch == '"' || ch == '\'' || 
        ch == '.' || ch == '-' || ch == '+' || 
        (ch >= '0' && ch <= '9') || 
        (ch >= 'a' && ch <= 'z') || 
        (ch >= 'A' && ch <= 'Z') || 
        ch == '_' || ch == ' ' || ch == '\t' || 
        ch == '\r' || ch == '\n') {
      file.write(ch);
    }
  }
  
  return false; // Return false to indicate transfer is not complete
}


// Function to clear Netatmo tokens
void clearNetatmoTokens() {
  Serial.println(F("[NETATMO] Clearing access and refresh tokens"));
  netatmoAccessToken[0] = '\0';
  netatmoRefreshToken[0] = '\0';
  saveTokensToConfig();
}


// Configuration backup and recovery functions

// Function to backup the current configuration
bool backupConfig() {
  Serial.println(F("[CONFIG] Backing up configuration"));
  
  if (!LittleFS.begin()) {
    Serial.println(F("[CONFIG] Failed to mount file system"));
    return false;
  }
  
  // Check if config.json exists
  if (!LittleFS.exists("/config.json")) {
    Serial.println(F("[CONFIG] No configuration file to backup"));
    return false;
  }
  
  // Read the current config
  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println(F("[CONFIG] Failed to open config file for reading"));
    return false;
  }
  
  String configData = configFile.readString();
  configFile.close();
  
  // Write to backup file
  File backupFile = LittleFS.open("/config.bak", "w");
  if (!backupFile) {
    Serial.println(F("[CONFIG] Failed to open backup file for writing"));
    return false;
  }
  
  size_t bytesWritten = backupFile.print(configData);
  backupFile.close();
  
  if (bytesWritten == 0) {
    Serial.println(F("[CONFIG] Failed to write backup file"));
    return false;
  }
  
  Serial.println(F("[CONFIG] Configuration backup successful"));
  return true;
}

// Function to restore configuration from backup
bool restoreConfig() {
  Serial.println(F("[CONFIG] Restoring configuration from backup"));
  
  if (!LittleFS.begin()) {
    Serial.println(F("[CONFIG] Failed to mount file system"));
    return false;
  }
  
  // Check if backup file exists
  if (!LittleFS.exists("/config.bak")) {
    Serial.println(F("[CONFIG] No backup file found"));
    return false;
  }
  
  // Read the backup file
  File backupFile = LittleFS.open("/config.bak", "r");
  if (!backupFile) {
    Serial.println(F("[CONFIG] Failed to open backup file for reading"));
    return false;
  }
  
  String backupData = backupFile.readString();
  backupFile.close();
  
  // Validate the backup data
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, backupData);
  
  if (err) {
    // Special handling for NoMemory errors
    if (err == DeserializationError::NoMemory) {
      Serial.print(F("[CONFIG] Not enough memory to parse backup: "));
      Serial.println(err.c_str());
      Serial.println(F("[CONFIG] This is a memory limitation, not file corruption"));
      
      // For backup validation, we should be conservative and not use it
      return false;
    }
    
    // For other errors, treat as corruption
    Serial.print(F("[CONFIG] Backup file is corrupted: "));
    Serial.println(err.c_str());
    return false;
  }
  
  // Write to config file
  File configFile = LittleFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println(F("[CONFIG] Failed to open config file for writing"));
    return false;
  }
  
  size_t bytesWritten = configFile.print(backupData);
  configFile.close();
  
  if (bytesWritten == 0) {
    Serial.println(F("[CONFIG] Failed to write config file"));
    return false;
  }
  
  Serial.println(F("[CONFIG] Configuration restored successfully"));
  return true;
}

// Function to check and repair configuration
bool checkAndRepairConfig() {
  Serial.println(F("[CONFIG] Checking configuration integrity"));
  
  yield(); // Feed the watchdog
  
  if (!LittleFS.begin()) {
    Serial.println(F("[CONFIG] Failed to mount file system"));
    return false;
  }
  
  yield(); // Feed the watchdog
  
  // Check if config.json exists
  if (!LittleFS.exists("/config.json")) {
    Serial.println(F("[CONFIG] Configuration file not found"));
    
    yield(); // Feed the watchdog
    
    // Try to restore from backup
    if (LittleFS.exists("/config.bak")) {
      Serial.println(F("[CONFIG] Attempting to restore from backup"));
      if (restoreConfig()) {
        Serial.println(F("[CONFIG] Configuration restored from backup"));
        return true;
      }
    }
    
    yield(); // Feed the watchdog
    
    // Create default config if restore failed
    Serial.println(F("[CONFIG] Creating default configuration"));
    createDefaultConfig();
    return LittleFS.exists("/config.json");
  }
  
  yield(); // Feed the watchdog
  
  // Validate the config file
  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println(F("[CONFIG] Failed to open config file for reading"));
    return false;
  }
  
  yield(); // Feed the watchdog
  
  size_t fileSize = configFile.size();
  if (fileSize == 0) {
    configFile.close();
    Serial.println(F("[CONFIG] Configuration file is empty"));
    
    yield(); // Feed the watchdog
    
    // Try to restore from backup
    if (restoreConfig()) {
      Serial.println(F("[CONFIG] Configuration restored from backup"));
      return true;
    }
    
    yield(); // Feed the watchdog
    
    // Create default config if restore failed
    Serial.println(F("[CONFIG] Creating default configuration"));
    createDefaultConfig();
    return LittleFS.exists("/config.json");
  }
  
  yield(); // Feed the watchdog
  
  // Read the file content
  String configData = configFile.readString();
  configFile.close();
  
  yield(); // Feed the watchdog
  
  // Validate JSON
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, configData);
  
  yield(); // Feed the watchdog
  
  if (err) {
    // Special handling for NoMemory errors
    if (err == DeserializationError::NoMemory) {
      Serial.print(F("[CONFIG] Not enough memory to parse configuration: "));
      Serial.println(err.c_str());
      
      // Don't treat memory errors as corruption
      Serial.println(F("[CONFIG] This is a memory limitation, not file corruption"));
      Serial.println(F("[CONFIG] Keeping existing configuration"));
      
      yield(); // Feed the watchdog
      
      // Return true to indicate we're keeping the existing config
      return true;
    }
    
    // For other errors, treat as corruption
    Serial.print(F("[CONFIG] Configuration file is corrupted: "));
    Serial.println(err.c_str());
    
    yield(); // Feed the watchdog
    
    // Try to restore from backup
    if (restoreConfig()) {
      Serial.println(F("[CONFIG] Configuration restored from backup"));
      return true;
    }
    
    yield(); // Feed the watchdog
    
    // Create default config if restore failed
    Serial.println(F("[CONFIG] Creating default configuration"));
    createDefaultConfig();
    return LittleFS.exists("/config.json");
  }
  
  yield(); // Feed the watchdog
  
  // Configuration is valid, create a backup if one doesn't exist
  if (!LittleFS.exists("/config.bak")) {
    Serial.println(F("[CONFIG] Creating backup of valid configuration"));
    backupConfig();
  }
  
  yield(); // Feed the watchdog
  
  Serial.println(F("[CONFIG] Configuration is valid"));
  return true;
}


// Crash handler for ESP8266

#include <user_interface.h>

// Global variables to store crash information
String lastResetReason = "";
String crashDetails = "";

// Function to initialize crash handler
void setupCrashHandler() {
  // Get the last reset reason
  rst_info *resetInfo = ESP.getResetInfoPtr();
  
  switch (resetInfo->reason) {
    case REASON_DEFAULT_RST:
      lastResetReason = "Normal startup by power on";
      break;
    case REASON_WDT_RST:
      lastResetReason = "Hardware watchdog reset";
      break;
    case REASON_EXCEPTION_RST:
      lastResetReason = "Exception reset";
      crashDetails = getExceptionDetails(resetInfo);
      break;
    case REASON_SOFT_WDT_RST:
      lastResetReason = "Software watchdog reset";
      break;
    case REASON_SOFT_RESTART:
      lastResetReason = "Software restart";
      break;
    case REASON_DEEP_SLEEP_AWAKE:
      lastResetReason = "Deep sleep wake";
      break;
    case REASON_EXT_SYS_RST:
      lastResetReason = "External system reset";
      break;
    default:
      lastResetReason = "Unknown reason: " + String(resetInfo->reason);
  }
  
  Serial.println(F("[CRASH] Last reset reason: ") + lastResetReason);
  
  if (crashDetails.length() > 0) {
    Serial.println(F("[CRASH] Crash details: ") + crashDetails);
  }
  
  // Register exception handler
  ESP.wdtDisable();
  ESP.wdtEnable(WDTO_8S);
}

// Function to get exception details
String getExceptionDetails(rst_info *resetInfo) {
  String details = "";
  
  // Exception cause
  switch (resetInfo->exccause) {
    case 0:
      details += "Illegal instruction";
      break;
    case 3:
      details += "Load/store alignment error";
      break;
    case 6:
      details += "Division by zero";
      break;
    case 9:
      details += "Unaligned load or store";
      break;
    case 28:
      details += "Load prohibited";
      break;
    case 29:
      details += "Store prohibited";
      break;
    default:
      details += "Unknown exception (" + String(resetInfo->exccause) + ")";
  }
  
  // Exception address
  details += " at 0x" + String(resetInfo->epc1, HEX);
  
  return details;
}

// Function to get crash information as HTML
String getCrashInfoHtml() {
  String html = "<h2>System Diagnostics</h2>";
  html += "<p><strong>Last Reset Reason:</strong> " + lastResetReason + "</p>";
  
  if (crashDetails.length() > 0) {
    html += "<p><strong>Crash Details:</strong> " + crashDetails + "</p>";
  }
  
  html += "<p><strong>Free Heap:</strong> " + String(ESP.getFreeHeap()) + " bytes</p>";
  html += "<p><strong>Heap Fragmentation:</strong> " + String(ESP.getHeapFragmentation()) + "%</p>";
  html += "<p><strong>Free Stack:</strong> " + String(ESP.getFreeContStack()) + " bytes</p>";
  html += "<p><strong>Flash Size:</strong> " + String(ESP.getFlashChipSize() / 1024) + " KB</p>";
  html += "<p><strong>Sketch Size:</strong> " + String(ESP.getSketchSize() / 1024) + " KB</p>";
  html += "<p><strong>Free Sketch Space:</strong> " + String(ESP.getFreeSketchSpace() / 1024) + " KB</p>";
  
  return html;
}



// Global variables for deferred proxy request
static bool proxyPending = false;
static String proxyEndpoint = "";
static AsyncWebServerRequest* proxyRequest = nullptr;

// Function to process deferred proxy requests with memory optimization
void processProxyRequest() {
  if (!proxyPending || proxyRequest == nullptr) {
    return;
  }
  
  unsigned long startTime = millis();
  Serial.println(F("[NETATMO] Processing deferred proxy request"));
  
  // Set API call in progress flag to prevent concurrent web requests
  apiCallInProgress = true;
  
  Serial.println(F("[NETATMO] Memory stats before proxy request:"));
  
  // Defragment heap if needed
  if (shouldDefragment()) {
    Serial.println(F("[NETATMO] Defragmenting heap before proxy request"));
    defragmentHeap();
  }
  
  // Clear the flag immediately to prevent repeated attempts if this fails
  proxyPending = false;
  String endpoint = proxyEndpoint;
  AsyncWebServerRequest* request = proxyRequest;
  proxyRequest = nullptr;
  proxyEndpoint = "";
  
  unsigned long setupTime = millis();
  Serial.print(F("[TIMING] Setup time: "));
  Serial.print(setupTime - startTime);
  Serial.println(F(" ms"));
  
  // Create a new client for the API call
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure(); // Skip certificate validation to save memory
  
  // Use a smaller buffer size for the HTTP client
  HTTPClient https;
  https.setTimeout(5000); // Reduced timeout to avoid watchdog issues
  
  String apiUrl = "https://api.netatmo.com/api/" + endpoint;
  Serial.print(F("[NETATMO] Proxying request to: "));
  Serial.println(apiUrl);
  
  if (!https.begin(*client, apiUrl)) {
    Serial.println(F("[NETATMO] Error - Failed to connect"));
    request->send(500, "application/json", "{\"error\":\"Failed to connect to Netatmo API\"}");
    return;
  }
  
  // Add authorization header
  String authHeader = "Bearer ";
  authHeader += netatmoAccessToken;
  https.addHeader("Authorization", authHeader);
  https.addHeader("Accept", "application/json");
  
  unsigned long beforeRequestTime = millis();
  Serial.print(F("[TIMING] HTTP client setup time: "));
  Serial.print(beforeRequestTime - setupTime);
  Serial.println(F(" ms"));
  
  // Make the request with yield to avoid watchdog issues
  Serial.println(F("[NETATMO] Sending request..."));
  int httpCode = https.GET();
  yield(); // Allow the watchdog to be fed
  
  unsigned long afterRequestTime = millis();
  Serial.print(F("[TIMING] HTTP request time: "));
  Serial.print(afterRequestTime - beforeRequestTime);
  Serial.println(F(" ms"));
  
  if (httpCode != HTTP_CODE_OK) {
    Serial.print(F("[NETATMO] Error - HTTP code: "));
    Serial.println(httpCode);
    
    // Get error payload with yield - use streaming to avoid memory issues
    String errorPayload = "";
    if (https.getSize() > 0) {
      const size_t bufSize = 128;
      char buf[bufSize];
      WiFiClient* stream = https.getStreamPtr();
      
      // Read first chunk only for error message
      size_t len = stream->available();
      if (len > bufSize) len = bufSize;
      if (len > 0) {
        int c = stream->readBytes(buf, len);
        if (c > 0) {
          buf[c] = 0;
          errorPayload = String(buf);
        }
      }
    } else {
      errorPayload = https.getString();
    }
    
    yield(); // Allow the watchdog to be fed
    
    Serial.print(F("[NETATMO] Error payload: "));
    Serial.println(errorPayload);
    https.end();
    
    unsigned long errorHandlingTime = millis();
    Serial.print(F("[TIMING] Error handling time: "));
    Serial.print(errorHandlingTime - afterRequestTime);
    Serial.println(F(" ms"));
    
    // If we get a 401 or 403, check if it's an invalid token error
    if (httpCode == 401 || httpCode == 403) {
      // Check if the error payload indicates an invalid token
      if (isInvalidTokenError(errorPayload)) {
        Serial.println(F("[NETATMO] Token is invalid, need new API keys"));
        String errorResponse = "{\"error\":\"invalid_token\",\"message\":\"Invalid API keys. Please reconfigure Netatmo credentials.\"}";
        proxyRequest->send(401, "application/json", errorResponse);
        proxyPending = false;
        proxyRequest = nullptr;
        return;
      } else {
        Serial.println(F("[NETATMO] Token expired, trying to refresh"));
      }
      
      unsigned long beforeRefreshTime = millis();
      Serial.print(F("[TIMING] Before token refresh: "));
      Serial.print(beforeRefreshTime - errorHandlingTime);
      Serial.println(F(" ms"));
      
      // Try to refresh the token
      if (refreshNetatmoToken()) {
        unsigned long afterRefreshTime = millis();
        Serial.print(F("[TIMING] Token refresh time: "));
        Serial.print(afterRefreshTime - beforeRefreshTime);
        Serial.println(F(" ms"));
        
        Serial.println(F("[NETATMO] Token refreshed, retrying request"));
        
        // Create a new client for the retry
        std::unique_ptr<BearSSL::WiFiClientSecure> client2(new BearSSL::WiFiClientSecure);
        client2->setInsecure(); // Skip certificate validation to save memory
        
        HTTPClient https2;
        https2.setTimeout(5000); // Reduced timeout
        
        if (!https2.begin(*client2, apiUrl)) {
          Serial.println(F("[NETATMO] Error - Failed to connect on retry"));
          request->send(500, "application/json", "{\"error\":\"Failed to connect to Netatmo API on retry\"}");
          return;
        }
        
        // Add authorization header with new token
        String newAuthHeader = "Bearer ";
        newAuthHeader += netatmoAccessToken;
        https2.addHeader("Authorization", newAuthHeader);
        https2.addHeader("Accept", "application/json");
        
        unsigned long beforeRetryTime = millis();
        Serial.print(F("[TIMING] Retry setup time: "));
        Serial.print(beforeRetryTime - afterRefreshTime);
        Serial.println(F(" ms"));
        
        // Make the request again with yield
        Serial.println(F("[NETATMO] Sending retry request..."));
        int httpCode2 = https2.GET();
        yield(); // Allow the watchdog to be fed
        
        unsigned long afterRetryTime = millis();
        Serial.print(F("[TIMING] Retry request time: "));
        Serial.print(afterRetryTime - beforeRetryTime);
        Serial.println(F(" ms"));
        
        if (httpCode2 != HTTP_CODE_OK) {
          Serial.print(F("[NETATMO] Error on retry - HTTP code: "));
          Serial.println(httpCode2);
          
          // Get error payload with yield - use streaming to avoid memory issues
          String errorPayload2 = "";
          if (https2.getSize() > 0) {
            const size_t bufSize = 128;
            char buf[bufSize];
            WiFiClient* stream = https2.getStreamPtr();
            
            // Read first chunk only for error message
            size_t len = stream->available();
            if (len > bufSize) len = bufSize;
            if (len > 0) {
              int c = stream->readBytes(buf, len);
              if (c > 0) {
                buf[c] = 0;
                errorPayload2 = String(buf);
              }
            }
          } else {
            errorPayload2 = https2.getString();
          }
          
          yield(); // Allow the watchdog to be fed
          
          Serial.print(F("[NETATMO] Error payload on retry: "));
          Serial.println(errorPayload2);
          https2.end();
          request->send(httpCode2, "application/json", errorPayload2);
          return;
        }
        
        unsigned long beforeStreamTime = millis();
        Serial.print(F("[TIMING] Before streaming response: "));
        Serial.print(beforeStreamTime - afterRetryTime);
        Serial.println(F(" ms"));
        
        // Stream the response directly to the client to avoid memory issues
        WiFiClient* stream = https2.getStreamPtr();
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        
        // Use a small buffer to stream the data
        const size_t bufSize = 256;
        uint8_t buf[bufSize];
        int totalRead = 0;
        
        while (https2.connected() && (totalRead < https2.getSize() || https2.getSize() <= 0)) {
          // Read available data
          size_t available = stream->available();
          if (available) {
            // Read up to buffer size
            size_t readBytes = available > bufSize ? bufSize : available;
            int bytesRead = stream->readBytes(buf, readBytes);
            
            if (bytesRead > 0) {
              // Write to response
              response->write(buf, bytesRead);
              totalRead += bytesRead;
            }
            
            yield(); // Allow the watchdog to be fed
          } else {
            yield(); // Just feed the watchdog, no delay needed
          }
        }
        
        unsigned long afterStreamTime = millis();
        Serial.print(F("[TIMING] Streaming response time: "));
        Serial.print(afterStreamTime - beforeStreamTime);
        Serial.println(F(" ms"));
        
        https2.end();
        request->send(response);
        Serial.println(F("[NETATMO] Response streamed to client after token refresh"));
        
        unsigned long totalTime = millis() - startTime;
        Serial.print(F("[TIMING] Total retry request time: "));
        Serial.print(totalTime);
        Serial.println(F(" ms"));
        
        return;
      } else {
        Serial.println(F("[NETATMO] Failed to refresh token"));
      }
    }
    
    request->send(httpCode, "application/json", errorPayload);
    return;
  }
  
  unsigned long beforeStreamingTime = millis();
  Serial.print(F("[TIMING] Before streaming original response: "));
  Serial.print(beforeStreamingTime - afterRequestTime);
  Serial.println(F(" ms"));
  
  // Stream the response directly to the client to avoid memory issues
  WiFiClient* stream = https.getStreamPtr();
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  
  // Use a small buffer to stream the data
  const size_t bufSize = 256;
  uint8_t buf[bufSize];
  int totalRead = 0;
  
  while (https.connected() && (totalRead < https.getSize() || https.getSize() <= 0)) {
    // Read available data
    size_t available = stream->available();
    if (available) {
      // Read up to buffer size
      size_t readBytes = available > bufSize ? bufSize : available;
      int bytesRead = stream->readBytes(buf, readBytes);
      
      if (bytesRead > 0) {
        // Write to response
        response->write(buf, bytesRead);
        totalRead += bytesRead;
      }
      
      yield(); // Allow the watchdog to be fed
    } else {
      yield(); // Just feed the watchdog, no delay needed
    }
  }
  
  unsigned long afterStreamingTime = millis();
  Serial.print(F("[TIMING] Streaming original response time: "));
  Serial.print(afterStreamingTime - beforeStreamingTime);
  Serial.println(F(" ms"));
  
  https.end();
  request->send(response);
  Serial.println(F("[NETATMO] Response streamed to client"));
  
  // Reset API call in progress flag
  apiCallInProgress = false;
  
  Serial.println(F("[NETATMO] Memory stats after proxy request:"));
  
  unsigned long totalTime = millis() - startTime;
  Serial.print(F("[TIMING] Total request processing time: "));
  Serial.print(totalTime);
  Serial.println(F(" ms"));
}


// Global variables for deferred operations
bool settingsSavePending = false;
bool apiCallInProgress = false;

// Function to process pending settings save
void processSettingsSave() {
  if (!settingsSavePending) {
    return;
  }
  
  Serial.println(F("[NETATMO] Processing pending settings save"));
  settingsSavePending = false;
  
  // Debug: Print the values being saved
  Serial.println(F("[CONFIG] Values being saved to config.json:"));
  Serial.print(F("[CONFIG] netatmoDeviceId: '"));
  Serial.print(netatmoDeviceId);
  Serial.println(F("'"));
  Serial.print(F("[CONFIG] netatmoModuleId: '"));
  Serial.print(netatmoModuleId);
  Serial.println(F("'"));
  Serial.print(F("[CONFIG] netatmoIndoorModuleId: '"));
  Serial.print(netatmoIndoorModuleId);
  Serial.println(F("'"));
  
  // Save to config file with yield calls
  Serial.println(F("[CONFIG] Saving settings to config.json"));
  
  yield(); // Feed the watchdog
  
  if (!LittleFS.begin()) {
    Serial.println(F("[CONFIG] Failed to mount file system"));
    return;
  }
  
  yield(); // Feed the watchdog
  
  // First, read the existing config file to preserve all settings
  JsonDocument doc;
  bool configExists = false;
  
  if (LittleFS.exists("/config.json")) {
    File configFile = LittleFS.open("/config.json", "r");
    if (configFile) {
      yield(); // Feed the watchdog
      DeserializationError error = deserializeJson(doc, configFile);
      configFile.close();
      
      yield(); // Feed the watchdog
      
      if (!error) {
        configExists = true;
        Serial.println(F("[CONFIG] Successfully read existing config"));
      } else {
        Serial.print(F("[CONFIG] Error parsing config file: "));
        Serial.println(error.c_str());
        // Continue with empty doc if parsing fails
      }
    }
  }
  
  yield(); // Feed the watchdog
  
  // Update only the Netatmo settings in the JSON document
  doc["netatmoDeviceId"] = netatmoDeviceId;
  doc["netatmoStationId"] = netatmoStationId;  // Also save the station ID
  doc["netatmoModuleId"] = netatmoModuleId;
  doc["netatmoIndoorModuleId"] = netatmoIndoorModuleId;
  doc["useNetatmoOutdoor"] = useNetatmoOutdoor;
  doc["prioritizeNetatmoIndoor"] = prioritizeNetatmoIndoor;
  
  // Debug output for Netatmo settings being saved
  Serial.println(F("[CONFIG] Saving Netatmo settings:"));
  Serial.print(F("[CONFIG] Device ID: '"));
  Serial.print(netatmoDeviceId);
  Serial.println(F("'"));
  Serial.print(F("[CONFIG] Station ID: '"));
  Serial.print(netatmoStationId);
  Serial.println(F("'"));
  Serial.print(F("[CONFIG] Module ID: '"));
  Serial.print(netatmoModuleId);
  Serial.println(F("'"));
  Serial.print(F("[CONFIG] Indoor Module ID: '"));
  Serial.print(netatmoIndoorModuleId);
  Serial.println(F("'"));
  
  yield(); // Feed the watchdog
  
  // Create a new config file
  File outFile = LittleFS.open("/config.json.new", "w");
  if (!outFile) {
    Serial.println(F("[CONFIG] Failed to open temp config file for writing"));
    return;
  }
  
  yield(); // Feed the watchdog
  
  // Serialize the updated JSON document to the file
  size_t bytesWritten = serializeJson(doc, outFile);
  if (bytesWritten == 0) {
    Serial.println(F("[CONFIG] Failed to write to config file"));
    outFile.close();
    return;
  }
  
  Serial.print(F("[CONFIG] Wrote "));
  Serial.print(bytesWritten);
  Serial.println(F(" bytes to config.json.new"));
  
  outFile.close();
  
  yield(); // Feed the watchdog
  
  // Replace the old config file with the new one
  if (LittleFS.exists("/config.json")) {
    Serial.println(F("[CONFIG] Removing old config.json file"));
    if (LittleFS.remove("/config.json")) {
      Serial.println(F("[CONFIG] Old config.json removed successfully"));
    } else {
      Serial.println(F("[CONFIG] Failed to remove old config.json"));
      return;
    }
  } else {
    Serial.println(F("[CONFIG] No existing config.json to remove"));
  }
  
  yield(); // Feed the watchdog
  
  Serial.println(F("[CONFIG] Renaming config.json.new to config.json"));
  if (LittleFS.rename("/config.json.new", "/config.json")) {
    Serial.println(F("[CONFIG] Settings saved successfully"));
    
    // Verify the file was saved correctly
    File verifyFile = LittleFS.open("/config.json", "r");
    if (verifyFile) {
      Serial.print(F("[CONFIG] Verification: config.json size is "));
      Serial.print(verifyFile.size());
      Serial.println(F(" bytes"));
      
      // Read and print the first 100 bytes
      if (verifyFile.size() > 0) {
        char buffer[101];
        size_t bytesRead = verifyFile.readBytes(buffer, 100);
        buffer[bytesRead] = '\0';
        Serial.println(F("[CONFIG] First 100 bytes of saved config:"));
        Serial.println(buffer);
      }
      
      verifyFile.close();
    } else {
      Serial.println(F("[CONFIG] Failed to open config.json for verification"));
    }
  } else {
    Serial.println(F("[CONFIG] Failed to rename config file"));
  }
  
  yield(); // Feed the watchdog
  
  Serial.println(F("[NETATMO] Settings saved successfully"));
}


// Function to enhance API responses with CORS headers and logging
void enhanceApiResponse(AsyncWebServerRequest *request, const char* contentType, const String &payload) {
  // Create a response
  AsyncResponseStream *response = request->beginResponseStream(contentType);
  
  // Add CORS headers
  response->addHeader("Access-Control-Allow-Origin", "*");
  response->addHeader("Access-Control-Allow-Methods", "GET");
  response->addHeader("Access-Control-Allow-Headers", "Content-Type");
  
  // Add the payload
  response->print(payload);
  
  // Send the response
  request->send(response);
}

// Function to stream a file with enhanced headers


// Function to fetch Netatmo stations and devices - Redirects to the new implementation
String fetchNetatmoDevices() {
  Serial.println(F("[NETATMO] fetchNetatmoDevices() called - triggering async fetch"));
  
  // We need to use the external function to trigger the fetch
  // since we don't have direct access to the global variables
  triggerNetatmoDevicesFetch();
  
  // Return a placeholder response - the actual data will be fetched asynchronously
  return "{\"body\":{\"devices\":[]},\"status\":\"fetching\"}";
}


// Function to fetch indoor temperature from Netatmo
void fetchNetatmoIndoorTemperature() {
  Serial.println(F("\n[NETATMO] Fetching indoor temperature..."));
  Serial.print(F("[NETATMO] Indoor Module ID: '"));
  Serial.print(netatmoIndoorModuleId);
  Serial.println(F("'"));
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[NETATMO] Skipped: WiFi not connected"));
    netatmoIndoorTempAvailable = false;
    return;
  }
  
  if (strlen(netatmoDeviceId) == 0 || strlen(netatmoIndoorModuleId) == 0) {
    Serial.println(F("[NETATMO] Skipped: Missing device or indoor module ID"));
    Serial.print(F("[NETATMO] Device ID: '"));
    Serial.print(netatmoDeviceId);
    Serial.println(F("'"));
    Serial.print(F("[NETATMO] Indoor Module ID: '"));
    Serial.print(netatmoIndoorModuleId);
    Serial.println(F("'"));
    netatmoIndoorTempAvailable = false;
    return;
  }
  
  String token = getNetatmoToken();
  if (token.length() == 0) {
    Serial.println(F("[NETATMO] Skipped: No access token available"));
    netatmoIndoorTempAvailable = false;
    return;
  }
  
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure(); // Skip certificate validation
  
  HTTPClient https;
  
  // Try using the getstationsdata endpoint instead of getmeasure
  String url = "https://api.netatmo.com/api/getstationsdata?device_id=";
  url += netatmoDeviceId;
  
  Serial.print(F("[NETATMO] GET "));
  Serial.println(url);
  Serial.print(F("[NETATMO] Using token: "));
  Serial.println(token.substring(0, 10) + "...");
  
  if (https.begin(*client, url)) {
    https.addHeader("Authorization", "Bearer " + token);
    https.addHeader("Accept", "application/json");
    https.addHeader("User-Agent", "ESPTimeCast/1.0");
    
    Serial.println(F("[NETATMO] Sending request..."));
    int httpCode = https.GET();
    Serial.print(F("[NETATMO] HTTP response code: "));
    Serial.println(httpCode);
    
    if (httpCode == HTTP_CODE_OK) {
      String payload = https.getString();
      Serial.println(F("[NETATMO] Response:"));
      Serial.println(payload);
      
      JsonDocument doc; // Increased from 4096 to 8192
      
      if (parseNetatmoJson(payload, doc)) {
        // Navigate through the JSON to find the indoor module
        JsonArray devices = doc["body"]["devices"];
        
        Serial.print(F("[NETATMO] Number of devices found: "));
        Serial.println(devices.size());
        
        bool deviceFound = false;
        bool moduleFound = false;
        
        for (JsonObject device : devices) {
          String deviceId = device["_id"].as<String>();
          Serial.print(F("[NETATMO] Found device ID: "));
          Serial.println(deviceId);
          
          if (deviceId == netatmoDeviceId) {
            deviceFound = true;
            Serial.println(F("[NETATMO] Device ID matched!"));
            
            // First check if the indoor module is the main device itself
            if (String(netatmoIndoorModuleId) == deviceId) {
              moduleFound = true;
              Serial.println(F("[NETATMO] Indoor module is the main device"));
              
              JsonObject dashboard_data = device["dashboard_data"];
              
              if (dashboard_data["Temperature"].is<JsonVariant>()) {
                float temp = dashboard_data["Temperature"];
                netatmoIndoorTemp = temp;
                netatmoIndoorTempAvailable = true;
                Serial.print(F("[NETATMO] Indoor temperature: "));
                Serial.print(netatmoIndoorTemp);
                Serial.println(F("°C"));
                return;
              } else {
                Serial.println(F("[NETATMO] Temperature data not found in dashboard_data"));
                Serial.println(F("[NETATMO] Available dashboard_data fields:"));
                for (JsonPair kv : dashboard_data) {
                  Serial.print(kv.key().c_str());
                  Serial.print(F(": "));
                  Serial.println(kv.value().as<String>());
                }
              }
            }
            
            // If not found in main device, check modules
            if (!moduleFound) {
              JsonArray modules = device["modules"];
              Serial.print(F("[NETATMO] Number of modules found: "));
              Serial.println(modules.size());
              
              for (JsonObject module : modules) {
                String moduleId = module["_id"].as<String>();
                String moduleName = module["module_name"].as<String>();
                Serial.print(F("[NETATMO] Found module: "));
                Serial.print(moduleId);
                Serial.print(F(" ("));
                Serial.print(moduleName);
                Serial.println(F(")"));
                
                if (moduleId == String(netatmoIndoorModuleId)) {
                  moduleFound = true;
                  Serial.println(F("[NETATMO] Indoor module ID matched!"));
                  
                  JsonObject dashboard_data = module["dashboard_data"];
                  
                  if (dashboard_data["Temperature"].is<JsonVariant>()) {
                    float temp = dashboard_data["Temperature"];
                    netatmoIndoorTemp = temp;
                    netatmoIndoorTempAvailable = true;
                    Serial.print(F("[NETATMO] Indoor temperature: "));
                    Serial.print(netatmoIndoorTemp);
                    Serial.println(F("°C"));
                    return;
                  } else {
                    Serial.println(F("[NETATMO] Temperature data not found in dashboard_data"));
                    Serial.println(F("[NETATMO] Available dashboard_data fields:"));
                    for (JsonPair kv : dashboard_data) {
                      Serial.print(kv.key().c_str());
                      Serial.print(F(": "));
                      Serial.println(kv.value().as<String>());
                    }
                  }
                  break;
                }
              }
            }
            
            if (!moduleFound) {
              Serial.println(F("[NETATMO] Error: Indoor module ID not found in the device's modules"));
              netatmoIndoorTempAvailable = false;
            }
            break;
          }
        }
        
        if (!deviceFound) {
          Serial.println(F("[NETATMO] Error: Device ID not found in the response"));
          netatmoIndoorTempAvailable = false;
        }
      } else {
        Serial.println(F("[NETATMO] JSON parsing failed"));
        netatmoIndoorTempAvailable = false;
      }
    } else {
      Serial.print(F("[NETATMO] HTTP error: "));
      Serial.println(httpCode);
      
      // Get the error response
      String errorPayload = https.getString();
      Serial.println(F("[NETATMO] Error response:"));
      Serial.println(errorPayload);
      
      // Try to parse the error message
      JsonDocument errorDoc;
      DeserializationError errorParseError = deserializeJson(errorDoc, errorPayload);
      if (!errorParseError && errorDoc["error"].is<JsonVariant>()) {
        String errorType = errorDoc["error"].as<String>();
        String errorMessage = errorDoc["error_description"].is<JsonVariant>() ? 
                             errorDoc["error_description"].as<String>() : "Unknown error";
        
        Serial.print(F("[NETATMO] Error type: "));
        Serial.println(errorType);
        Serial.print(F("[NETATMO] Error message: "));
        Serial.println(errorMessage);
        
        // Handle specific error types
        if (errorType == "access_denied" || errorMessage.indexOf("blocked") >= 0) {
          Serial.println(F("[NETATMO] Request blocked - check your Netatmo app permissions"));
        }
      }
      
      // Check for "request is blocked" HTML response
      if (errorPayload.indexOf("The request is blocked") > 0) {
        handleBlockedRequest(errorPayload);
      }
      
      // If we get a 403 error (Forbidden), the token is expired or invalid
      if (httpCode == 403 || httpCode == 401) {
        // Check if the error payload indicates an invalid token
        if (isInvalidTokenError(errorPayload)) {
          Serial.println(F("[NETATMO] 403/401 error - invalid access token, need new API keys"));
          // Don't try to refresh, need new API keys
        } else {
          Serial.println(F("[NETATMO] 403/401 error - token expired"));
          forceNetatmoTokenRefresh();  // Force token refresh on next API call
        }
      }
      
      netatmoIndoorTempAvailable = false;
    }
    
    https.end();
  } else {
    Serial.println(F("[NETATMO] Unable to connect to Netatmo API"));
    netatmoIndoorTempAvailable = false;
  }
}


// Define DEBUG_FILE_DUMP to enable file dumping
// Comment this out in production to save memory
// #define DEBUG_FILE_DUMP

// Function to read and dump file contents to serial log
void dumpFileContents(const char* filePath) {
#ifdef DEBUG_FILE_DUMP
  Serial.print(F("[DEBUG] Dumping contents of file: "));
  Serial.println(filePath);
  
  if (!LittleFS.exists(filePath)) {
    Serial.println(F("[DEBUG] Error - File not found"));
    return;
  }
  
  File file = LittleFS.open(filePath, "r");
  if (!file) {
    Serial.println(F("[DEBUG] Error - Failed to open file for reading"));
    return;
  }
  
  Serial.println(F("[DEBUG] File content start >>>"));
  
  // Read and print the file in smaller chunks to reduce memory usage
  const size_t bufSize = 32; // Reduced buffer size
  char buf[bufSize];
  size_t totalRead = 0;
  
  while (file.available()) {
    size_t bytesRead = file.readBytes(buf, bufSize - 1);
    if (bytesRead > 0) {
      buf[bytesRead] = '\0'; // Null-terminate
      Serial.print(buf);
      totalRead += bytesRead;
      yield(); // Feed the watchdog
    }
  }
  
  Serial.println();
  Serial.println(F("<<< [DEBUG] File content end"));
  Serial.print(F("[DEBUG] Total bytes read: "));
  Serial.println(totalRead);
  
  file.close();
#else
  // In production mode, just log that we're skipping the dump
  Serial.print(F("[DEBUG] File dump disabled for: "));
  Serial.println(filePath);
#endif
}


// Function to list all files in the LittleFS filesystem


// Helper function to format temperature based on units
String formatTemperature(float temperature, bool roundToInteger) {
  char buffer[10]; // Buffer for formatted temperature
  
  // Apply temperature adjustment if needed
  if (strcmp(weatherUnits, "metric") == 0) {
    temperature += tempAdjust;
    if (roundToInteger) {
      // Format as integer with no decimal places
      sprintf(buffer, "%d", (int)round(temperature));
    } else {
      // Format with exactly one decimal place
      sprintf(buffer, "%.1f", temperature);
      
      // Check if the decimal part is .0 and remove it if so
      int len = strlen(buffer);
      if (len > 2 && buffer[len-2] == '.' && buffer[len-1] == '0') {
        buffer[len-2] = '\0'; // Terminate string before the decimal point
      }
    }
  } else if (strcmp(weatherUnits, "imperial") == 0) {
    // Convert from Celsius to Fahrenheit and apply adjustment
    temperature = (temperature * 9.0 / 5.0) + 32.0 + tempAdjust;
    if (roundToInteger) {
      // Format as integer with no decimal places
      sprintf(buffer, "%d", (int)round(temperature));
    } else {
      // Format with exactly one decimal place
      sprintf(buffer, "%.1f", temperature);
      
      // Check if the decimal part is .0 and remove it if so
      int len = strlen(buffer);
      if (len > 2 && buffer[len-2] == '.' && buffer[len-1] == '0') {
        buffer[len-2] = '\0'; // Terminate string before the decimal point
      }
    }
  } else { // standard (Kelvin)
    temperature = temperature + 273.15 + tempAdjust;
    if (roundToInteger) {
      // Format as integer with no decimal places
      sprintf(buffer, "%d", (int)round(temperature));
    } else {
      // Format with exactly one decimal place
      sprintf(buffer, "%.1f", temperature);
      
      // Check if the decimal part is .0 and remove it if so
      int len = strlen(buffer);
      if (len > 2 && buffer[len-2] == '.' && buffer[len-1] == '0') {
        buffer[len-2] = '\0'; // Terminate string before the decimal point
      }
    }
  }
  
  // Debug output to verify formatting
  Serial.print(F("[FORMAT] Formatted temperature: "));
  Serial.println(buffer);
  
  return String(buffer);
}


// Function to handle "request is blocked" errors from Netatmo
void handleBlockedRequest(String errorPayload) {
  Serial.println(F("[NETATMO] Detected 'request is blocked' error"));
  
  // Extract the error reference code if present
  String errorRef = "";
  int refStart = errorPayload.indexOf("errorref");
  if (refStart > 0) {
    refStart = errorPayload.indexOf(">", refStart) + 1;
    int refEnd = errorPayload.indexOf("<", refStart);
    if (refStart > 0 && refEnd > refStart) {
      errorRef = errorPayload.substring(refStart, refEnd);
      errorRef.trim();
      Serial.print(F("[NETATMO] Error reference: "));
      Serial.println(errorRef);
    }
  }
  
  Serial.println(F("[NETATMO] This error typically occurs due to:"));
  Serial.println(F("1. Incorrect app permissions in Netatmo developer portal"));
  Serial.println(F("2. Rate limiting by Netatmo API"));
  Serial.println(F("3. IP address restrictions or geoblocking"));
  
  // Clear tokens to force re-authentication on next attempt
  Serial.println(F("[NETATMO] Clearing tokens to force re-authentication"));
  netatmoAccessToken[0] = '\0';
  netatmoRefreshToken[0] = '\0';
  saveTokensToConfig();
  
  // Set a longer delay before next attempt
  Serial.println(F("[NETATMO] Will retry after a longer delay (2 minutes)"));
  needTokenRefresh = true;
  lastBlockedRequest = millis();
}


// Function to set up HTTP client with improved timeout settings


// Function to log the complete API request details - simplified version


// Memory cleanup functions

// Function to clean up memory after API calls
void cleanupAfterAPICall() {
  yield();
}

// Function to clean up memory before API calls
void prepareForAPICall() {
  yield();
}

// Function to release unused memory
void releaseUnusedMemory() {
  yield();
}


// Memory defragmentation functions

// Function to print memory statistics
void printMemoryStats() {
  Serial.print(F("[MEMORY] Free heap: "));
  Serial.print(ESP.getFreeHeap());
  Serial.println(F(" bytes"));
  
  Serial.print(F("[MEMORY] Heap fragmentation: "));
  Serial.print(ESP.getHeapFragmentation());
  Serial.println(F("%"));
  
  Serial.print(F("[MEMORY] Largest free block: "));
  Serial.print(ESP.getMaxFreeBlockSize());
  Serial.println(F(" bytes"));
  
  // Add stack information
  Serial.print(F("[MEMORY] Free stack: "));
  Serial.print(ESP.getFreeContStack());
  Serial.println(F(" bytes"));
}

// Heap "defragmentation" is not effective on ESP8266's allocator.
// Replaced with a simple yield to feed the watchdog.
void defragmentHeap() {
  yield();
}

// Function to force garbage collection with safer yield handling
void forceGarbageCollection() {
  yield();
}


// Function to check if defragmentation is needed
bool shouldDefragment() {
  int fragmentation = ESP.getHeapFragmentation();
  int freeStack = ESP.getFreeContStack();
  
  // Check both fragmentation and stack space
  return (fragmentation > 50 || freeStack < 2048);
}


// Function to check if the Netatmo access token is valid
bool isNetatmoTokenValid() {
  // Check if the token exists
  if (strlen(netatmoAccessToken) == 0) {
    Serial.println(F("[NETATMO] No access token available"));
    return false;
  }
  
  // Check if the token is properly formatted (should be a long string)
  if (strlen(netatmoAccessToken) < 20) {
    Serial.println(F("[NETATMO] Access token appears to be invalid (too short)"));
    return false;
  }
  
  // OAuth2 tokens can contain many special characters including |, ~, etc.
  // Instead of checking for valid characters, just log the token format
  Serial.print(F("[NETATMO] Access token format check: length="));
  Serial.println(strlen(netatmoAccessToken));
  
  return true;
}


// Function to check network connectivity
bool checkNetworkConnectivity() {
  Serial.println(F("[NETWORK] Checking network connectivity..."));
  
  // Check if WiFi is connected
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[NETWORK] WiFi not connected"));
    return false;
  }
  
  // Print WiFi information
  Serial.print(F("[NETWORK] WiFi connected to: "));
  Serial.println(WiFi.SSID());
  Serial.print(F("[NETWORK] IP address: "));
  Serial.println(WiFi.localIP());
  Serial.print(F("[NETWORK] Signal strength: "));
  Serial.print(WiFi.RSSI());
  Serial.println(F(" dBm"));
  
  // Try to resolve a domain name
  Serial.print(F("[NETWORK] Resolving api.netatmo.com... "));
  IPAddress netatmoIp;
  bool dnsResult = WiFi.hostByName("api.netatmo.com", netatmoIp);
  
  if (dnsResult) {
    Serial.print(F("Success: "));
    Serial.println(netatmoIp.toString());
  } else {
    Serial.println(F("Failed"));
  }
  
  return dnsResult;
}


// Optimized handlers for web requests to reduce memory usage

// Function to set up optimized handlers
void setupOptimizedHandlers() {
  // Replace standard handlers with optimized versions
  
  // Netatmo devices.js handler
  server.on("/netatmo-devices.js", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println(F("[WEBSERVER] Request: /netatmo-devices.js"));
    
    // Check if we should handle this request
    if (!shouldHandleWebRequest()) {
      request->send(503, "text/plain", "Server busy, try again later");
      return;
    }
    
    // Use direct file serving instead of streaming to avoid AsyncWebServer issues
    request->send(LittleFS, "/netatmo-devices.js", "application/javascript");
  });
  
  // Netatmo HTML handler
  server.on("/netatmo.html", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println(F("[WEBSERVER] Request: /netatmo.html"));
    
    // Check if we should handle this request
    if (!shouldHandleWebRequest()) {
      request->send(503, "text/plain", "Server busy, try again later");
      return;
    }
    
    // Use direct file serving instead of streaming to avoid AsyncWebServer issues
    request->send(LittleFS, "/netatmo.html", "text/html");
  });
  
  // Index HTML handler
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println(F("[WEBSERVER] Request: /"));
    
    // Check if we should handle this request
    if (!shouldHandleWebRequest()) {
      request->send(503, "text/plain", "Server busy, try again later");
      return;
    }
    
    // Use direct file serving instead of streaming to avoid AsyncWebServer issues
    request->send(LittleFS, "/index.html", "text/html");
  });
  
  // CSS handler
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println(F("[WEBSERVER] Request: /style.css"));
    
    // Check if we should handle this request
    if (!shouldHandleWebRequest()) {
      request->send(503, "text/plain", "Server busy, try again later");
      return;
    }
    
    // Use direct file serving instead of streaming to avoid AsyncWebServer issues
    request->send(LittleFS, "/style.css", "text/css");
  });
}


// Memory optimization functions

// Function to optimize BearSSL client memory usage
void optimizeBearSSLClient(BearSSL::WiFiClientSecure* client) {
  // Set smaller buffer sizes to reduce memory usage
  client->setBufferSizes(512, 512);
  
  // Skip certificate validation to save memory
  client->setInsecure();
}

// Function to optimize HTTP client memory usage
void optimizeHTTPClient(HTTPClient& https) {
  // Set a reasonable timeout
  https.setTimeout(10000);
  
  // Disable chunked transfer encoding to save memory
  https.useHTTP10(true);
}

// Function to create a memory-efficient authorization header
void createAuthHeader(char* buffer, size_t bufferSize, const char* token) {
  snprintf(buffer, bufferSize, "Bearer %s", token);
}


// Function to parse Netatmo JSON with proper error handling and memory management
bool parseNetatmoJson(String &payload, JsonDocument &doc) {
  Serial.println(F("[NETATMO] Parsing JSON response..."));
  
  // Always use a filter — critical for ESP8266 memory with ArduinoJson v7
  JsonDocument filter;
  JsonObject filter_body = filter["body"].to<JsonObject>();
  JsonArray filter_devices = filter_body["devices"].to<JsonArray>();
  JsonObject device = filter_devices.add<JsonObject>();
  device["_id"] = true;
  device["station_name"] = true;
  device["dashboard_data"]["Temperature"] = true;
  JsonArray modules = device["modules"].to<JsonArray>();
  JsonObject module = modules.add<JsonObject>();
  module["_id"] = true;
  module["module_name"] = true;
  module["dashboard_data"]["Temperature"] = true;

  DeserializationError error = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  
  if (error) {
    Serial.print(F("[NETATMO] JSON parse error: "));
    Serial.println(error.c_str());
    return false;
  }
  
  Serial.println(F("[NETATMO] JSON parsed successfully"));
  return true;
}





// Function to set up the save settings handler
void setupSaveSettingsHandler() {
  // Use a GET request instead of POST to avoid the body parsing issues
  server.on("/api/netatmo/save-settings-simple", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println(F("[NETATMO] Handling simple save settings request"));
    
    // Extract parameters from URL query string
    if (request->hasParam("deviceId")) {
      String value = request->getParam("deviceId")->value();
      strlcpy(netatmoDeviceId, value.c_str(), sizeof(netatmoDeviceId));
      
      // Also update stationId if it's empty (for backward compatibility)
      if (strlen(netatmoStationId) == 0) {
        strlcpy(netatmoStationId, value.c_str(), sizeof(netatmoStationId));
      }
    }
    
    // Add support for explicitly setting stationId
    if (request->hasParam("stationId")) {
      String value = request->getParam("stationId")->value();
      if (value == "none") {
        netatmoStationId[0] = '\0'; // Empty string
      } else {
        strlcpy(netatmoStationId, value.c_str(), sizeof(netatmoStationId));
      }
    }
    
    if (request->hasParam("moduleId")) {
      String value = request->getParam("moduleId")->value();
      if (value == "none") {
        netatmoModuleId[0] = '\0'; // Empty string
      } else {
        strlcpy(netatmoModuleId, value.c_str(), sizeof(netatmoModuleId));
      }
    }
    
    if (request->hasParam("indoorModuleId")) {
      String value = request->getParam("indoorModuleId")->value();
      if (value == "none") {
        netatmoIndoorModuleId[0] = '\0'; // Empty string
      } else {
        strlcpy(netatmoIndoorModuleId, value.c_str(), sizeof(netatmoIndoorModuleId));
      }
    }
    
    if (request->hasParam("useOutdoor")) {
      String value = request->getParam("useOutdoor")->value();
      useNetatmoOutdoor = (value == "true" || value == "1");
    }
    
    if (request->hasParam("prioritizeIndoor")) {
      String value = request->getParam("prioritizeIndoor")->value();
      prioritizeNetatmoIndoor = (value == "true" || value == "1");
      
      // Set tempSource based on prioritizeNetatmoIndoor setting
      tempSource = prioritizeNetatmoIndoor ? 1 : 0;
      Serial.print(F("[NETATMO] Setting tempSource to: "));
      Serial.println(tempSource);
    }
    
    // Send response
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Settings received\"}");
    
    // Set flag to save settings in the main loop
    settingsSavePending = true;
    
    // Debug output
    Serial.println(F("[NETATMO] Settings received:"));
    Serial.print(F("  netatmoDeviceId: "));
    Serial.println(netatmoDeviceId);
    Serial.print(F("  netatmoStationId: "));
    Serial.println(netatmoStationId);
    Serial.print(F("  netatmoModuleId: "));
    Serial.println(netatmoModuleId);
    Serial.print(F("  netatmoIndoorModuleId: "));
    Serial.println(netatmoIndoorModuleId);
    Serial.print(F("  useNetatmoOutdoor: "));
    Serial.println(useNetatmoOutdoor ? "true" : "false");
    Serial.print(F("  prioritizeNetatmoIndoor: "));
    Serial.println(prioritizeNetatmoIndoor ? "true" : "false");
  });
  
}


/*
 * Netatmo Handler
 * 
 * This file handles Netatmo API integration.
 * 
 * Important file paths:
 * - /netatmo_stations_data.json: Contains full stations data from the Netatmo API
 *   Used by the web interface to populate device selection dropdowns
 * 
 * - /netatmo_config.json: Contains the selected device configuration
 *   Used by the clock system to read the configured Netatmo settings
 */

// Global variables for token exchange
static String pendingCode = "";
static bool tokenExchangePending = false;
static bool fetchDevicesPending = false;
static bool fetchStationsDataPending = false;

// Helper function to URL encode a string (memory-efficient version)
String urlEncode(const char* input) {
  const char *hex = "0123456789ABCDEF";
  String result = "";
  
  while (*input) {
    char c = *input++;
    if (isAlphaNumeric(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      result += c;
    } else {
      result += '%';
      result += hex[c >> 4];
      result += hex[c & 0xF];
    }
  }
  
  return result;
}

// Function to refresh the Netatmo token
// Function to refresh the Netatmo token
bool refreshNetatmoToken() {
  unsigned long startTime = millis();
  Serial.println(F("[NETATMO-DEBUG] Starting token refresh function"));
  Serial.println(F("[NETATMO] Refreshing token"));
  
  if (strlen(netatmoRefreshToken) == 0) {
    Serial.println(F("[NETATMO] Error - No refresh token"));
    return false;
  }
  
  // Prepare memory for API call - use the same function as successful calls
  prepareForAPICall();
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[NETATMO] Error - WiFi not connected"));
    return false;
  }
  
  // Add a ping test to verify basic network connectivity
  Serial.println(F("[NETATMO-DEBUG] Testing network connectivity..."));
  IPAddress ip;
  if (!WiFi.hostByName("api.netatmo.com", ip)) {
    Serial.println(F("[NETATMO-DEBUG] DNS resolution failed for api.netatmo.com"));
    return false;
  }
  
  Serial.print(F("[NETATMO-DEBUG] api.netatmo.com resolved to: "));
  Serial.println(ip.toString());
  
  // Ping the resolved IP to test connectivity
  Serial.print(F("[NETATMO] Pinging "));
  Serial.print(ip.toString());
  Serial.println(F("..."));
  
  // Create a new client for the API call
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  optimizeBearSSLClient(client.get()); // Use our optimization function
  
  HTTPClient https;
  optimizeHTTPClient(https); // Use our optimization function
  
  // Use the same URL format as successful API calls
  String apiUrl = "https://api.netatmo.com/oauth2/token";
  Serial.print(F("[NETATMO] Connecting to: "));
  Serial.println(apiUrl);
  
  // Now try the HTTPS connection with SSL error diagnostics
  Serial.println(F("[NETATMO-DEBUG] Initializing HTTPS connection..."));
  if (!https.begin(*client, apiUrl)) {
    int sslError = client->getLastSSLError();
    char errorMsg[100];
    client->getLastSSLError(errorMsg, sizeof(errorMsg));
    
    Serial.print(F("[NETATMO-DEBUG] Error - Failed to connect. SSL Error Code: "));
    Serial.println(sslError);
    Serial.print(F("[NETATMO-DEBUG] SSL Error Message: "));
    Serial.println(errorMsg);
    return false;
  }
  
  // Add headers like successful API calls
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");
  https.addHeader("Accept", "application/json");
  https.addHeader("User-Agent", "ESPTimeCast/1.0");
  
  // Build the POST data in chunks to minimize memory usage
  static char postData[512]; // Increased buffer size for POST data
  memset(postData, 0, sizeof(postData));
  
  // Build the POST data manually to minimize memory usage
  strcat(postData, "grant_type=refresh_token");
  strcat(postData, "&client_id=");
  strcat(postData, netatmoClientId);
  strcat(postData, "&client_secret=");
  strcat(postData, netatmoClientSecret);
  strcat(postData, "&refresh_token=");
  strcat(postData, netatmoRefreshToken);
  
  // Defragment heap right before making the request
  defragmentHeap();
  yield(); // Allow the watchdog to be fed
  
  // Make the request with yield to avoid watchdog issues
  Serial.println(F("[NETATMO-DEBUG] Sending request..."));
  int httpCode = https.POST(postData);
  yield(); // Allow the watchdog to be fed
  
  Serial.print(F("[NETATMO-DEBUG] HTTP response code: "));
  Serial.println(httpCode);
  
  if (httpCode != HTTP_CODE_OK) {
    Serial.print(F("[NETATMO-DEBUG] Error - HTTP code: "));
    Serial.println(httpCode);
    
    // Add SSL error diagnostics for connection errors
    if (httpCode <= 0) {
      int sslError = client->getLastSSLError();
      char errorMsg[100];
      client->getLastSSLError(errorMsg, sizeof(errorMsg));
      
      Serial.print(F("[NETATMO-DEBUG] SSL Error Code: "));
      Serial.println(sslError);
      Serial.print(F("[NETATMO-DEBUG] SSL Error Message: "));
      Serial.println(errorMsg);
    }
    
    // Get error payload with yield
    String errorPayload = https.getString();
    yield(); // Allow the watchdog to be fed
    
    Serial.print(F("[NETATMO] Error payload: "));
    Serial.println(errorPayload);
    https.end();
    
    // If we get a 401 or 403, try to refresh the token
    if (httpCode == 401 || httpCode == 403) {
      Serial.println(F("[NETATMO] Invalid credentials or refresh token, clearing tokens"));
      netatmoRefreshToken[0] = '\0';
      netatmoAccessToken[0] = '\0';
      saveTokensToConfig();
      netatmoStatus = NETATMO_AUTH_FAIL;
    } else if (httpCode <= 0) {
      // This is a connection error
      Serial.println(F("[NETATMO] Connection error, not clearing refresh token"));
      netatmoStatus = NETATMO_CONN_FAIL;
    }
    
    return false;
  }
  
  // Get the response with yield
  String response = https.getString();
  yield(); // Allow the watchdog to be fed
  
  Serial.println(F("[NETATMO] Response received"));
  https.end();
  
  // Parse the response with a larger JSON document size
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);
  
  if (error) {
    Serial.print(F("[NETATMO] JSON parse error: "));
    Serial.println(error.c_str());
    return false;
  }
  
  // Extract the tokens
  if (!doc["access_token"].is<JsonVariant>() || !doc["refresh_token"].is<JsonVariant>()) {
    Serial.println(F("[NETATMO] Missing tokens in response"));
    return false;
  }
  
  // Memory optimization: Extract tokens directly as strings
  const char* accessToken = doc["access_token"];
  const char* refreshToken = doc["refresh_token"];
  
  Serial.println(F("[NETATMO] Saving new tokens"));
  Serial.print(F("[NETATMO] Access token length: "));
  Serial.println(strlen(accessToken));
  Serial.print(F("[NETATMO] Refresh token length: "));
  Serial.println(strlen(refreshToken));
  
  strlcpy(netatmoAccessToken, accessToken, sizeof(netatmoAccessToken));
  strlcpy(netatmoRefreshToken, refreshToken, sizeof(netatmoRefreshToken));
  
  // Reset the auth failure flag since we got a valid token
  
  // Save the tokens to config
  saveTokensToConfig();
  
  // Report total time taken
  Serial.print(F("[TIMING] Total token refresh took: "));
  Serial.print(millis() - startTime);
  Serial.println(F(" ms"));
  
  return true;
}

// Function to trigger Netatmo device fetch from external files
void triggerNetatmoDevicesFetch() {
  Serial.println(F("[NETATMO] Device fetch triggered externally"));
  fetchDevicesPending = true;
}

// Function to get cached device data

// Function to exchange authorization code for tokens
void exchangeAuthCode(const String &code) {
  Serial.println(F("[NETATMO] Starting token exchange process"));
  
  // This will be called in the next loop iteration
  pendingCode = code;
  
  // Set a flag to process this in the main loop
  tokenExchangePending = true;
}

// Global flag to indicate that credentials need to be saved
static bool saveCredentialsPending = false;

// Function to setup Netatmo OAuth handler
void setupNetatmoHandler() {
  Serial.println(F("[NETATMO] Setting up Netatmo OAuth handler..."));
  
  // Check if LittleFS is mounted
  if (!LittleFS.begin()) {
    Serial.println(F("[NETATMO] ERROR: Failed to mount LittleFS"));
    return;
  }
  Serial.println(F("[NETATMO] LittleFS mounted successfully"));
  
  // Instead of creating new objects, use simpler approach
  Serial.println(F("[NETATMO] Setting up API endpoints"));
  
  // Endpoint to save Netatmo credentials without rebooting
  server.on("/api/netatmo/save-credentials", HTTP_POST, [](AsyncWebServerRequest *request) {
    Serial.println(F("[NETATMO] Handling save credentials"));
    
    if (!request->hasParam("clientId", true) || !request->hasParam("clientSecret", true)) {
      Serial.println(F("[NETATMO] Error - Missing parameters"));
      request->send(400, "text/plain", "Missing parameters");
      return;
    }
    
    String clientId = request->getParam("clientId", true)->value();
    String clientSecret = request->getParam("clientSecret", true)->value();
    
    Serial.print(F("[NETATMO] Client ID: "));
    Serial.println(clientId);
    Serial.println(F("[NETATMO] Client Secret is set"));
    
    // Save to global variables
    strlcpy(netatmoClientId, clientId.c_str(), sizeof(netatmoClientId));
    strlcpy(netatmoClientSecret, clientSecret.c_str(), sizeof(netatmoClientSecret));
    
    // Set flag to save in main loop instead of doing it here
    saveCredentialsPending = true;
    
    // Send a simple response
    request->send(200, "application/json", "{\"success\":true}");
    
    Serial.println(F("[NETATMO] Credentials saved to memory, will be written to config in main loop"));
    
    // Debug: Print the current values
    Serial.print(F("[NETATMO] Current netatmoClientId: "));
    Serial.println(netatmoClientId);
    Serial.print(F("[NETATMO] Current netatmoClientSecret length: "));
    Serial.println(strlen(netatmoClientSecret));
  });
  
  // Endpoint to get auth URL without redirecting
  server.on("/api/netatmo/get-auth-url", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println(F("[NETATMO] Handling get auth URL request"));
    
    // Debug output to check client ID and secret
    Serial.print(F("[NETATMO] Client ID length: "));
    Serial.println(strlen(netatmoClientId));
    Serial.print(F("[NETATMO] Client Secret length: "));
    Serial.println(strlen(netatmoClientSecret));
    
    if (strlen(netatmoClientId) == 0 || strlen(netatmoClientSecret) == 0) {
      Serial.println(F("[NETATMO] Error - No credentials"));
      request->send(400, "application/json", "{\"error\":\"Missing credentials\"}");
      return;
    }
    
    // Generate authorization URL with minimal memory usage
    // Build the redirect URI with pre-encoded format to save memory
    String authUrl = "https://api.netatmo.com/oauth2/authorize";
    authUrl += "?client_id=";
    authUrl += urlEncode(netatmoClientId);
    authUrl += "&redirect_uri=http%3A%2F%2F";
    authUrl += WiFi.localIP().toString();
    authUrl += "%2Fapi%2Fnetatmo%2Fcallback";
    authUrl += "&scope=read_station%20read_homecoach&state=state&response_type=code";
    
    Serial.print(F("[NETATMO] Auth URL: "));
    Serial.println(authUrl);
    
    // Return the URL as JSON
    String response = "{\"url\":\"";
    response += authUrl;
    response += "\"}";
    
    request->send(200, "application/json", response);
  });
  
  // Endpoint to initiate OAuth flow
  server.on("/api/netatmo/auth", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println(F("[NETATMO] Handling auth request"));
    
    // Debug output to check client ID and secret
    Serial.print(F("[NETATMO] Client ID length: "));
    Serial.println(strlen(netatmoClientId));
    Serial.print(F("[NETATMO] Client Secret length: "));
    Serial.println(strlen(netatmoClientSecret));
    
    if (strlen(netatmoClientId) == 0 || strlen(netatmoClientSecret) == 0) {
      Serial.println(F("[NETATMO] Error - No credentials"));
      request->send(400, "text/plain", "Missing credentials");
      return;
    }
    
    // Generate authorization URL with minimal memory usage
    // Store the URL in a static variable to avoid stack issues
    static String authUrl;
    authUrl = "https://api.netatmo.com/oauth2/authorize";
    authUrl += "?client_id=";
    authUrl += urlEncode(netatmoClientId);
    authUrl += "&redirect_uri=";
    
    // Use pre-encoded redirect URI to save memory
    authUrl += "http%3A%2F%2F";
    authUrl += WiFi.localIP().toString();
    authUrl += "%2Fapi%2Fnetatmo%2Fcallback";
    authUrl += "&scope=read_station%20read_homecoach&state=state&response_type=code";
    
    Serial.print(F("[NETATMO] Auth URL: "));
    Serial.println(authUrl);
    
    // Use a simpler response with a meta refresh instead of a redirect
    String html = F("<!DOCTYPE html>");
    html += F("<html><head>");
    html += F("<meta http-equiv='refresh' content='0;url=");
    html += authUrl;
    html += F("'>");
    html += F("<title>Redirecting...</title>");
    html += F("</head><body>");
    html += F("<p>Redirecting to Netatmo authorization page...</p>");
    html += F("<p>If you are not redirected, <a href='");
    html += authUrl;
    html += F("'>click here</a>.</p>");
    html += F("</body></html>");
    
    request->send(200, "text/html", html);
    Serial.println(F("[NETATMO] Auth page sent"));
  });
  // Endpoint to handle OAuth callback
  server.on("/api/netatmo/callback", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println(F("[NETATMO] Handling OAuth callback"));
    
    // Simple error handling to save memory
    if (request->hasParam("error")) {
      String error = request->getParam("error")->value();
      Serial.print(F("[NETATMO] OAuth error: "));
      Serial.println(error);
      
      // Simple HTML response to save memory
      String html = F("<html><body><h1>Auth Error</h1><p>Error: ");
      html += error;
      html += F("</p><a href='/netatmo.html'>Back</a></body></html>");
      
      request->send(200, "text/html", html);
      return;
    }
    
    if (!request->hasParam("code")) {
      Serial.println(F("[NETATMO] Error - No authorization code in callback"));
      request->send(400, "text/plain", "No code parameter");
      return;
    }
    
    String code = request->getParam("code")->value();
    Serial.print(F("[NETATMO] Received code: "));
    Serial.println(code);
    
    // Store the code for later processing in the main loop
    // This avoids deep stack issues during the callback
    pendingCode = code;
    tokenExchangePending = true;
    
    // Simplified HTML response with auto-redirect to minimize memory usage
    static const char SUCCESS_HTML[] PROGMEM = 
      "<!DOCTYPE html>"
      "<html><head>"
      "<meta charset='UTF-8'>"
      "<meta http-equiv='refresh' content='15;url=/netatmo.html'>"
      "<title>ESPTimeCast - Authorization</title>"
      "<style>body{font-family:Arial;text-align:center;margin:50px}</style>"
      "</head><body>"
      "<h1>Authorization Successful</h1>"
      "<p>Exchanging code for tokens...</p>"
      "<p>You will be redirected to the Netatmo settings page in 15 seconds.</p>"
      "<p>If not redirected, <a href='/netatmo.html'>click here</a>.</p>"
      "</body></html>";
    
    request->send_P(200, "text/html", SUCCESS_HTML);
  });
  
  // Endpoint to get the access token
  server.on("/api/netatmo/token", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println(F("[NETATMO] Handling get token request"));
    
    if (strlen(netatmoAccessToken) == 0) {
      Serial.println(F("[NETATMO] Error - No access token"));
      request->send(401, "application/json", "{\"error\":\"Not authenticated with Netatmo\"}");
      return;
    }
    
    // Create a JSON response with the access token
    String response = "{\"access_token\":\"";
    response += netatmoAccessToken;
    response += "\"}";
    
    request->send(200, "application/json", response);
  });
  
  // Clear tokens endpoint — no HTTPS call, just wipes stored tokens
  server.on("/api/netatmo/clear-tokens", HTTP_GET, [](AsyncWebServerRequest *request) {
    netatmoAccessToken[0] = '\0';
    netatmoRefreshToken[0] = '\0';
    saveTokensToConfig();
    netatmoStatus = NETATMO_NO_TOKEN;
    request->send(200, "application/json", "{\"status\":\"tokens cleared\"}");
  });

  // Add a token refresh endpoint — defers to main loop to avoid stack overflow
  server.on("/api/netatmo/refresh-token", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println(F("[NETATMO] Handling token refresh request"));
    if (strlen(netatmoRefreshToken) == 0) {
      request->send(401, "application/json", "{\"error\":\"No refresh token available\"}");
      return;
    }
    needTokenRefresh = true;
    netatmoAccessToken[0] = '\0'; // Force re-fetch
    request->send(200, "application/json", "{\"status\":\"token refresh scheduled\"}");
  });
  
  // Endpoint to save Netatmo settings without rebooting
  server.on("/api/netatmo/save-settings", HTTP_POST, [](AsyncWebServerRequest *request) {
    Serial.println(F("[NETATMO] Handling save settings request"));
    
    // Process all parameters
    for (int i = 0; i < request->params(); i++) {
      const AsyncWebParameter* p = request->getParam(i);
      String name = p->name();
      String value = p->value();
      
      if (name == "netatmoDeviceId") {
        strlcpy(netatmoDeviceId, value.c_str(), sizeof(netatmoDeviceId));
      }
      else if (name == "netatmoModuleId") {
        // Handle "none" value
        if (value == "none") {
          netatmoModuleId[0] = '\0'; // Empty string
        } else {
          strlcpy(netatmoModuleId, value.c_str(), sizeof(netatmoModuleId));
        }
      }
      else if (name == "netatmoIndoorModuleId") {
        // Handle "none" value
        if (value == "none") {
          netatmoIndoorModuleId[0] = '\0'; // Empty string
        } else {
          strlcpy(netatmoIndoorModuleId, value.c_str(), sizeof(netatmoIndoorModuleId));
        }
      }
      else if (name == "useNetatmoOutdoor") {
        useNetatmoOutdoor = (value == "true" || value == "on" || value == "1");
      }
      else if (name == "prioritizeNetatmoIndoor") {
        prioritizeNetatmoIndoor = (value == "true" || value == "on" || value == "1");
        // Set tempSource based on prioritizeNetatmoIndoor setting
        tempSource = prioritizeNetatmoIndoor ? 1 : 0;
        Serial.print(F("[NETATMO] Setting tempSource to: "));
        Serial.println(tempSource);
      }
    }
    
    // Save to config file
    saveTokensToConfig();
    
    // Send success response
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Settings saved\"}");
  });
  
  // Add a proxy endpoint for Netatmo API calls to avoid CORS issues
  server.on("/api/netatmo/proxy", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println(F("[NETATMO] Handling proxy request"));
    
    // Check if we have a valid access token
    if (strlen(netatmoAccessToken) == 0) {
      Serial.println(F("[NETATMO] Error - No access token"));
      request->send(401, "application/json", "{\"error\":\"Not authenticated with Netatmo\"}");
      return;
    }
    
    // Debug output
    Serial.print(F("[NETATMO] Using access token: "));
    Serial.println(netatmoAccessToken);
    
    // Check which API endpoint to call
    String endpoint = "getstationsdata";
    if (request->hasParam("endpoint")) {
      endpoint = request->getParam("endpoint")->value();
    }
    
    // Check if there's already a pending request
    if (proxyPending) {
      request->send(429, "application/json", "{\"error\":\"Another request is already in progress\"}");
      return;
    }
    
    // Set up the deferred request
    proxyPending = true;
    proxyEndpoint = endpoint;
    proxyRequest = request;
    
    // The actual request will be processed in the loop() function via processProxyRequest()
    // This prevents blocking the web server and avoids watchdog timeouts
    
    // Note: We don't call request->send() here because we'll do that in processProxyRequest()
  });
  
  // Add a debug endpoint to check token status
  server.on("/api/netatmo/token-status", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println(F("[NETATMO] Handling token status request"));
    
    String response = "{";
    response += "\"hasAccessToken\":" + String(strlen(netatmoAccessToken) > 0 ? "true" : "false");
    response += ",\"hasRefreshToken\":" + String(strlen(netatmoRefreshToken) > 0 ? "true" : "false");
    response += ",\"accessTokenLength\":" + String(strlen(netatmoAccessToken));
    response += ",\"refreshTokenLength\":" + String(strlen(netatmoRefreshToken));
    
    // Add the first few characters of the tokens for verification
    if (strlen(netatmoAccessToken) > 10) {
      response += ",\"accessTokenPrefix\":\"" + String(netatmoAccessToken).substring(0, 10) + "...\"";
    }
    
    if (strlen(netatmoRefreshToken) > 10) {
      response += ",\"refreshTokenPrefix\":\"" + String(netatmoRefreshToken).substring(0, 10) + "...\"";
    }
    
    response += "}";
    
    request->send(200, "application/json", response);
  });
  
  // Add an endpoint to get the current API keys
  server.on("/api/netatmo/get-credentials", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println(F("[NETATMO] Handling get credentials request"));
    
    String response = "{";
    response += "\"clientId\":\"" + String(netatmoClientId) + "\"";
    response += ",\"clientSecret\":\"" + String(netatmoClientSecret) + "\"";
    response += ",\"hasClientSecret\":" + String(strlen(netatmoClientSecret) > 0 ? "true" : "false");
    response += "}";
    
    request->send(200, "application/json", response);
  });
  
  // Add an endpoint to get memory information and trigger defragmentation
  server.on("/api/memory", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println(F("[MEMORY] Handling memory info request"));
    
    // Check if defragmentation was requested
    bool defrag = false;
    if (request->hasParam("defrag")) {
      defrag = request->getParam("defrag")->value() == "true";
    }
    
    // Get memory stats
    uint32_t freeHeap = ESP.getFreeHeap();
    uint8_t fragmentation = ESP.getHeapFragmentation();
    uint32_t maxFreeBlock = ESP.getMaxFreeBlockSize();
    uint32_t freeStack = ESP.getFreeContStack();
    
    // Print memory stats to serial
    printMemoryStats();
    
    // Perform defragmentation if requested
    if (defrag) {
      Serial.println(F("[MEMORY] Defragmentation requested via API"));
      defragmentHeap();
      
      // Get updated stats
      freeHeap = ESP.getFreeHeap();
      fragmentation = ESP.getHeapFragmentation();
      maxFreeBlock = ESP.getMaxFreeBlockSize();
      freeStack = ESP.getFreeContStack();
    }
    
    // Create response
    String response = "{";
    response += "\"freeHeap\":" + String(freeHeap);
    response += ",\"fragmentation\":" + String(fragmentation);
    response += ",\"maxFreeBlock\":" + String(maxFreeBlock);
    response += ",\"freeStack\":" + String(freeStack);
    response += ",\"defragPerformed\":" + String(defrag ? "true" : "false");
    response += "}";
    
    request->send(200, "application/json", response);
  });
  
  // Add an endpoint to trigger device fetch
  server.on("/api/netatmo/fetch-devices", HTTP_POST, [](AsyncWebServerRequest *request) {
    Serial.println(F("[NETATMO] Handling fetch devices request"));
    
    // Check if we have a valid access token
    if (strlen(netatmoAccessToken) == 0) {
      Serial.println(F("[NETATMO] Error - No access token"));
      request->send(401, "application/json", "{\"error\":\"Not authenticated with Netatmo\"}");
      return;
    }
    
    // Set the flag to fetch devices in the main loop
    fetchDevicesPending = true;
    
    // Send a response indicating that the fetch has been initiated
    request->send(200, "application/json", "{\"status\":\"initiated\",\"message\":\"Device fetch initiated\"}");
  });
  
  // Add an endpoint to retrieve the saved device data
  server.on("/api/netatmo/saved-devices", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println(F("[NETATMO] Handling saved devices request"));
    
    // Check if the file exists
    if (!LittleFS.exists("/netatmo_config.json")) {
      Serial.println(F("[NETATMO] Device data file not found"));
      request->send(404, "application/json", "{\"error\":\"Device data not found\"}");
      return;
    }
    
    // Open the file
    File deviceFile = LittleFS.open("/netatmo_config.json", "r");
    if (!deviceFile) {
      Serial.println(F("[NETATMO] Failed to open device data file"));
      request->send(500, "application/json", "{\"error\":\"Failed to open device data file\"}");
      return;
    }
    
    // Create a response stream
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    
    // Stream the file content to the response
    const size_t bufSize = 256;
    uint8_t buf[bufSize];
    
    while (deviceFile.available()) {
      size_t bytesRead = deviceFile.read(buf, bufSize);
      if (bytesRead > 0) {
        response->write(buf, bytesRead);
      }
      yield(); // Allow the watchdog to be fed
    }
    
    deviceFile.close();
    request->send(response);
  });
  
  // Add an endpoint to retrieve mock device data when API is unavailable
  server.on("/api/netatmo/mock-devices", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println(F("[NETATMO] Handling mock devices request"));
    
    // Instead of reading from file, use a hardcoded minimal JSON response
    // This avoids file system operations that might cause yield issues
    static const char MOCK_JSON[] PROGMEM = 
      "{\"body\":{\"devices\":[{\"_id\":\"70:ee:50:00:00:01\",\"station_name\":\"Mock Weather Station\",\"type\":\"NAMain\",\"modules\":[{\"_id\":\"70:ee:50:00:00:02\",\"module_name\":\"Mock Outdoor Module\",\"type\":\"NAModule1\"},{\"_id\":\"70:ee:50:00:00:03\",\"module_name\":\"Mock Indoor Module\",\"type\":\"NAModule4\"}]}]},\"status\":\"ok\"}";
    
    request->send_P(200, "application/json", MOCK_JSON);
  });
  
  // Add an endpoint to test connectivity to Netatmo API
  server.on("/api/netatmo/test-connection", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println(F("[NETATMO] Testing connection to Netatmo API"));
    
    // Create a simpler response with just DNS resolution test
    // This avoids potentially problematic network operations in the handler
    IPAddress ip;
    bool dnsResolved = WiFi.hostByName("api.netatmo.com", ip);
    
    String response = "{\"tests\":[{\"name\":\"DNS Resolution\",\"success\":";
    response += dnsResolved ? "true" : "false";
    response += ",\"details\":\"";
    response += dnsResolved ? ip.toString() : "Failed to resolve hostname";
    response += "\"}]}";
    
    request->send(200, "application/json", response);
  });
  
  // Add an endpoint to refresh stations data
  server.on("/api/netatmo/refresh-stations", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println(F("[NETATMO] Handling refresh stations request"));
    
    // Check if we have a valid access token
    if (strlen(netatmoAccessToken) == 0) {
      Serial.println(F("[NETATMO] Error - No access token"));
      request->send(401, "application/json", "{\"error\":\"Not authenticated with Netatmo\"}");
      return;
    }
    
    // Set a flag to fetch stations data in the main loop
    fetchStationsDataPending = true;
    
    // Send a response indicating that the refresh has been initiated
    request->send(200, "application/json", "{\"status\":\"initiated\",\"message\":\"Stations refresh initiated\"}");
  });
  
  // Add an endpoint to get stations data
  server.on("/api/netatmo/stations", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println(F("[NETATMO] Handling get stations request"));
    
    // Check if the file exists
    if (!LittleFS.exists("/netatmo_config.json")) {
      Serial.println(F("[NETATMO] Device data file not found"));
      request->send(404, "application/json", "{\"error\":\"Device data not found\"}");
      return;
    }
    
    // Open the file
    File deviceFile = LittleFS.open("/netatmo_config.json", "r");
    if (!deviceFile) {
      Serial.println(F("[NETATMO] Failed to open device data file"));
      request->send(500, "application/json", "{\"error\":\"Failed to open device data file\"}");
      return;
    }
    
    // Create a response stream
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    
    // Stream the file content to the response
    const size_t bufSize = 256;
    uint8_t buf[bufSize];
    
    while (deviceFile.available()) {
      size_t bytesRead = deviceFile.read(buf, bufSize);
      if (bytesRead > 0) {
        response->write(buf, bytesRead);
      }
      yield(); // Allow the watchdog to be fed
    }
    
    deviceFile.close();
    request->send(response);
  });
  
  Serial.println(F("[NETATMO] OAuth handler setup complete"));
}

// Function to be called from loop() to process pending token exchange
void processTokenExchange() {
  if (!tokenExchangePending || pendingCode.isEmpty()) {
    return;
  }
  
  Serial.println(F("[NETATMO] Processing token exchange"));
  
  // Clear the flag immediately to prevent repeated attempts if this fails
  tokenExchangePending = false;
  String code = pendingCode;
  pendingCode = "";
  
  // Set API call in progress flag to prevent web requests
  apiCallInProgress = true;
  
  // Skip memory checks and defragmentation to avoid yield issues
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[NETATMO] Error - WiFi not connected"));
    apiCallInProgress = false;
    return;
  }
  
  // Memory optimization: Use static client to reduce stack usage
  static BearSSL::WiFiClientSecure client;
  client.setInsecure(); // Skip certificate validation to save memory
  
  // Use moderate buffer sizes for balance between efficiency and memory usage
  client.setBufferSizes(512, 512); // Balanced buffer sizes
  
  HTTPClient https;
  https.setTimeout(10000); // 10 second timeout
  
  Serial.println(F("[NETATMO] Connecting to token endpoint"));
  if (!https.begin(client, "https://api.netatmo.com/oauth2/token")) {
    Serial.println(F("[NETATMO] Error - Failed to connect"));
    apiCallInProgress = false;
    return;
  }
  
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");
  
  // Use a pre-encoded redirect URI to save memory
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%s", WiFi.localIP().toString().c_str());
  
  // Build the POST data in chunks to minimize memory usage
  // Use static buffers to avoid heap fragmentation
  static char postData[512]; // Increased buffer size for POST data
  memset(postData, 0, sizeof(postData));
  
  // Build the POST data manually to minimize memory usage
  strcat(postData, "grant_type=authorization_code");
  strcat(postData, "&client_id=");
  strcat(postData, netatmoClientId);
  strcat(postData, "&client_secret=");
  strcat(postData, netatmoClientSecret);
  strcat(postData, "&code=");
  strncat(postData, code.c_str(), 100); // Limit code length
  strcat(postData, "&redirect_uri=http%3A%2F%2F");
  strcat(postData, ipStr);
  strcat(postData, "%2Fapi%2Fnetatmo%2Fcallback");
  
  Serial.println(F("[NETATMO] Sending token request"));
  int httpCode = https.POST(postData);
  
  if (httpCode != HTTP_CODE_OK) {
    Serial.print(F("[NETATMO] Error - HTTP code: "));
    Serial.println(httpCode);
    https.end();
    apiCallInProgress = false;
    return;
  }
  
  // Memory optimization: Process the response in smaller chunks
  // Get the response
  String response = https.getString();
  https.end();
  
  Serial.println(F("[NETATMO] Parsing response"));
  
  // Memory optimization: Use a smaller JSON buffer
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);
  
  if (error) {
    Serial.print(F("[NETATMO] JSON parse error: "));
    Serial.println(error.c_str());
    apiCallInProgress = false;
    return;
  }
  
  // Extract the tokens
  if (!doc["access_token"].is<JsonVariant>() || !doc["refresh_token"].is<JsonVariant>()) {
    Serial.println(F("[NETATMO] Missing tokens in response"));
    return;
  }
  
  // Memory optimization: Extract tokens directly as strings
  const char* accessToken = doc["access_token"];
  const char* refreshToken = doc["refresh_token"];
  
  // Save the tokens
  Serial.println(F("[NETATMO] Saving tokens"));
  strlcpy(netatmoAccessToken, accessToken, sizeof(netatmoAccessToken));
  strlcpy(netatmoRefreshToken, refreshToken, sizeof(netatmoRefreshToken));
  
  // Set flag to save tokens in main loop instead of saving directly
  saveCredentialsPending = true;
  
  Serial.println(F("[NETATMO] Token exchange complete"));
  
  // Reset API call in progress flag
  apiCallInProgress = false;
  
  // Perform heap defragmentation after API call
  Serial.println(F("[NETATMO] Performing post-API call heap defragmentation"));
  defragmentHeap();
  
  // Trigger a fetch of the stations data
  fetchStationsDataPending = true;
}

// Function to be called from loop() to fetch Netatmo devices
void processFetchDevices() {
  if (!fetchDevicesPending) {
    return;
  }
  
  Serial.println(F("[NETATMO] Fetching Netatmo devices"));
  
  Serial.println(F("[NETATMO] Memory stats before fetch devices:"));
  
  // Defragment heap if needed
  if (shouldDefragment()) {
    Serial.println(F("[NETATMO] Defragmenting heap before fetch devices"));
    defragmentHeap();
  }
  
  // Clear the flag immediately to prevent repeated attempts if this fails
  fetchDevicesPending = false;
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[NETATMO] Error - WiFi not connected"));
    return;
  }
  
  if (strlen(netatmoAccessToken) == 0) {
    Serial.println(F("[NETATMO] Error - No access token"));
    return;
  }
  
  // Create a new client for the API call
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  optimizeBearSSLClient(client.get()); // Use our optimization function
  
  HTTPClient https;
  optimizeHTTPClient(https); // Use our optimization function
  
  // Use getstationsdata endpoint which we know works
  String apiUrl = "https://api.netatmo.com/api/getstationsdata";
  Serial.print(F("[NETATMO] Fetching device list from: "));
  Serial.println(apiUrl);
  
  if (!https.begin(*client, apiUrl)) {
    Serial.println(F("[NETATMO] Error - Failed to connect"));
    return;
  }
  
  // Add authorization header
  String authHeader = "Bearer ";
  authHeader += netatmoAccessToken;
  https.addHeader("Authorization", authHeader);
  https.addHeader("Accept", "application/json");
  
  // Make the request with yield to avoid watchdog issues
  Serial.println(F("[NETATMO] Sending request..."));
  int httpCode = https.GET();
  yield(); // Allow the watchdog to be fed
  
  if (httpCode != HTTP_CODE_OK) {
    Serial.print(F("[NETATMO] Error - HTTP code: "));
    Serial.println(httpCode);
    
    // Get more detailed error information for connection issues
    if (httpCode == -1) {
      Serial.print(F("[NETATMO] Connection error: "));
      Serial.println(https.errorToString(httpCode));
      
      // Check if we can ping the API server
      Serial.println(F("[NETATMO] Attempting to ping api.netatmo.com..."));
      IPAddress ip;
      bool resolved = WiFi.hostByName("api.netatmo.com", ip);
      
      if (resolved) {
        Serial.print(F("[NETATMO] DNS resolution successful. IP: "));
        Serial.println(ip.toString());
      } else {
        Serial.println(F("[NETATMO] DNS resolution failed"));
      }
      
      https.end();
      return;
    }
    
    // Get error payload with yield - use streaming to avoid memory issues
    String errorPayload = "";
    if (https.getSize() > 0) {
      const size_t bufSize = 128;
      char buf[bufSize];
      WiFiClient* stream = https.getStreamPtr();
      
      // Read first chunk only for error message
      size_t len = stream->available();
      if (len > bufSize) len = bufSize;
      if (len > 0) {
        int c = stream->readBytes(buf, len);
        if (c > 0) {
          buf[c] = 0;
          errorPayload = String(buf);
        }
      }
    } else {
      errorPayload = https.getString();
    }
    
    yield(); // Allow the watchdog to be fed
    
    Serial.print(F("[NETATMO] Error payload: "));
    Serial.println(errorPayload);
    https.end();
    
    // If we get a 401 or 403, try to refresh the token
    if (httpCode == 401 || httpCode == 403) {
      Serial.println(F("[NETATMO] Token expired, trying to refresh"));
      
      // Try to refresh the token
      if (refreshNetatmoToken()) {
        Serial.println(F("[NETATMO] Token refreshed, retrying request"));
      } else {
        Serial.println(F("[NETATMO] Failed to refresh token"));
      }
    } else {
    }
    return;
  }
  
  // Instead of trying to parse the response in memory, let's save it to a file
  // and then parse it in smaller chunks
  
  // First, check if we have enough space
  FSInfo fs_info;
  LittleFS.info(fs_info);
  
  if (fs_info.totalBytes - fs_info.usedBytes < https.getSize() * 2) {
    Serial.println(F("[NETATMO] Not enough space to save device data"));
    https.end();
    apiCallInProgress = false;
    return;
  }
  
  // Create the devices directory if it doesn't exist
  if (!LittleFS.exists("/devices")) {
    LittleFS.mkdir("/devices");
  }
  
  // Remove existing file if it exists to ensure clean state
  if (LittleFS.exists("/netatmo_config.json")) {
    Serial.println(F("[NETATMO] Removing existing devices file"));
    LittleFS.remove("/netatmo_config.json");
    yield(); // Allow the watchdog to be fed
  }
  
  // Open a file to save the raw response
  File deviceFile = LittleFS.open("/netatmo_config.json", "w");
  if (!deviceFile) {
    Serial.println(F("[NETATMO] Failed to open file for writing"));
    https.end();
    apiCallInProgress = false;
    return;
  }
  
  // Record the start time for timing
  unsigned long startTime = millis();
  Serial.println(F("[TIMING] Starting file write at: ") + String(millis() - startTime) + F(" ms after request completion"));
  
  // Stream the response directly to the file
  WiFiClient* stream = https.getStreamPtr();
  int totalRead = 0;
  
  // Log the first few bytes of the stream for debugging
  if (stream->available() >= 10) {
    char firstBytes[11];
    int bytesRead = stream->readBytes(firstBytes, 10);
    firstBytes[bytesRead] = '\0';
    Serial.print(F("[JSON] First bytes of buffer: "));
    for (int i = 0; i < bytesRead; i++) {
      if (firstBytes[i] >= 32 && firstBytes[i] <= 126) {
        Serial.print((char)firstBytes[i]);
      } else {
        Serial.print("\\x");
        Serial.print(firstBytes[i], HEX);
      }
    }
    Serial.println();
    
    // Put the bytes back into the stream (not possible with ESP8266WiFiClient)
    // Instead, we'll write them to the file directly
    deviceFile.write((uint8_t*)firstBytes, bytesRead);
    totalRead += bytesRead;
  }
  
  // Check if the response is chunked
  bool isChunked = isChunkedResponse(https);
  Serial.print(F("[NETATMO] Is chunked response (final decision): "));
  Serial.println(isChunked ? "Yes" : "No");
  
  if (isChunked) {
    Serial.println(F("[NETATMO] Detected chunked transfer encoding"));
    
    // Use our specialized function to handle chunked encoding
    if (writeChunkedResponseToFile(stream, deviceFile)) {
      Serial.println(F("[NETATMO] Successfully processed chunked response"));
    } else {
      Serial.println(F("[NETATMO] Error processing chunked response"));
    }
  } else {
    // Handle non-chunked response (original code)
    const size_t bufSize = 512; // Moderate buffer size for balance between efficiency and memory usage
    uint8_t buf[bufSize];
    int expectedSize = https.getSize();
    
    Serial.print(F("[NETATMO] Expected response size: "));
  Serial.print(expectedSize);
  Serial.println(F(" bytes"));
  
  // Process in larger chunks with fewer yield calls
  while (https.connected() && (totalRead < expectedSize || expectedSize <= 0)) {
    // Read available data
    size_t available = stream->available();
    if (available) {
      // Read up to buffer size
      size_t readBytes = available > bufSize ? bufSize : available;
      int bytesRead = stream->readBytes(buf, readBytes);
      
      if (bytesRead > 0) {
        // Write to file
        deviceFile.write(buf, bytesRead);
        totalRead += bytesRead;
        
        // Print progress less frequently (every 4KB)
        if (totalRead % 4096 == 0) {
          Serial.print(F("[NETATMO] Read "));
          Serial.print(totalRead);
          Serial.println(F(" bytes"));
          yield(); // Only yield every 4KB to reduce context switches
        }
      }
      
      yield(); // Allow the watchdog to be fed
    } else if (totalRead >= expectedSize && expectedSize > 0) {
      // We've read all the data
      break;
    } else {
      // No data available, just feed the watchdog
      yield();
    }
  }
  }
  
  deviceFile.close();
  https.end();
  
  // Reset API call in progress flag
  apiCallInProgress = false;
  
  // Calculate and log the time taken
  unsigned long duration = millis() - startTime;
  Serial.print(F("[TIMING] File write time: "));
  Serial.print(duration);
  Serial.println(F(" ms"));
  Serial.print(F("[TIMING] Total time from request to file completion: "));
  Serial.print(duration);
  Serial.println(F(" ms"));
  
  Serial.print(F("[NETATMO] Stations data saved to file, bytes: "));
  Serial.println(totalRead);
  
  // Debug: Print the first 100 bytes of the file
  Serial.println(F("[DEBUG] First 100 bytes of saved file:"));
  File checkFile = LittleFS.open("/netatmo_config.json", "r");
  if (checkFile) {
    char debugBuf[101];
    int readSize = checkFile.readBytes(debugBuf, 100);
    debugBuf[readSize] = '\0';
    Serial.println(debugBuf);
    checkFile.close();
  }
  
  // Dump the entire file content for debugging
  Serial.println(F("[DEBUG] Dumping contents of file: /netatmo_config.json"));
  dumpFileContents("/netatmo_config.json");
  
  // Set a success response
  
  Serial.println(F("[NETATMO] Memory stats after fetch devices:"));
  
  // Perform heap defragmentation after API call
  Serial.println(F("[NETATMO] Performing post-API call heap defragmentation"));
  defragmentHeap();
}

// Function to be called from loop() to process pending credential saves
void processSaveCredentials() {
  if (!saveCredentialsPending) {
    return;
  }
  
  Serial.println(F("[NETATMO] Processing pending credential save"));
  
  // Debug: Print the current values before saving
  Serial.print(F("[NETATMO] Saving netatmoClientId: "));
  Serial.println(netatmoClientId);
  Serial.print(F("[NETATMO] Saving netatmoClientSecret length: "));
  Serial.println(strlen(netatmoClientSecret));
  Serial.print(F("[NETATMO] Saving netatmoAccessToken length: "));
  Serial.println(strlen(netatmoAccessToken));
  Serial.print(F("[NETATMO] Saving netatmoRefreshToken length: "));
  Serial.println(strlen(netatmoRefreshToken));
  
  // Clear the flag immediately to prevent repeated attempts if this fails
  saveCredentialsPending = false;
  
  // Save to config file
  saveTokensToConfig();
  
  Serial.println(F("[NETATMO] Credentials saved to config file"));
  
  // Debug: Verify the values were saved by reading the config file
  if (LittleFS.begin()) {
    File f = LittleFS.open("/config.json", "r");
    if (f) {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, f);
      f.close();
      
      if (!err) {
        Serial.print(F("[NETATMO] Verified netatmoClientId in config: "));
        if (doc["netatmoClientId"].is<JsonVariant>()) {
          Serial.println(doc["netatmoClientId"].as<String>());
        } else {
          Serial.println(F("(not found)"));
        }
        
        Serial.print(F("[NETATMO] Verified netatmoClientSecret in config: "));
        if (doc["netatmoClientSecret"].is<JsonVariant>()) {
          Serial.println(F("(present but not shown)"));
        } else {
          Serial.println(F("(not found)"));
        }
      }
    }
  }
}
// Function to fetch stations data directly
void fetchStationsData() {
  unsigned long startTime = millis();
  Serial.println(F("[NETATMO] Fetching stations data"));
  
  // Prepare memory for API call
  prepareForAPICall();
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[NETATMO] Error - WiFi not connected"));
    return;
  }
  
  if (strlen(netatmoAccessToken) == 0) {
    Serial.println(F("[NETATMO] Error - No access token"));
    return;
  }
  
  // Create a new client for the API call
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  optimizeBearSSLClient(client.get()); // Use our optimization function
  
  HTTPClient https;
  optimizeHTTPClient(https); // Use our optimization function
  
  // Use homesdata endpoint instead of getstationsdata
  String apiUrl = "https://api.netatmo.com/api/homesdata";
  Serial.print(F("[NETATMO] Fetching from: "));
  Serial.println(apiUrl);
  
  // Log the full request details
  Serial.println(F("[NETATMO] Request details:"));
  Serial.print(F("URL: "));
  Serial.println(apiUrl);
  Serial.print(F("Method: GET"));
  Serial.println();
  Serial.println(F("Headers:"));
  Serial.print(F("  Authorization: Bearer "));
  // Print first 5 chars and last 5 chars of token with ... in between
  if (strlen(netatmoAccessToken) > 10) {
    Serial.print(String(netatmoAccessToken).substring(0, 5));
    Serial.print(F("..."));
    Serial.println(String(netatmoAccessToken).substring(strlen(netatmoAccessToken) - 5));
  } else {
    Serial.print(netatmoAccessToken[0]);
    Serial.print(F("..."));
    Serial.println(netatmoAccessToken[strlen(netatmoAccessToken)-1]);
  }
  Serial.println(F("  Accept: application/json"));
  
  // Now try the HTTPS connection
  Serial.println(F("[NETATMO] Initializing HTTPS connection..."));
  if (!https.begin(*client, apiUrl)) {
    Serial.println(F("[NETATMO] Error - Failed to connect"));
    return;
  }
  
  // Add authorization header using our memory-efficient function
  char authHeader[256]; // Generous buffer size for the header
  createAuthHeader(authHeader, sizeof(authHeader), netatmoAccessToken);
  https.addHeader("Authorization", authHeader);
  https.addHeader("Accept", "application/json");
  
  unsigned long beforeRequestTime = millis();
  Serial.print(F("[TIMING] HTTP client setup time: "));
  Serial.print(beforeRequestTime - startTime); // Changed from afterDefragTime to startTime
  Serial.println(F(" ms"));
  
  // Make the request with yield to avoid watchdog issues
  Serial.println(F("[NETATMO] Sending request..."));
  
  // Defragment heap right before making the request
  defragmentHeap();
  
  unsigned long requestStartTime = millis();
  int httpCode = https.GET();
  yield(); // Allow the watchdog to be fed
  
  unsigned long requestEndTime = millis();
  Serial.print(F("[TIMING] HTTP request time: "));
  Serial.print(requestEndTime - requestStartTime);
  Serial.println(F(" ms"));
  
  
  Serial.print(F("[NETATMO] HTTP response code: "));
  Serial.println(httpCode);
  
  if (httpCode != HTTP_CODE_OK) {
    Serial.print(F("[NETATMO] Error - HTTP code: "));
    Serial.println(httpCode);
    
    // Get error payload with yield
    String errorPayload = https.getString();
    yield(); // Allow the watchdog to be fed
    
    Serial.print(F("[NETATMO] Error payload: "));
    Serial.println(errorPayload);
    https.end();
    
    // If we get a 401 or 403, try to refresh the token
    if (httpCode == 401 || httpCode == 403) {
      Serial.println(F("[NETATMO] Token expired, trying to refresh"));
      
      // Try to refresh the token
      if (refreshNetatmoToken()) {
        Serial.println(F("[NETATMO] Token refreshed, will retry on next loop"));
        fetchStationsDataPending = true; // Deferred retry instead of recursive call
      } else {
        Serial.println(F("[NETATMO] Failed to refresh token"));
      }
    }
    
    // Try the getstationsdata endpoint as a fallback
    else if (httpCode == -1 || httpCode == 404) {
      Serial.println(F("[NETATMO] Trying getstationsdata endpoint as fallback"));
      fetchStationsDataFallback();
    }
    
    return;
  }
  
  // Log response headers in detail
  Serial.println(F("[NETATMO] Response headers:"));
  for (int i = 0; i < https.headers(); i++) {
    Serial.print(F("  "));
    Serial.print(https.headerName(i));
    Serial.print(F(": "));
    Serial.println(https.header(i));
  }
  
  // Log content length and type
  Serial.print(F("[NETATMO] Content-Length: "));
  Serial.println(https.getSize());
  Serial.print(F("[NETATMO] Content-Type: "));
  Serial.println(https.header("Content-Type"));
  
  // Create the devices directory if it doesn't exist
  if (!LittleFS.exists("/devices")) {
    LittleFS.mkdir("/devices");
  }
  
  // Open a file to save the raw response
  File deviceFile = LittleFS.open("/netatmo_config.json", "w");
  if (!deviceFile) {
    Serial.println(F("[NETATMO] Failed to open file for writing"));
    https.end();
    return;
  }
  
  // Stream the response directly to the file
  WiFiClient* stream = https.getStreamPtr();
  const size_t bufSize = 256;
  uint8_t buf[bufSize];
  int totalRead = 0;
  int expectedSize = https.getSize();
  
  Serial.print(F("[NETATMO] Expected response size: "));
  Serial.print(expectedSize);
  Serial.println(F(" bytes"));
  
  // Store start time for accurate file write timing
  unsigned long fileWriteStartTime = millis();
  Serial.print(F("[TIMING] Starting file write at: "));
  Serial.print(fileWriteStartTime - requestEndTime);
  Serial.println(F(" ms after request completion"));  
  
  // For logging the first part of the response - use fixed buffer instead of String
  char responsePreview[512]; // Fixed-size buffer for preview
  int previewLength = 0;
  bool previewCaptured = false;
  
  // For tracking progress
  unsigned long lastProgressTime = millis();
  
  // Variables for timeout detection
  unsigned long lastDataTime = millis();
  const unsigned long dataTimeout = 5000; // 5 second timeout if no new data
  bool receivedData = false;
  
  while (https.connected() && (totalRead < expectedSize || expectedSize <= 0)) {
    // Read available data
    // Log progress every 5 seconds
    unsigned long currentTime = millis();
    if (currentTime - lastProgressTime > 5000) {
      Serial.print(F("[TIMING] Still reading stream after "));
      Serial.print(currentTime - fileWriteStartTime);
      Serial.println(F(" ms"));
      Serial.print(F("[TIMING] Bytes read so far: "));
      Serial.println(totalRead);
      lastProgressTime = currentTime;
    }
    
    // Check for timeout - exit if no data received for dataTimeout milliseconds
    if (receivedData && (currentTime - lastDataTime > dataTimeout)) {
      Serial.print(F("[NETATMO] Data timeout after "));
      Serial.print(currentTime - lastDataTime);
      Serial.println(F(" ms with no new data"));
      Serial.println(F("[NETATMO] Assuming transfer complete"));
      break; // Exit the loop if no data received for timeout period
    }
    
    size_t available = stream->available();
    if (available) {
      // Reset timeout since we're receiving data
      lastDataTime = millis();
      receivedData = true;
      
      // Read up to buffer size
      size_t readBytes = available > bufSize ? bufSize : available;
      int bytesRead = stream->readBytes(buf, readBytes);
      
      if (bytesRead > 0) {
        // Write to file and check if chunked transfer is complete
        bool transferComplete = writeCleanJsonFromBuffer(buf, bytesRead, deviceFile);
        totalRead += bytesRead;
        
        // If chunked transfer is complete, break out of the loop
        if (transferComplete) {
          Serial.println(F("[NETATMO] Chunked transfer complete, exiting read loop"));
          break;
        }
        
        // Capture the first part of the response for logging using fixed buffer
        if (!previewCaptured && previewLength < sizeof(responsePreview) - 1) {
          // Copy bytes to the preview buffer, ensuring we don't overflow
          int bytesToCopy = min((int)bytesRead, (int)(sizeof(responsePreview) - 1 - previewLength));
          if (bytesToCopy > 0) {
            memcpy(responsePreview + previewLength, buf, bytesToCopy);
            previewLength += bytesToCopy;
            responsePreview[previewLength] = '\0'; // Ensure null termination
          }
          
          if (previewLength >= sizeof(responsePreview) - 1) {
            previewCaptured = true;
          }
        }
        
        // Print progress every 1KB
        if (totalRead % 1024 == 0) {
          Serial.print(F("[NETATMO] Read "));
          Serial.print(totalRead);
          Serial.println(F(" bytes"));
        }
      }
      
      yield(); // Allow the watchdog to be fed
    } else if (totalRead >= expectedSize && expectedSize > 0) {
      // We've read all the data
      break;
      // We've read all the data
      Serial.println(F("[NETATMO] Expected size reached, exiting read loop"));
    } else {
      // No data available, just feed the watchdog
      yield();
    }
  }
  
  // Close the file and end the HTTP client as early as possible
  deviceFile.close();
  https.end();
  
  // Force garbage collection to free memory
  forceGarbageCollection();
  
  unsigned long afterFileWriteTime = millis();
  Serial.print(F("[TIMING] File write time: "));
  Serial.print(afterFileWriteTime - fileWriteStartTime);
  Serial.println(F(" ms"));
  
  Serial.print(F("[TIMING] Total time from request to file completion: "));
  Serial.print(afterFileWriteTime - requestEndTime);
  Serial.println(F(" ms"));
  
  Serial.print(F("[NETATMO] Stations data saved to file, bytes: "));
  Serial.println(totalRead);
  
  // Read and log the first 100 bytes of the saved file
  File checkFile = LittleFS.open("/netatmo_config.json", "r");
  if (checkFile) {
    Serial.println(F("[DEBUG] First 100 bytes of saved file:"));
    char checkBuf[101];
    size_t bytesRead = checkFile.readBytes(checkBuf, 100);
    checkBuf[bytesRead] = '\0';
    Serial.println(checkBuf);
    checkFile.close();
  }
  
  // Skip file dumping in production to save memory
#ifdef DEBUG_FILE_DUMP
  // Dump the entire file content to logs for debugging
  dumpFileContents("/netatmo_config.json");
#else
  Serial.println(F("[DEBUG] File dump disabled to save memory"));
#endif
  
  // Log the first part of the response
  Serial.println(F("[NETATMO] Response preview:"));
  Serial.println(responsePreview);
  
  
  // Defragment heap after API call and before extracting device info
  defragmentHeap();
  
  unsigned long beforeExtractTime = millis();
  Serial.print(F("[TIMING] Before extracting device info: "));
  Serial.print(beforeExtractTime - afterFileWriteTime);
  Serial.println(F(" ms"));
  
  // Clean up memory after API call
  cleanupAfterAPICall();
  
  // Now extract the device and module IDs for easy access
  extractDeviceInfo();
  
  // Final memory cleanup
  releaseUnusedMemory();
}
// Function to extract device info from the saved JSON file
void extractDeviceInfo() {
  unsigned long startExtractTime = millis();
  Serial.println(F("[NETATMO] Extracting device info"));
  
  if (!LittleFS.exists("/netatmo_config.json")) {
    Serial.println(F("[NETATMO] Error - Device data file not found"));
    return;
  }
  
  File deviceFile = LittleFS.open("/netatmo_config.json", "r");
  if (!deviceFile) {
    Serial.println(F("[NETATMO] Error - Failed to open device data file"));
    return;
  }
  
  unsigned long fileOpenTime = millis();
  Serial.print(F("[TIMING] File open time: "));
  Serial.print(fileOpenTime - startExtractTime);
  Serial.println(F(" ms"));
  
  // Use a filter to extract only the data we need
  JsonDocument filter;
  
  // Set up the filter to only extract the module IDs and types
  filter["body"]["homes"][0]["modules"][0]["id"] = true;
  filter["body"]["homes"][0]["modules"][0]["type"] = true;
  filter["body"]["homes"][0]["modules"][0]["name"] = true;
  filter["body"]["homes"][0]["modules"][0]["bridge"] = true;
  
  // Use a smaller JSON document with the filter
  JsonDocument doc; // Even smaller with filter
  DeserializationError error = deserializeJson(doc, deviceFile, DeserializationOption::Filter(filter));
  deviceFile.close();
  
  // Force garbage collection after JSON parsing
  forceGarbageCollection();
  
  if (error) {
    Serial.print(F("[NETATMO] Error parsing device data: "));
    Serial.println(error.c_str());
    return;
  }
  
  // Check if we have a homesdata response
  if (doc["body"].is<JsonVariant>() && doc["body"]["homes"].is<JsonVariant>()) {
    Serial.println(F("[NETATMO] Processing homesdata response"));
    
    JsonArray homes = doc["body"]["homes"];
    if (homes.size() > 0) {
      JsonObject home = homes[0];
      
      // Use direct char* access instead of String conversion
      const char* homeName = home["name"] | "Unknown";
      Serial.print(F("[NETATMO] Found home: "));
      Serial.println(homeName);
      
      // Look for modules of type NAMain (weather stations)
      JsonArray modules = home["modules"];
      
      for (JsonObject module : modules) {
        const char* moduleType = module["type"] | "";
        const char* moduleId = module["id"] | "";
        const char* moduleName = module["name"] | "";
        
        Serial.print(F("[NETATMO] Found module: "));
        Serial.print(moduleId);
        Serial.print(F(" ("));
        Serial.print(moduleName);
        Serial.print(F("), type: "));
        Serial.println(moduleType);
        
        // Save main station ID
        if (moduleType && strcmp(moduleType, "NAMain") == 0 && strlen(netatmoDeviceId) == 0) {
          strlcpy(netatmoDeviceId, moduleId, sizeof(netatmoDeviceId));
          Serial.print(F("[NETATMO] Set main station ID: "));
          Serial.println(netatmoDeviceId);
          
          // Find modules connected to this station
          for (JsonObject subModule : modules) {
            // Check if this module is connected to the main station
            if (subModule["bridge"].is<JsonVariant>() && strcmp(subModule["bridge"], moduleId) == 0) {
              const char* subModuleType = subModule["type"];
              const char* subModuleId = subModule["id"];
              const char* subModuleName = subModule["name"];
              
              Serial.print(F("[NETATMO] Found connected module: "));
              Serial.print(subModuleId);
              Serial.print(F(" ("));
              Serial.print(subModuleName);
              Serial.print(F("), type: "));
              Serial.println(subModuleType);
              
              // Save outdoor module ID
              if (strcmp(subModuleType, "NAModule1") == 0 && strlen(netatmoModuleId) == 0) {
                strlcpy(netatmoModuleId, subModuleId, sizeof(netatmoModuleId));
                Serial.print(F("[NETATMO] Set outdoor module ID: "));
                Serial.println(netatmoModuleId);
              }
              
              // Save indoor module ID
              if (strcmp(subModuleType, "NAModule4") == 0 && strlen(netatmoIndoorModuleId) == 0) {
                strlcpy(netatmoIndoorModuleId, subModuleId, sizeof(netatmoIndoorModuleId));
                Serial.print(F("[NETATMO] Set indoor module ID: "));
                Serial.println(netatmoIndoorModuleId);
              }
            }
          }
        }
      }
    }
  } 
  // Check if we have a getstationsdata response
  else if (doc["body"].is<JsonVariant>() && doc["body"]["devices"].is<JsonVariant>()) {
    Serial.println(F("[NETATMO] Processing getstationsdata response"));
    
    JsonArray devices = doc["body"]["devices"];
    if (devices.size() > 0) {
      JsonObject device = devices[0];
      const char* deviceId = device["_id"];
      const char* stationName = device["station_name"];
      
      Serial.print(F("[NETATMO] Found device: "));
      Serial.print(deviceId);
      Serial.print(F(" ("));
      Serial.print(stationName);
      Serial.println(F(")"));
      
      // Save device ID if not already set
      if (strlen(netatmoDeviceId) == 0) {
        strlcpy(netatmoDeviceId, deviceId, sizeof(netatmoDeviceId));
        Serial.print(F("[NETATMO] Set device ID: "));
        Serial.println(netatmoDeviceId);
      }
      
      // Extract module info
      JsonArray modules = device["modules"];
      for (JsonObject module : modules) {
        const char* moduleId = module["_id"];
        const char* moduleName = module["module_name"];
        const char* moduleType = module["type"];
        
        Serial.print(F("[NETATMO] Found module: "));
        Serial.print(moduleId);
        Serial.print(F(" ("));
        Serial.print(moduleName);
        Serial.print(F("), type: "));
        Serial.println(moduleType);
        
        // Save outdoor module ID if not already set
        if (strcmp(moduleType, "NAModule1") == 0 && strlen(netatmoModuleId) == 0) {
          strlcpy(netatmoModuleId, moduleId, sizeof(netatmoModuleId));
          Serial.print(F("[NETATMO] Set outdoor module ID: "));
          Serial.println(netatmoModuleId);
        }
        
        // Save indoor module ID if not already set
        if (strcmp(moduleType, "NAModule4") == 0 && strlen(netatmoIndoorModuleId) == 0) {
          strlcpy(netatmoIndoorModuleId, moduleId, sizeof(netatmoIndoorModuleId));
          Serial.print(F("[NETATMO] Set indoor module ID: "));
          Serial.println(netatmoIndoorModuleId);
        }
      }
    }
  } else {
    Serial.println(F("[NETATMO] Unknown response format"));
  }
  
  // Save the updated device and module IDs to config
  saveTokensToConfig();
  
  unsigned long endExtractTime = millis();
  Serial.print(F("[TIMING] Device info extraction time: "));
  Serial.print(endExtractTime - startExtractTime);
  Serial.println(F(" ms"));
}
// Function to be called from loop() to process pending stations data fetch
void processFetchStationsData() {
  if (!fetchStationsDataPending) {
    return;
  }
  
  // Clear the flag immediately to prevent repeated attempts if this fails
  fetchStationsDataPending = false;
  
  // Call the fetch stations data function
  fetchStationsData();
}
// Function to fetch stations data using getstationsdata endpoint as fallback
void fetchStationsDataFallback() {
  Serial.println(F("[NETATMO] Fetching stations data using fallback endpoint"));
  
  
  // Defragment heap before making the API call
  defragmentHeap();
  
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[NETATMO] Error - WiFi not connected"));
    return;
  }
  
  if (strlen(netatmoAccessToken) == 0) {
    Serial.println(F("[NETATMO] Error - No access token"));
    return;
  }
  
  // Create a new client for the API call
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  optimizeBearSSLClient(client.get()); // Use our optimization function
  
  HTTPClient https;
  optimizeHTTPClient(https); // Use our optimization function
  
  // Use getstationsdata endpoint as fallback
  String apiUrl = "https://api.netatmo.com/api/getstationsdata";
  Serial.print(F("[NETATMO] Fetching from fallback endpoint: "));
  Serial.println(apiUrl);
  
  // Log the full request details
  Serial.println(F("[NETATMO] Fallback request details:"));
  Serial.print(F("URL: "));
  Serial.println(apiUrl);
  Serial.print(F("Method: GET"));
  Serial.println();
  Serial.println(F("Headers:"));
  Serial.print(F("  Authorization: Bearer "));
  // Print first 5 chars and last 5 chars of token with ... in between
  if (strlen(netatmoAccessToken) > 10) {
    Serial.print(String(netatmoAccessToken).substring(0, 5));
    Serial.print(F("..."));
    Serial.println(String(netatmoAccessToken).substring(strlen(netatmoAccessToken) - 5));
  } else {
    Serial.print(netatmoAccessToken[0]);
    Serial.print(F("..."));
    Serial.println(netatmoAccessToken[strlen(netatmoAccessToken)-1]);
  }
  Serial.println(F("  Accept: application/json"));
  
  // Now try the HTTPS connection
  Serial.println(F("[NETATMO] Initializing HTTPS connection..."));
  if (!https.begin(*client, apiUrl)) {
    Serial.println(F("[NETATMO] Error - Failed to connect"));
    return;
  }
  
  // Add authorization header
  String authHeader = "Bearer ";
  authHeader += netatmoAccessToken;
  https.addHeader("Authorization", authHeader);
  https.addHeader("Accept", "application/json");
  
  // Make the request with yield to avoid watchdog issues
  Serial.println(F("[NETATMO] Sending fallback request..."));
  int httpCode = https.GET();
  yield(); // Allow the watchdog to be fed
  
  Serial.print(F("[NETATMO] HTTP response code: "));
  Serial.println(httpCode);
  
  if (httpCode != HTTP_CODE_OK) {
    Serial.print(F("[NETATMO] Error - HTTP code: "));
    Serial.println(httpCode);
    
    // Get error payload with yield
    String errorPayload = https.getString();
    yield(); // Allow the watchdog to be fed
    
    Serial.print(F("[NETATMO] Error payload: "));
    Serial.println(errorPayload);
    https.end();
    return;
  }
  
  // Log response headers in detail
  Serial.println(F("[NETATMO] Response headers:"));
  for (int i = 0; i < https.headers(); i++) {
    Serial.print(F("  "));
    Serial.print(https.headerName(i));
    Serial.print(F(": "));
    Serial.println(https.header(i));
  }
  
  // Log content length and type
  Serial.print(F("[NETATMO] Content-Length: "));
  Serial.println(https.getSize());
  Serial.print(F("[NETATMO] Content-Type: "));
  Serial.println(https.header("Content-Type"));
  
  // Create the devices directory if it doesn't exist
  if (!LittleFS.exists("/devices")) {
    LittleFS.mkdir("/devices");
  }
  
  // Open a file to save the raw response
  File deviceFile = LittleFS.open("/netatmo_config.json", "w");
  if (!deviceFile) {
    Serial.println(F("[NETATMO] Failed to open file for writing"));
    https.end();
    return;
  }
  
  // Stream the response directly to the file
  WiFiClient* stream = https.getStreamPtr();
  const size_t bufSize = 256;
  uint8_t buf[bufSize];
  int totalRead = 0;
  int expectedSize = https.getSize();
  
  Serial.print(F("[NETATMO] Expected response size: "));
  Serial.print(expectedSize);
  Serial.println(F(" bytes"));
  
  // For logging the first part of the response - use fixed buffer
  char responsePreview[512]; // Fixed-size buffer for preview
  int previewLength = 0;
  bool previewCaptured = false;
  
  while (https.connected() && (totalRead < expectedSize || expectedSize <= 0)) {
    // Read available data
    size_t available = stream->available();
    if (available) {
      // Read up to buffer size
      size_t readBytes = available > bufSize ? bufSize : available;
      int bytesRead = stream->readBytes(buf, readBytes);
      
      if (bytesRead > 0) {
        // Write to file
        deviceFile.write(buf, bytesRead);
        totalRead += bytesRead;
        
        // Capture the first part of the response for logging using fixed buffer
        if (!previewCaptured && previewLength < sizeof(responsePreview) - 1) {
          // Copy bytes to the preview buffer, ensuring we don't overflow
          int bytesToCopy = min((int)bytesRead, (int)(sizeof(responsePreview) - 1 - previewLength));
          if (bytesToCopy > 0) {
            memcpy(responsePreview + previewLength, buf, bytesToCopy);
            previewLength += bytesToCopy;
            responsePreview[previewLength] = '\0'; // Ensure null termination
          }
          
          if (previewLength >= sizeof(responsePreview) - 1) {
            previewCaptured = true;
          }
        }
        
        // Print progress every 1KB
        if (totalRead % 1024 == 0) {
          Serial.print(F("[NETATMO] Read "));
          Serial.print(totalRead);
          Serial.println(F(" bytes"));
        }
      }
      
      yield(); // Allow the watchdog to be fed
    } else if (totalRead >= expectedSize && expectedSize > 0) {
      // We've read all the data
      break;
    } else {
      // No data available, wait a bit
      delay(10);
      yield();
    }
  }
  
  deviceFile.close();
  https.end();
  
  Serial.print(F("[NETATMO] Fallback stations data saved to file, bytes: "));
  Serial.println(totalRead);
  
  // Log the first part of the response
  Serial.println(F("[NETATMO] Fallback response preview:"));
  Serial.println(responsePreview);
  
  
  // Defragment heap after API call and before extracting device info
  defragmentHeap();
  
  
  // Now extract the device and module IDs for easy access
  extractDeviceInfo();
}
// External declarations


// A simple, minimal implementation to call the Netatmo API





// Function to check if the error payload indicates an invalid token
bool isInvalidTokenError(const String &errorPayload) {
  // Check if the error payload contains "Invalid access token"
  if (errorPayload.indexOf("Invalid access token") >= 0) {
    Serial.println(F("[NETATMO] Error: Invalid access token detected in response"));
    return true;
  }
  return false;
}
