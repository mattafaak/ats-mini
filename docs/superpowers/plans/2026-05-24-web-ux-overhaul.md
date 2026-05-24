# ATS Mini Web UX Overhaul Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Modernize the web interface with full radio controls, implement scan-to-memory with RDS/EiBi station metadata, add theme sync via CSS variables, and expand test coverage.

**Architecture:** Server-rendered HTML on ESP32 (ESPAsyncWebServer) with 5-tab navigation; inline JS `fetch()` for all controls (no form navigation); CSS variables injected per-page for theme sync; new `/api/scan` endpoint for scan-to-memory with progress polling; RDS dwell + EiBi lookup for station names; new `scanToMemory()` function in Scan.cpp that ranks by RSSI and writes to memory slots.

**Tech Stack:** ESPAsyncWebServer on port 80, inline JS (no framework), CSS variables, SI4735 RDS, LittleFS, NVS preferences, EiBi binary schedules.

---

## File Structure

| File | Change | Responsibility |
|------|--------|---------------|
| `WebServer.cpp` | Major rewrite (~580->~1700 lines) | All HTML pages, API endpoints, theme sync, navigation bar |
| `WebServer.h` | Minor additions | Export new page/API function decls |
| `Scan.cpp` | Major additions (+250 lines) | `scanToMemory()` auto/manual modes, progress tracking |
| `Scan.h` | Minor additions | Declare new scan functions + status struct |
| `Station.cpp` | Minor additions (+40 lines) | RDS dwell helper for FM station name capture |
| `Station.h` | Minor | Declare `rdsDwell()` |
| `test_serial.py` | Major additions (+500 lines) | 15+ new tests for web API, scan, HTML pages, theme sync |

---

### Task 1: Remaining Cleanup — Memory name display, Scheduler check, unused vars

**Files:**
- Modify: `ats-mini/WebServer.cpp` — expose memory name in webMemoryPage()

- [ ] **Step 1: Add memory name display to webMemoryPage()**

The Memory struct has a `name[10]` field that's saved/loaded via `prefs.putBytes()` but never displayed on the web page. In `webMemoryPage()` (around line 477), change the populated-memory row:

```cpp
// Before:
items += freq + bandModeDesc[memories[j].mode] + "</TD></TR>";

// After:
String memName = memories[j].name[0] ? " - " + String(memories[j].name) : "";
items += freq + memName + " " + String(bandModeDesc[memories[j].mode]) + "</TD></TR>";
```

- [ ] **Step 2: Verify Scheduler.cpp includes**

Scheduler.cpp includes Common.h, WiFiManager.h, Station.h, Tuning.h, Utils.h, Menu.h, Storage.h, Scheduler.h (lines 1-8). All needed functions (processRssiSnr, checkRds, identifyFrequency, ntpSyncTime, prefsTickTime, netTickTime, clockTickTime, clockGetHM) are covered. No changes needed.

- [ ] **Step 3: Verify unused variables removed**

`elapsedButton` and `lastStrengthCheck` were removed in Phase 2 Task 14. Check clean build:
```bash
make clean && make 2>&1 | grep -E "error|unused"
```

- [ ] **Step 4: Commit**
```bash
git add ats-mini/WebServer.cpp
git commit -m "fix: display memory names on web memory page"
```

---

### Task 2: Theme Sync — `/api/theme` Endpoint

**Files:**
- Modify: `ats-mini/WebServer.cpp` — add `/api/theme` GET/POST handler in webInit()

- [ ] **Step 1: Add 18-field hex color helper**

Add before `webInit()`:

```cpp
// Helper: append a hex color field to JSON string
static void jsonColor(String &json, const char *key, uint16_t color) {
  char buf[32];
  snprintf(buf, sizeof(buf), "\"%s\":\"#%04X\",", key, color);
  json += buf;
}
```

- [ ] **Step 2: Add theme API handler in webInit()**

Before `server.begin()` (line 191), add:

```cpp
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
```

- [ ] **Step 3: Build and verify**

```bash
make 2>&1 | grep error
```

- [ ] **Step 4: Commit**

```bash
git add ats-mini/WebServer.cpp
git commit -m "feat: add /api/theme endpoint for web-device theme sync"
```

---

### Task 3: CSS Variable Injection & Themed Stylesheet

**Files:**
- Modify: `ats-mini/WebServer.cpp` — add CSS variable generation in `webPage()`, rewrite `webStyleSheet()` to use CSS vars

