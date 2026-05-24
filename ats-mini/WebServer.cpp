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

// Helper: append a hex color field to JSON string
static void jsonColor(String &json, const char *key, uint16_t color) {
  char buf[32];
  snprintf(buf, sizeof(buf), "\"%s\":\"#%04X\",", key, color);
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
    json += "\"agc_index\":" + String(radioState.agcIndex) + ",";
    json += "\"rssi_dBuv\":" + String(radioState.rssi) + ",";
    json += "\"snr_dB\":" + String(radioState.snr) + ",";
    json += "\"battery_V\":" + String(batteryMonitor()) + ",";
    json += String("\"muted\":") + (audioIsMuted() ? "true" : "false") + ",";
    json += String("\"squelch_enabled\":") + (audioIsSquelched() ? "true" : "false") + ",";
    json += String("\"sleep\":") + (sleepOn() ? "true" : "false") + ",";
    json += "\"wifi_mode\":\"" + wifiStatusStr + "\",";
    json += "\"wifi_rssi\":" + String(ws == 2 ? WiFi.RSSI() : 0) + ",";
    json += "\"ip_address\":\"" + String(getWiFiIPAddress()) + "\"";
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
      doMode(idx - radioState.mode);
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
    if(request->hasParam("idx")) {
      themeIdx = constrain(request->getParam("idx")->value().toInt(), 0, getTotalThemes() - 1);
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
"TH.HEADING{background:var(--menu-hdr);color:var(--menu-hl-text);text-align:center}"
"TD.LABEL{text-align:right;color:var(--param)}"
"INPUT[type=text],INPUT[type=password],SELECT{"
  "width:95%;padding:0.5em;"
  "background:var(--input-bg);color:var(--input-text);border:1px solid var(--input-border)"
"}"
"INPUT[type=submit],BUTTON{"
  "padding:0.5em 1em;"
  "background:var(--button-bg);color:var(--button-text);border:1px solid var(--menu-border);cursor:pointer"
"}"
".CENTER{text-align:center}"
"NAV{text-align:center;padding:0.5em;background:var(--nav-bg);border-bottom:1px solid var(--menu-border)}"
"NAV A{color:var(--nav-text);text-decoration:none;margin:0 0.5em}"
"NAV A:hover{color:var(--menu-hl-text)}"
".SLIDER{width:80%}"
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
    "--bg:#%04X;--fg:#%04X;"
    "--menu-bg:#%04X;--menu-border:#%04X;"
    "--menu-item:#%04X;--menu-hdr:#%04X;"
    "--menu-hl-bg:#%04X;--menu-hl-text:#%04X;"
    "--param:#%04X;"
    "--box-bg:#%04X;--box-border:#%04X;--box-text:#%04X;"
    "--box-off-bg:#%04X;--box-off-text:#%04X;"
    "--scan-rssi:#%04X;--scan-snr:#%04X;"
    "--s-meter:#%04X;"
    "--nav-bg:#%04X;--nav-text:#%04X;"
    "--input-bg:#%04X;--input-border:#%04X;"
    "--input-text:#%04X;"
    "--button-bg:#%04X;--button-text:#%04X;"
    "}</STYLE>",
    TH.bg, TH.text,
    TH.menu_bg, TH.menu_border,
    TH.menu_item, TH.menu_hdr,
    TH.menu_hl_bg, TH.menu_hl_text,
    TH.menu_param,
    TH.box_bg, TH.box_border, TH.box_text,
    TH.box_off_bg, TH.box_off_text,
    TH.scan_rssi, TH.scan_snr,
    TH.smeter_bar,
    TH.menu_bg, TH.menu_item,
    TH.menu_bg, TH.menu_border,
    TH.text,
    TH.menu_hl_bg, TH.menu_hl_text
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
  String freq = radioState.mode == FM?
    String(radioState.frequency / 100.0) + "MHz "
  : String(radioState.frequency + radioState.bfo / 1000.0) + "kHz ";

  if(getWiFiStatus() == 2)
  {
    ip = WiFi.localIP().toString();
    ssid = WiFi.SSID();
  }
  else
  {
    ip = WiFi.softAPIP().toString();
    ssid = RECEIVER_NAME;
  }

  return webPage(
"<H1>ATS-Mini Pocket Receiver</H1>"
"<TABLE COLUMNS=2>"
"<TR>"
  "<TD CLASS='LABEL'>IP Address</TD>"
  "<TD><A HREF='http://" + ip + "'>" + ip + "</A> (" + ssid + ")</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>MAC Address</TD>"
  "<TD>" + String(getMACAddress()) + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Firmware</TD>"
  "<TD>" + String(getVersion(true)) + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Band</TD>"
  "<TD>" + String(getCurrentBand()->bandName) + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Frequency</TD>"
  "<TD>" + freq + String(bandModeDesc[radioState.mode]) + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Signal Strength</TD>"
  "<TD>" + String(radioState.rssi) + "dBuV</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Signal to Noise</TD>"
  "<TD>" + String(radioState.snr) + "dB</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Battery Voltage</TD>"
  "<TD>" + String(batteryMonitor()) + "V</TD>"
"</TR>"
"</TABLE>"
"<DIV STYLE='text-align:center;margin-top:1em'>"
"<BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=band&value=prev'})\" STYLE='width:auto;padding:0.5em 1em'>Prev Band</BUTTON>&nbsp;"
"<BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=band&value=next'})\" STYLE='width:auto;padding:0.5em 1em'>Next Band</BUTTON>"
"</DIV>"
, 5);  // auto-refresh every 5 seconds
}

static const String webControlsPage()
{
  return webPage(
"<H1>Controls</H1>"
"<TABLE>"

// Tuning section
"<TR><TH CLASS='HEADING'>Tuning</TH></TR>"
"<TR><TD>"
  "Freq: <INPUT TYPE='text' ID='f' SIZE='10'> "
  "<BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=tune&value='+document.getElementById('f').value})\">Go</BUTTON>"
  "<BR>"
  "<BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=seek&value=down'})\">&lt;&lt; Seek</BUTTON> "
  "<BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=seek&value=up'})\">Seek &gt;&gt;</BUTTON>"
  "<BR>Step: <SELECT ONCHANGE=\"fetch('/api/command',{method:'POST',body:'cmd=step&value='+this.value})\">"
    "<OPTION VALUE='0'>Auto</OPTION><OPTION VALUE='1'>1</OPTION><OPTION VALUE='2'>5</OPTION>"
    "<OPTION VALUE='3'>9</OPTION><OPTION VALUE='4'>10</OPTION><OPTION VALUE='5'>100</OPTION>"
  "</SELECT>"
"</TD></TR>"

// Audio section
"<TR><TH CLASS='HEADING'>Audio</TH></TR>"
"<TR><TD>"
  "Vol: <INPUT TYPE='range' MIN='0' MAX='63' VALUE='" + String(radioState.vol) + "' CLASS='SLIDER' "
  "ONINPUT=\"fetch('/api/command',{method:'POST',body:'cmd=volume&value='+this.value})\"> "
  "<SPAN>" + String(radioState.vol) + "</SPAN>"
  "<BR>"
  "<BUTTON ONCLICK=\"var b=this;fetch('/api/command',{method:'POST',body:'cmd=mute&value='+(b.textContent=='Mute')});b.textContent=b.textContent=='Mute'?'Unmute':'Mute'\">Mute</BUTTON>"
  "<BR>Squelch: <INPUT TYPE='range' MIN='0' MAX='127' VALUE='0' CLASS='SLIDER' "
  "ONINPUT=\"fetch('/api/command',{method:'POST',body:'cmd=squelch&value='+this.value})\">"
  "<BR><BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=squelch_param&value=rssi'})\">RSSI</BUTTON> "
  "<BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=squelch_param&value=snr'})\">SNR</BUTTON>"
"</TD></TR>"

// Band/Mode section
"<TR><TH CLASS='HEADING'>Band / Mode</TH></TR>"
"<TR><TD>"
  "<BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=band&value=prev'})\">Prev</BUTTON> "
  "<SPAN>" + String(getCurrentBand()->bandName) + "</SPAN> "
  "<BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=band&value=next'})\">Next</BUTTON>"
  "<BR>"
  "<BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=mode&value=0'})\">FM</BUTTON>"
  "<BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=mode&value=1'})\">AM</BUTTON>"
  "<BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=mode&value=2'})\">LSB</BUTTON>"
  "<BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=mode&value=3'})\">USB</BUTTON>"
  "<BR>BW: <SELECT ONCHANGE=\"fetch('/api/command',{method:'POST',body:'cmd=bandwidth&value='+this.value})\">"
    "<OPTION>Auto</OPTION><OPTION>1.0k</OPTION><OPTION>2.0k</OPTION>"
    "<OPTION>2.5k</OPTION><OPTION>3.0k</OPTION><OPTION>4.0k</OPTION><OPTION>6.0k</OPTION>"
  "</SELECT>"
"</TD></TR>"

// Settings section
"<TR><TH CLASS='HEADING'>Settings</TH></TR>"
"<TR><TD>"
  "AGC: <INPUT TYPE='range' MIN='0' MAX='37' VALUE='" + String(radioState.agcIndex) + "' CLASS='SLIDER' "
  "ONINPUT=\"fetch('/api/command',{method:'POST',body:'cmd=agc&value='+this.value})\">"
  "<BR>AVC: <INPUT TYPE='range' MIN='0' MAX='90' VALUE='0' CLASS='SLIDER' "
  "ONINPUT=\"fetch('/api/command',{method:'POST',body:'cmd=avc&value='+this.value})\">"
  "<BR>Cal: <BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=cal&value=-1'})\">-</BUTTON> "
  "<BUTTON ONCLICK=\"fetch('/api/command',{method:'POST',body:'cmd=cal&value=1'})\">+</BUTTON>"
  "<BR>Brightness: <INPUT TYPE='range' MIN='10' MAX='255' VALUE='" + String(radioState.brightness) + "' CLASS='SLIDER' "
  "ONINPUT=\"fetch('/api/command',{method:'POST',body:'cmd=brightness&value='+this.value})\">"
  "<BR><BUTTON ONCLICK=\"var b=this;fetch('/api/command',{method:'POST',body:'cmd=sleep&value='+(b.textContent=='Sleep')});b.textContent=b.textContent=='Sleep'?'Wake':'Sleep'\">Sleep</BUTTON>"
  "<BR>FM Region: <SELECT ONCHANGE=\"fetch('/api/command',{method:'POST',body:'cmd=fm_region&value='+this.value})\">"
    "<OPTION VALUE='0'>USA</OPTION><OPTION VALUE='1'>Europe</OPTION><OPTION VALUE='2'>Japan</OPTION>"
  "</SELECT>"
  "<BR>RDS: <SELECT ONCHANGE=\"fetch('/api/command',{method:'POST',body:'cmd=rds&value='+this.value})\">"
    "<OPTION VALUE='0'>Off</OPTION><OPTION VALUE='1'>PS</OPTION><OPTION VALUE='7'>PS+PI+CT</OPTION>"
  "</SELECT>"
  "<BR><BUTTON ONCLICK=\"var b=this;fetch('/api/command',{method:'POST',body:'cmd=zoom&value='+(b.textContent=='Zoom')});b.textContent=b.textContent=='Zoom'?'Normal':'Zoom'\">Zoom</BUTTON>"
  "<BR><BUTTON ONCLICK=\"var b=this;fetch('/api/command',{method:'POST',body:'cmd=scroll&value='+(b.textContent=='Normal'?'reverse':'normal')});b.textContent=b.textContent=='Normal'?'Reverse':'Normal'\">Normal</BUTTON>"
"</TD></TR>"

"</TABLE>"
, 5);
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
    else
    {
      String freq = memories[j].mode == FM?
        String(memories[j].freq / 1000000.0) + "MHz "
      : String(memories[j].freq / 1000.0) + "kHz ";
      String memName = memories[j].name[0] ? " - " + String(memories[j].name) : "";
      items += freq + memName + " " + String(bandModeDesc[memories[j].mode]) + "</TD></TR>";
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
