#include <Arduino.h>
#include "Common.h"
#include "Rotary.h"
#include "Button.h"
#include "Menu.h"
#include "Draw.h"
#include "Utils.h"
#include "AudioManager.h"
#include "EIBI.h"
#include "Remote.h"
#include "BleMode.h"
#include "Station.h"
#include "Tuning.h"

// External variables defined in ats-mini.ino
extern ButtonTracker pb1;

// RSSI/SNR display update throttling counter
static uint32_t rssiUpdateCounter = 0;

// Force the RSSI/SNR display to refresh on the next poll cycle.
// Call after frequency or band changes so the display doesn't show stale values.
void resetRssiSnrDisplay()
{
  rssiUpdateCounter = 31;  // next (++ & 7) == 0 → immediate update
}

ICACHE_RAM_ATTR int16_t accelerateEncoder(int8_t dir)
{
  const uint32_t speedThresholds[] = {350, 60, 45, 35, 25}; // ms between clicks
  const uint16_t accelFactors[] =      {1,  2,  4,  8, 16}; // corresponding multipliers
  static volatile uint32_t lastEncoderTime = 0;
  static volatile uint32_t lastSpeed = speedThresholds[0];
  static volatile uint16_t lastAccelFactor = accelFactors[0];
  static volatile int8_t lastEncoderDir = 0;

  uint32_t currentTime = millis();
  lastSpeed = ((currentTime - lastEncoderTime) * 7 + lastSpeed * 3) / 10;

  // Reset acceleration on timeout or direction change
  if (lastSpeed > speedThresholds[0] || lastEncoderDir != dir) {
    lastSpeed = speedThresholds[0];
    lastAccelFactor = accelFactors[0];
  } else {
    // Lookup acceleration factor
    for (int8_t i = LAST_ITEM(speedThresholds); i >= 0; i--) {
      if (lastSpeed <= speedThresholds[i] && lastAccelFactor < accelFactors[i]) {
        lastAccelFactor = accelFactors[i];
        break;
      }
    }
  }
  lastEncoderTime = currentTime;
  lastEncoderDir = dir;

  // Apply acceleration with direction
  return(dir * lastAccelFactor);
}

//
// Switch radio to given band
//
void useBand(const Band *band)
{
  // Set current frequency and mode, reset BFO (atomic for web task reads)
  taskENTER_CRITICAL(&radioStateMux);
  radioState.frequency = band->currentFreq;
  radioState.mode = band->bandMode;
  radioState.bfo = 0;
  taskEXIT_CRITICAL(&radioStateMux);

  if(band->bandMode==FM)
  {
    rx.setFM(band->minimumFreq, band->maximumFreq, band->currentFreq, getCurrentStep()->step);
    rx.setSeekFmLimits(band->minimumFreq, band->maximumFreq);

    // More sensitive seek thresholds
    // https://github.com/pu2clr/SI4735/issues/7#issuecomment-810963604
    rx.setSeekFmRssiThreshold(5); // default is 20
    rx.setSeekFmSNRThreshold(2); // default is 3

    rx.setFMDeEmphasis(fmRegions[radioState.fmRegionIdx].value);
    rx.RdsInit();
    rx.setRdsConfig(1, 2, 2, 2, 2);
    rx.setGpioCtl(1, 0, 0);   // G8PTN: Enable GPIO1 as output
    rx.setGpio(0, 0, 0);      // G8PTN: Set GPIO1 = 0
  }
  else
  {
    if(band->bandMode==AM)
    {
      rx.setAM(band->minimumFreq, band->maximumFreq, band->currentFreq, getCurrentStep()->step);
      // More sensitive seek thresholds
      // https://github.com/pu2clr/SI4735/issues/7#issuecomment-810963604
      rx.setSeekAmRssiThreshold(10); // default is 25
      rx.setSeekAmSNRThreshold(3); // default is 5
    }
    else
    {
      // Configure SI4732 for SSB (SI4732 step not used, set to 0)
      rx.setSSB(band->minimumFreq, band->maximumFreq, band->currentFreq, 0, radioState.mode);
      // G8PTN: Always enabled
      rx.setSSBAutomaticVolumeControl(1);
      // G8PTN: Commented out
      // To move frequency forward, need to move the BFO backwards
      if (radioState.mode == USB)
        rx.setSSBBfo(-(radioState.bfo + band->usbCal));
      else if (radioState.mode == LSB)
        rx.setSSBBfo(-(radioState.bfo + band->lsbCal));
      else
        rx.setSSBBfo(-radioState.bfo);  // No calibration if not USB/LSB
    }


    // G8PTN: Enable GPIO1 as output
    rx.setGpioCtl(1, 0, 0);
    // G8PTN: Set GPIO1 = 1
    rx.setGpio(1, 0, 0);
    // Consider the range all defined current band
    rx.setSeekAmLimits(band->minimumFreq, band->maximumFreq);
  }

  // Set step and spacing based on mode (FM, AM, SSB)
  doStep(0);
  // Set softMuteMaxAtt based on mode (AM, SSB)
  doSoftMute(0);
  // Set disableAgc and agcNdx values based on mode (FM, AM , SSB)
  doAgc(0);
  // Set currentAVC values based on mode (AM, SSB)
  doAvc(0);
  // Wait a bit for things to calm down
  delay(100);
  // Clear signal strength readings
  radioState.rssi = 0;
  radioState.snr  = 0;
}

