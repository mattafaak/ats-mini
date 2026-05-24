#ifndef TUNING_H
#define TUNING_H

#include <stdint.h>
#include <stdbool.h>
#include "Common.h"

// Tuning and command dispatch functions.
// Extracted from ats-mini.ino into a dedicated module.

// ISR-safe encoder acceleration
int16_t accelerateEncoder(int8_t dir);

// Hardware band switching
void useBand(const Band *band);

// Frequency and BFO tuning
bool updateFrequency(int newFreq, bool wrap = true);
bool updateBFO(int newBFO, bool wrap = true);

// Seek, tune, and digit input commands
bool doSeek(int16_t enc, int16_t enca);
bool doTune(int16_t enc);
bool doDigit(int16_t enc);
bool clickFreq(bool shortPress);

// Seek progress callback
void showFrequencySeek(uint16_t freq);

// Lightweight abort check for blocking operations
bool consumeAbortPending();

// RSSI/SNR polling and squelch processing
bool processRssiSnr();

#endif
