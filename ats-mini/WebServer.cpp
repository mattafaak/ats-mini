#include "Battery.h"
#include "Common.h"
#include "WiFiManager.h"
#include "WebServer.h"
#include "Storage.h"
#include "Themes.h"
#include "Utils.h"
#include "Menu.h"
#include "Draw.h"
#include "Station.h"
#include "Tuning.h"
#include "AudioManager.h"
#include "Scan.h"

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>

// Settings
static String loginUsername = "";
static String loginPassword = "";
static bool wifiScanHidden = false;

// AsyncWebServer object on port 80
static AsyncWebServer server(80);

//
// Helper: get wifiScanHidden state for external use
//
String getWiFiScanHidden(void) {
  return wifiScanHidden ? " CHECKED " : "";
}

static void webSetConfig(AsyncWebServerRequest *request);
static const String webInputField(const String &name, const String &value, bool pass = false);
static const String webStyleSheet();
static const String webPage(const String &body, int refreshSec = 0);
static const String webUtcOffsetSelector();
static const String webThemeSelector();
static const String webRadioPage();
static const String webMemoryPage();
static const String webControlsPage();
static const String webScanPage();

// Convert RGB565 (uint16_t) to 24-bit RGB for CSS
static uint32_t rgb565to888(uint16_t c) {
  uint32_t r = ((c >> 11) & 0x1F);
  uint32_t g = ((c >> 5) & 0x3F);
  uint32_t b = (c & 0x1F);
  r = (r << 3) | (r >> 2);
  g = (g << 2) | (g >> 4);
  b = (b << 3) | (b >> 2);
  return (r << 16) | (g << 8) | b;
}

// Helper: append a hex color field to JSON string
static void jsonColor(String &json, const char *key, uint16_t color) {
  char buf[32];
  snprintf(buf, sizeof(buf), "\"%s\":\"#%06X\",", key, rgb565to888(color));
  json += buf;
}

