#include "Common.h"
#include "Utils.h"
#include "AudioManager.h"
#include "Menu.h"
#include "Tuning.h"
#include "Draw.h"
#include "Scan.h"
#include "Station.h"
#include "Storage.h"

// Tuning delays after rx.setFrequency()
#define TUNE_DELAY_DEFAULT 30
#define TUNE_DELAY_FM      60
#define TUNE_DELAY_AM_SSB  80

#define SCAN_POLL_TIME    10 // Tuning status polling interval (msecs)
#define SCAN_POINTS      200 // Number of frequencies to scan

// Waterfall scan state (distinct from SCAN_IDLE/RUNNING/DONE in Scan.h)
#define WSCAN_OFF   0
#define WSCAN_RUN   1
#define WSCAN_DONE  2

static struct
{
  uint8_t rssi;
  uint8_t snr;
} scanData[SCAN_POINTS];

static uint32_t scanTime = millis();
static uint8_t  scanState = WSCAN_OFF;

static uint16_t scanStartFreq;
static uint16_t scanStep;
static uint16_t scanCount;
static uint8_t  scanMinRSSI;
static uint8_t  scanMaxRSSI;
static uint8_t  scanMinSNR;
static uint8_t  scanMaxSNR;

static inline uint8_t min(uint8_t a, uint8_t b) { return(a<b? a:b); }
static inline uint8_t max(uint8_t a, uint8_t b) { return(a>b? a:b); }

// Candidate for auto mode ranking
typedef struct {
  uint16_t freq;
  uint8_t rssi;
} Candidate;

static int candidateDesc(const void *a, const void *b) {
  return ((const Candidate*)b)->rssi - ((const Candidate*)a)->rssi;
}

ScanStatus scanStatus = {0};

portMUX_TYPE scanStatusMux = portMUX_INITIALIZER_UNLOCKED;

// --- Async auto-scan state machine ---
// Splits scanToMemoryAuto() into tick-driven steps so the web handler
// never blocks for more than a few microseconds.
#define AUTO_PHASE_IDLE    0
#define AUTO_PHASE_SWEEP   1
#define AUTO_PHASE_PROCESS 2

typedef struct {
  uint8_t phase;           // AUTO_PHASE_*
  uint8_t count;           // target result count (clamped 1-20)
  Candidate *candidates;   // heap-allocated array (NULL when idle, freed on completion)
  uint16_t candidateCount;
  uint16_t freq;           // current sweep frequency
  uint16_t step;           // frequency step
  uint16_t totalSteps;     // for progress pct
  uint16_t stepCount;      // steps completed in current sweep
  uint16_t minFreq;        // band minimum
  uint16_t maxFreq;        // band maximum
  uint8_t slot;            // next empty memory slot for Phase 2
  uint8_t written;         // results saved in Phase 2
  uint16_t origFreq;       // restore values
  int origBfo;
  uint32_t settleStart;    // timestamp for 80ms settling
  uint32_t rdsStart;       // timestamp for RDS dwell
  bool settled;            // true once settle time has passed
  bool identified;         // true once station name obtained
  char nameBuf[12];        // station name buffer for Phase 2
} AutoScanState;

static AutoScanState autoScan = {0};

void scanCopyStatus(ScanStatus *dst)
{
  portENTER_CRITICAL(&scanStatusMux);
  memcpy(dst, &scanStatus, sizeof(ScanStatus));
  portEXIT_CRITICAL(&scanStatusMux);
}

float scanGetRSSI(uint16_t freq)
{
  // Input frequency must be in range of existing data
  if((scanState!=SCAN_DONE) || (freq<scanStartFreq) || (freq>=scanStartFreq+scanStep*scanCount))
    return(0.0);

  uint8_t result = scanData[(freq - scanStartFreq) / scanStep].rssi;
  return((result - scanMinRSSI) / (float)(scanMaxRSSI - scanMinRSSI + 1));
}

float scanGetSNR(uint16_t freq)
{
  // Input frequency must be in range of existing data
  if((scanState!=SCAN_DONE) || (freq<scanStartFreq) || (freq>=scanStartFreq+scanStep*scanCount))
    return(0.0);

  uint8_t result = scanData[(freq - scanStartFreq) / scanStep].snr;
  return((result - scanMinSNR) / (float)(scanMaxSNR - scanMinSNR + 1));
}

