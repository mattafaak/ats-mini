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

    request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Unknown cmd\"}");
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

static const String webStyleSheet()
{
  return
"BODY"
"{"
  "margin: 0;"
  "padding: 0;"
"}"
"H1"
"{"
  "text-align: center;"
"}"
"TABLE"
"{"
  "width: 100%;"
  "max-width: 768px;"
  "border: 0px;"
  "margin-left: auto;"
  "margin-right: auto;"
"}"
"TH, TD"
"{"
  "padding: 0.5em;"
"}"
"TH.HEADING"
"{"
  "background-color: #80A0FF;"
  "column-span: all;"
  "text-align: center;"
"}"
"TD.LABEL"
"{"
  "text-align: right;"
"}"
"INPUT[type=text], INPUT[type=password], SELECT"
"{"
  "width: 95%;"
  "padding: 0.5em;"
"}"
"INPUT[type=submit]"
"{"
  "width: 50%;"
  "padding: 0.5em 0;"
"}"
".CENTER"
"{"
  "text-align: center;"
"}"
;
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
"</HEAD>"
"<BODY STYLE='font-family: sans-serif;'>" + body + "</BODY>"
"</HTML>"
;
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
"<P ALIGN='CENTER'>"
  "<A HREF='/memory'>Memory</A>&nbsp;|&nbsp;<A HREF='/config'>Config</A>"
"</P>"
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
"<P ALIGN='CENTER'>"
  "<A HREF='/'>Status</A>&nbsp;|&nbsp;<A HREF='/config'>Config</A>"
"</P>"
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
"<P ALIGN='CENTER'>"
  "<A HREF='/'>Status</A>"
  "&nbsp;|&nbsp;<A HREF='/memory'>Memory</A>"
"</P>"
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
