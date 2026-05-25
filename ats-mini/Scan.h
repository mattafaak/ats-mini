#ifndef SCAN_H
#define SCAN_H

#include <stdint.h>
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define SCAN_POINTS 200
#define MAX_STEP_CANDIDATES SCAN_POINTS

// Spectrum scan (existing -- used by waterfall display)
void scanRun(uint16_t centerFreq, uint16_t step);
float scanGetRSSI(uint16_t freq);
float scanGetSNR(uint16_t freq);

// Scan-to-memory status
#define SCAN_IDLE    0
#define SCAN_RUNNING 1
#define SCAN_DONE    2
#define SCAN_ABORTED 3

// Auto scan phases for tick-driven processing
#define SCAN_PHASE_IDLE    0
#define SCAN_PHASE_SWEEP   1
#define SCAN_PHASE_PROCESS 2

typedef struct {
  uint8_t running;
  uint8_t mode;         // 0=auto, 1=manual
  uint16_t currentFreq;
  uint8_t currentRSSI;
  uint8_t currentSNR;
  uint8_t progress;     // 0-100
  uint8_t bookmarkCount;
  uint16_t bookmarks[20];
  uint8_t bookmarkRSSI[20];
  uint8_t resultCount;
  struct {
    uint8_t slot;       // 1-99
    uint16_t freq;      // kHz
    uint8_t rssi;
    char name[12];
  } results[20];
} ScanStatus;

extern ScanStatus scanStatus;

// Thread-safe access (web task reads, main loop writes)
extern portMUX_TYPE scanStatusMux;

void scanToMemoryAuto(uint8_t count);
void scanToMemoryManual();
void scanManualStep();
void scanBookmark();
void scanStop();
void scanAbort();
bool scanIsRunning();
const ScanStatus* scanGetStatus();

// Async auto-scan: non-blocking, tick-driven from main loop
void scanRequestAuto(uint8_t count);
void scanProcessTick();

// Thread-safe status copy for web handler
void scanCopyStatus(ScanStatus *dst);

#endif