static void scanInit(uint16_t centerFreq, uint16_t step)
{
  if (step < 1) step = 1;
  scanStep    = step;
  scanCount   = 0;
  scanMinRSSI = 255;
  scanMaxRSSI = 0;
  scanMinSNR  = 255;
  scanMaxSNR  = 0;
  scanState  = WSCAN_RUN;
  scanTime    = millis();

  const Band *band = getCurrentBand();
  int freq = scanStep * (centerFreq / scanStep - SCAN_POINTS / 2);

  // Adjust to band boundaries
  if(freq + scanStep * (SCAN_POINTS - 1) > band->maximumFreq)
    freq = band->maximumFreq - scanStep * (SCAN_POINTS - 1);
  if(freq < band->minimumFreq)
    freq = band->minimumFreq;
  scanStartFreq = freq;

  // Clear scan data
  memset(scanData, 0, sizeof(scanData));
}

static bool scanTickTime()
{
  // Scan must be on
  if((scanState!=WSCAN_RUN) || (scanCount>=SCAN_POINTS)) return(false);

  // Wait for the right time
  if(millis() - scanTime < SCAN_POLL_TIME) return(true);

  // This is our current frequency to scan
  uint16_t freq = scanStartFreq + scanStep * scanCount;

  // Poll for the tuning status
  rx.getStatus(0, 0);
  if(!rx.getTuneCompleteTriggered())
  {
    scanTime = millis();
    return(true);
  }

  // If frequency not yet set, set it and wait until next call to measure
  if(rx.getCurrentFrequency() != freq)
  {
    rx.setFrequency(freq); // Implies tuning delay
    scanTime = millis() - SCAN_POLL_TIME;
    return(true);
  }

  // Measure RSSI/SNR values
  rx.getCurrentReceivedSignalQuality();
  scanData[scanCount].rssi = rx.getCurrentRSSI();
  scanData[scanCount].snr  = rx.getCurrentSNR();

  // Measure range of values
  scanMinRSSI = min(scanData[scanCount].rssi, scanMinRSSI);
  scanMaxRSSI = max(scanData[scanCount].rssi, scanMaxRSSI);
  scanMinSNR  = min(scanData[scanCount].snr, scanMinSNR);
  scanMaxSNR  = max(scanData[scanCount].snr, scanMaxSNR);

  // Next frequency to scan
  freq += scanStep;

  // Set next frequency to scan or expire scan
  if((++scanCount >= SCAN_POINTS) || !isFreqInBand(getCurrentBand(), freq) || consumeAbortPending())
    scanState = SCAN_DONE;
  else
    rx.setFrequency(freq); // Implies tuning delay

  // Save last scan time
  scanTime = millis() - SCAN_POLL_TIME;

  // Return current scan status
  return(scanState==WSCAN_RUN);
}

//
// Run entire scan once
//
void scanRun(uint16_t centerFreq, uint16_t step)
{
  // Set tuning delay
  rx.setMaxDelaySetFrequency(radioState.mode == FM ? TUNE_DELAY_FM : TUNE_DELAY_AM_SSB);
  // Mute the audio
  audioTempMute(true);
  // Flag is set by rotary encoder and cleared on seek/scan entry
  seekStop = false;
  // Save current frequency
  uint16_t curFreq = rx.getFrequency();
  // Scan the whole range
  for(scanInit(centerFreq, step) ; scanTickTime(););
  // Restore current frequency
  rx.setFrequency(curFreq);
  // Unmute the audio
  audioTempMute(false);
  // Restore tuning delay
  rx.setMaxDelaySetFrequency(TUNE_DELAY_DEFAULT);
}

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

  {
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

      // delay(1) yields to FreeRTOS, feeding the watchdog
      delay(1);

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
  }

restore:
  updateFrequency(origFreq, false);
  if (isSSB()) updateBFO(origBfo, false);
  audioTempMute(false);
  clearStationInfo();
  identifyFrequency(getEffectiveFreq());
}

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
  int i = scanStatus.bookmarkCount;
  scanStatus.bookmarks[i] = scanStatus.currentFreq;
  scanStatus.bookmarkRSSI[i] = radioState.rssi;
  scanStatus.bookmarkCount++;
}

