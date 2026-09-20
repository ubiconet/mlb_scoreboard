#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <qrcode.h>

#include <Adafruit_ST7789.h>

#include "config.h"
#include "network.h"

extern Adafruit_ST7789 display;

namespace {
WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

// PROVISIONING: no network, portal + setup screen locked.
// CONNECTING:    associating / probing for internet (portal stays available).
// ONLINE:        internet confirmed; AP torn down, setup screen releases.
enum NetworkState { PROVISIONING, CONNECTING, ONLINE };
NetworkState state = PROVISIONING;

const char DEFAULT_CONFIG[] = R"json({"version":1,"teams":[]})json";

bool dnsRunning = false;
bool apUp = false;
bool otaStarted = false;
String deviceHostname = NETWORK_HOSTNAME;

// Credentials in NVS (last known good) and the pair being tried from the portal.
String savedSsid;
String savedPassword;
String pendingSsid;
String pendingPassword;
bool hasPending = false;
bool clockDisplayEnabled = true;

uint32_t stateStartedAt = 0;
uint32_t lastProbeAt = 0;
uint32_t disconnectStartedAt = 0;
uint32_t onlineAt = 0;
uint32_t lastReconnectAt = 0;
uint32_t lastDebugAt = 0;
wl_status_t lastLoggedWiFiStatus = WL_NO_SHIELD;
bool setupScreenVisible = false;
// Portal priority mode: while someone is actively using the setup pages,
// background work pauses so the web server gets the core and the radio to
// itself (page loads over this device's marginal Wi-Fi stall otherwise).
// Every request refreshes the window.
uint32_t portalActiveUntil = 0;
void markPortalActivity() {
  portalActiveUntil = millis() + PORTAL_ACTIVITY_WINDOW_MS;
}

// Only delays the very first boot's CONNECTING->ONLINE transition (see runNetworkStateMachine),
// so the Wi-Fi setup/connecting screen isn't just a flash; later reconnects skip this.
uint32_t networkTaskStartedAt = 0;
bool firstBootConnectHeld = true;

// Cross-task handoff to the main loop, which owns the display. The network
// task only sets these; handleNetworkDisplay() reads and clears them.
enum SetupDisplayMode { SETUP_CONNECTING, SETUP_AP_INSTRUCTIONS, SETUP_ONLINE_PORTAL };
volatile bool redrawSetupPending = false;
volatile SetupDisplayMode redrawModeV = SETUP_CONNECTING;
volatile uint32_t setupIpV = 0;
volatile bool releasePending = false;

// Async scan cache, only touched from the network task (server handlers run there).
String scanOptionsHtml;
bool scanActive = false;
uint32_t lastScanAt = 0;

void requestSetupRedraw(SetupDisplayMode mode, IPAddress ip) {
  redrawModeV = mode;
  setupIpV = ip;
  redrawSetupPending = true;
}

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_CONNECTED: return "CONNECTED";
    case WL_NO_SSID_AVAIL: return "NO_SSID_AVAIL";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    case WL_IDLE_STATUS: return "IDLE";
    default: return "OTHER";
  }
}

void logWiFiStatus(bool force = false) {
  wl_status_t status = WiFi.status();
  if (!force && status == lastLoggedWiFiStatus && millis() - lastDebugAt < NETWORK_DEBUG_INTERVAL_MS) {
    return;
  }
  lastLoggedWiFiStatus = status;
  lastDebugAt = millis();
  Serial.printf("[NET %lu] state=%s wifi=%s rssi=%d ip=%s ap=%s\n",
                millis(),
                state == ONLINE ? "ONLINE" : (state == CONNECTING ? "CONNECTING" : "PROVISIONING"),
                wifiStatusName(status),
                WiFi.RSSI(),
                WiFi.localIP().toString().c_str(),
                WiFi.softAPIP().toString().c_str());
}

void startPortalInfrastructure() {
  if (!dnsRunning) {
    dnsServer.start(53, "*", WiFi.softAPIP());
    dnsRunning = true;
  }
}

void stopPortalInfrastructure() {
  if (dnsRunning) {
    dnsServer.stop();
    dnsRunning = false;
  }
  if (apUp) {
    WiFi.softAPdisconnect(true);
    apUp = false;
  }
}

