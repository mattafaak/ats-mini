# ATS Mini Web UX Overhaul & Scan-to-Memory Design

**Goal:** Modernize the web interface, add full radio controls, implement scan-to-memory with station metadata, and expand test coverage.

**Architecture:** Server-rendered HTML on ESP32 (ESPAsyncWebServer) with tabbed navigation; inline JS for fetch-based controls; CSS variables for theme sync; new `/api/scan` endpoint for scan-to-memory; RDS + EiBi for station metadata.

**Tech Stack:** ESPAsyncWebServer on port 80, inline JS (no framework), CSS variables, SI4735 RDS, LittleFS, NVS memories.

---

## 1. Tabs and Routes

The web interface moves from 3 pages to 5 tabs, all linked in a top navigation bar:

| Tab | Route | Auto-refresh | Purpose |
|-----|-------|-------------|---------|
| Status | `/` | 5s | Real-time: band, freq, mode, signal strength bar, SNR, battery |
| Controls | `/controls` | 5s | All radio controls (see Section 2) |
| Memory | `/memory` | No | 99-slot memory view (existing, enhanced) |
| Scan | `/scan` | Polls during scan | Scan-to-memory interface |
| Config | `/config` | No | WiFi, credentials, timezone, theme, scroll, zoom (existing) |

Navigation bar rendered on every page: `Status | Controls | Memory | Scan | Config`

## 2. Controls Page (`/controls`)

Single scrollable page, grouped sections. All interactive elements use `fetch()` POST to `/api/command` — no form navigation.

### Tuning Section
- **Frequency input**: text field + "Go" button → `cmd=tune&value=<kHz>`
- **Seek buttons**: "<< Seek" / "Seek >>" → `cmd=seek&value=up|down`
- **Step selector**: dropdown matching current mode's steps → `cmd=step&value=<index>`

### Audio Section
- **Volume slider**: range input 0-63, shows numeric value → `cmd=volume&value=<N>`
- **Mute button**: toggle → `cmd=mute&value=true|false`
- **Squelch slider**: range input 0-127 → `cmd=squelch&value=<N>` 
- **Squelch param toggle**: RSSI/SNR → `cmd=squelch_param&value=rssi|snr`
- **Soft mute**: display-only (or dropdown) → `cmd=softmute&value=<N>`

### Band / Mode Section
- **Band selector**: prev/next buttons + current band name → `cmd=band&value=next|prev`
- **Mode buttons**: button group (FM / AM / LSB / USB) → `cmd=mode&value=<index>`
- **Bandwidth**: dropdown of available BWs → `cmd=bandwidth&value=<index>`

### Settings Section
- **AGC/Attenuation**: slider or numbered input → `cmd=agc&value=<N>`
- **AVC max gain**: slider (AM/SSB only) → `cmd=avc&value=<N>`
- **Calibration**: +/- buttons (SSB only) → `cmd=cal&value=<N>` (N is +/-1)
- **Brightness**: slider 10-255 → `cmd=brightness&value=<N>`
- **Sleep**: toggle + sleep mode selector → `cmd=sleep&value=on|off`
- **FM Region**: dropdown → `cmd=fm_region&value=<N>`
- **RDS mode**: toggle or dropdown → `cmd=rds&value=<N>`
- **Zoom**: toggle → `cmd=zoom&value=on|off`
- **Scroll direction**: toggle → `cmd=scroll&value=normal|reverse`

### New /api/command Commands

All existing commands remain. New ones added:

| cmd | value | Behavior |
|-----|-------|----------|
| `seek` | `up` / `down` | Calls `doSeek(1,1)` or `doSeek(-1,-1)` |
| `mode` | `0`-`3` | Calls `doMode()` with delta to reach index |
| `bandwidth` | `0`-`N` | Calls `doBandwidth()` with delta to reach index |
| `squelch` | `0`-`127` | Sets squelch value directly |
| `squelch_param` | `rssi` / `snr` | Toggles bit 7 of squelch byte |
| `agc` | `0`-`37` | Sets AGC index directly |
| `avc` | `0`-`90` | Sets AVC index directly |
| `cal` | `+1` / `-1` | Calls `doCal(1)` or `doCal(-1)` |
| `brightness` | `10`-`255` | Calls `doBrt()` with computed delta |
| `softmute` | `0`-`32` | Sets soft mute max attenuation |
| `fm_region` | `0`-`N` | Sets FM region index |
| `rds` | `0`-`N` | Sets RDS mode |
| `zoom` | `on` / `off` | Sets `radioState.zoomLevel` |
| `scroll` | `normal` / `reverse` | Sets `radioState.scrollDir` |

### Mobile Responsiveness

CSS media queries in the existing stylesheet:
- `@media (max-width: 480px)`: sliders go full-width, input widths adjust, font sizes reduce
- All tables use `max-width: 100%` and `overflow-x: auto` where needed
- Buttons wrap naturally on small screens (flex-wrap)

## 3. Scan-to-Memory (`/scan`)

New dedicated page with two modes, toggled by radio buttons.