- [ ] **Step 1: Add CSS variable injection helper**

Add before `webPage()`:

```cpp
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
```

- [ ] **Step 2: Inject CSS variables into webPage()**

Insert the theme vars block right after the existing stylesheet `<STYLE>` tag:

```cpp
// In webPage(), around line 348:
"<STYLE>" + webStyleSheet() + "</STYLE>"
+ webThemeVars() +
```

- [ ] **Step 3: Rewrite webStyleSheet() to use CSS variables**

Replace the entire function body:

```cpp
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
```

- [ ] **Step 4: Add navigation bar helper**

Add before `webPage()`:

```cpp
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
```

- [ ] **Step 5: Inject nav into webPage()**

Change the BODY tag to include nav:

```cpp
"<BODY STYLE='font-family: sans-serif;'>" + webNav() + body + "</BODY>"
```

- [ ] **Step 6: Remove old per-page navigation links**

Remove the old link bars from:
- `webRadioPage()` — remove lines 415-417 (`<P ALIGN='CENTER'><A HREF='/memory'>Memory</A>...`)
- `webMemoryPage()` — remove lines 483-485
- `webConfigPage()` — remove lines 504-507

- [ ] **Step 7: Build**

```bash
make 2>&1 | grep error
```

- [ ] **Step 8: Commit**

```bash
git add ats-mini/WebServer.cpp
git commit -m "feat: CSS variable theme sync, nav bar, themed stylesheet"
```

---

### Task 4: New Routes — `/controls` and `/scan`

**Files:**
- Modify: `ats-mini/WebServer.cpp` — register new routes in webInit()

- [ ] **Step 1: Add forward declarations**

After the existing static decls (line 41):

```cpp
static const String webControlsPage();
static const String webScanPage();
```

- [ ] **Step 2: Register /controls and /scan routes**

After the `/memory` route handler (line 65), add:

```cpp
  server.on("/controls", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    request->send(200, "text/html", webControlsPage());
  });

  server.on("/scan", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    request->send(200, "text/html", webScanPage());
  });
```

- [ ] **Step 3: Build**

```bash
make 2>&1 | grep error
```

- [ ] **Step 4: Commit**

```bash
git add ats-mini/WebServer.cpp
git commit -m "feat: add /controls and /scan route handlers"
```

---

### Task 5: Controls Page HTML

**Files:**
- Modify: `ats-mini/WebServer.cpp` — add `webControlsPage()` after `webRadioPage()`

- [ ] **Step 1: Implement webControlsPage()**

Add after `webRadioPage()`:

```cpp
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
  "<BR><BUTTON ONCLICK=\"var b=this;fetch('/api/command',{method:'POST',body:'cmd=sleep&value='+(b.textContent=='Sleep'));b.textContent=b.textContent=='Sleep'?'Wake':'Sleep'\">Sleep</BUTTON>"
  "<BR>FM Region: <SELECT ONCHANGE=\"fetch('/api/command',{method:'POST',body:'cmd=fm_region&value='+this.value})\">"
    "<OPTION VALUE='0'>USA</OPTION><OPTION VALUE='1'>Europe</OPTION><OPTION VALUE='2'>Japan</OPTION>"
  "</SELECT>"
  "<BR>RDS: <SELECT ONCHANGE=\"fetch('/api/command',{method:'POST',body:'cmd=rds&value='+this.value})\">"
    "<OPTION VALUE='0'>Off</OPTION><OPTION VALUE='1'>PS</OPTION><OPTION VALUE='7'>PS+PI+CT</OPTION>"
  "</SELECT>"
  "<BR><BUTTON ONCLICK=\"var b=this;fetch('/api/command',{method:'POST',body:'cmd=zoom&value='+(b.textContent=='Zoom'));b.textContent=b.textContent=='Zoom'?'Normal':'Zoom'\">Zoom</BUTTON>"
  "<BR><BUTTON ONCLICK=\"var b=this;fetch('/api/command',{method:'POST',body:'cmd=scroll&value='+(b.textContent=='Normal'?'reverse':'normal')});b.textContent=b.textContent=='Normal'?'Reverse':'Normal'\">Normal</BUTTON>"
"</TD></TR>"

"</TABLE>"
, 5);
}
```

- [ ] **Step 2: Build**

```bash
make 2>&1 | grep error
```

- [ ] **Step 3: Commit**

