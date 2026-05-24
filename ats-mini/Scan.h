#ifndef SCAN_H
#define SCAN_H

#include <stdint.h>

void scanRun(uint16_t centerFreq, uint16_t step);
float scanGetRSSI(uint16_t freq);
float scanGetSNR(uint16_t freq);

#endif
