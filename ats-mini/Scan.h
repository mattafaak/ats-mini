#ifndef SCAN_H
#define SCAN_H

#include <stdint.h>
#include <stddef.h>

#define SCAN_POINTS 200

// Spectrum scan (existing -- used by waterfall display)
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