```bash
git add ats-mini/WebServer.cpp
git commit -m "feat: add /controls page with tuning, audio, band/mode, and settings"
```

---

### Task 6: New API Commands (15 handlers for seek, mode, bandwidth, squelch, agc, avc, cal, brightness, etc.)

**Files:**
- Modify: `ats-mini/WebServer.cpp` — add new command handlers in `/api/command` POST handler

- [ ] **Step 1: Add command handlers before the "Unknown cmd" fallback**

Insert these after the `sleep` handler (line ~185) in the `/api/command` handler:

```cpp
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
      while (radioState.brightness != v) doBrt(radioState.brightness < v ? 1 : -1);
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
```

- [ ] **Step 2: Add missing includes if needed**

Check that WebServer.cpp includes `AudioManager.h` (already at line 12) for `audioSquelchClose()`.

- [ ] **Step 3: Build**

```bash
make 2>&1 | grep error
```

- [ ] **Step 4: Commit**

```bash
git add ats-mini/WebServer.cpp
git commit -m "feat: add 15 new /api/command handlers for full radio control"
```

---

### Task 7: Scan Status Struct and Scan.h Declarations

**Files:**
- Rewrite: `ats-mini/Scan.h` — add scan status struct and scan-to-memory function decls

- [ ] **Step 1: Rewrite Scan.h**

```cpp
#ifndef SCAN_H
#define SCAN_H

#include <stdint.h>
#include <stddef.h>

#define SCAN_POINTS 200

// Spectrum scan (existing — used by waterfall display)
void scanRun(uint16_t centerFreq, uint16_t step);
float scanGetRSSI(uint16_t freq);
float scanGetSNR(uint16_t freq);

// Scan-to-memory status
#define SCAN_IDLE    0
#define SCAN_RUNNING 1
#define SCAN_DONE    2
#define SCAN_ABORTED 3

typedef struct {
  uint8_t running;
  uint8_t mode;         // 0=auto, 1=manual
  uint16_t currentFreq;
  uint8_t currentRSSI;
  uint8_t currentSNR;
  uint8_t progress;     // 0-100
  uint8_t bookmarkCount;
  uint16_t bookmarks[20];
  uint8_t resultCount;
  struct {
    uint8_t slot;       // 1-99
    uint16_t freq;      // kHz
    uint8_t rssi;
    char name[12];
  } results[20];
} ScanStatus;

extern ScanStatus scanStatus;

void scanToMemoryAuto(uint8_t count);
void scanToMemoryManual();
void scanManualStep();
void scanBookmark();
void scanStop();
void scanAbort();
bool scanIsRunning();
const ScanStatus* scanGetStatus();

#endif
```

- [ ] **Step 2: Build to verify header compiles**

```bash
make 2>&1 | grep error
```

- [ ] **Step 3: Commit**

```bash
git add ats-mini/Scan.h
git commit -m "feat: add scan-to-memory status struct and function declarations"
```

---

### Task 8: Scan-to-Memory Implementation in Scan.cpp

**Files:**
- Modify: `ats-mini/Scan.cpp` — add scan-to-memory functions at end of file

- [ ] **Step 1: Add includes and helpers at top of Scan.cpp**

Add to top of file (after existing includes):

```cpp
#include "Station.h"
#include "Menu.h"
#include "Storage.h"
#include "AudioManager.h"
```

- [ ] **Step 2: Add scan status global and candidate struct**

Add before `scanInit()`:

```cpp
ScanStatus scanStatus = {0};
```

Add after existing static data (line 35):

```cpp
// Candidate for auto mode ranking
typedef struct {
  uint16_t freq;
  uint8_t rssi;
} Candidate;

static int candidateDesc(const void *a, const void *b) {
  return ((const Candidate*)b)->rssi - ((const Candidate*)a)->rssi;
}
```

- [ ] **Step 3: Implement scanToMemoryAuto()**

Add after `scanRun()` at end of file:

