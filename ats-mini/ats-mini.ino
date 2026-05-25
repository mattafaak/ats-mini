// =================================
// INCLUDE FILES
// =================================

#include "About.h"
#include "Common.h"
#include <Wire.h>
#include "Rotary.h"
#include "Button.h"
#include "Menu.h"
#include "Draw.h"
#include "Storage.h"
#include "Themes.h"
#include "Utils.h"
#include "AudioManager.h"
#include "EIBI.h"
#include "Remote.h"
#include "BleMode.h"
#include "EventHandler.h"
#include "Scheduler.h"
#include "Tuning.h"
#include "Scan.h"

RadioState radioState = {0};
portMUX_TYPE radioStateMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE encoderMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE audioMuteMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE seekStopMux = portMUX_INITIALIZER_UNLOCKED;

// SI473/5 and UI
#define ELAPSED_COMMAND      10000  // time to turn off the last command controlled by encoder. Time to goes back to the VFO control // G8PTN: Increased time and corrected comment
#define DEFAULT_VOLUME          35  // change it for your favorite sound volume
#define DEFAULT_SLEEP            0  // Default sleep interval, range = 0 (off) to 255 in steps of 5
#define SEEK_TIMEOUT        600000  // Max seek timeout (ms)

// =================================
// CONSTANTS AND VARIABLES
// =================================

// Maximum encoder counts to accumulate (higher = fewer lost steps during busy periods)
#define MAX_ENCODER_ACCUM  20

volatile bool seekStop = false; // G8PTN: Added flag to abort seeking on rotary encoder detection

uint32_t elapsedRSSI = millis();

uint32_t lastRDSCheck = millis();
uint32_t lastNTPCheck = millis();
uint32_t lastScheduleCheck = millis();

uint32_t elapsedCommand = millis();
volatile int16_t encoderCount = 0;
volatile int16_t encoderCountAccel = 0;

uint32_t elapsedSleep = millis();           // Display sleep timer


// Background screen refresh
uint32_t background_timer = millis();   // Background screen refresh timer.

//
// Devices
//
Rotary encoder  = Rotary(ENCODER_PIN_B, ENCODER_PIN_A);
ButtonTracker pb1 = ButtonTracker();
TFT_eSPI tft    = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);
SI4735_fixed rx;