//
// Tune using BFO, using algorithm from Goshante's ATS-20_EX firmware
//
bool updateBFO(int newBFO, bool wrap)
{
  Band *band = getCurrentBand();
  int newFreq = radioState.frequency;

  // No BFO outside SSB modes
  if(!isSSB()) newBFO = 0;

  // If new BFO exceeds allowed bounds...
  if(newBFO > MAX_BFO || newBFO < -MAX_BFO)
  {
    // Compute correction
    int fCorrect = (newBFO / MAX_BFO) * MAX_BFO;
    // Correct new frequency and BFO
    newFreq += fCorrect / 1000;
    newBFO  -= fCorrect;
  }

  // Do not let new frequency exceed band limits
  int f = newFreq * 1000 + newBFO;
  if(f < band->minimumFreq * 1000)
  {
    if(!wrap) return false;
    newFreq = band->maximumFreq;
    newBFO  = 0;
  }
  else if(f > band->maximumFreq * 1000)
  {
    if(!wrap) return false;
    newFreq = band->minimumFreq;
    newBFO  = 0;
  }

  // If need to change frequency...
  if(newFreq != radioState.frequency)
  {
    // Apply new frequency
    rx.setFrequency(newFreq);

    // Re-apply to remove noise
    doAgc(0);
    // Update current frequency
    radioState.frequency = rx.getFrequency();
  }

  // Update current BFO
  radioState.bfo = newBFO;

  // To move frequency forward, need to move the BFO backwards
  if (radioState.mode == USB)
    rx.setSSBBfo(-(radioState.bfo + band->usbCal));
  else if (radioState.mode == LSB)
    rx.setSSBBfo(-(radioState.bfo + band->lsbCal));
  else
    rx.setSSBBfo(-radioState.bfo);  // No calibration if not USB/LSB

  // Save current band frequency, w.r.t. new BFO value
  band->currentFreq = radioState.frequency + radioState.bfo / 1000;
  return true;
}

//
// Tune to a new frequency, resetting BFO if present
//
bool updateFrequency(int newFreq, bool wrap)
{
  Band *band = getCurrentBand();

  // Do not let new frequency exceed band limits
  if(newFreq < band->minimumFreq)
  {
    if(!wrap) return false; else newFreq = band->maximumFreq;
  }
  else if(newFreq > band->maximumFreq)
  {
    if(!wrap) return false; else newFreq = band->minimumFreq;
  }

  // Set new frequency
  rx.setFrequency(newFreq);

  // Clear BFO, if present
  if(radioState.bfo) updateBFO(0, true);

  // Update current frequency
  radioState.frequency = rx.getFrequency();

  // Save current band frequency
  band->currentFreq = radioState.frequency + radioState.bfo / 1000;

  // Accelerate RSSI/SNR display update so the viewer sees the new signal
  // within 200ms instead of waiting up to 1.6s for the throttled counter.
  resetRssiSnrDisplay();
  return true;
}

// This function is called by blocking operations that need a lightweight abort check.
bool consumeAbortPending()
{
  noInterrupts();
  bool pending = seekStop;
  seekStop = false;
  interrupts();
  if(pending) return true;
  if(bleConsumeAbortPending(radioState.bleMode)) return true;
  if(serialConsumeAbortPending(radioState.usbMode)) return true;

  // Checking isPressed without debouncing because this helper is used from
  // blocking operations that do not run the normal event loop often enough.
  if(pb1.update(digitalRead(ENCODER_PUSH_BUTTON) == LOW, 0).isPressed)
  {
    // Wait till the button is released, otherwise the main loop will register a click
    { uint32_t _t = millis(); while(pb1.update(digitalRead(ENCODER_PUSH_BUTTON) == LOW).isPressed) { if(millis() - _t > 5000) break; delay(100); } }
    return true;
  }

  return false;
}

// This function is called by the seek function process.
void showFrequencySeek(uint16_t freq)
{
  radioState.frequency = freq;
  drawScreen();
}