```cpp
//
// Auto scan: sweep band, collect RSSI, rank top N, save to memory
//
void scanToMemoryAuto(uint8_t count) {
  if (count > 20) count = 20;
  if (count == 0) count = 10;

  Band *band = getCurrentBand();
  uint16_t step = getCurrentStep()->step;
  if (radioState.mode != FM && step < 5) step = 5;

  uint16_t origFreq = radioState.frequency;
  int origBfo = radioState.bfo;

  audioTempMute(true);
  seekStop = false;
  memset(&scanStatus, 0, sizeof(scanStatus));
  scanStatus.running = SCAN_RUNNING;
  scanStatus.mode = 0;

  // Phase 1: sweep band collecting RSSI
  uint16_t freq = band->minimumFreq;
  uint16_t totalSteps = ((band->maximumFreq - band->minimumFreq) / step) + 1;
  uint16_t stepCount = 0;
  Candidate candidates[SCAN_POINTS];
  uint16_t candidateCount = 0;

  while (freq <= band->maximumFreq && stepCount < SCAN_POINTS) {
    if (consumeAbortPending() || seekStop) {
      scanStatus.running = SCAN_ABORTED;
      goto restore;
    }

    if (isSSB()) updateBFO(0, true);
    if (updateFrequency(freq, false)) {
      uint32_t t = millis();
      while ((millis() - t) < 80) {
        rx.getCurrentReceivedSignalQuality();
        if (rx.getCurrentRSSI() > 0) break;
        delay(5);
      }
      rx.getCurrentReceivedSignalQuality();
      if (candidateCount < SCAN_POINTS) {
        candidates[candidateCount].freq = freq;
        candidates[candidateCount].rssi = rx.getCurrentRSSI();
        candidateCount++;
      }
      scanStatus.currentFreq = freq;
      scanStatus.currentRSSI = rx.getCurrentRSSI();
      scanStatus.currentSNR = rx.getCurrentSNR();
    }

    scanStatus.progress = (uint8_t)((uint32_t)stepCount * 50 / totalSteps);
    freq += step;
    stepCount++;
  }

  // Phase 2: sort by RSSI descending, take top N
  qsort(candidates, candidateCount, sizeof(Candidate), candidateDesc);

  uint8_t slot = 0;
  for (int i = 0; i < MEMORY_COUNT; i++) {
    if (memories[i].freq == 0) { slot = i; break; }
  }

  uint8_t written = 0;
  for (int i = 0; i < candidateCount && written < count && slot < MEMORY_COUNT; i++) {
    if (candidates[i].rssi == 0) continue;

    updateFrequency(candidates[i].freq, false);
    if (isSSB()) {
      uint32_t fHz = (uint32_t)candidates[i].freq * 1000;
      updateBFO(bfoFromHz(fHz), false);
    }

    // Station ID
    char name[12] = "";
    if (radioState.mode == FM) {
      uint32_t rdsStart = millis();
      while ((millis() - rdsStart) < 1500) {
        checkRds();
        const char *n = getStationName();
        if (n && n[0] && n[0] != '*') { strlcpy(name, n, sizeof(name)); break; }
        delay(100);
      }
    } else {
      identifyFrequency(candidates[i].freq, false);
      const char *n = getStationName();
      if (n && n[0] && n[0] != '*') strlcpy(name, n, sizeof(name));
    }

    // Write to memory
    uint32_t fHz = (uint32_t)candidates[i].freq * 1000;
    memories[slot].freq = fHz;
    memories[slot].band = bandIdx;
    memories[slot].mode = radioState.mode;
    strlcpy(memories[slot].name, name, sizeof(memories[slot].name));
    prefsSaveMemory(slot, true);

    scanStatus.results[written].slot = slot + 1;
    scanStatus.results[written].freq = candidates[i].freq;
    scanStatus.results[written].rssi = candidates[i].rssi;
    strlcpy(scanStatus.results[written].name, name, 12);
    written++;
    slot++;
  }

  scanStatus.resultCount = written;
  scanStatus.running = SCAN_DONE;

restore:
  updateFrequency(origFreq, false);
  if (isSSB()) updateBFO(origBfo, false);
  audioTempMute(false);
  clearStationInfo();
  identifyFrequency(getEffectiveFreq());
}
```

- [ ] **Step 4: Implement manual scan + bookmark + stop**

Add after `scanToMemoryAuto()`:

```cpp
void scanToMemoryManual() {
  Band *band = getCurrentBand();
  uint16_t step = getCurrentStep()->step;
  if (radioState.mode != FM && step < 5) step = 5;

  memset(&scanStatus, 0, sizeof(scanStatus));
  scanStatus.running = SCAN_RUNNING;
  scanStatus.mode = 1;
  scanStatus.currentFreq = band->minimumFreq;

  audioTempMute(true);
  seekStop = false;
  if (isSSB()) updateBFO(0, true);
  updateFrequency(scanStatus.currentFreq, false);
  audioTempMute(false);
}

void scanManualStep() {
  if (scanStatus.running != SCAN_RUNNING || scanStatus.mode != 1) return;

  Band *band = getCurrentBand();
  uint16_t step = getCurrentStep()->step;
  if (radioState.mode != FM && step < 5) step = 5;

  scanStatus.currentFreq += step;
  if (scanStatus.currentFreq > band->maximumFreq) {
    scanStatus.running = SCAN_DONE;
    return;
  }

  audioTempMute(true);
  if (isSSB()) updateBFO(0, true);
  updateFrequency(scanStatus.currentFreq, false);
  rx.getCurrentReceivedSignalQuality();
  scanStatus.currentRSSI = rx.getCurrentRSSI();
  scanStatus.currentSNR = rx.getCurrentSNR();
  audioTempMute(false);

  uint16_t range = band->maximumFreq - band->minimumFreq;
  if (range) scanStatus.progress = (uint8_t)((uint32_t)(scanStatus.currentFreq - band->minimumFreq) * 100 / range);
}

void scanBookmark() {
  if (scanStatus.running != SCAN_RUNNING || scanStatus.mode != 1) return;
  if (scanStatus.bookmarkCount >= 20) return;
  scanStatus.bookmarks[scanStatus.bookmarkCount++] = scanStatus.currentFreq;
}

void scanStop() {
  if (scanStatus.running != SCAN_RUNNING) return;
  scanStatus.running = SCAN_DONE;

  uint8_t slot = 0;
  for (int i = 0; i < MEMORY_COUNT; i++) {
    if (memories[i].freq == 0) { slot = i; break; }
  }

  Band *band = getCurrentBand();
  uint16_t origFreq = radioState.frequency;
  int origBfo = radioState.bfo;
  uint8_t written = 0;

  audioTempMute(true);

  for (int i = 0; i < scanStatus.bookmarkCount && slot < MEMORY_COUNT; i++, slot++) {
    uint16_t freq = scanStatus.bookmarks[i];
    if (isSSB()) updateBFO(0, true);
    if (!updateFrequency(freq, false)) continue;

    char name[12] = "";
    if (radioState.mode == FM) {
      clearStationInfo();
      uint32_t rdsStart = millis();
      while ((millis() - rdsStart) < 1500) {
        checkRds();
        const char *n = getStationName();
        if (n && n[0] && n[0] != '*') { strlcpy(name, n, sizeof(name)); break; }
        delay(100);
      }
    } else {
      identifyFrequency(freq, false);
      const char *n = getStationName();
      if (n && n[0] && n[0] != '*') strlcpy(name, n, sizeof(name));
    }

    uint32_t fHz = (uint32_t)freq * 1000;
    memories[slot].freq = fHz;
    memories[slot].band = bandIdx;
    memories[slot].mode = radioState.mode;
    strlcpy(memories[slot].name, name, sizeof(memories[slot].name));
    prefsSaveMemory(slot, true);

    scanStatus.results[written].slot = slot + 1;
    scanStatus.results[written].freq = freq;
    scanStatus.results[written].rssi = scanStatus.currentRSSI;
    strlcpy(scanStatus.results[written].name, name, 12);
    written++;
  }

  scanStatus.resultCount = written;

  updateFrequency(origFreq, false);
  if (isSSB()) updateBFO(origBfo, false);
  audioTempMute(false);
  clearStationInfo();
  identifyFrequency(getEffectiveFreq());
}

void scanAbort() {
  scanStatus.running = SCAN_ABORTED;
  seekStop = true;
}

bool scanIsRunning() {
  return scanStatus.running == SCAN_RUNNING;
}

const ScanStatus* scanGetStatus() {
  return &scanStatus;
}
```

- [ ] **Step 5: Build**

```bash
make 2>&1 | grep error
```

- [ ] **Step 6: Commit**

```bash
git add ats-mini/Scan.cpp
git commit -m "feat: implement scan-to-memory auto/manual with station ID"
```

---

### Task 9: Scan Web API Endpoint

**Files:**
- Modify: `ats-mini/WebServer.cpp` — add `/api/scan` POST handler with all scan commands

- [ ] **Step 1: Add `#include "Scan.h"` to WebServer.cpp includes**

```cpp
#include "Scan.h"
```

- [ ] **Step 2: Add scan API handler in webInit()**

Before `server.begin()` (line 191), add:

```cpp
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
```

- [ ] **Step 3: Build**

```bash
make 2>&1 | grep error
```

- [ ] **Step 4: Commit**

```bash
git add ats-mini/WebServer.cpp
git commit -m "feat: add /api/scan endpoint for scan-to-memory control"
```

---

### Task 10: Scan Page HTML

**Files:**
- Modify: `ats-mini/WebServer.cpp` — add `webScanPage()` full HTML with JS polling

- [ ] **Step 1: Implement webScanPage()**

Add after `webControlsPage()`:

```cpp
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
```

- [ ] **Step 2: Build**

```bash
make 2>&1 | grep error
```

- [ ] **Step 3: Commit**

```bash
git add ats-mini/WebServer.cpp
git commit -m "feat: add /scan page with auto/manual scan UI and polling"
```

---

### Task 11: Enhanced Status Page — Signal bar, Station Name, Improved /api/status

**Files:**
- Modify: `ats-mini/WebServer.cpp` — rewrite `webRadioPage()` with signal bar, add station fields to `/api/status`

- [ ] **Step 1: Rewrite webRadioPage() with signal bar and station info**

Replace the existing `webRadioPage()`:

```cpp
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
, 5);
}
```

- [ ] **Step 2: Add station fields to /api/status JSON**

In the JSON builder (around line 98), add after `json += "\"mode\"...` and before the closing:

```cpp
    json += "\"station_name\":\"" + String(getStationName()) + "\",";
    json += "\"program_info\":\"" + String(getProgramInfo()) + "\",";
    json += "\"rssi\":" + String(radioState.rssi) + ",";
    json += "\"snr\":" + String(radioState.snr) + ",";
    json += "\"main_muted\":" + (audioIsMainMuted() ? "true" : "false") + ",";
    json += "\"squelched\":" + (audioIsSquelched() ? "true" : "false") + ",";
    json += "\"squelch\":" + String(radioState.squelch[radioState.mode] & 0x7F) + ",";
    json += "\"squelch_is_snr\":" + ((radioState.squelch[radioState.mode] & 0x80) ? "true" : "false") + ",";
    json += "\"theme_idx\":" + String(themeIdx) + ",";
```

- [ ] **Step 3: Build**

```bash
make 2>&1 | grep error
```

- [ ] **Step 4: Commit**

```bash
git add ats-mini/WebServer.cpp
git commit -m "feat: add signal bar, station info, enhanced /api/status JSON"
```

---

### Task 12: Enhanced Memory Page — Station names, fixed freq formatting

**Files:**
- Modify: `ats-mini/WebServer.cpp` — fix `webMemoryPage()` to show names and fix freq format

- [ ] **Step 1: Update memory row formatting**

In `webMemoryPage()` (line ~474), replace the populated-row block:

```cpp
    if(!memories[j].freq)
      items += "---</TD></TR>";
    else {
      String freq;
      if (memories[j].mode == FM)
        freq = String((float)memories[j].freq / 1000000.0, 1) + " MHz";
      else
        freq = String(memories[j].freq / 1000) + " kHz";
      if (memories[j].name[0]) freq += " - " + String(memories[j].name);
      items += freq + " " + String(bandModeDesc[memories[j].mode]) + "</TD></TR>";
    }
```

- [ ] **Step 2: Build**

```bash
make 2>&1 | grep error
```

- [ ] **Step 3: Commit**

```bash
git add ats-mini/WebServer.cpp
git commit -m "feat: show station names on memory page, fix FM freq format"
```

---

### Task 13: Test Suite Expansion — 15+ new tests

**Files:**
- Modify: `ats-mini/test_serial.py` — add 15 new test functions

- [ ] **Step 1: Add web IP discovery helper**

Add near the top of the file (after existing helpers):

```python
def get_web_ip():
    """Try mDNS, fallback to AP IP."""
    import subprocess
    try:
        result = subprocess.run(["avahi-resolve-host-name", "atsmini.local"],
                              capture_output=True, text=True, timeout=3)
        if result.returncode == 0:
            return result.stdout.strip().split('\t')[1]
    except:
        pass
    return "10.1.1.1"  # AP fallback
```

- [ ] **Step 2: Add new test functions**

Add after `test_web_api()`:

```python
def test_web_api_tune_valid():
    ip = get_web_ip()
    if not ip: return
    for freq_khz in [9410, 15190, 17120]:
        code, body = _curl(f"http://{ip}/api/command", method="-X POST",
                          data=f"cmd=tune&value={freq_khz}", show_url=False)
        assert code == 200, f"Tune {freq_khz} failed: {body}"
        time.sleep(1)
        s = get_status(ser)
        assert s and abs(s["freq_khz"] - freq_khz) <= 2, f"Freq mismatch: {s}"
    print(f"  Tuned 3 frequencies OK")

def test_web_api_tune_out_of_band():
    ip = get_web_ip()
    if not ip: return
    code, body = _curl(f"http://{ip}/api/command", method="-X POST",
                      data="cmd=tune&value=0", show_url=False)
    assert code == 400, f"Expected 400, got {code}"

def test_web_api_volume():
    ip = get_web_ip()
    if not ip: return
    for vol in [0, 30, 63]:
        code, body = _curl(f"http://{ip}/api/command", method="-X POST",
                          data=f"cmd=volume&value={vol}", show_url=False)
        assert code == 200
        time.sleep(0.5)
        s = get_status(ser)
        assert s and s["vol"] == vol, f"Vol mismatch: {s}"
    print(f"  Volume 0/30/63 OK")

def test_web_api_mute():
    ip = get_web_ip()
    if not ip: return
    code, body = _curl(f"http://{ip}/api/command", method="-X POST",
                      data="cmd=mute&value=true", show_url=False)
    assert code == 200
    time.sleep(0.3)
    code, body = _curl(f"http://{ip}/api/status", show_url=False)
    assert '"main_muted":true' in body or '"muted":true' in body
    code, body = _curl(f"http://{ip}/api/command", method="-X POST",
                      data="cmd=mute&value=false", show_url=False)
    assert code == 200
    print(f"  Mute toggle OK")

def test_web_api_band():
    ip = get_web_ip()
    if not ip: return
    s = get_status(ser)
    assert s
    orig = s["band"]
    code, body = _curl(f"http://{ip}/api/command", method="-X POST",
                      data="cmd=band&value=next", show_url=False)
    assert code == 200
    time.sleep(1)
    s = get_status(ser)
    assert s and s["band"] != orig, f"Band unchanged: {s['band']}"
    print(f"  Band cycle: {orig} -> {s['band']}")

def test_web_api_seek():
    ip = get_web_ip()
    if not ip: return
    code, body = _curl(f"http://{ip}/api/command", method="-X POST",
                      data="cmd=seek&value=up", show_url=False)
    assert code == 200
    time.sleep(2)
    s = get_status(ser)
    assert s and s["freq_khz"] > 0
    print(f"  Seek OK, freq={s['freq_khz']}")

def test_web_api_invalid_cmd():
    ip = get_web_ip()
    if not ip: return
    code, body = _curl(f"http://{ip}/api/command", method="-X POST",
                      data="cmd=invalid_xyz", show_url=False)
    assert code == 400

def test_web_api_theme_get():
    ip = get_web_ip()
    if not ip: return
    import json
    code, body = _curl(f"http://{ip}/api/theme", show_url=False)
    assert code == 200
    data = json.loads(body)
    assert "idx" in data and "name" in data and "colors" in data
    assert "bg" in data["colors"] and "fg" in data["colors"]
    assert 0 <= data["idx"] < data["themeCount"]
    print(f"  Theme API: idx={data['idx']} name={data['name']}")

def test_web_api_theme_set():
    ip = get_web_ip()
    if not ip: return
    import json
    code, body = _curl(f"http://{ip}/api/theme", show_url=False)
    orig = json.loads(body)
    new_idx = (orig["idx"] + 1) % orig["themeCount"]
    code, body = _curl(f"http://{ip}/api/theme", method="-X POST",
                      data=f"idx={new_idx}", show_url=False)
    assert code == 200
    code, body = _curl(f"http://{ip}/api/theme", show_url=False)
    assert json.loads(body)["idx"] == new_idx
    _curl(f"http://{ip}/api/theme", method="-X POST", data=f"idx={orig['idx']}", show_url=False)
    print(f"  Theme set: {orig['idx']}->{new_idx}->{orig['idx']}")

def test_scan_auto():
    code, body = send_cmd(ser, 'Z', wait=10)
    assert "END" in body, f"Scan failed: {body}"
    code, body = send_cmd(ser, '$', wait=1)
    assert body, "No memory list"
    count = sum(1 for line in body.split('\n') if line.startswith('#'))
    print(f"  Scan found {count} signals")

def test_scan_invalid():
    ip = get_web_ip()
    if not ip: return
    code, body = _curl(f"http://{ip}/api/scan", method="-X POST",
                      data="cmd=scan&mode=invalid", show_url=False)
    assert code == 400

def test_status_page():
    ip = get_web_ip()
    if not ip: return
    code, body = _curl(f"http://{ip}/", show_url=False)
    assert code == 200
    for keyword in ["ATS-Mini", "Status", "Controls", "Memory", "Scan", "Config",
                    "Band", "Frequency", "Signal", "Battery"]:
        assert keyword in body, f"Missing {keyword} on status page"

def test_controls_page():
    ip = get_web_ip()
    if not ip: return
    code, body = _curl(f"http://{ip}/controls", show_url=False)
    assert code == 200
    for keyword in ["Volume", "Mute", "Squelch", "Seek", "Mode", "Brightness"]:
        assert keyword in body, f"Missing {keyword} on controls page"

def test_scan_page():
    ip = get_web_ip()
    if not ip: return
    code, body = _curl(f"http://{ip}/scan", show_url=False)
    assert code == 200
    for keyword in ["Scan to Memory", "Auto Scan", "Manual Scan", "Bookmark"]:
        assert keyword in body, f"Missing {keyword} on scan page"

def test_memory_page():
    ip = get_web_ip()
    if not ip: return
    code, body = _curl(f"http://{ip}/memory", show_url=False)
    assert code == 200
    assert "Memory" in body

def test_theme_sync():
    ip = get_web_ip()
    if not ip: return
    import json
    code, body = _curl(f"http://{ip}/api/theme", show_url=False)
    data = json.loads(body)
    orig_idx = data["idx"]
    new_idx = (orig_idx + 1) % data["themeCount"]
    _curl(f"http://{ip}/api/theme", method="-X POST", data=f"idx={new_idx}", show_url=False)
    code, body = _curl(f"http://{ip}/api/theme", show_url=False)
    assert json.loads(body)["idx"] == new_idx
    _curl(f"http://{ip}/api/theme", method="-X POST", data=f"idx={orig_idx}", show_url=False)
    print(f"  Theme sync OK")
```