### Auto Mode
1. User selects step size (default: current band step) and slot count N (default: 10, max: 20)
2. Clicks "Start Auto Scan" → `POST /api/scan` with `body: cmd=scan&mode=auto&n=10`
3. Firmware calls a new `scanToMemory()` function:
   - Iterates current band from `minimumFreq` to `maximumFreq` by step
   - At each step: tunes, settles (adaptive 80ms max), reads RSSI, stores `{freq, rssi}`
   - After full scan: sorts by RSSI descending, takes top N
   - For each of the top N: writes to sequential memory slot using existing memory save path
   - For FM candidates: after ranking, dwells 1-2s per candidate to decode RDS station name
   - For AM/SW: calls `identifyFrequency()` for EiBi/named frequency match
4. Progress available via polling `POST /api/scan` with `body: cmd=scan&mode=status`
5. Returns JSON: `{running:bool, current_freq, current_rssi, progress_pct, results:[{slot, freq, rssi, name}]}`

### Manual Mode
1. Clicks "Start Manual Scan" → `POST /api/scan` with `body: cmd=scan&mode=manual`
2. Firmware steps through the band at ~1 step/sec
3. Web page polls status every 1s, shows current freq + RSSI + SNR
4. User clicks "Bookmark" → `POST /api/scan` with `body: cmd=scan&mode=bookmark`
5. Bookmarked freqs shown in a list on the page
6. "Stop & Save" → finalizes, writes all bookmarks to sequential memory slots
7. Same metadata integration as auto mode

### Status JSON Response
```json
{
  "running": true,
  "mode": "auto",
  "current_freq": 10390,
  "current_rssi": 45,
  "current_snr": 22,
  "progress_pct": 67,
  "bookmarks": [],
  "results": [
    {"slot": 1, "freq": 10390, "rssi": 52, "name": "WNYC"},
    {"slot": 2, "freq": 10110, "rssi": 48, "name": ""}
  ]
}
```

### Station Metadata Sources
| Band | Source | Latency | Availability |
|------|--------|---------|-------------|
| FM | RDS (PS) | 1-2s dwell per candidate | Only for stations broadcasting RDS |
| AM | EiBi schedules | Instant (pre-loaded) | International broadcasters |
| SW/LSB/USB | Named freqs + EiBi | Instant | Ham bands, utility stations |
| All | Frequency lookup table | Instant | Built-in CB channel map |

### Files Changed
- **New:** `Scan.cpp`/`Scan.h` — `scanToMemory()` function, blocking scan with progress polling
- **Modified:** `WebServer.cpp` — `/scan` HTML page, `/api/scan` POST handler, status JSON builder
- **Modified:** `WebServer.h` — export scan status
- **Modified:** `Station.cpp` — RDS dwell helper (tune, wait for RDS, return name)
- **Modified:** `Menu.cpp` — expose `getMemoryByIndex()` or reuse existing `memories[]` extern

## 4. Theme Sync

### `/api/theme` Endpoint
- **GET**: returns JSON
  ```json
  {
    "idx": 3,
    "name": "Night",
    "colors": {
      "bg": "#000814",
      "fg": "#c0c0c0",
      "menu_bg": "#040c18",
      "menu_border": "#102030",
      "menu_item": "#2080ff",
      "menu_hdr": "#2080ff",
      "menu_hl_bg": "#004080",
      "menu_hl_text": "#ffffff",
      "param": "#20ff20",
      "box_bg": "#040c18",
      "box_border": "#102030",
      "box_text": "#c0c0c0",
      "box_off_bg": "#400000",
      "box_off_text": "#ffffff",
      "scan_rssi": "#00ff00",
      "scan_snr": "#ffff00",
      "s_meter": "#00dcdc"
    },
    "themeCount": 9
  }
  ```
- **POST**: accepts `idx=<N>` to switch theme, or individual color overrides `bg=%23ff0000`

### CSS Variable Injection

Every HTML page gets an inline `<style>` block with CSS variables:
```css
:root {
  --bg: #000814; --fg: #c0c0c0;
  --menu-bg: #040c18; --menu-border: #102030;
  --menu-item: #2080ff; --menu-hdr: #2080ff;
  --menu-hl-bg: #004080; --menu-hl-text: #ffffff;
  --param: #20ff20;
  --box-bg: #040c18; --box-border: #102030; --box-text: #c0c0c0;
  --box-off-bg: #400000; --box-off-text: #ffffff;
  --scan-rssi: #00ff00; --scan-snr: #ffff00;
  --s-meter: #00dcdc;
  --nav-bg: var(--menu-bg); --nav-text: var(--menu-item);
  --input-bg: var(--menu-bg); --input-border: var(--menu-border);
  --input-text: var(--fg);
  --button-bg: var(--menu-hl-bg); --button-text: var(--menu-hl-text);
}
```

The stylesheet (`webStyleSheet()`) references these variables instead of hardcoded colors. Re-theming the device immediately updates all pages on next refresh.

