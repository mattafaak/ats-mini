#ifndef STATION_H
#define STATION_H

#include <stdint.h>
#include <stdbool.h>

const char *getStationName();
const char *getRadioText();
const char *getProgramInfo();
uint16_t getRdsPiCode();
void clearStationInfo();
bool checkRds();
bool identifyFrequency(uint16_t freq, bool periodic = false);

#endif
