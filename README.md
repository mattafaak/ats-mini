# ATS Mini

![](docs/source/_static/esp32-si4732-ui-theme.jpg)

This firmware is for use on the SI4732 (ESP32-S3) Mini/Pocket Receiver

Based on the following sources:

* Volos Projects:    https://github.com/VolosR/TEmbedFMRadio
* PU2CLR, Ricardo:   https://github.com/pu2clr/SI4735
* Ralph Xavier:      https://github.com/ralphxavier/SI4735
* Goshante:          https://github.com/goshante/ats20_ats_ex
* G8PTN, Dave:       https://github.com/G8PTN/ATS_MINI

## Releases

Check out the [Releases](https://github.com/esp32-si4732/ats-mini/releases) page.

## Documentation

The hardware, software and flashing documentation is available at <https://esp32-si4732.github.io/ats-mini/>

## Discuss

* [GitHub Discussions](https://github.com/esp32-si4732/ats-mini/discussions) - the best place for feature requests, observations, sharing, etc.
* [TalkRadio Telegram Chat](https://t.me/talkradio/174172) - informal space to chat in Russian and English.

---

## Fork Changes

This fork ([mattafaak/ats-mini](https://github.com/mattafaak/ats-mini)) contains significant refactoring, new features, and stability fixes beyond the upstream.

### Web UI Overhaul

The web dashboard has been rebuilt with five tabbed pages and live polling:

- **Status** — signal bars, station info (RDS/EiBi name), RSSI/SNR, audio mute state, WiFi status, clock, and memory list. Auto-refreshes every 5 seconds via `/api/status`.
- **Controls** — full radio control from the browser: tune (kHz/Hz input), volume, mute, band/mode/step/bandwidth cycling, AGC/ATTN, seek up/down, sleep, and brightness. All controls use `fetch()` POST to `/api/command`.
- **Memory** — 99-slot memory table showing slot, frequency, band, mode, and station name (from EiBi or RDS). Names are saved alongside frequency on memory store.
- **Scan** — scan-to-memory with auto (top-N by RSSI) and manual (bookmark) modes. Progress bar, real-time frequency/RSSI display, result table with station names.
- **Config** — unchanged from upstream (WiFi credentials, theme, UTC offset, firmware update).

**Theme sync**: `/api/theme` serves the current theme's colors as CSS custom properties. All pages dynamically re-theme without a full reload.

### Scan-to-Memory

Full auto-scan and manual-scan-to-memory, driven asynchronously from the main loop:

- **Auto mode**: sweeps the current band measuring RSSI per step, selects the top-N frequencies, tunes each with an 80ms settle and RDS dwell (FM), then saves to the next empty memory slots with station name.
- **Manual mode**: tune to a frequency and bookmark it via the Scan page. Stop the scan to save all bookmarks to memory.
- **Non-blocking**: the sweep and save phases run one step per main-loop tick, so the web server never blocks. The web UI polls `/api/scan` for progress (0-100%) and results.
- **EiBi integration**: on SW bands, station names are identified via EiBi database matching during the save phase.

### Stability & Correctness Fixes

- **BLE thread safety**: all NimBLE ring buffer operations (`rxBuf`/`txBuf`) are guarded by `portMUX_TYPE` critical sections to prevent data corruption on dual-core ESP32-S3 between the NimBLE host task and the main loop.
- **BLE `flush()` timeout**: prevented infinite spin when `onStatus` never fires by adding a 500ms timeout.
- **WiFi reconnection watchdog**: detects link loss and reconnects immediately (bypassing the 3-second CONNECT_TIME debounce) with a 10-second cooldown to prevent rapid cycling.
- **Audio mute state machine**: `audioTempMute(false)` and `audioMuteForce(false)` no longer corrupt the `effectiveMuted` state when squelch or main mute is active.
- **Web security**: HTTP basic auth enforced on all sensitive endpoints (`/setconfig`, `/api/command`, `/api/theme`, `/api/scan`). Station/program names are JSON-escaped to prevent injection.
- **SSB frequency display**: negative BFO values no longer wrap to garbage due to unsigned arithmetic.
- **`millis()` overflow**: all periodic-task timestamp variables changed from signed `long` to `uint32_t`, preventing runaway behavior after 24.8 days of uptime.
- **EIBI**: fixed binary search off-by-one (`right = total - 1`) and unsigned underflow in `eibiPrev()`.
- **Menu corruption**: short menus (Sleep mode, Mode) no longer show duplicate entries.
- **Scan correctness**: per-frequency RSSI stored at bookmark time (not stop time). Non-contiguous empty memory slots handled correctly.
- **BLE light sleep**: `bleStop()`/`bleInit()` called around CPU light sleep so NimBLE survives sleep cycles.
- **BLE Central disconnect check**: fixed inverted condition that prevented reconnect on disconnect failure.

### Performance

- Adaptive main-loop delay (5ms during activity, 20-50ms at idle) reduces CPU wakeups.
- Background redraw conditioned on minute change instead of every loop.
- `F()` macro for hot-path display strings to keep strings in flash.
- Reduced blocking delays in WiFi init and seek operations.
- `pow()` for integer step computation replaced with a lookup table.
- `String::reserve()` used in all HTML builders to avoid O(n²) reallocation.
- Scan-to-memory candidates heap-allocated instead of 800+ bytes on the 8KB web server task stack.
- Menu changes save only settings (`SAVE_SETTINGS`) instead of all bands and memories (`SAVE_ALL`), reducing NVS wear.

### Internal Refactoring

- **`RadioState` struct**: all global state consolidated into a single `RadioState radioState` struct for improved cache locality and debuggability.
- **Module extraction**: `EventHandler`, `Scheduler`, `WiFiManager`, `WebServer`, `Tuning`, `AudioManager` (4-channel mute state machine), `DisplayController` (ST7789 sleep/brightness), `MenuDraw`, `MenuData`, and `RadioDriver` (SI4732 abstraction) extracted from monolithic files into focused modules.
- **`BleBase`**: shared BLE lifecycle and abort-flag logic consolidated into a single header.
- **`Tuning.h`**: tuning and BFO commands factored out of `ats-mini.ino`.
- **Compatibility macros** removed after full migration.

### Testing

The test suite has grown from 27 to 44 tests covering:

- Serial command protocol (volume, mode, step, bandwidth, band, AGC/ATTN, frequency entry, brightness, sleep, calibration, memory, seek, scan, abort, timeout recovery)
- Web API (`/api/status`, `/api/command` tune/volume/mute/band/seek, `/api/theme` get/set, `/api/scan` auto/invalid)
- HTML page loading (status, controls, scan, memory)
- Theme sync verification
- Volume adjustment on quiet frequencies (no audio artifacts)
- Mute-before-operation pattern for all band/frequency changes

Run with: `python3 ats-mini/test_serial.py` (requires `pyserial`).

### Attributions

Based on [esp32-si4732/ats-mini](https://github.com/esp32-si4732/ats-mini) by the original authors listed above. Fork maintainer: [@mattafaak](https://github.com/mattafaak).