void scanStop() {
  if (scanStatus.running != SCAN_RUNNING) return;
  scanStatus.running = SCAN_DONE;

  uint8_t slot = 0;
  for (int i = 0; i < MEMORY_COUNT; i++) {
    if (memories[i].freq == 0) { slot = i; break; }
  }

  uint16_t origFreq = radioState.frequency;
  int origBfo = radioState.bfo;
  uint8_t written = 0;

  audioTempMute(true);

  for (int i = 0; i < scanStatus.bookmarkCount && written < MEMORY_COUNT; i++) {
    // Find the next empty slot (there may be gaps)
    while (slot < MEMORY_COUNT && memories[slot].freq != 0) slot++;
    if (slot >= MEMORY_COUNT) break;
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
    scanStatus.results[written].rssi = scanStatus.bookmarkRSSI[i];
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

// --- Async auto-scan implementation ---

static void autoCleanup()
{
  if (autoScan.candidates) {
    heap_caps_free(autoScan.candidates);
    autoScan.candidates = NULL;
  }
  autoScan.phase = AUTO_PHASE_IDLE;
}

void scanRequestAuto(uint8_t count)
{
  if (count > 20) count = 20;
  if (count == 0) count = 10;
  if (autoScan.phase != AUTO_PHASE_IDLE) return;

  Band *band = getCurrentBand();
  uint16_t step = getCurrentStep()->step;
  if (radioState.mode != FM && step < 5) step = 5;

  Candidate *cands = (Candidate *)heap_caps_malloc(
    sizeof(Candidate) * MAX_STEP_CANDIDATES, MALLOC_CAP_8BIT);
  if (!cands) return;

  memset(&autoScan, 0, sizeof(autoScan));
  autoScan.candidates = cands;
  autoScan.count = count;
  autoScan.freq = band->minimumFreq;
  autoScan.step = step;
  autoScan.totalSteps = ((band->maximumFreq - band->minimumFreq) / step) + 1;
  if (autoScan.totalSteps > MAX_STEP_CANDIDATES)
    autoScan.totalSteps = MAX_STEP_CANDIDATES;
  autoScan.minFreq = band->minimumFreq;
  autoScan.maxFreq = band->maximumFreq;
  autoScan.origFreq = radioState.frequency;
  autoScan.origBfo = radioState.bfo;
  autoScan.phase = AUTO_PHASE_SWEEP;

  portENTER_CRITICAL(&scanStatusMux);
  memset(&scanStatus, 0, sizeof(scanStatus));
  scanStatus.running = SCAN_RUNNING;
  scanStatus.mode = 0;
  portEXIT_CRITICAL(&scanStatusMux);

  audioTempMute(true);
  seekStop = false;
  if (isSSB()) updateBFO(0, true);
}

void scanProcessTick()
{
  if (autoScan.phase == AUTO_PHASE_IDLE) return;

  // Check abort
  if (consumeAbortPending() || seekStop) {
    goto doAbort;
  }

  // Yield to FreeRTOS watchdog
  delay(1);

  if (autoScan.phase == AUTO_PHASE_SWEEP) {
    // First call for this freq: tune and start settle timer
    if (!autoScan.settled) {
      if (isSSB()) updateBFO(0, true);
      if (updateFrequency(autoScan.freq, false)) {
        autoScan.settleStart = millis();
        autoScan.settled = true;
      }
      return;
    }

    // Wait for 80ms settling
    if (millis() - autoScan.settleStart < 80)
      return;

    // Measure RSSI/SNR
    rx.getCurrentReceivedSignalQuality();
    if (autoScan.candidateCount < MAX_STEP_CANDIDATES) {
      autoScan.candidates[autoScan.candidateCount].freq = autoScan.freq;
      autoScan.candidates[autoScan.candidateCount].rssi = rx.getCurrentRSSI();
      autoScan.candidateCount++;
    }

    portENTER_CRITICAL(&scanStatusMux);
    scanStatus.currentFreq = autoScan.freq;
    scanStatus.currentRSSI = rx.getCurrentRSSI();
    scanStatus.currentSNR = rx.getCurrentSNR();
    scanStatus.progress = (uint8_t)((uint32_t)autoScan.stepCount * 50 / autoScan.totalSteps);
    portEXIT_CRITICAL(&scanStatusMux);

    // Advance to next frequency
    autoScan.freq += autoScan.step;
    autoScan.stepCount++;
    autoScan.settled = false;

    // Check if sweep complete
    if (autoScan.freq > autoScan.maxFreq || autoScan.stepCount >= MAX_STEP_CANDIDATES) {
      qsort(autoScan.candidates, autoScan.candidateCount, sizeof(Candidate), candidateDesc);

      // Find first empty memory slot
      for (int i = 0; i < MEMORY_COUNT; i++) {
        if (memories[i].freq == 0) { autoScan.slot = i; break; }
      }

      autoScan.phase = AUTO_PHASE_PROCESS;
      autoScan.settled = false;
    }
    return;
  }

  if (autoScan.phase == AUTO_PHASE_PROCESS) {
    // Process each candidate one per tick
    while (autoScan.written < autoScan.count && autoScan.slot < MEMORY_COUNT) {
      Candidate *c = &autoScan.candidates[autoScan.written];
      if (c->rssi == 0) { autoScan.written++; continue; }

      // First call for this candidate: tune and settle
      if (!autoScan.settled) {
        if (isSSB()) updateBFO(0, true);
        updateFrequency(c->freq, false);
        autoScan.settleStart = millis();
        autoScan.settled = true;
        autoScan.rdsStart = 0;
        autoScan.identified = false;
        autoScan.nameBuf[0] = '\0';
        return;
      }

      // Wait for settling
      if (millis() - autoScan.settleStart < 80)
        return;

      // Station identification
      if (!autoScan.identified) {
        if (radioState.mode == FM) {
          if (autoScan.rdsStart == 0) {
            clearStationInfo();
            autoScan.rdsStart = millis();
          }
          checkRds();
          const char *n = getStationName();
          if (n && n[0] && n[0] != '*') {
            strlcpy(autoScan.nameBuf, n, sizeof(autoScan.nameBuf));
            autoScan.identified = true;
          } else if (millis() - autoScan.rdsStart > 1500) {
            autoScan.identified = true; // timeout, name stays empty
          } else {
            return; // Next tick checks RDS again
          }
        } else {
          identifyFrequency(c->freq, false);
          const char *n = getStationName();
          if (n && n[0] && n[0] != '*')
            strlcpy(autoScan.nameBuf, n, sizeof(autoScan.nameBuf));
          autoScan.identified = true;
        }
        if (!autoScan.identified) return;
      }

      // Save to memory
      uint32_t fHz = (uint32_t)c->freq * 1000;
      memories[autoScan.slot].freq = fHz;
      memories[autoScan.slot].band = bandIdx;
      memories[autoScan.slot].mode = radioState.mode;
      strlcpy(memories[autoScan.slot].name, autoScan.nameBuf, sizeof(memories[autoScan.slot].name));
      prefsSaveMemory(autoScan.slot, true);

      portENTER_CRITICAL(&scanStatusMux);
      scanStatus.results[autoScan.written].slot = autoScan.slot + 1;
      scanStatus.results[autoScan.written].freq = c->freq;
      scanStatus.results[autoScan.written].rssi = c->rssi;
      strlcpy(scanStatus.results[autoScan.written].name, autoScan.nameBuf, 12);
      scanStatus.progress = (uint8_t)(50 + (uint32_t)autoScan.written * 50 / autoScan.count);
      portEXIT_CRITICAL(&scanStatusMux);

      autoScan.written++;
      autoScan.slot++;
      autoScan.settled = false;
      autoScan.identified = false;
    }

    // All candidates processed
    goto doFinish;
  }

  return;

doAbort:
  audioTempMute(false);
  updateFrequency(autoScan.origFreq, false);
  if (isSSB()) updateBFO(autoScan.origBfo, false);
  clearStationInfo();
  identifyFrequency(getEffectiveFreq());
  portENTER_CRITICAL(&scanStatusMux);
  scanStatus.running = SCAN_ABORTED;
  portEXIT_CRITICAL(&scanStatusMux);
  autoCleanup();
  return;

doFinish:
  audioTempMute(false);
  updateFrequency(autoScan.origFreq, false);
  if (isSSB()) updateBFO(autoScan.origBfo, false);
  clearStationInfo();
  identifyFrequency(getEffectiveFreq());
  portENTER_CRITICAL(&scanStatusMux);
  scanStatus.running = SCAN_DONE;
  scanStatus.resultCount = autoScan.written;
  portEXIT_CRITICAL(&scanStatusMux);
  autoCleanup();
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