//
// Hardware initialization and setup
//
void setup()
{
  // Enable serial port
  Serial.begin(115200);

  // Encoder pins. Enable internal pull-ups
  pinMode(ENCODER_PUSH_BUTTON, INPUT_PULLUP);
  pinMode(ENCODER_PIN_A, INPUT_PULLUP);
  pinMode(ENCODER_PIN_B, INPUT_PULLUP);

  // Initially disable the audio amplifier until the SI4732 has been setup,
  // if the target board exposes a separate amplifier enable pin.
  if(PIN_AMP_EN >= 0)
  {
    pinMode(PIN_AMP_EN, OUTPUT);
    digitalWrite(PIN_AMP_EN, LOW);
  }

  // Enable SI4732 VDD
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);
  delay(100);

  // The line below may be necessary to setup I2C pins on ESP32
  Wire.begin(ESP32_I2C_SDA, ESP32_I2C_SCL);

  // TFT display brightness control (PWM)
  // Note: At brightness levels below 100%, switching from the PWM may cause power spikes and/or RFI
  ledcAttach(PIN_LCD_BL, 16000, 8);  // Pin assignment, 16kHz, 8-bit
  ledcWrite(PIN_LCD_BL, 0);          // Default value 0%

  // TFT display setup
  tft.begin();
  tft.setRotation(3);

  #if !defined(LILYGO_SI473X)
  // Detect and fix the mirrored & inverted display
  // https://github.com/esp32-si4732/ats-mini/issues/41
  uint8_t did3 = tft.readcommand8(ST7789_RDDID, 3);
  // 0x048181B3 - the original display
  // 0x04858552 - high gamma display
  // 0x00009307 - inverted & mirrored display
  if(did3 == 0x93)
  {
    tft.invertDisplay(0);
    tft.writecommand(TFT_MADCTL);
    tft.writedata(TFT_MAD_MV | TFT_MAD_MX | TFT_MAD_MY | TFT_MAD_BGR);
  }
  else if(did3 == 0x85)
  {
    tft.writecommand(0x26); // GAMSET
    tft.writedata(8);       // Gamma Curve 3

    tft.writecommand(0x55); // WRCACE (content adaptive brightness and color)
    tft.writedata(0xB1);    // High enhancement, UI mode
  }
  #endif

  tft.fillScreen(TH.bg);
  spr.createSprite(320, 170);
  spr.setTextDatum(MC_DATUM);
  spr.setSwapBytes(true);
  spr.setFreeFont(&Orbitron_Light_24);
  spr.setTextColor(TH.text, TH.bg);

  // Press and hold Encoder button to force an preferences reset
  // Note: preferences reset is recommended after firmware updates
  if(digitalRead(ENCODER_PUSH_BUTTON)==LOW)
  {
    nvsErase();
    diskInit(true);

    ledcWrite(PIN_LCD_BL, 255);       // Default value 255 = 100%
    tft.setTextSize(2);
    tft.setTextColor(TH.text, TH.bg);
    tft.println(getVersion(true));
    tft.println();
    tft.setTextColor(TH.text_warn, TH.bg);
    tft.print("Resetting Preferences");
    while(digitalRead(ENCODER_PUSH_BUTTON) == LOW) delay(100);
  }

  // Initialize flash file system
  diskInit();

  if(!ESP.getPsramSize()) {
    ledcWrite(PIN_LCD_BL, 255);       // Default value 255 = 100%
    tft.setTextSize(2);
    tft.setTextColor(TH.text_warn, TH.bg);
    tft.println("PSRAM not detected");
#ifdef CONFIG_SPIRAM_MODE_OCT
    tft.println("(try the QSPI f/w version)");
#else
    tft.println("(try the OSPI f/w version)");
#endif
  while(1);
  }

  // Check for SI4732 connected on I2C interface
  // If the SI4732 is not detected, then halt with no further processing
  rx.setI2CFastModeCustom(800000UL);

  // Looks for the I2C bus address and set it.  Returns 0 if error
  int16_t si4735Addr = rx.getDeviceI2CAddress(RESET_PIN);
  if(!si4735Addr)
  {
    ledcWrite(PIN_LCD_BL, 255);       // Default value 255 = 100%
    tft.setTextSize(2);
    tft.setTextColor(TH.text_warn, TH.bg);
    tft.println("Si4732 not detected");
    while(1);
  }

  rx.setup(RESET_PIN, MW_BAND_TYPE);

  // Attached pin to allows SI4732 library to mute audio as required to minimise loud clicks
  rx.setAudioMuteMcuPin(AUDIO_MUTE);

  // Set defaults for prefsLoad() fallbacks
  radioState.vol              = DEFAULT_VOLUME;
  radioState.brightness      = 130;
  radioState.amAvcIdx        = 48;
  radioState.ssbAvcIdx       = 48;
  radioState.amSoftMuteIdx   = 4;
  radioState.ssbSoftMuteIdx  = 4;
  radioState.softMuteMaxAtt  = 4;
  radioState.scrollDir       = 1;

  // If loading preferences fails...
  if(!prefsLoad(SAVE_SETTINGS|SAVE_VERIFY))
  {
    // Save default preferences
    prefsSave(SAVE_SETTINGS);
    // Show initial screen with the QR code
    spr.fillSprite(TH.bg);
    ledcWrite(PIN_LCD_BL, radioState.brightness);
    drawAboutHelp(0);
    // Wait for an encoder click
    while(digitalRead(ENCODER_PUSH_BUTTON)!=LOW) delay(100);
    while(digitalRead(ENCODER_PUSH_BUTTON)==LOW) delay(100);
  }

  // If loading memories fails, save default memories
  if(!prefsLoad(SAVE_MEMORIES|SAVE_VERIFY)) prefsSave(SAVE_MEMORIES);

  // If loading bands fails, save default bands
  if(!prefsLoad(SAVE_BANDS|SAVE_VERIFY)) prefsSave(SAVE_BANDS);

  // Audio Amplifier Enable. G8PTN: Added
  // After the SI4732 has been setup, enable the audio amplifier
  if(PIN_AMP_EN >= 0) digitalWrite(PIN_AMP_EN, HIGH);

  // SI4732 STARTUP!
  selectBand(bandIdx, false);
  delay(50);
  rx.setVolume(radioState.vol);
  rx.setMaxSeekTime(SEEK_TIMEOUT);

  // Draw display for the first time
  drawScreen();
  ledcWrite(PIN_LCD_BL, radioState.brightness);

  // Interrupt actions for Rotary encoder
  // Note: Moved to end of setup to avoid inital interrupt actions
  // ICACHE_RAM_ATTR void rotaryEncoder(); see rotaryEncoder implementation below.
  attachInterrupt(digitalPinToInterrupt(ENCODER_PIN_A), rotaryEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_PIN_B), rotaryEncoder, CHANGE);

  // Connect WiFi, if necessary
  netInit(radioState.wifiMode);

  // Start Bluetooth LE, if necessary
  bleInit(radioState.bleMode);
}