### Individual CSS variable overrides for the ColorTheme struct fields:
| ColorTheme field | CSS variable |
|-----------------|-------------|
| `bg` | `--bg` |
| `fg` | `--fg` |
| `menu_bg` | `--menu-bg` |
| `menu_border` | `--menu-border` |
| `menu_item` | `--menu-item` |
| `menu_hdr` | `--menu-hdr` |
| `menu_hl_bg` | `--menu-hl-bg` |
| `menu_hl_text` | `--menu-hl-text` |
| `menu_param` | `--param` |
| `box_bg` | `--box-bg` |
| `box_border` | `--box-border` |
| `box_text` | `--box-text` |
| `box_off_bg` | `--box-off-bg` |
| `box_off_text` | `--box-off-text` |
| `scan_rssi` | `--scan-rssi` |
| `scan_snr` | `--scan-snr` |
| `s_meter` | `--s-meter` |

### Files Changed
- `WebServer.cpp` — new `/api/theme` handler, CSS variable generation in `webPage()`, themed stylesheet
- `Themes.h` — expose `theme[]` array extern, `themeIdx` extern, `getTotalThemes()`, `ColorTheme` struct
- `Common.h` — `ColorTheme` struct is already in Common.h

## 5. FCC Database Integration (Future Phase)

Not implemented in this phase. Architecture reserved:
- Build-time Python script downloads FCC LMS data, produces compact binary
- Firmware loads index into RAM at startup, reads records from LittleFS
- New `Station.cpp:identifyFmFreq(freq)` function for binary search lookup
- Data format and exact schema defined when this phase begins

## 6. Enhanced Test Suite

### New test_serial.py Tests

**Web API coverage:**
- `test_web_api_tune_valid()` — tune to multiple freqs, verify JSON response and actual freq change
- `test_web_api_tune_out_of_band()` — verify 400 returned
- `test_web_api_tune_bad_value()` — verify 400 returned for non-numeric
- `test_web_api_volume()` — set to 0, 63, verify via `?` status
- `test_web_api_mute()` — toggle on/off, verify `audioIsMuted()` from status
- `test_web_api_band()` — next/prev, verify band name changes on status page
- `test_web_api_sleep()` — on/off, verify status field
- `test_web_api_invalid_cmd()` — verify 400
- `test_web_api_auth()` — with credentials set, verify /setconfig returns 401 without auth
- `test_web_api_theme_get` — GET /api/theme, verify all expected keys present
- `test_web_api_theme_set` — POST idx=0, verify theme changes (needs visual confirmation or revert)

**Scan-to-memory tests:**
- `test_scan_auto()` — run auto scan on current band, verify results populate memory slots
- `test_scan_manual()` — run manual scan, bookmark 3 freqs, stop, verify memory slots filled
- `test_scan_abort()` — start scan, abort mid-way, verify clean state
- `test_scan_invalid_mode()` — POST with bad mode, verify error

**HTML page rendering tests:**
- `test_status_page` — GET `/`, verify expected HTML elements and status data rendered
- `test_controls_page` — GET `/controls`, verify control elements present
- `test_scan_page` — GET `/scan`, verify scan UI rendered

**Theme sync tests:**
- `test_theme_get_json()` — GET /api/theme, verify idx, name, colors all present
- `test_theme_set_json()` — POST to change theme, GET to verify change persisted

**Regression:**
- All 27 existing tests continue to pass

### Files Changed
- `test_serial.py` — 15+ new test functions in existing patterns

---

## 7. File Change Summary

| File | Change | Impact |
|------|--------|--------|
| `WebServer.cpp` | Major: new pages, endpoints, theme sync, scan API | ~580 → ~1500 lines |
| `WebServer.h` | Minor: new exports | 1-2 new declarations |
| `Scan.cpp` | Major: new `scanToMemory()` | New function ~200 lines |
| `Scan.h` | Minor: new declarations | 4-5 new function signatures |
| `Station.cpp` | Minor: RDS dwell helper | ~40 lines |
| `Station.h` | Minor: new declaration | 1 new function |
| `Themes.h` | Minor: add externs | 2-3 new lines |
| `Common.h` | Already has ColorTheme struct | No change needed |
| `Menu.cpp` | Already exports memories | No change needed |
| `test_serial.py` | Major: 15+ new tests | ~700 → ~1200 lines |
| `docs/superpowers/specs/` | New design document | This file |

---

## 8. Verification

1. **Build**: `arduino-cli compile --fqbn esp32:esp32:esp32s3:<options>` — zero errors
2. **Web UI render**: navigate to `/`, `/controls`, `/memory`, `/scan`, `/config` — all render with correct content and navigation
3. **Controls**: volume slider, mute, mode, band, seek, tuning input, bandwidth, AGC, brightness all work and update device state
4. **Scan-to-memory (auto)**: scan completes, top N signals saved to memory, visible on /memory page
5. **Scan-to-memory (manual)**: bookmarks save on stop, memory slots populated correctly
6. **Theme sync**: change theme on device, web pages reflect new colors on refresh. Change via POST /api/theme, device screen updates
7. **Mobile**: layout reflows correctly at 375px and 480px widths
8. **Tests**: `python3 test_serial.py` — all 40+ tests pass
9. **Regression**: existing 27 tests still pass
