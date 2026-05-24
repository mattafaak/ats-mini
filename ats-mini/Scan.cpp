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

#define SCAN_OFF    0   // Scanner off, no data
#define SCAN_RUN    1   // Scanner running
#define SCAN_DONE   2   // Scanner done, valid data in scanData[]

static struct
{
  uint8_t rssi;
  uint8_t snr;
} scanData[SCAN_POINTS];

static uint32_t scanTime = millis();
static uint8_t  scanState = SCAN_OFF;

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
  scanStep    = step;
  scanCount   = 0;
  scanMinRSSI = 255;
  scanMaxRSSI = 0;
  scanMinSNR  = 255;
  scanMaxSNR  = 0;
  scanState  = SCAN_RUN;
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
  if((scanState!=SCAN_RUN) || (scanCount>=SCAN_POINTS)) return(false);

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
  return(scanState==SCAN_RUN);
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