- [ ] **Step 3: Register all new tests**

Add before the final summary section:

```python
# Web API expansion
test("web_api_tune_valid", test_web_api_tune_valid)
test("web_api_tune_out_of_band", test_web_api_tune_out_of_band)
test("web_api_volume", test_web_api_volume)
test("web_api_mute", test_web_api_mute)
test("web_api_band", test_web_api_band)
test("web_api_seek", test_web_api_seek)
test("web_api_invalid_cmd", test_web_api_invalid_cmd)
test("web_api_theme_get", test_web_api_theme_get)
test("web_api_theme_set", test_web_api_theme_set)

# Scan tests
test("scan_auto", test_scan_auto)
test("scan_invalid", test_scan_invalid)

# HTML page tests
test("status_page", test_status_page)
test("controls_page", test_controls_page)
test("scan_page", test_scan_page)
test("memory_page", test_memory_page)

# Theme sync
test("theme_sync", test_theme_sync)
```

- [ ] **Step 4: Run tests**

```bash
cd ats-mini && python3 test_serial.py 2>&1
```

Expected: 42+ tests pass (27 existing + 15 new).

- [ ] **Step 5: Commit**

```bash
git add ats-mini/test_serial.py
git commit -m "test: add 15 new web API, scan, HTML page, and theme sync tests"
```

---

## Verification

1. **Build**: `make 2>&1 | grep error` — zero errors
2. **Theme sync**: `curl http://atsmini.local/api/theme` returns JSON with all 18 color fields; POST `idx=0` switches theme; webpage colors match device on refresh
3. **Navigation**: All 5 tabs render on every page with correct links
4. **Controls page**: Volume slider, mute, seek, mode buttons, bandwidth dropdown all change device state via fetch()
5. **New API commands**: `curl -X POST -d 'cmd=seek&value=up' http://atsmini.local/api/command` triggers seek
6. **Scan-to-memory**: Auto scan fills memory slots with top signals; FM stations get RDS names
7. **Status page**: Signal bar, station name, RDS text render correctly
8. **Memory page**: Shows station names in addition to freq/mode
9. **Tests**: `python3 test_serial.py` — all 42+ tests pass
10. **Regression**: All 27 existing tests still pass
