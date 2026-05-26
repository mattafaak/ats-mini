#ifndef BLE_BASE_H
#define BLE_BASE_H

#include <BLEDevice.h>

namespace BleBase {

extern bool initialized;

inline void init(const char* deviceName)
{
    if (!initialized) {
        initialized = true;
        BLEDevice::init(deviceName);
    }
}

inline void reset()
{
    initialized = false;
}

// Atomic-style read-and-clear for volatile abort flags.
// Returns true if the flag was set before clearing.
static inline bool consumeAbortFlag(volatile bool& flag)
{
    bool pending = flag;
    flag = false;
    return pending;
}

}  // namespace BleBase

#endif
