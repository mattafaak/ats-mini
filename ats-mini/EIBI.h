#ifndef EIBI_H
#define EIBI_H

struct BandLabel
{
  uint16_t freq_start;  // Starting frequency
  uint16_t freq_end;    // Ending frequency
  const char *name;     // Band name
};

struct StationSchedule
{
  uint16_t freq;        // Frequency in kHz
  int8_t   start_h;     // Starting hour (0..23, -1 = any)
  int8_t   start_m;     // Starting minute
  int8_t   end_h;       // Ending hour
  int8_t   end_m;       // Ending minute
  char     name[32];    // Station name (UTF-8)
};

bool eibiAvailable();
bool eibiLoadSchedule();
const StationSchedule *eibiLookup(uint16_t freq, uint8_t hour, uint8_t minute, size_t *offset=NULL);
const StationSchedule *eibiPrev(uint16_t freq, uint8_t hour, uint8_t minute, size_t *offset);
const StationSchedule *eibiNext(uint16_t freq, uint8_t hour, uint8_t minute, size_t *offset);
const StationSchedule *eibiAtSameFreq(uint8_t hour, uint8_t minute, size_t *offset, bool same);

// High-level lookup that owns offset tracking internally.
// Returns station name for the given frequency and time, or NULL.
// Handles periodic re-check via internal static state.
const char *eibiFindName(uint16_t freq, uint8_t hour, uint8_t minute, bool periodic);

#endif // EIBI_H
