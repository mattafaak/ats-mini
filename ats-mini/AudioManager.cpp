#include "AudioManager.h"
#include "Common.h"
#include "Draw.h"
#include <driver/gpio.h>

//
// Mute state machine with 4 independent channels:
//   Force, Main, Squelch, Temp
//
// The hardware mute is applied only when transitioning between
// "all channels clear" and "any channel active".
//

// Current main mute status
static volatile bool mainMuted = false;

// Current squelch mute status
static volatile bool squelchMuted = false;

// Effective mute status (last requested state)
static volatile bool effectiveMuted = false;

//
// Apply or release hardware mute.
// The delay between AUDIO_MUTE pin toggle and rx.setAudioMute()
// is required for amplifier mode D protection; see the NS4160
// datasheet.
//
static void applyMute(bool mute)
{
  if(mute) {
    if(PIN_AMP_EN >= 0) digitalWrite(PIN_AMP_EN, LOW);
#ifdef BOARD_HAS_MUTE_CIRCUIT
    digitalWrite(AUDIO_MUTE, HIGH);
#endif
    delay(50);
    rx.setAudioMute(true);
  } else {
#ifdef BOARD_HAS_MUTE_CIRCUIT
    digitalWrite(AUDIO_MUTE, LOW);
#endif
    delay(50);
    rx.setAudioMute(false);
    if(PIN_AMP_EN >= 0) digitalWrite(PIN_AMP_EN, HIGH);
  }
}

//
// Initialize mute pins and state
//
void audioInit(void)
{
  mainMuted = false;
  squelchMuted = false;
  effectiveMuted = false;
#ifdef BOARD_HAS_MUTE_CIRCUIT
  digitalWrite(AUDIO_MUTE, LOW);
#endif
  delay(50);
  rx.setAudioMute(false);
  if(PIN_AMP_EN >= 0) digitalWrite(PIN_AMP_EN, HIGH);
}

//
// Force mute — unconditionally overrides all other channels
//
void audioMuteForce(bool on)
{
  bool doMute = false;
  portENTER_CRITICAL(&audioMuteMux);
  if(on) {
    effectiveMuted = true;
    doMute = true;
  } else {
    if(!mainMuted && !squelchMuted) {
      effectiveMuted = false;
      doMute = true;
    }
  }
  portEXIT_CRITICAL(&audioMuteMux);
  if(doMute) applyMute(on);
}

//
// Main mute — user volume toggle
//
void audioMuteMain(bool on)
{
  bool doMute = false;
  portENTER_CRITICAL(&audioMuteMux);
  if(on) {
    effectiveMuted = true;
    if(!mainMuted && !squelchMuted) doMute = true;
    mainMuted = true;
  } else {
    effectiveMuted = false;
    if(mainMuted && !squelchMuted) doMute = true;
    mainMuted = false;
  }
  portEXIT_CRITICAL(&audioMuteMux);
  if(doMute) applyMute(on);
}

//
// Squelch — true = close (mute), false = open (unmute)
//
void audioSquelchClose(bool on)
{
  bool doMute = false;
  portENTER_CRITICAL(&audioMuteMux);
  if(on) {
    effectiveMuted = true;
    if(!mainMuted && !squelchMuted) doMute = true;
    squelchMuted = true;
  } else {
    effectiveMuted = false;
    if(!mainMuted && squelchMuted) doMute = true;
    squelchMuted = false;
  }
  portEXIT_CRITICAL(&audioMuteMux);
  if(doMute) applyMute(on);
}

//
// Temporary mute — used during seek/scan/band changes
//
void audioTempMute(bool on)
{
  bool doMute = false;
  portENTER_CRITICAL(&audioMuteMux);
  if(on) {
    effectiveMuted = true;
    if(!mainMuted && !squelchMuted) doMute = true;
  } else {
    if(!mainMuted && !squelchMuted) {
      effectiveMuted = false;
      doMute = true;
    }
  }
  portEXIT_CRITICAL(&audioMuteMux);
  if(doMute) applyMute(on);
}

//
// Query functions
//
bool audioIsMuted(void)
{
  return effectiveMuted;
}

bool audioIsMainMuted(void)
{
  return mainMuted;
}

bool audioIsSquelched(void)
{
  return squelchMuted;
}