bool probeInternet() {
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(3000);
  http.begin(NETWORK_PROBE_ANCHOR_URL);
  int code = http.GET();
  http.end();
  return code > 0;
}

void startArduinoOTA() {
  if (otaStarted) {
    return;
  }
  ArduinoOTA.setHostname(deviceHostname.c_str());
  ArduinoOTA.setPassword(NETWORK_OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] Network update starting");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("[OTA] Network update complete");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] Progress: %u%%\r", (progress * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]\n", error);
  });
  ArduinoOTA.begin();
  otaStarted = true;
  Serial.println("[OTA] Arduino network OTA ready");
}

void enterOnline() {
  state = ONLINE;
  onlineAt = millis();
  if (hasPending) {
    preferences.begin("network", false);
    preferences.putString("ssid", pendingSsid);
    preferences.putString("password", pendingPassword);
    preferences.end();
    savedSsid = pendingSsid;
    savedPassword = pendingPassword;
    hasPending = false;
  }
  stopPortalInfrastructure();
  startArduinoOTA();
  requestSetupRedraw(SETUP_ONLINE_PORTAL, WiFi.localIP());
  Serial.printf("[NET] Online: ip=%s rssi=%d gateway=%s\n",
                WiFi.localIP().toString().c_str(),
                WiFi.RSSI(),
                WiFi.gatewayIP().toString().c_str());
  logWiFiStatus(true);
}

void enterProvisioning() {
  state = PROVISIONING;
  stateStartedAt = millis();
  WiFi.disconnect();
  if (!apUp) {
    WiFi.softAP(NETWORK_AP_SSID, NETWORK_AP_PASSWORD);
    apUp = true;
    startPortalInfrastructure();
  }
  requestSetupRedraw(SETUP_AP_INSTRUCTIONS, WiFi.softAPIP());
  Serial.println("[NET] Provisioning: waiting for portal credentials");
  logWiFiStatus(true);
  Serial.printf("[NET] AP ready: ssid=%s ip=%s\n",
                NETWORK_AP_SSID,
                WiFi.softAPIP().toString().c_str());
}

void enterConnecting() {
  state = CONNECTING;
  stateStartedAt = millis();
  lastProbeAt = 0;
  lastReconnectAt = millis();
  Serial.printf("[NET] Connecting to SSID '%s'\n", savedSsid.c_str());
  requestSetupRedraw(SETUP_CONNECTING, WiFi.softAPIP());
}

void tryReconnectWithSavedNetwork() {
  if (savedSsid.isEmpty()) {
    enterProvisioning();
    return;
  }
  // Keep the modem awake. Default Wi-Fi power save (min-modem sleep) makes
  // the ESP32 miss beacons on a marginal link and disassociate in storms a
  // minute or so after connect — exactly the drop/reconnect cycles that
  // killed the schedule/news fetches. The scoreboard is mains-powered, so
  // the extra ~40 mA is irrelevant.
  WiFi.setSleep(false);
  WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
  enterConnecting();
}

int prefTeam1 = 143; // Default PHI
int prefTeam2 = 144; // Default ATL
int prefTeam3 = 111; // Default BOS

struct MlbTeamOption {
  int id;
  const char* name;
};

const MlbTeamOption MLB_TEAMS[] = {
  {0, "-- None --"},
  {108, "Los Angeles Angels (LAA)"},
  {109, "Arizona Diamondbacks (ARI)"},
  {110, "Baltimore Orioles (BAL)"},
  {111, "Boston Red Sox (BOS)"},
  {112, "Chicago Cubs (CHC)"},
  {113, "Cincinnati Reds (CIN)"},
  {114, "Cleveland Guardians (CLE)"},
  {115, "Colorado Rockies (COL)"},
  {116, "Detroit Tigers (DET)"},
  {117, "Houston Astros (HOU)"},
  {118, "Kansas City Royals (KC)"},
  {119, "Los Angeles Dodgers (LAD)"},
  {120, "Washington Nationals (WSH)"},
  {121, "New York Mets (NYM)"},
  {133, "Oakland Athletics (ATH)"},
  {134, "Pittsburgh Pirates (PIT)"},
  {135, "San Diego Padres (SD)"},
  {136, "Seattle Mariners (SEA)"},
  {137, "San Francisco Giants (SF)"},
  {138, "St. Louis Cardinals (STL)"},
  {139, "Tampa Bay Rays (TB)"},
  {140, "Texas Rangers (TEX)"},
  {141, "Toronto Blue Jays (TOR)"},
  {142, "Minnesota Twins (MIN)"},
  {143, "Philadelphia Phillies (PHI)"},
  {144, "Atlanta Braves (ATL)"},
  {145, "Chicago White Sox (CWS)"},
  {146, "Miami Marlins (MIA)"},
  {147, "New York Yankees (NYY)"},
  {158, "Milwaukee Brewers (MIL)"}
};
const size_t MLB_TEAMS_COUNT = sizeof(MLB_TEAMS) / sizeof(MLB_TEAMS[0]);