//
// Handle encoder rotation in seek mode
//
bool doSeek(int16_t enc, int16_t enca)
{
  // SSB seek is BFO-only (no frequency change), so no audio pop — skip mute.
  if(!isSSB()) audioTempMute(true);

  if(seekMode() == SEEK_DEFAULT)
  {
    if(isSSB())
    {
      updateBFO(radioState.bfo + enca * getCurrentStep()->step, true);
    }
    else
    {
      // Clear stale parameters
      clearStationInfo();
      radioState.rssi = radioState.snr = 0;

      // Clear stale abort state before starting seek
      consumeAbortPending();
      rx.seekStationProgress(showFrequencySeek, consumeAbortPending, enc>0? 1 : 0);
      updateFrequency(rx.getFrequency(), true);
    }
  }
  else if(seekMode() == SEEK_SCHEDULE && enc)
  {
    uint8_t hour, minute;
    // Clock is valid because the above seekMode() call checks that
    clockGetHM(&hour, &minute);

    size_t offset = -1;
    const StationSchedule *schedule = enc > 0 ?
      eibiNext(radioState.frequency + radioState.bfo / 1000, hour, minute, &offset) :
      eibiPrev(radioState.frequency + radioState.bfo / 1000, hour, minute, &offset);

    if(schedule) updateFrequency(schedule->freq, false);
  }

  // Clear current station name and information
  clearStationInfo();
  // Check for named frequencies
  identifyFrequency(radioState.frequency + radioState.bfo / 1000);
  // Will need a redraw
  // enable amp (SSB seek skipped the mute, so skip unmute too)
  if(!isSSB()) audioTempMute(false);
  return(true);
}

//
// Handle tuning
//
bool doTune(int16_t enc)
{
  //
  // SSB tuning
  //
  if(isSSB())
  {
    uint32_t step = getCurrentStep()->step;
    uint32_t stepAdjust = (radioState.frequency * 1000 + radioState.bfo) % step;
    step = !stepAdjust? step : enc>0? step - stepAdjust : stepAdjust;

    updateBFO(radioState.bfo + enc * step, true);
  }

  //
  // Normal tuning
  //
  else
  {
    uint16_t step = getCurrentStep()->step;
    uint16_t stepAdjust = radioState.frequency % step;
    stepAdjust = (radioState.mode==FM) && (step==20)? (stepAdjust+10) % step : stepAdjust;
    step = !stepAdjust? step : enc>0? step - stepAdjust : stepAdjust;

    // Tune to a new frequency
    updateFrequency(radioState.frequency + step * enc, true);
  }

  // Clear current station name and information
  clearStationInfo();
  // Check for named frequencies
  identifyFrequency(radioState.frequency + radioState.bfo / 1000);
  // Will need a redraw
  return(true);
}

//
// Rotate digit
//
bool doDigit(int16_t enc)
{
  bool updated = false;

  // SSB tuning
  if(isSSB())
  {
    updated = updateBFO(radioState.bfo + enc * getFreqInputStep(), false);
  }

  //
  // Normal tuning
  //
  else
  {
    // Tune to a new frequency
    updated = updateFrequency(radioState.frequency + enc * getFreqInputStep(), false);
  }

  if (updated) {
    // Clear current station name and information
    clearStationInfo();
    // Check for named frequencies
    identifyFrequency(radioState.frequency + radioState.bfo / 1000);
  }

  // Will need a redraw
  return(updated);
}


bool clickFreq(bool shortPress)
{
  if (shortPress) {
    bool updated = false;

     // SSB tuning
     if(isSSB()) {
       updated = updateBFO(radioState.bfo - (radioState.frequency * 1000 + radioState.bfo) % getFreqInputStep(), false);
     } else {
       // Normal tuning
       updated = updateFrequency(radioState.frequency - radioState.frequency % getFreqInputStep(), false);
     }

     if (updated) {
       // Clear current station name and information
       clearStationInfo();
       // Check for named frequencies
       identifyFrequency(radioState.frequency + radioState.bfo / 1000);
     }
     return true;
  }
  return false;
}

bool processRssiSnr()
{
  bool needRedraw = false;

  rx.getCurrentReceivedSignalQuality();
  int newRSSI = rx.getCurrentRSSI();
  int newSNR = rx.getCurrentSNR();

  // Apply squelch if the volume is not muted
  uint8_t squelchValue = radioState.squelch[radioState.mode] & 0x7f;
  uint8_t squelchParam = (radioState.squelch[radioState.mode] & 0x80)? newSNR:newRSSI;
  if(squelchValue)
  {
    if(squelchParam >= squelchValue && audioIsSquelched())
    {
      audioSquelchClose(false);
    }
    else if(squelchParam < squelchValue && !audioIsSquelched())
    {
      audioSquelchClose(true);
    }
  }
  else if(audioIsSquelched())
  {
    audioSquelchClose(false);
  }

  // G8PTN: Based on 1.2s interval, update RSSI & SNR
  if(!(rssiUpdateCounter++ & 7))
  {
    // Show RSSI status only if this condition has changed
    if(newRSSI != radioState.rssi)
    {
      radioState.rssi = newRSSI;
      needRedraw = true;
    }
    // Show SNR status only if this condition has changed
    if(newSNR != radioState.snr)
    {
      radioState.snr = newSNR;
      needRedraw = true;
    }
  }
  return needRedraw;
}
