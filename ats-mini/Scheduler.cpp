#include "Common.h"
#include "WiFiManager.h"
#include "Station.h"
#include "Tuning.h"
#include "Utils.h"
#include "Menu.h"
#include "Storage.h"
#include "Scheduler.h"

extern uint32_t elapsedRSSI;
extern uint32_t lastRDSCheck;
extern uint32_t lastScheduleCheck;
extern uint32_t lastNTPCheck;
extern uint32_t background_timer;

bool runScheduler(uint32_t currentTime)
{
  bool needRedraw = false;

  if((currentTime - elapsedRSSI) > MIN_ELAPSED_RSSI_TIME)
  {
    needRedraw |= processRssiSnr();
    elapsedRSSI = currentTime;
  }

  if((currentTime - lastRDSCheck) > RDS_CHECK_TIME)
  {
    needRedraw |= (radioState.mode == FM) && (radioState.snr >= 12) && checkRds();
    lastRDSCheck = currentTime;
  }

  if((currentTime - lastScheduleCheck) > SCHEDULE_CHECK_TIME)
  {
    needRedraw |= identifyFrequency(getEffectiveFreq(), true);
    lastScheduleCheck = currentTime;
  }

  if((currentTime - lastNTPCheck) > NTP_CHECK_TIME)
  {
    needRedraw |= ntpSyncTime();
    lastNTPCheck = currentTime;
  }

  prefsTickTime();
  netTickTime();
  needRedraw |= clockTickTime();

  if(needRedraw) background_timer = currentTime;
  if((currentTime - background_timer) > BACKGROUND_REFRESH_TIME)
  {
    if(radioState.cmd == CMD_NONE)
    {
      // Only force redraw when the clock minute has changed, so the time
      // display stays current without wasting CPU cycles on idle frames.
      static uint8_t lastBgMinute = 0;
      uint8_t h, m;
      if(clockGetHM(&h, &m) && m != lastBgMinute) {
        lastBgMinute = m;
        needRedraw = true;
      }
    }
    background_timer = currentTime;
  }

  return needRedraw;
}