void loadSavedNetwork() {
  preferences.begin("network", true);
  savedSsid = preferences.getString("ssid", "");
  savedPassword = preferences.getString("password", "");
  prefTeam1 = preferences.getInt("team1", 143);
  prefTeam2 = preferences.getInt("team2", 144);
  prefTeam3 = preferences.getInt("team3", 111);
  clockDisplayEnabled = preferences.getBool("show_clock", true);
  preferences.end();
}

String buildTeamOptionsHtml(int selectedId) {
  String html = "";
  for (size_t i = 0; i < MLB_TEAMS_COUNT; i++) {
    html += "<option value=\"";
    html += String(MLB_TEAMS[i].id);
    html += "\"";
    if (MLB_TEAMS[i].id == selectedId) html += " selected";
    html += ">";
    html += MLB_TEAMS[i].name;
    html += "</option>";
  }
  return html;
}


String htmlEscape(const String& text) {
  String out;
  out.reserve(text.length());
  for (size_t i = 0; i < text.length(); ++i) {
    char c = text[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

void refreshScanCache() {
  int8_t result = WiFi.scanComplete();
  if (scanActive && result >= 0) {
    scanOptionsHtml = "";
    for (int index = 0; index < result; ++index) {
      String ssid = WiFi.SSID(index);
      if (!ssid.isEmpty()) {
        String escaped = htmlEscape(ssid);
        scanOptionsHtml += "<option value=\"" + escaped + "\">" + escaped + " (" +
                           String(WiFi.RSSI(index)) + " dBm)</option>";
      }
    }
    WiFi.scanDelete();
    scanActive = false;
    lastScanAt = millis();
  } else if (result == WIFI_SCAN_FAILED) {
    scanActive = false;
    lastScanAt = millis();
  }
  if (!scanActive && state != ONLINE && millis() - lastScanAt > NETWORK_SCAN_REFRESH_MS) {
    WiFi.scanNetworks(true, true);
    scanActive = true;
  }
}

void renderConnectingScreen() {
  display.fillScreen(0x012B);
  display.setTextColor(ST77XX_WHITE);
  display.setTextSize(2);
  display.setCursor(8, 8);
  display.print("SETUP WI-FI");
  display.setTextSize(1);
  display.setCursor(8, 100);
  display.print("Connecting to network...");
  if (!savedSsid.isEmpty()) {
    display.setTextColor(0xFD20); // Gold
    display.setCursor(8, 118);
    display.print(savedSsid);
  }
}

void renderAccessPointInstructions(const String& portalAddress, bool stationConnected) {
  String portalUrl = "http://" + portalAddress + "/";

  display.fillScreen(0x012B);
  display.setTextColor(ST77XX_WHITE);
  display.setTextSize(2);
  display.setCursor(8, 8);
  // Wi-Fi is already established once online; only the AP-mode screen is initial setup.
  display.print(stationConnected ? "SCOREBOARD SETUP" : "SETUP WI-FI");
  display.setTextSize(1);
  display.setCursor(8, 38);
  if (stationConnected) {
    display.print("1. Wi-Fi Connected!");
    display.setCursor(8, 55);
    display.print("Device IP Address:");
    display.setTextSize(2);
    display.setTextColor(0xFD20); // Gold
    display.setCursor(8, 72);
    display.print(portalAddress);

    display.setTextSize(1);
    display.setTextColor(ST77XX_WHITE);
    display.setCursor(8, 105);
    display.print("2. Scan QR or open URL");
    display.setCursor(8, 120);
    display.print("to adjust team settings.");
  } else {
    display.print("1. Connect to:");
    display.setTextSize(2);
    display.setCursor(8, 51);
    display.print(NETWORK_AP_SSID);
    display.setTextSize(1);
    display.setCursor(8, 78);
    display.print("Password:");
    display.setTextSize(2);
    display.setCursor(8, 91);
    display.print(NETWORK_AP_PASSWORD);

    display.setTextSize(1);
    display.setCursor(8, 125);
    display.print("2. Scan QR or open:");
    display.setCursor(8, 139);
    display.print(portalAddress);
    display.setCursor(8, 165);
    display.print("3. Enter Wi-Fi settings");
  }

  // QR always links to this screen's portal address (AP portal or device LAN IP).
  uint8_t qrData[qrcode_getBufferSize(2)];
  QRCode qrCode;
  qrcode_initText(&qrCode, qrData, 2, ECC_LOW, portalUrl.c_str());

  const int scale = 4;
  const int left = 208;
  const int top = 65;
  display.fillRect(left - 4, top - 4, (qrCode.size * scale) + 8, (qrCode.size * scale) + 8, ST77XX_WHITE);
  for (uint8_t y = 0; y < qrCode.size; y++) {
    for (uint8_t x = 0; x < qrCode.size; x++) {
      if (qrcode_getModule(&qrCode, x, y)) {
        display.fillRect(left + (x * scale), top + (y * scale), scale, scale, ST77XX_BLACK);
      }
    }
  }
}

String readConfig() {
  File file = LittleFS.open("/config.json", "r");
  if (!file) {
    return DEFAULT_CONFIG;
  }
  String config = file.readString();
  file.close();
  return config;
}

void redirectToPortal() {
  markPortalActivity();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void servePortal() {
  markPortalActivity();
  server.sendHeader("Cache-Control", "max-age=300");
  String page = R"html(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MLB Scoreboard Setup</title><style>
body{margin:0;background:#061b46;color:#fff;font:16px system-ui,sans-serif}
main{max-width:440px;margin:5vh auto;padding:24px;background:#0b2b62;border:2px solid #dfe9ff;border-radius:8px}
h1{margin-top:0;font-size:24px}label{display:block;margin:14px 0 4px;font-weight:600}input,select{box-sizing:border-box;width:100%;padding:10px;border:0;border-radius:4px;font-size:15px}
button{margin-top:20px;width:100%;padding:12px;background:#f5c400;border:0;border-radius:4px;font-weight:700;font-size:16px;color:#000;cursor:pointer}.hint{color:#c5d3ee;font-size:14px;line-height:1.4}
hr{border:0;border-top:1px solid #1c4587;margin:20px 0}
</style></head><body><main><h1>MLB Scoreboard Setup</h1>
<p class="hint">Configure Wi-Fi connection and select your favorite teams in order of priority.</p>
<form method="post" action="/save">
<label for="network">Nearby Wi-Fi Networks</label>
<select id="network" onchange="ssid.value=this.value"><option value="">Enter network manually</option>)html";
  page += scanOptionsHtml;
  page += R"html(</select>
<label for="ssid">Wi-Fi Network Name</label><input id="ssid" name="ssid" value=")html";
  page += htmlEscape(savedSsid);
  page += R"html(" required maxlength="32" autocomplete="off">
<label for="password">Wi-Fi Password</label><input id="password" name="password" type="password" maxlength="63" autocomplete="off">
<p class="hint">Leave the password blank and the network unchanged to save team preferences without touching the connection.</p>
<hr>
<h3>Favorite Team Priorities</h3>
<label for="team1">Priority 1 Team (Primary)</label><select id="team1" name="team1">)html";
  page += buildTeamOptionsHtml(prefTeam1);
  page += R"html(</select>
<label for="team2">Priority 2 Team</label><select id="team2" name="team2">)html";
  page += buildTeamOptionsHtml(prefTeam2);
  page += R"html(</select>
<label for="team3">Priority 3 Team</label><select id="team3" name="team3">)html";
  page += buildTeamOptionsHtml(prefTeam3);
  page += R"html(</select>
<hr><label style="display:flex;align-items:center;gap:10px" for="show-clock"><input style="width:auto" id="show-clock" name="show_clock" type="checkbox" value="1")html";
  if (clockDisplayEnabled) page += " checked";
  page += R"html(>Display current time on score boards when no game is live</label>
<button type="submit">Save & Connect Scoreboard</button></form>
<hr><p class="hint"><a style="color:#f5c400" href="/update">Upload new firmware (.bin)</a></p></main></body></html>)html";
  server.send(200, "text/html", page);
}

void serveStatus() {
  markPortalActivity();
  const char* name = state == ONLINE ? "online" : (state == CONNECTING ? "connecting" : "provisioning");
  server.send(200, "application/json", String("{\"state\":\"") + name + "\"}");
}

void saveNetwork() {
  markPortalActivity();
  pendingSsid = server.arg("ssid");
  pendingPassword = server.arg("password");
  if (server.hasArg("team1")) prefTeam1 = server.arg("team1").toInt();
  if (server.hasArg("team2")) prefTeam2 = server.arg("team2").toInt();
  if (server.hasArg("team3")) prefTeam3 = server.arg("team3").toInt();
  clockDisplayEnabled = server.hasArg("show_clock");

  // Network settings only count as changed when a new SSID is supplied or a
  // password is (re-)entered; otherwise this is a team-preferences-only save
  // and the Wi-Fi connection must be left alone.
  bool networkChanging = !pendingSsid.isEmpty() &&
                         (pendingSsid != savedSsid || !pendingPassword.isEmpty());

  if (savedSsid.isEmpty() && !networkChanging) {
    server.send(400, "text/plain", "Wi-Fi network name is required");
    return;
  }

  // Team preferences always persist; credentials only when they changed.
  preferences.begin("network", false);
  preferences.putInt("team1", prefTeam1);
  preferences.putInt("team2", prefTeam2);
  preferences.putInt("team3", prefTeam3);
  preferences.putBool("show_clock", clockDisplayEnabled);
  if (networkChanging) {
    preferences.putString("ssid", pendingSsid);
    preferences.putString("password", pendingPassword);
  }
  preferences.end();

  if (!networkChanging) {
    Serial.printf("[NET] Teams-only save: [%d, %d, %d] (network untouched)\n",
                  prefTeam1, prefTeam2, prefTeam3);
    server.send(200, "text/html", R"html(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Saved</title><style>body{margin:0;background:#061b46;color:#fff;font:16px system-ui,sans-serif}
main{max-width:420px;margin:8vh auto;padding:28px;background:#0b2b62;border:2px solid #dfe9ff;border-radius:8px;text-align:center}
</style></head><body><main><h1>Teams Saved</h1>
<p>Team priorities updated — the scoreboard picks them up within a minute.</p>
<p>Wi-Fi settings were not changed.</p>
<p><a style="color:#f5c400" href="/">Back to configuration</a></p></main></body></html>)html");
    return;
  }

  savedSsid = pendingSsid;
  savedPassword = pendingPassword;
  hasPending = true;

  WiFi.disconnect();
  WiFi.begin(pendingSsid.c_str(), pendingPassword.c_str());
  enterConnecting();

  server.send(200, "text/html", R"html(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Connecting</title><style>body{margin:0;background:#061b46;color:#fff;font:16px system-ui,sans-serif}
main{max-width:420px;margin:8vh auto;padding:28px;background:#0b2b62;border:2px solid #dfe9ff;border-radius:8px;text-align:center}
#status{font-size:20px;font-weight:700}</style></head><body><main><h1>Connecting...</h1>
<p id="status">Trying the network and checking internet access.</p>
<p><a style="color:#f5c400" href="/">Back to configuration</a></p>
<script>setInterval(function(){fetch('/status').then(function(r){return r.json()}).then(function(s){
if(s.state==='online'){document.getElementById('status').textContent='Connected! The scoreboard is going online.'}
else if(s.state==='provisioning'){document.getElementById('status').textContent='Could not connect or no internet. Check the password and try again.'}
})},2000)</script></main></body></html>)html");
  Serial.printf("Saved network '%s' and Teams [%d, %d, %d]\n", pendingSsid.c_str(), prefTeam1, prefTeam2, prefTeam3);
}


void saveConfig() {
  markPortalActivity();
  if (server.arg("portal") != NETWORK_PORTAL_PASSWORD) {
    server.send(401, "text/plain", "Invalid portal password");
    return;
  }
  JsonDocument document;
  if (deserializeJson(document, server.arg("config"))) {
    server.send(400, "text/plain", "Invalid JSON configuration");
    return;
  }
  File file = LittleFS.open("/config.tmp", "w");
  if (!file) {
    server.send(500, "text/plain", "Unable to write configuration");
    return;
  }
  serializeJson(document, file);
  file.close();
  LittleFS.remove("/config.json");
  LittleFS.rename("/config.tmp", "/config.json");
  server.send(200, "text/html", "<h1>Saved</h1><p>Runtime settings updated.</p>");
}

void serveConfig() {
  markPortalActivity();
  server.send(200, "application/json", readConfig());
}

void serveUpdatePage() {
  markPortalActivity();
  server.send(200, "text/html", R"html(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Firmware Update</title><style>
body{margin:0;background:#061b46;color:#fff;font:16px system-ui,sans-serif}
main{max-width:440px;margin:8vh auto;padding:24px;background:#0b2b62;border:2px solid #dfe9ff;border-radius:8px}
h1{margin-top:0;font-size:24px}input{box-sizing:border-box;width:100%;padding:10px;border:0;border-radius:4px;font-size:15px;background:#fff}
button{margin-top:16px;width:100%;padding:12px;background:#f5c400;border:0;border-radius:4px;font-weight:700;font-size:16px;color:#000;cursor:pointer}
.hint{color:#c5d3ee;font-size:14px;line-height:1.4}
</style></head><body><main><h1>Firmware Update</h1>
<p class="hint">Upload a new compiled .bin firmware image. The scoreboard will reboot automatically once the update finishes.</p>
<form method="POST" action="/update" enctype="multipart/form-data">
<input type="file" name="update" accept=".bin" required>
<button type="submit">Upload & Flash</button>
</form></main></body></html>)html");
}

void handleUpdateUpload() {
  markPortalActivity();
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[UPDATE] Receiving firmware: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("[UPDATE] Success: %u bytes. Rebooting...\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end();
  }
}

void handleUpdateResult() {
  markPortalActivity();
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", Update.hasError() ? "Update FAILED" : "Update OK, rebooting...");
  delay(500);
  ESP.restart();
}

void registerPortalRoutes() {
  server.on("/", HTTP_GET, servePortal);
  server.on("/status", HTTP_GET, serveStatus);
  server.on("/config", HTTP_GET, serveConfig);
  server.on("/save", HTTP_POST, saveNetwork);
  server.on("/config", HTTP_POST, saveConfig);
  server.on("/update", HTTP_GET, serveUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateResult, handleUpdateUpload);
  server.onNotFound(redirectToPortal);
}

void runNetworkStateMachine() {
  logWiFiStatus();
  if (state == CONNECTING) {
    if (WiFi.status() != WL_CONNECTED && millis() - lastReconnectAt >= NETWORK_RECONNECT_RETRY_MS) {
      lastReconnectAt = millis();
      Serial.println("[NET] Retrying saved Wi-Fi connection");
      WiFi.reconnect();
    }
    bool holdForFirstBoot = firstBootConnectHeld &&
                            millis() - networkTaskStartedAt < NETWORK_FIRST_CONNECT_MIN_MS;
    if (WiFi.status() == WL_CONNECTED && !holdForFirstBoot) {
      firstBootConnectHeld = false;
      enterOnline();
    } else if (millis() - stateStartedAt >= NETWORK_CONNECT_AND_PROBE_TIMEOUT_MS) {
      enterProvisioning();
    }
  } else if (state == ONLINE) {
    if (WiFi.status() == WL_CONNECTED) {
      disconnectStartedAt = 0;
    } else {
      if (disconnectStartedAt == 0) {
        disconnectStartedAt = millis();
      } else if (millis() - disconnectStartedAt >= NETWORK_RECONNECT_GRACE_MS) {
        Serial.println("[NET] Connection lost; retrying saved network");
        setupScreenVisible = true;
        tryReconnectWithSavedNetwork();
      }
    }
    if (setupScreenVisible && millis() - onlineAt >= NETWORK_SETUP_SCREEN_MS) {
      setupScreenVisible = false;
      releasePending = true;
    }
  } else if (state == PROVISIONING) {
    if (!savedSsid.isEmpty() && millis() - stateStartedAt >= NETWORK_PROVISIONING_RETRY_MS) {
      Serial.println("[NET] Retrying saved Wi-Fi after provisioning timeout");
      tryReconnectWithSavedNetwork();
    }
  }
}
} // namespace

bool isOnline() {
  return state == ONLINE;
}

bool isProvisioning() {
  return state == PROVISIONING;
}

bool portalEngaged() {
  return millis() < portalActiveUntil;
}

const char* getSavedWifiSsid() {
  return savedSsid.c_str();
}

String getDeviceIp() {
  return (state == ONLINE) ? WiFi.localIP().toString() : String("");
}

bool isClockDisplayEnabled() {
  return clockDisplayEnabled;
}

void getPreferredTeamIds(int outTeamIds[3]) {
  outTeamIds[0] = prefTeam1;
  outTeamIds[1] = prefTeam2;
  outTeamIds[2] = prefTeam3;
}


void startNetworkServices() {
  networkTaskStartedAt = millis();
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS unavailable; runtime settings disabled");
  }
  registerPortalRoutes();
  WiFi.mode(WIFI_AP_STA);
  // Keep the modem awake from the very first connection (previously only
  // reconnects did this): default power save makes the radio sleep between
  // beacons, which stalls inbound portal page loads from phones.
  WiFi.setSleep(false);
  String macAddress = WiFi.macAddress();
  macAddress.replace(":", "");
  if (macAddress.length() >= 5) {
    deviceHostname = "scoreboard-" + macAddress.substring(macAddress.length() - 5);
    deviceHostname.toLowerCase();
  }
  WiFi.setHostname(deviceHostname.c_str());
  Serial.printf("[NET] Device hostname: %s\n", deviceHostname.c_str());
  // The web server starts now and serves over whichever interface is up,
  // but the setup AP only comes up in enterProvisioning() — i.e. when
  // there's genuinely no saved network or connecting failed. Starting it
  // eagerly at every boot broadcast MLB_SCOREBOARD for the whole connect
  // window, letting phones that remember it auto-join and lose their
  // route the moment the device went online (setup page then "hung").
  server.begin();
  setupScreenVisible = true;

  loadSavedNetwork();
  if (savedSsid.isEmpty()) {
    enterProvisioning();
  } else {
    WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
    enterConnecting();
  }
}

void handleNetworkServices() {
  if (dnsRunning) {
    dnsServer.processNextRequest();
  }
  server.handleClient();
  if (otaStarted) {
    ArduinoOTA.handle();
  }
  refreshScanCache();
  runNetworkStateMachine();
}

void handleNetworkDisplay() {
  if (redrawSetupPending) {
    redrawSetupPending = false;
    IPAddress ip(setupIpV);
    switch (redrawModeV) {
      case SETUP_CONNECTING:
      case SETUP_ONLINE_PORTAL:
        // Connecting and online transitions draw nothing: the boot logo
        // stays on screen while Wi-Fi connects behind it (loop() also
        // holds the logo for BOOT_SPLASH_HOLD_MS). Only the AP
        // provisioning instructions ever replace the logo — they're the
        // one screen the device can't work without.
        break;
      case SETUP_AP_INSTRUCTIONS:
        renderAccessPointInstructions(ip.toString(), false);
        break;
    }
  }
}

bool consumeScoreboardRelease() {
  if (!releasePending) {
    return false;
  }
  releasePending = false;
  return true;
}

void startNetworkTask() {
  xTaskCreatePinnedToCore(
    [](void*) {
      while (true) {
        handleNetworkServices();
        vTaskDelay(pdMS_TO_TICKS(2));
      }
    },
    "NetworkTask",
    8192,
    nullptr,
    2,  // above the MLB data task: the portal must preempt feed fetches
    nullptr,
    0
  );
}