//
// Reads encoder via interrupt
// Uses Rotary.h and Rotary.cpp implementation to process encoder via
// interrupt. If you do not add ICACHE_RAM_ATTR declaration, the system
// will reboot during attachInterrupt call. The ICACHE_RAM_ATTR macro
// places this function into RAM.
//
ICACHE_RAM_ATTR void rotaryEncoder()
{
  // Rotary encoder events
  uint8_t encoderStatus = encoder.process();
  if(encoderStatus)
  {
    int8_t delta = encoderStatus==DIR_CW? 1 : -1;
    int16_t accelDelta = accelerateEncoder(delta);

    // Do not accumulate too many encoder steps if event loop doesn't consume them
    if(abs(encoderCount) < MAX_ENCODER_ACCUM)
    {
      portENTER_CRITICAL_ISR(&encoderMux);
      encoderCount += delta;
      encoderCountAccel += accelDelta;
      portEXIT_CRITICAL_ISR(&encoderMux);
    }

    // Only abort seek on significant encoder movement (>=3 steps) to prevent
    // accidental abort from a single brush contact or vibration.
    if(abs(encoderCount) >= 3) {
      portENTER_CRITICAL_ISR(&seekStopMux);
      seekStop = true;
      portEXIT_CRITICAL_ISR(&seekStopMux);
    }
  }
}


//
// Main event loop
//
void loop()
{
  uint32_t currentTime = millis();
  bool needRedraw = false;

  // Handle encoder, button, serial, and BLE input
  needRedraw |= handleEncoderInput();

  // Re-capture timestamp after input processing (handleEncoderInput may have updated elapsedCommand)
  currentTime = millis();

  // Disable commands control
  if((currentTime - elapsedCommand) > ELAPSED_COMMAND)
  {
    if(radioState.cmd != CMD_NONE && radioState.cmd != CMD_SEEK && radioState.cmd != CMD_SCAN && radioState.cmd != CMD_MEMORY)
    {
      radioState.cmd = CMD_NONE;
      needRedraw = true;
    }

    elapsedCommand = currentTime;
  }

  // Display sleep timeout
  if(radioState.sleep && !sleepOn() && ((currentTime - elapsedSleep) > radioState.sleep * 1000))
  {
    sleepOn(true);
    // CPU sleep can take long time, renew the timestamps
    elapsedSleep = elapsedCommand = currentTime = millis();
  }

  // Run periodic housekeeping tasks
  needRedraw |= runScheduler(currentTime);

  // Scan-to-memory tick (non-blocking auto-scan)
  scanProcessTick();

  // Redraw screen if necessary
  if(needRedraw) drawScreen();

  // Adaptive loop delay for better encoder responsiveness
  static uint32_t lastActivity = 0;
  uint32_t now = millis();
  if (encoderCount != 0 || radioState.cmd != CMD_NONE) {
      lastActivity = now;
      delay(5);
  } else if ((now - lastActivity) > 2000) {
      delay(50);
  } else {
      delay(20);
  }
}