//
// Initialize internal web server
//
void webInit()
{
  // Read web UI credentials from preferences
  prefs.begin("network", true, STORAGE_PARTITION);
  loginUsername = prefs.getString("loginusername", "");
  loginPassword = prefs.getString("loginpassword", "");
  wifiScanHidden = prefs.getBool("wifiscanhidden", false);
  prefs.end();

  // Initialize mDNS
  MDNS.begin("atsmini"); // Set the hostname to "atsmini.local"
  MDNS.addService("http", "tcp", 80);

  server.on("/", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    request->send(200, "text/html", webRadioPage());
  });

  server.on("/memory", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    request->send(200, "text/html", webMemoryPage());
  });

  server.on("/controls", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    request->send(200, "text/html", webControlsPage());
  });

  server.on("/scan", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    request->send(200, "text/html", webScanPage());
  });

  server.on("/config", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    if(loginUsername != "" && loginPassword != "")
      if(!request->authenticate(loginUsername.c_str(), loginPassword.c_str()))
        return request->requestAuthentication();
    request->send(200, "text/html", webConfigPage());
  });

  server.onNotFound([] (AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  // This method saves configuration form contents
  server.on("/setconfig", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    if(loginUsername != "" && loginPassword != "")
      if(!request->authenticate(loginUsername.c_str(), loginPassword.c_str()))
        return request->requestAuthentication();
    webSetConfig(request);
  });

  // JSON status API
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    int cal = (radioState.mode == USB) ? getCurrentBand()->usbCal :
              (radioState.mode == LSB) ? getCurrentBand()->lsbCal : 0;
    int8_t ws = getWiFiStatus();
    String wifiStatusStr = (ws == 2) ? "station" :
                           (ws == 1) ? "ap" :
                           (ws == 0) ? "disabled" : "disconnected";
    String json = "{";
    json += "\"firmware\":" + String(VER_APP) + ",";
    json += "\"frequency_khz\":" + String(getEffectiveFreq()) + ",";
    json += "\"bfo\":" + String(radioState.bfo) + ",";
    json += "\"cal\":" + String(cal) + ",";
    json += "\"band\":\"" + String(getCurrentBand()->bandName) + "\",";
    json += "\"mode\":\"" + String(bandModeDesc[radioState.mode]) + "\",";
    json += "\"step\":\"" + String(getCurrentStep()->desc) + "\",";
    json += "\"bandwidth\":\"" + String(getCurrentBandwidth()->desc) + "\",";
    json += "\"volume\":" + String(radioState.vol) + ",";
    json += "\"band_min\":" + String(getCurrentBand()->minimumFreq) + ",";
    json += "\"band_max\":" + String(getCurrentBand()->maximumFreq) + ",";
    json += "\"agc_index\":" + String(radioState.agcIndex) + ",";
    json += "\"avc_index\":" + String(isSSB() ? radioState.ssbAvcIdx : radioState.amAvcIdx) + ",";
    json += "\"rssi_dBuv\":" + String(radioState.rssi) + ",";
    json += "\"snr_dB\":" + String(radioState.snr) + ",";
    json += "\"battery_V\":" + String(batteryMonitor()) + ",";
    json += String("\"muted\":") + (audioIsMuted() ? "true" : "false") + ",";
    json += String("\"squelch_enabled\":") + (audioIsSquelched() ? "true" : "false") + ",";
    json += String("\"sleep\":") + (sleepOn() ? "true" : "false") + ",";
    json += "\"wifi_mode\":\"" + wifiStatusStr + "\",";
    json += "\"wifi_rssi\":" + String(ws == 2 ? WiFi.RSSI() : 0) + ",";
    json += "\"ip_address\":\"" + String(getWiFiIPAddress()) + "\",";
    json += "\"station_name\":\"" + String(getStationName()) + "\",";
    json += "\"program_info\":\"" + String(getProgramInfo()) + "\",";
    json += "\"rssi\":" + String(radioState.rssi) + ",";
    json += "\"snr\":" + String(radioState.snr) + ",";
    json += String("\"main_muted\":") + (audioIsMainMuted() ? "true" : "false") + ",";
    json += String("\"squelched\":") + (audioIsSquelched() ? "true" : "false") + ",";
    json += "\"squelch\":" + String(radioState.squelch[radioState.mode] & 0x7F) + ",";
    json += String("\"squelch_is_snr\":") + ((radioState.squelch[radioState.mode] & 0x80) ? "true" : "false") + ",";
    json += "\"theme_idx\":" + String(themeIdx);
    json += "}";
    request->send(200, "application/json", json);
  });

  // Remote control API
  server.on("/api/command", HTTP_POST, [](AsyncWebServerRequest *request) {
    if(loginUsername != "" && loginPassword != "")
      if(!request->authenticate(loginUsername.c_str(), loginPassword.c_str()))
        return request->requestAuthentication();
    String cmd = request->arg("cmd");
    String value = request->arg("value");

    if (cmd == "tune") {
      long int freqKhz = value.toInt();
      if (freqKhz <= 0) {
        request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Bad frequency\"}");
        return;
      }
      long int freqHz = freqKhz * 1000;
      Band *band = getCurrentBand();
      uint16_t targetFreq = freqFromHz(freqHz, radioState.mode);
      int targetBfo = isSSB() ? bfoFromHz(freqHz) : 0;
      if (!isFreqInBand(band, targetFreq)) {
        request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Out of band\"}");
        return;
      }
      if (!updateFrequency(targetFreq, false)) {
        request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Out of band\"}");
        return;
      }
      if (isSSB())
        updateBFO(targetBfo, false);
      else if (radioState.bfo)
        updateBFO(0, true);
      clearStationInfo();
      identifyFrequency(getEffectiveFreq());
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    if (cmd == "volume") {
      int v = value.toInt();
      if (v < 0) v = 0;
      if (v > 63) v = 63;
      radioState.vol = v;
      rx.setVolume(radioState.vol);
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    if (cmd == "mute") {
      audioMuteMain(value == "true" || value == "1");
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    if (cmd == "band") {
      if (value == "next") doBand(1);
      else if (value == "prev") doBand(-1);
      else {
        request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Use next/prev\"}");
        return;
      }
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    if (cmd == "sleep") {
      sleepOn(value == "on" || value == "true" || value == "1");
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    // --- seek ---
    if (cmd == "seek") {
      if (value == "up") doSeek(1, 1);
      else if (value == "down") doSeek(-1, -1);
      else { request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Use up/down\"}"); return; }
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    // --- mode: 0=FM, 1=AM, 2=LSB, 3=USB ---
    if (cmd == "mode") {
      int idx = value.toInt();
      if (idx < 0 || idx > 3) { request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Bad mode\"}"); return; }
      if (idx == 0) {
        doBand(0 - bandIdx);  // FM is only on the FM band
      } else if (radioState.mode == FM) {
        // Switching from FM to AM/LSB/USB: first leave FM band
        doBand(1 - bandIdx);  // ALL band supports all modes
        doMode(idx - radioState.mode);
      } else {
        doMode(idx - radioState.mode);
      }
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    // --- bandwidth: index delta from current ---
    if (cmd == "bandwidth") {
      int idx = value.toInt();
      doBandwidth(idx - getCurrentBand()->bandwidthIdx);
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    // --- step: index delta from current ---
    if (cmd == "step") {
      int idx = value.toInt();
      doStep(idx - getCurrentBand()->currentStepIdx);
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    // --- squelch: direct value 0-127, preserves bit 7 (RSSI/SNR toggle) ---
    if (cmd == "squelch") {
      int v = constrain(value.toInt(), 0, 127);
      uint8_t s = radioState.squelch[radioState.mode];
      radioState.squelch[radioState.mode] = (s & 0x80) | v;
      audioSquelchClose(v > 0 && v >= radioState.rssi);
      prefsRequestSave(SAVE_SETTINGS);
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    // --- squelch_param: toggle RSSI/SNR bit ---
    if (cmd == "squelch_param") {
      uint8_t s = radioState.squelch[radioState.mode];
      if (value == "snr") radioState.squelch[radioState.mode] = s | 0x80;
      else radioState.squelch[radioState.mode] = s & ~0x80;
      prefsRequestSave(SAVE_SETTINGS);
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    // --- agc: direct value 0-37 ---
    if (cmd == "agc") {
      int v = constrain(value.toInt(), 0, 37);
      doAgc(v - radioState.agcIndex);
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    // --- avc: direct value 0-90 ---
    if (cmd == "avc") {
      int v = constrain(value.toInt(), 0, 90);
      int cur = (radioState.mode == USB || radioState.mode == LSB) ? radioState.ssbAvcIdx : radioState.amAvcIdx;
      doAvc(v - cur);
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    // --- cal: +/-1 ---
    if (cmd == "cal") {
      int dir = value.toInt();
      if (dir != 1 && dir != -1) { request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Use 1 or -1\"}"); return; }
      doCal(dir);
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    // --- brightness: direct value 10-255 ---
    if (cmd == "brightness") {
      int v = constrain(value.toInt(), 10, 255);
      doBrt(v - radioState.brightness);
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    // --- softmute: direct value 0-32 ---
    if (cmd == "softmute") {
      int v = constrain(value.toInt(), 0, 32);
      doSoftMute(v - radioState.softMuteMaxAtt);
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    // --- fm_region: index ---
    if (cmd == "fm_region") {
      int idx = constrain(value.toInt(), 0, getTotalFmRegions() - 1);
      doFmRegion(idx - radioState.fmRegionIdx);
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    // --- rds: mode bitmask ---
    if (cmd == "rds") {
      radioState.rdsMode = constrain(value.toInt(), 0, 63);
      prefsRequestSave(SAVE_SETTINGS);
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    // --- zoom: toggle ---
    if (cmd == "zoom") {
      radioState.zoomLevel = (value == "on" || value == "true" || value == "1") ? 1 : 0;
      prefsRequestSave(SAVE_SETTINGS);
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    // --- scroll: normal/reverse ---
    if (cmd == "scroll") {
      radioState.scrollDir = (value == "reverse") ? -1 : 1;
      prefsRequestSave(SAVE_SETTINGS);
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Unknown cmd\"}");
  });

  // Theme API — GET returns current theme JSON, POST changes theme by idx
  server.on("/api/theme", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{";
    json += "\"idx\":" + String(themeIdx) + ",";
    json += "\"name\":\"" + String(TH.name) + "\",";
    json += "\"themeCount\":" + String(getTotalThemes()) + ",";
    json += "\"colors\":{";
    jsonColor(json, "bg", TH.bg);
    jsonColor(json, "fg", TH.text);
    jsonColor(json, "menu_bg", TH.menu_bg);
    jsonColor(json, "menu_border", TH.menu_border);
    jsonColor(json, "menu_item", TH.menu_item);
    jsonColor(json, "menu_hdr", TH.menu_hdr);
    jsonColor(json, "menu_hl_bg", TH.menu_hl_bg);
    jsonColor(json, "menu_hl_text", TH.menu_hl_text);
    jsonColor(json, "param", TH.menu_param);
    jsonColor(json, "box_bg", TH.box_bg);
    jsonColor(json, "box_border", TH.box_border);
    jsonColor(json, "box_text", TH.box_text);
    jsonColor(json, "box_off_bg", TH.box_off_bg);
    jsonColor(json, "box_off_text", TH.box_off_text);
    jsonColor(json, "scan_rssi", TH.scan_rssi);
    jsonColor(json, "scan_snr", TH.scan_snr);
    jsonColor(json, "s_meter", TH.smeter_bar);
    json.remove(json.length() - 1); // trailing comma
    json += "}}";
    request->send(200, "application/json", json);
  });

  server.on("/api/theme", HTTP_POST, [](AsyncWebServerRequest *request) {
    if(loginUsername != "" && loginPassword != "")
      if(!request->authenticate(loginUsername.c_str(), loginPassword.c_str()))
        return request->requestAuthentication();
    if(request->hasParam("idx", true)) {
      themeIdx = constrain(request->getParam("idx", true)->value().toInt(), 0, getTotalThemes() - 1);
      prefsRequestSave(SAVE_SETTINGS, true);
    }
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  // Scan-to-memory API
  server.on("/api/scan", HTTP_POST, [](AsyncWebServerRequest *request) {
    String cmd = request->arg("cmd");

    // Status poll — non-blocking, returns JSON
    if (cmd == "status" || cmd == "poll") {
      const ScanStatus *s = scanGetStatus();
      String json = "{";
      json += "\"running\":" + String(s->running) + ",";
      json += "\"mode\":" + String(s->mode) + ",";
      json += "\"current_freq\":" + String(s->currentFreq) + ",";
      json += "\"current_rssi\":" + String(s->currentRSSI) + ",";
      json += "\"current_snr\":" + String(s->currentSNR) + ",";
      json += "\"progress_pct\":" + String(s->progress) + ",";
      json += "\"bookmark_count\":" + String(s->bookmarkCount) + ",\"bookmarks\":[";
      for (int i = 0; i < s->bookmarkCount; i++) {
        if (i) json += ",";
        json += String(s->bookmarks[i]);
      }
      json += "],\"result_count\":" + String(s->resultCount) + ",\"results\":[";
      for (int i = 0; i < s->resultCount; i++) {
        if (i) json += ",";
        json += "{\"slot\":" + String(s->results[i].slot) +
                ",\"freq\":" + String(s->results[i].freq) +
                ",\"rssi\":" + String(s->results[i].rssi) +
                ",\"name\":\"" + String(s->results[i].name) + "\"}";
      }
      json += "]}";
      request->send(200, "application/json", json);
      return;
    }

    // Start scan
    if (cmd == "scan" || cmd == "start") {
      if (scanIsRunning()) {
        request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Scan already running\"}");
        return;
      }
      String mode = request->arg("mode");
      if (mode == "auto") {
        int n = constrain(request->arg("n").toInt(), 1, 20);
        scanToMemoryAuto(n);
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else if (mode == "manual") {
        scanToMemoryManual();
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else {
        request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Use mode=auto or mode=manual\"}");
      }
      return;
    }

    // Manual scan: step one frequency, bookmark, stop
    if (cmd == "step") { scanManualStep(); request->send(200, "application/json", "{\"status\":\"ok\"}"); return; }
    if (cmd == "bookmark") { scanBookmark(); request->send(200, "application/json", "{\"status\":\"ok\"}"); return; }
    if (cmd == "stop") { scanStop(); request->send(200, "application/json", "{\"status\":\"ok\"}"); return; }
    if (cmd == "abort") { scanAbort(); request->send(200, "application/json", "{\"status\":\"ok\"}"); return; }

    request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Unknown scan cmd\"}");
  });

  // Start web server
  server.begin();
}

void webSetConfig(AsyncWebServerRequest *request)
{
  uint32_t prefsSave = 0;

  // Start modifying preferences
  prefs.begin("network", false, STORAGE_PARTITION);

  // Save user name and password
  if(request->hasParam("username", true) && request->hasParam("password", true))
  {
    loginUsername = request->getParam("username", true)->value();
    loginPassword = request->getParam("password", true)->value();

    prefs.putString("loginusername", loginUsername);
    prefs.putString("loginpassword", loginPassword);
  }

  // Save SSIDs and their passwords
  bool haveSSID = false;
  for(int j=0 ; j<3 ; j++)
  {
    char nameSSID[16], namePASS[16];

    sprintf(nameSSID, "wifissid%d", j+1);
    sprintf(namePASS, "wifipass%d", j+1);

    if(request->hasParam(nameSSID, true) && request->hasParam(namePASS, true))
    {
      String ssid = request->getParam(nameSSID, true)->value();
      String pass = request->getParam(namePASS, true)->value();
      prefs.putString(nameSSID, ssid);
      prefs.putString(namePASS, pass);
      haveSSID |= ssid != "" && pass != "";
    }
  }

  // Save hidden SSID scanning preference
  wifiScanHidden = request->hasParam("wifiscanhidden", true);
  prefs.putBool("wifiscanhidden", wifiScanHidden);

  // Save time zone
  if(request->hasParam("utcoffset", true))
  {
    String utcOffset = request->getParam("utcoffset", true)->value();
    radioState.utcOffset = constrain(utcOffset.toInt(), -12, 14);
    clockRefreshTime();
    prefsSave |= SAVE_SETTINGS;
  }

  // Save theme
  if(request->hasParam("theme", true))
  {
    String theme = request->getParam("theme", true)->value();
    themeIdx = constrain(theme.toInt(), 0, getTotalThemes() - 1);
    prefsSave |= SAVE_SETTINGS;
  }

  // Save scroll direction and menu zoom
  radioState.scrollDir = request->hasParam("scroll", true)? -1 : 1;
  radioState.zoomLevel        = request->hasParam("zoom", true);
  prefsSave |= SAVE_SETTINGS;

  // Done with the preferences
  prefs.end();

  // Save preferences immediately
  prefsRequestSave(prefsSave, true);

  // Show config page again
  request->redirect("/config");

  // If we are currently in AP mode, and infrastructure mode requested,
  // and there is at least one SSID / PASS pair, request network connection
  if(haveSSID && (radioState.wifiMode>NET_AP_ONLY) && (getWiFiStatus() != 2))
    netRequestConnect();
}

static const String webInputField(const String &name, const String &value, bool pass)
{
  String newValue(value);

  newValue.replace("\"", "&quot;");
  newValue.replace("'", "&apos;");

  return(
    "<INPUT TYPE='" + String(pass? "PASSWORD":"TEXT") + "' NAME='" +
    name + "' VALUE='" + newValue + "'>"
  );
}

static const String webStyleSheet() {
  return
"BODY{margin:0;padding:0;background:var(--bg);color:var(--fg)}"
"H1{text-align:center;color:var(--menu-hdr)}"
"TABLE{width:100%;max-width:768px;border:0;margin:auto}"
"TH,TD{padding:0.5em}"
"TH.HEADING{background:var(--menu-bg);color:var(--menu-hdr);border:2px solid var(--menu-border);text-align:center}"
"TD.LABEL{text-align:right;color:var(--param)}"
"INPUT[type=text],INPUT[type=password],SELECT{"
  "width:95%;padding:0.5em;"
  "background:var(--input-bg);color:var(--input-text);border:1px solid var(--input-border)"
"}"
"INPUT[type=submit],BUTTON{"
  "padding:0.5em 1em;margin:0.15em;"
  "background:var(--button-bg);color:var(--button-text);"
  "border:1px solid var(--menu-border);border-radius:6px;cursor:pointer;font-size:1em"
"}"
"BUTTON:hover{background:var(--menu-hl-bg)}"
".CENTER{text-align:center}"
"NAV{text-align:center;padding:0.5em;background:var(--nav-bg);border-bottom:1px solid var(--menu-border)}"
"NAV A{color:var(--nav-text);text-decoration:none;margin:0 0.5em}"
"NAV A:hover{color:var(--menu-hl-text)}"
".SLIDER{width:80%}"
".BTN-GROUP{display:flex;flex-wrap:wrap;gap:0.4em;align-items:center;margin:0.4em 0}"
".CTRL-ROW{margin:0.6em 0}"
"@media(max-width:480px){"
  "INPUT[type=text],SELECT{width:98%}"
  ".SLIDER{width:100%}"
  "BUTTON{width:100%;margin:0.2em 0}"
  "TD.LABEL{white-space:nowrap;font-size:0.9em}"
"}"
;
}

static const String webThemeVars() {
  char buf[600];
  snprintf(buf, sizeof(buf),
    "<STYLE>:root{"
    "--bg:#%06X;--fg:#%06X;"
    "--menu-bg:#%06X;--menu-border:#%06X;"
    "--menu-item:#%06X;--menu-hdr:#%06X;"
    "--menu-hl-bg:#%06X;--menu-hl-text:#%06X;"
    "--param:#%06X;"
    "--box-bg:#%06X;--box-border:#%06X;--box-text:#%06X;"
    "--box-off-bg:#%06X;--box-off-text:#%06X;"
    "--scan-rssi:#%06X;--scan-snr:#%06X;"
    "--s-meter:#%06X;"
    "--nav-bg:#%06X;--nav-text:#%06X;"
    "--input-bg:#%06X;--input-border:#%06X;"
    "--input-text:#%06X;"
    "--button-bg:#%06X;--button-text:#%06X;"
    "}</STYLE>",
    rgb565to888(TH.bg), rgb565to888(TH.text),
    rgb565to888(TH.menu_bg), rgb565to888(TH.menu_border),
    rgb565to888(TH.menu_item), rgb565to888(TH.menu_hdr),
    rgb565to888(TH.menu_hl_bg), rgb565to888(TH.menu_hl_text),
    rgb565to888(TH.menu_param),
    rgb565to888(TH.box_bg), rgb565to888(TH.box_border), rgb565to888(TH.box_text),
    rgb565to888(TH.box_off_bg), rgb565to888(TH.box_off_text),
    rgb565to888(TH.scan_rssi), rgb565to888(TH.scan_snr),
    rgb565to888(TH.smeter_bar),
    rgb565to888(TH.menu_bg), rgb565to888(TH.menu_item),
    rgb565to888(TH.menu_bg), rgb565to888(TH.menu_border),
    rgb565to888(TH.text),
    rgb565to888(TH.menu_hl_bg), rgb565to888(TH.menu_hl_text)
  );
  return String(buf);
}

static const String webNav() {
  return
"<NAV>"
"<A HREF='/'>Status</A>"
"<A HREF='/controls'>Controls</A>"
"<A HREF='/memory'>Memory</A>"
"<A HREF='/scan'>Scan</A>"
"<A HREF='/config'>Config</A>"
"</NAV>";
}

static const String webPage(const String &body, int refreshSec)
{
  String refresh = refreshSec > 0 ?
    "<META HTTP-EQUIV='refresh' CONTENT='" + String(refreshSec) + "'>" : "";

  return
"<!DOCTYPE HTML>"
"<HTML>"
"<HEAD>"
  "<META CHARSET='UTF-8'>"
  "<META NAME='viewport' CONTENT='width=device-width, initial-scale=1.0'>"
  "<TITLE>ATS-Mini Config</TITLE>"
  + refresh +
  "<STYLE>" + webStyleSheet() + "</STYLE>"
  + webThemeVars() +
"</HEAD>"
"<BODY STYLE='font-family: sans-serif;'>" + webNav() + body + "</BODY>"
"</HTML>"
;
}

static const String webScanPage()
{
  return webPage(
"<H1>Scan to Memory</H1>"
"<TABLE>"
"<TR><TH CLASS='HEADING'>Auto Scan</TH></TR>"
"<TR><TD>"
  "Slots: <INPUT TYPE='number' ID='n' VALUE='10' MIN='1' MAX='20'> "
  "<BUTTON ID='bauto' ONCLICK=\"sa()\">Start Auto Scan</BUTTON>"
"</TD></TR>"
"<TR><TH CLASS='HEADING'>Manual Scan</TH></TR>"
"<TR><TD>"
  "<BUTTON ID='bman' ONCLICK=\"sm()\">Start Manual Scan</BUTTON> "
  "<BUTTON ID='bbm' ONCLICK=\"bm()\" DISABLED>Bookmark</BUTTON> "
  "<BUTTON ID='bstop' ONCLICK=\"st()\" DISABLED>Stop &amp; Save</BUTTON>"
"</TD></TR>"
"<TR><TH CLASS='HEADING'>Status</TH></TR>"
"<TR><TD ID='st'>Idle</TD></TR>"
"</TABLE>"
"<DIV ID='sr'></DIV>"
"<SCRIPT>"
"var t;"
"function sa(){"
  "var n=document.getElementById('n').value;"
  "fetch('/api/scan',{method:'POST',body:'cmd=scan&mode=auto&n='+n})"
  ".then(function(r){return r.json()}).then(function(d){"
    "if(d.status=='ok'){document.getElementById('st').textContent='Scanning...';"
    "document.getElementById('bauto').disabled=true;t=setInterval(po,1000);}"
  "});"
"}"
"function sm(){"
  "fetch('/api/scan',{method:'POST',body:'cmd=scan&mode=manual'})"
  ".then(function(r){return r.json()}).then(function(d){"
    "if(d.status=='ok'){document.getElementById('st').textContent='Manual scanning...';"
    "document.getElementById('bman').disabled=true;"
    "document.getElementById('bbm').disabled=false;"
    "document.getElementById('bstop').disabled=false;"
    "fetch('/api/scan',{method:'POST',body:'cmd=step'});"
    "t=setInterval(mp,1000);}"
  "});"
"}"
"function bm(){fetch('/api/scan',{method:'POST',body:'cmd=bookmark'});}"
"function st(){"
  "fetch('/api/scan',{method:'POST',body:'cmd=stop'}).then(function(){"
    "clearInterval(t);"
    "document.getElementById('bman').disabled=false;"
    "document.getElementById('bbm').disabled=true;"
    "document.getElementById('bstop').disabled=true;"
    "po();"
  "});"
"}"
"function ab(){"
  "fetch('/api/scan',{method:'POST',body:'cmd=abort'});"
  "clearInterval(t);"
  "document.getElementById('bauto').disabled=false;"
  "document.getElementById('bman').disabled=false;"
  "document.getElementById('bbm').disabled=true;"
  "document.getElementById('bstop').disabled=true;"
  "document.getElementById('st').textContent='Aborted';"
"}"
"function po(){"
  "fetch('/api/scan',{method:'POST',body:'cmd=status'})"
  ".then(function(r){return r.json()}).then(function(d){"
    "var h='';"
    "if(d.running==1){"
      "document.getElementById('st').textContent='Scanning: '+d.progress_pct+'% Freq: '+d.current_freq+' RSSI: '+d.current_rssi+' SNR: '+d.current_snr;"
    "}else if(d.running==2){"
      "document.getElementById('st').textContent='Complete';clearInterval(t);"
      "document.getElementById('bauto').disabled=false;"
      "document.getElementById('bman').disabled=false;"
      "document.getElementById('bbm').disabled=true;"
      "document.getElementById('bstop').disabled=true;"
      "if(d.results&&d.results.length){"
        "h='<table><tr><th>Slot</th><th>Freq</th><th>RSSI</th><th>Name</th></tr>';"
        "for(var i=0;i<d.results.length;i++){"
          "h+='<tr><td>'+d.results[i].slot+'</td><td>'+d.results[i].freq+'</td><td>'+d.results[i].rssi+'</td><td>'+d.results[i].name+'</td></tr>';"
        "}"
        "h+='</table>';"
      "}"
    "}else if(d.running==3){"
      "document.getElementById('st').textContent='Aborted';clearInterval(t);"
      "document.getElementById('bauto').disabled=false;"
      "document.getElementById('bman').disabled=false;"
    "}"
    "document.getElementById('sr').innerHTML=h;"
  "});"
"}"
"function mp(){"
  "fetch('/api/scan',{method:'POST',body:'cmd=step'}).then(function(){"
    "fetch('/api/scan',{method:'POST',body:'cmd=status'})"
    ".then(function(r){return r.json()}).then(function(d){"
      "if(d.running==1){"
        "var b=d.bookmarks&&d.bookmarks.length?' BM: '+d.bookmarks.join(','):'';"
        "document.getElementById('st').textContent='Freq: '+d.current_freq+' RSSI: '+d.current_rssi+' SNR: '+d.current_snr+b;"
      "}else if(d.running==2){"
        "clearInterval(t);po();"
      "}"
    "});"
  "});"
"}"
"</SCRIPT>"
  );
}

static const String webUtcOffsetSelector()
{
  String result = "";

  for(int i=0 ; i<getTotalUTCOffsets(); i++)
  {
    char text[64];

    sprintf(text,
      "<OPTION VALUE='%d'%s>%s</OPTION>",
      i, radioState.utcOffset==i? " SELECTED":"",
      utcOffsets[i].desc
    );

    result += text;
  }

  return(result);
}

static const String webThemeSelector()
{
  String result = "";

  for(int i=0 ; i<getTotalThemes(); i++)
  {
    char text[64];

    sprintf(text,
      "<OPTION VALUE='%d'%s>%s</OPTION>",
       i, themeIdx==i? " SELECTED":"", theme[i].name
    );

    result += text;
  }

  return(result);
}

static const String webRadioPage()
{
  String ip = "";
  String ssid = "";
  String freq = radioState.mode == FM
    ? String((float)radioState.frequency / 100.0, 1) + " MHz"
    : String(radioState.frequency + radioState.bfo / 1000) + " kHz";

  if(getWiFiStatus() == 2) { ip = WiFi.localIP().toString(); ssid = WiFi.SSID(); }
  else { ip = WiFi.softAPIP().toString(); ssid = RECEIVER_NAME; }

  uint8_t rssiPct = radioState.rssi > 100 ? 100 : radioState.rssi;
  String stationName = getStationName();
  String progInfo = getProgramInfo();

  return webPage(
"<H1>ATS-Mini Pocket Receiver</H1>"
"<TABLE>"
"<TR><TD CLASS='LABEL'>Band</TD><TD>" + String(getCurrentBand()->bandName) + "</TD></TR>"
"<TR><TD CLASS='LABEL'>Frequency</TD><TD>" + freq + " " + String(bandModeDesc[radioState.mode]) + "</TD></TR>"
"<TR><TD CLASS='LABEL'>Signal</TD><TD>"
  "<DIV STYLE='background:var(--box-bg);border:1px solid var(--box-border);height:1.2em'>"
  "<DIV STYLE='background:var(--s-meter);height:100%;width:" + String(rssiPct) + "&#37;'></DIV></DIV>"
  + String(radioState.rssi) + " dBuV / " + String(radioState.snr) + " dB SNR</TD></TR>"
"<TR><TD CLASS='LABEL'>Station</TD><TD>" + (stationName[0] ? stationName : "&mdash;") + "</TD></TR>"
"<TR><TD CLASS='LABEL'>Info</TD><TD>" + (progInfo[0] ? progInfo : "&mdash;") + "</TD></TR>"
"<TR><TD CLASS='LABEL'>Volume</TD><TD>" + String(radioState.vol) + "/63"
  + (audioIsMuted() ? " (Muted)" : "")
  + (audioIsSquelched() ? " (Squelched)" : "") + "</TD></TR>"
"<TR><TD CLASS='LABEL'>Battery</TD><TD>" + String(batteryMonitor(), 2) + "V</TD></TR>"
"<TR><TD CLASS='LABEL'>IP Address</TD><TD><A HREF='http://" + ip + "'>" + ip + "</A> (" + ssid + ")</TD></TR>"
"<TR><TD CLASS='LABEL'>Firmware</TD><TD>" + String(getVersion(true)) + "</TD></TR>"
"</TABLE>"
);
}

static const String webControlsPage()
{
  Band *band = getCurrentBand();
  uint16_t step = getCurrentStep()->step;
  int cal = (radioState.mode == USB) ? band->usbCal :
            (radioState.mode == LSB) ? band->lsbCal : 0;
  int8_t avc = isSSB() ? radioState.ssbAvcIdx : radioState.amAvcIdx;
  uint8_t agc = radioState.agcIndex;
  String freqStr = (radioState.mode == FM)
    ? String((float)radioState.frequency / 100.0, 1) + " MHz"
    : String(radioState.frequency) + " kHz";

  return webPage(
"<H1>Controls</H1>"
"<DIV ID='main'>"

// === Tuning + Band/Mode (combined) ===
"<DIV CLASS='CTRL-ROW'>"
  "<BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=band&value=prev'}).then(function(){setTimeout(sp,300)})\">&lt; Prev</BUTTON>"
  " <B ID='bn'>" + String(band->bandName) + "</B> "
  "<BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=band&value=next'}).then(function(){setTimeout(sp,300)})\">Next &gt;</BUTTON>"
"</DIV>"
"<DIV CLASS='CTRL-ROW'>"
  "<INPUT TYPE='range' MIN='" + String(band->minimumFreq) + "' MAX='" + String(band->maximumFreq) + "' STEP='" + String(step) + "' VALUE='" + String(radioState.frequency) + "' CLASS='SLIDER' ID='fs' STYLE='flex:1;min-width:0' "
  "ONINPUT=\"document.getElementById('fv').textContent=this.value;fetch('/api/command',{method:'POST',body:'cmd=tune&value='+this.value})\">"
  " <SPAN ID='fv'>" + freqStr + "</SPAN> <SPAN ID='fm'>" + String(bandModeDesc[radioState.mode]) + "</SPAN>"
"</DIV>"
"<DIV CLASS='BTN-GROUP'>"
  "Mode: <BUTTON ID='m0' ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=mode&value=0'}).then(function(){setTimeout(sp,300)})\">FM</BUTTON>"
  "<BUTTON ID='m1' ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=mode&value=1'}).then(function(){setTimeout(sp,300)})\">AM</BUTTON>"
  "<BUTTON ID='m2' ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=mode&value=2'}).then(function(){setTimeout(sp,300)})\">LSB</BUTTON>"
  "<BUTTON ID='m3' ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=mode&value=3'}).then(function(){setTimeout(sp,300)})\">USB</BUTTON>"
  " BW: <SELECT ID='bws' ONCHANGE=\"fetch('/api/command',{method:'POST',body:'cmd=bandwidth&value='+this.value}).then(function(){setTimeout(sp,300)})\">"
    "<OPTION VALUE='0'>Auto</OPTION><OPTION VALUE='1'>1.0k</OPTION><OPTION VALUE='2'>2.0k</OPTION>"
    "<OPTION VALUE='3'>2.5k</OPTION><OPTION VALUE='4'>3.0k</OPTION><OPTION VALUE='5'>4.0k</OPTION><OPTION VALUE='6'>6.0k</OPTION>"
  "</SELECT>"
  " Step: <SELECT ID='sts' ONCHANGE=\"fetch('/api/command',{method:'POST',body:'cmd=step&value='+this.value})\">"
    "<OPTION VALUE='0'>Auto</OPTION><OPTION VALUE='1'>1</OPTION><OPTION VALUE='2'>5</OPTION>"
    "<OPTION VALUE='3'>9</OPTION><OPTION VALUE='4'>10</OPTION><OPTION VALUE='5'>100</OPTION>"
  "</SELECT>"
"</DIV>"
"<DIV CLASS='BTN-GROUP'>"
  "<BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=seek&value=down'})\">&lt;&lt; Seek</BUTTON>"
  "<BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=seek&value=up'})\">Seek &gt;&gt;</BUTTON>"
"</DIV>"

// === Audio ===
"<DIV CLASS='CTRL-ROW'>"
  "Vol: <INPUT TYPE='range' MIN='0' MAX='63' VALUE='" + String(radioState.vol) + "' CLASS='SLIDER' "
  "ONINPUT=\"document.getElementById('vv').textContent=this.value;fetch('/api/command',{method:'POST',body:'cmd=volume&value='+this.value})\">"
  " <SPAN ID='vv'>" + String(radioState.vol) + "</SPAN>/63"
  " <BUTTON ID='mt' ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=mute&value='+(this.textContent=='Mute')).then(function(){setTimeout(sp,300)})\">Mute</BUTTON>"
"</DIV>"
"<DIV CLASS='CTRL-ROW'>"
  "Squelch: <INPUT TYPE='range' MIN='0' MAX='127' VALUE='0' CLASS='SLIDER' ID='sqs' "
  "ONINPUT=\"document.getElementById('sqv').textContent=this.value;fetch('/api/command',{method:'POST',body:'cmd=squelch&value='+this.value})\">"
  " <SPAN ID='sqv'>0</SPAN>"
  " <BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=squelch_param&value=rssi'})\">RSSI</BUTTON>"
  " <BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=squelch_param&value=snr'})\">SNR</BUTTON>"
"</DIV>"
"<DIV CLASS='BTN-GROUP'>"
  "RSSI: <SPAN ID='rssi'>" + String(radioState.rssi) + "</SPAN> dBuV"
  " SNR: <SPAN ID='snri'>" + String(radioState.snr) + "</SPAN> dB"
"</DIV>"

// === Settings ===
"<DIV CLASS='CTRL-ROW'>"
  "AGC: <INPUT TYPE='range' MIN='0' MAX='37' VALUE='" + String(agc) + "' CLASS='SLIDER' ID='agcs' "
  "ONINPUT=\"document.getElementById('agcv').textContent=this.value;fetch('/api/command',{method:'POST',body:'cmd=agc&value='+this.value})\">"
  " <SPAN ID='agcv'>" + String(agc) + "</SPAN>/37"
"</DIV>"
"<DIV CLASS='CTRL-ROW'>"
  "AVC: <INPUT TYPE='range' MIN='0' MAX='90' VALUE='" + String(avc) + "' CLASS='SLIDER' ID='avcs' "
  "ONINPUT=\"document.getElementById('avcv').textContent=this.value;fetch('/api/command',{method:'POST',body:'cmd=avc&value='+this.value})\">"
  " <SPAN ID='avcv'>" + String(avc) + "</SPAN>/90"
"</DIV>"
"<DIV CLASS='BTN-GROUP'>"
  "Cal: <BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=cal&value=-1'}).then(function(){setTimeout(sp,300)})\">&minus;</BUTTON>"
  " <SPAN ID='calv'>" + String(cal) + "</SPAN>"
  " <BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=cal&value=1'}).then(function(){setTimeout(sp,300)})\">+</BUTTON>"
"</DIV>"
"<DIV CLASS='CTRL-ROW'>"
  "Brightness: <INPUT TYPE='range' MIN='10' MAX='255' VALUE='" + String(radioState.brightness) + "' CLASS='SLIDER' "
  "ONINPUT=\"fetch('/api/command',{method:'POST',body:'cmd=brightness&value='+this.value})\">"
"</DIV>"
"<DIV CLASS='BTN-GROUP'>"
  "<BUTTON ONCLICK=\"var b=this;fetch('/api/command',{method:'POST',body:'cmd=sleep&value='+(b.textContent=='Sleep')});b.textContent=b.textContent=='Sleep'?'Wake':'Sleep'\">Sleep</BUTTON>"
  "<BUTTON ONCLICK=\"var b=this;fetch('/api/command',{method:'POST',body:'cmd=zoom&value='+(b.textContent=='Zoom')});b.textContent=b.textContent=='Zoom'?'Normal':'Zoom'\">Zoom</BUTTON>"
  "<BUTTON ONCLICK=\"var b=this;fetch('/api/command',{method:'POST',body:'cmd=scroll&value='+(b.textContent=='Normal'?'reverse':'normal')});b.textContent=b.textContent=='Normal'?'Reverse':'Normal'\">Normal</BUTTON>"
"</DIV>"
"<DIV CLASS='CTRL-ROW'>"
  "FM Region: <SELECT ID='fmr' ONCHANGE=\"fetch('/api/command',{method:'POST',body:'cmd=fm_region&value='+this.value})\">"
    "<OPTION VALUE='0'>USA</OPTION><OPTION VALUE='1'>Europe</OPTION><OPTION VALUE='2'>Japan</OPTION>"
  "</SELECT>"
  " RDS: <SELECT ID='rds' ONCHANGE=\"fetch('/api/command',{method:'POST',body:'cmd=rds&value='+this.value})\">"
    "<OPTION VALUE='0'>Off</OPTION><OPTION VALUE='1'>PS</OPTION><OPTION VALUE='7'>PS+PI+CT</OPTION>"
  "</SELECT>"
"</DIV>"

"</DIV>" // #main

"<SCRIPT>"
"function sp(){"
  "fetch('/api/status').then(function(r){return r.json()}).then(function(d){"
    "var fs=document.getElementById('fs');"
    "if(fs){fs.min=d.band_min;fs.max=d.band_max;fs.value=d.frequency_khz;}"
    "var fv=document.getElementById('fv');"
    "if(fv)fv.textContent=d.band=='VHF'?(d.frequency_khz/100).toFixed(1)+' MHz':d.frequency_khz+' kHz';"
    "var fm=document.getElementById('fm');if(fm)fm.textContent=d.mode;"
    "var bn=document.getElementById('bn');if(bn)bn.textContent=d.band;"
    "var mi={FM:0,AM:1,LSB:2,USB:3}[d.mode]||0;"
    "for(var i=0;i<4;i++){var me=document.getElementById('m'+i);if(me)me.style.fontWeight=i==mi?'bold':'normal';}"
    "var vv=document.getElementById('vv');if(vv)vv.textContent=d.volume;"
    "var mt=document.getElementById('mt');if(mt)mt.textContent=d.muted||d.main_muted?'Unmute':'Mute';"
    "var agcs=document.getElementById('agcs');if(agcs)agcs.value=d.agc_index;"
    "var agcv=document.getElementById('agcv');if(agcv)agcv.textContent=d.agc_index;"
    "var avcs=document.getElementById('avcs');if(avcs)avcs.value=d.avc_index;"
    "var avcv=document.getElementById('avcv');if(avcv)avcv.textContent=d.avc_index;"
    "var calv=document.getElementById('calv');if(calv)calv.textContent=d.cal;"
    "var rssi=document.getElementById('rssi');if(rssi)rssi.textContent=d.rssi_dBuv;"
    "var snri=document.getElementById('snri');if(snri)snri.textContent=d.snr_dB;"
    "var sqv=document.getElementById('sqv');if(sqv)sqv.textContent=d.squelch;"
    "var bws=document.getElementById('bws');if(bws){"
      "var m={Auto:0,'1.0k':1,'2.0k':2,'2.5k':3,'3.0k':4,'4.0k':5,'6.0k':6};"
      "var idx=m[d.bandwidth];if(idx!==undefined)bws.selectedIndex=idx;"
    "}"
    "var sts=document.getElementById('sts');if(sts){"
      "var sm={Auto:0,'1':1,'5':2,'9':3,'10':4,'100':5};"
      "var sidx=sm[d.step];if(sidx!==undefined)sts.selectedIndex=sidx;"
    "}"
  "});"
"}"
"setInterval(sp,2000);setTimeout(sp,300);"
"</SCRIPT>"
  );
}

static const String webMemoryPage()
{
  String items = "";
  items.reserve(8192);

  for(int j=0 ; j<MEMORY_COUNT ; j++)
  {
    char text[64];
    sprintf(text, "<TR><TD CLASS='LABEL' WIDTH='10%%'>%02d</TD><TD>", j+1);
    items += text;

    if(!memories[j].freq)
      items += "&nbsp;---&nbsp;</TD></TR>";
    else {
      String freq;
      if (memories[j].mode == FM)
        freq = String((float)memories[j].freq / 1000000.0, 1) + " MHz";
      else
        freq = String(memories[j].freq / 1000) + " kHz";
      if (memories[j].name[0]) freq += " - " + String(memories[j].name);
      items += freq + " " + String(bandModeDesc[memories[j].mode]) + "</TD></TR>";
    }
  }

  return webPage(
"<H1>ATS-Mini Pocket Receiver Memory</H1>"
"<TABLE COLUMNS=2>" + items + "</TABLE>"
);
}

const String webConfigPage()
{
  prefs.begin("network", true, STORAGE_PARTITION);
  String ssid1 = prefs.getString("wifissid1", "");
  String pass1 = prefs.getString("wifipass1", "");
  String ssid2 = prefs.getString("wifissid2", "");
  String pass2 = prefs.getString("wifipass2", "");
  String ssid3 = prefs.getString("wifissid3", "");
  String pass3 = prefs.getString("wifipass3", "");
  bool scanHidden = prefs.getBool("wifiscanhidden", false);
  prefs.end();

  return webPage(
"<H1>ATS-Mini Config</H1>"
"<FORM ACTION='/setconfig' METHOD='POST'>"
  "<TABLE COLUMNS=2>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>WiFi Network 1</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>SSID</TD>"
    "<TD>" + webInputField("wifissid1", ssid1) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("wifipass1", pass1, true) + "</TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>WiFi Network 2</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>SSID</TD>"
    "<TD>" + webInputField("wifissid2", ssid2) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("wifipass2", pass2, true) + "</TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>WiFi Network 3</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>SSID</TD>"
    "<TD>" + webInputField("wifissid3", ssid3) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("wifipass3", pass3, true) + "</TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>This Web UI Login Credentials</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Username</TD>"
    "<TD>" + webInputField("username", loginUsername) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("password", loginPassword, true) + "</TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>Settings</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Scan Hidden SSIDs</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='wifiscanhidden' VALUE='on'" +
    (scanHidden? " CHECKED ":"") + "></TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Time Zone</TD>"
    "<TD>"
      "<SELECT NAME='utcoffset'>" + webUtcOffsetSelector() + "</SELECT>"
    "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Theme</TD>"
    "<TD>"
      "<SELECT NAME='theme'>" + webThemeSelector() + "</SELECT>"
    "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Reverse Scrolling</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='scroll' VALUE='on'" +
    (radioState.scrollDir<0? " CHECKED ":"") + "></TD>"
  "</TR>"
   "<TR>"
    "<TD CLASS='LABEL'>Zoomed Menu</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='zoom' VALUE='on'" +
    (radioState.zoomLevel? " CHECKED ":"") + "></TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>"
    "<INPUT TYPE='SUBMIT' VALUE='Save'>"
  "</TH></TR>"
  "</TABLE>"
"</FORM>"
);
}
