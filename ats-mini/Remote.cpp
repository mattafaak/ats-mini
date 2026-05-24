#include "Battery.h"
#include "Common.h"
#include "Station.h"
#include "Themes.h"
#include "Utils.h"
#include "Menu.h"
#include "Draw.h"
#include "Remote.h"
#include "Tuning.h"
#include "AudioManager.h"

static RemoteState remoteSerialState;

static uint8_t char2nibble(char key)
{
  if((key >= '0') && (key <= '9')) return(key - '0');
  if((key >= 'A') && (key <= 'F')) return(key - 'A' + 10);
  if((key >= 'a') && (key <= 'f')) return(key - 'a' + 10);
  return(0);
}

//
// Capture current screen image to the remote
//
static void remoteCaptureScreen(Stream* stream)
{
  uint16_t width  = spr.width();
  uint16_t height = spr.height();

  // 14 bytes of BMP header
  stream->println("");
  stream->print("424d"); // BM
  // Image size
  stream->printf("%08x", (unsigned int)htonl(14 + 40 + 12 + width * height * 2));
  stream->print("00000000");
  // Offset to image data
  stream->printf("%08x", (unsigned int)htonl(14 + 40 + 12));
  // Image header
  stream->print("28000000"); // Header size
  stream->printf("%08x", (unsigned int)htonl(width));
  stream->printf("%08x", (unsigned int)htonl(height));
  stream->print("01001000"); // 1 plane, 16 bpp
  stream->print("03000000"); // Compression
  stream->print("00000000"); // Compressed image size
  stream->print("00000000"); // X res
  stream->print("00000000"); // Y res
  stream->print("00000000"); // Color map
  stream->print("00000000"); // Colors
  stream->print("00f80000"); // Red mask
  stream->print("e0070000"); // Green mask
  stream->println("1f000000"); // Blue mask

  // Image data
  for(int y=height-1 ; y>=0 ; y--)
  {
    for(int x=0 ; x<width ; x++)
    {
      stream->printf("%04x", htons(spr.readPixel(x, y)));
    }
    stream->println("");
  }
  stream->flush();
}

char remoteReadChar(Stream* stream)
{
  char key;
  uint32_t start = millis();
  while (!stream->available()) {
    if ((millis() - start) > 5000) return 0;  // 5 second timeout
    delay(1);
  }
  key = stream->read();
  stream->print(key);
  return key;
}

long int remoteReadInteger(Stream* stream)
{
  long int result = 0;
  uint32_t start = millis();
  while (true) {
    if ((millis() - start) > 5000) return 0;  // total timeout
    char ch = stream->peek();
    if (ch == 0xFF) {
      delay(1);
      continue;
    } else if ((ch >= '0') && (ch <= '9')) {
      ch = remoteReadChar(stream);
      if (!ch) return result;  // timeout, return partial result
      result = result * 10 + (ch - '0');
    } else {
      return result;
    }
  }
}

void remoteReadString(Stream* stream, char *bufStr, uint8_t bufLen)
{
  uint8_t length = 0;
  uint32_t start = millis();
  while (true) {
    if ((millis() - start) > 5000) { bufStr[length] = '\0'; return; }
    char ch = stream->peek();
    if (ch == 0xFF) {
      delay(1);
      continue;
    } else if (ch == ',' || ch < ' ') {
      bufStr[length] = '\0';
      return;
    } else {
      ch = remoteReadChar(stream);
      if (!ch) { bufStr[length] = '\0'; return; }
      bufStr[length] = ch;
      if (++length >= bufLen - 1) {
        bufStr[length] = '\0';
        return;
      }
    }
  }
}

static bool expectNewline(Stream* stream)
{
  char ch;
  uint32_t start = millis();
  while ((ch = stream->peek()) == 0xFF) {
    if ((millis() - start) > 5000) return false;
    delay(1);
  }
  if (ch == '\r') {
    stream->read();
    return true;
  }
  return false;
}

static bool remoteShowError(Stream* stream, const char *message)
{
  // Consume the remaining input
  while (stream->available()) remoteReadChar(stream);
  stream->printf("\r\nError: %s\r\n", message);
  return false;
}

static bool remoteSetFrequency(Stream *stream)
{
  stream->print('F');

  long int freqHz = remoteReadInteger(stream);
  if(freqHz <= 0)
    return remoteShowError(stream, "Invalid frequency");
  if(!expectNewline(stream))
    return remoteShowError(stream, "Expected newline");
  stream->println();

  Band *band = getCurrentBand();
  uint16_t targetFreq = freqFromHz(freqHz, radioState.mode);
  int targetBfo = isSSB() ? bfoFromHz(freqHz) : 0;
  if(!isFreqInBand(band, targetFreq) || (isSSB() && targetFreq == band->maximumFreq && targetBfo))
    return remoteShowError(stream, "Frequency is out of range for the current band");
  if(!updateFrequency(targetFreq, false))
    return remoteShowError(stream, "Frequency is out of range for the current band");

  if(isSSB())
    updateBFO(targetBfo, false);
  else if(radioState.bfo)
    updateBFO(0, true);

  clearStationInfo();
  identifyFrequency(getEffectiveFreq());

  return true;
}

static void remoteGetMemories(Stream* stream)
{
  for (uint8_t i = 0; i < getTotalMemories(); i++) {
    if (memories[i].freq) {
      stream->printf("#%02d,%s,%ld,%s\r\n", i + 1, bands[memories[i].band].bandName, memories[i].freq, bandModeDesc[memories[i].mode]);
    }
  }
}

static bool remoteSetMemory(Stream* stream)
{
  stream->print('#');
  Memory mem;
  uint32_t freq = 0;

  long int slot = remoteReadInteger(stream);
  if (remoteReadChar(stream) != ',')
    return remoteShowError(stream, "Expected ','");
  if (slot < 1 || slot > getTotalMemories())
    return remoteShowError(stream, "Invalid memory slot number");

  char band[8];
  remoteReadString(stream, band, 8);
  if (remoteReadChar(stream) != ',')
    return remoteShowError(stream, "Expected ','");
  mem.band = 0xFF;
  for (int i = 0; i < getTotalBands(); i++) {
    if (strcmp(bands[i].bandName, band) == 0) {
      mem.band = i;
      break;
    }
  }
  if (mem.band == 0xFF)
    return remoteShowError(stream, "No such band");

  freq = remoteReadInteger(stream);
  if (remoteReadChar(stream) != ',')
    return remoteShowError(stream, "Expected ','");

  char mode[4];
  remoteReadString(stream, mode, 4);
  if (!expectNewline(stream))
    return remoteShowError(stream, "Expected newline");
  stream->println();
  mem.mode = 15;
  for (int i = 0; i < getTotalModes(); i++) {
    if (strcmp(bandModeDesc[i], mode) == 0) {
      mem.mode = i;
      break;
    }
  }
  if (mem.mode == 15)
    return remoteShowError(stream, "No such mode");

  mem.freq = freq;

  if (!isMemoryInBand(&bands[mem.band], &mem)) {
    if (!freq) {
      // Clear slot
      memories[slot-1] = mem;
      return true;
    } else {
      // Handle duplicate band names (15M)
      mem.band = 0xFF;
      for (int i = getTotalBands()-1; i >= 0; i--) {
        if (strcmp(bands[i].bandName, band) == 0) {
          mem.band = i;
          break;
        }
      }
      if (mem.band == 0xFF)
        return remoteShowError(stream, "No such band");
      if (!isMemoryInBand(&bands[mem.band], &mem))
        return remoteShowError(stream, "Invalid frequency or mode");
    }
  }

  memories[slot-1] = mem;
  return true;
}

//
// Set current color theme from the remote
//
static void remoteSetColorTheme(Stream* stream)
{
  stream->print("Enter a string of hex colors (x0001x0002...): ");

  uint8_t *p = (uint8_t *)&(TH.bg);

  for(int i=0 ; ; i+=sizeof(uint16_t))
  {
    if(i >= sizeof(ColorTheme)-offsetof(ColorTheme, bg))
    {
      stream->println(" Ok");
      break;
    }

    if(remoteReadChar(stream) != 'x')
    {
      stream->println(" Err");
      break;
    }

    char nh;
    if(!(nh = remoteReadChar(stream))) { stream->println(" Err"); break; }
    p[i + 1]  = char2nibble(nh) * 16;
    if(!(nh = remoteReadChar(stream))) { stream->println(" Err"); break; }
    p[i + 1] |= char2nibble(nh);
    if(!(nh = remoteReadChar(stream))) { stream->println(" Err"); break; }
    p[i]      = char2nibble(nh) * 16;
    if(!(nh = remoteReadChar(stream))) { stream->println(" Err"); break; }
    p[i]     |= char2nibble(nh);
  }

  // Redraw screen
  drawScreen();
}

//
// Print current color theme to the remote
//
static void remoteGetColorTheme(Stream* stream)
{
  stream->printf("Color theme %s: ", TH.name);
  const uint8_t *p = (uint8_t *)&(TH.bg);

  for(int i=0 ; i<sizeof(ColorTheme)-offsetof(ColorTheme, bg) ; i+=sizeof(uint16_t))
  {
    stream->printf("x%02X%02X", p[i+1], p[i]);
  }

  stream->println();
}

//
// Print current status to the remote
//
void remotePrintStatus(Stream* stream, RemoteState* state)
{
  // Prepare information ready to be sent
  float remoteVoltage = batteryMonitor();

  // S-Meter conditional on compile option
  rx.getCurrentReceivedSignalQuality();
  uint8_t remoteRssi = rx.getCurrentRSSI();
  uint8_t remoteSnr = rx.getCurrentSNR();

  // Use rx.getFrequency to force read of capacitor value from SI4732/5
  rx.getFrequency();
  uint16_t tuningCapacitor = rx.getAntennaTuningCapacitor();

  // Remote serial
  stream->printf("%u,%u,%d,%d,%s,%s,%s,%s,%hu,%hu,%hu,%hu,%hu,%.2f,%hu\r\n",
                VER_APP,
                radioState.frequency,
                radioState.bfo,
                ((radioState.mode == USB) ? getCurrentBand()->usbCal :
                 (radioState.mode == LSB) ? getCurrentBand()->lsbCal : 0),
                getCurrentBand()->bandName,
                bandModeDesc[radioState.mode],
                getCurrentStep()->desc,
                getCurrentBandwidth()->desc,
                radioState.agcIndex,
                radioState.vol,
                remoteRssi,
                remoteSnr,
                tuningCapacitor,
                remoteVoltage,
                state->remoteSeqnum
                );
}

//
// Tick remote time, periodically printing status
//
void remoteTickTime(Stream* stream, RemoteState* state)
{
  if(state->remoteLogOn && (millis() - state->remoteTimer >= 500))
  {
    // Mark time and increment diagnostic sequence number
    state->remoteTimer = millis();
    state->remoteSeqnum++;
    // Show status
    remotePrintStatus(stream, state);
  }
}

//
// Scan current band and output frequency activity to serial
//
static void scanToSerial(Stream* stream)
{
  Band *band = getCurrentBand();
  uint16_t step = getCurrentStep()->step;
  if (radioState.mode != FM && step < 5) step = 5;

  audioTempMute(true);
  uint16_t origFreq = radioState.frequency;
  int origBfo = radioState.bfo;
  uint16_t freq = band->minimumFreq;
  int count = 0;

  stream->println("Scanning...");

  while (freq <= band->maximumFreq)
  {
    if (isSSB()) updateBFO(0, true);
    if (updateFrequency(freq, false))
    {
      // Adaptive settle: poll RSSI until signal detected or 80ms max
      uint32_t settle = millis();
      while ((millis() - settle) < 80) {
        rx.getCurrentReceivedSignalQuality();
        if (rx.getCurrentRSSI() > 0) break;
        delay(5);
      }
      rx.getCurrentReceivedSignalQuality();
      uint8_t rssi = rx.getCurrentRSSI();
      uint8_t snr = rx.getCurrentSNR();
      stream->printf("%u,%u,%u\r\n", freq, rssi, snr);
      if (rssi > 0) count++;
    }

    if (consumeAbortPending())
    {
      stream->println("ABORTED");
      break;
    }
    freq += step;
  }

  stream->printf("END,%d\r\n", count);

  updateFrequency(origFreq, false);
  if (isSSB()) updateBFO(origBfo, false);
  audioTempMute(false);
}

//
// Recognize and execute given remote command
//
int remoteDoCommand(Stream* stream, RemoteState* state, char key)
{
  int event = 0;

  switch(key)
  {
    case 'R': // Rotate Encoder Clockwise
      event |= 1 << REMOTE_DIRECTION;
      event |= REMOTE_PREFS;
      break;
    case 'r': // Rotate Encoder Counterclockwise
      event |= -1 << REMOTE_DIRECTION;
      event |= REMOTE_PREFS;
      break;
    case 'e': // Encoder Push Button
      event |= REMOTE_CLICK;
      break;
    case 'E': // Encoder Short Press
      event |= REMOTE_SHORT_PRESS;
      break;
    case 'B': // Band Up
      doBand(1);
      event |= REMOTE_PREFS;
      break;
    case 'b': // Band Down
      doBand(-1);
      event |= REMOTE_PREFS;
      break;
    case 'M': // Mode Up
      doMode(1);
      event |= REMOTE_PREFS;
      break;
    case 'm': // Mode Down
      doMode(-1);
      event |= REMOTE_PREFS;
      break;
    case 'S': // Step Up
      doStep(1);
      event |= REMOTE_PREFS;
      break;
    case 's': // Step Down
      doStep(-1);
      event |= REMOTE_PREFS;
      break;
    case 'W': // Bandwidth Up
      doBandwidth(1);
      event |= REMOTE_PREFS;
      break;
    case 'w': // Bandwidth Down
      doBandwidth(-1);
      event |= REMOTE_PREFS;
      break;
    case 'A': // AGC/ATTN Up
      doAgc(1);
      event |= REMOTE_PREFS;
      break;
    case 'a': // AGC/ATTN Down
      doAgc(-1);
      event |= REMOTE_PREFS;
      break;
    case 'V': // Volume Up
      doVolume(1);
      event |= REMOTE_PREFS;
      break;
    case 'v': // Volume Down
      doVolume(-1);
      event |= REMOTE_PREFS;
      break;
    case 'L': // Backlight Up
      doBrt(1);
      event |= REMOTE_PREFS;
      break;
    case 'l': // Backlight Down
      doBrt(-1);
      event |= REMOTE_PREFS;
      break;
    case 'O':
      sleepOn(true);
      break;
    case 'o':
      sleepOn(false);
      break;
    case 'I':
      doCal(1);
      event |= REMOTE_PREFS;
      break;
    case 'i':
      doCal(-1);
      event |= REMOTE_PREFS;
      break;
    case 'C':
      state->remoteLogOn = false;
      remoteCaptureScreen(stream);
      break;
    case 't':
      state->remoteLogOn = !state->remoteLogOn;
      break;
    case '?': {
      RemoteState temp = {};
      remotePrintStatus(stream, &temp);
      break;
    }
    case '>':  // Seek up
      doSeek(1, 1);
      break;
    case '<':  // Seek down
      doSeek(-1, -1);
      break;
    case 'Z':  // Frequency activity scan to serial
      scanToSerial(stream);
      break;

    case '$':
      remoteGetMemories(stream);
      break;
    case '#':
      if (remoteSetMemory(stream))
        event |= REMOTE_PREFS;
      break;
    case 'F':
      if (remoteSetFrequency(stream))
        event |= REMOTE_PREFS;
      break;
    case 'K': {
      stream->print('K');
      long int freqKhz = remoteReadInteger(stream);
      if (freqKhz <= 0) break;
      if (!expectNewline(stream)) {
        remoteShowError(stream, "Expected newline");
        break;
      }
      stream->println();
      long int freqHz = freqKhz * 1000;
      Band *band = getCurrentBand();
      uint16_t targetFreq = freqFromHz(freqHz, radioState.mode);
      int targetBfo = isSSB() ? bfoFromHz(freqHz) : 0;
      if (!isFreqInBand(band, targetFreq) || (isSSB() && targetFreq == band->maximumFreq && targetBfo)) {
        remoteShowError(stream, "Frequency is out of range for the current band");
        break;
      }
      if (!updateFrequency(targetFreq, false)) {
        remoteShowError(stream, "Frequency is out of range for the current band");
        break;
      }
      if (isSSB())
        updateBFO(targetBfo, false);
      else if (radioState.bfo)
        updateBFO(0, true);
      clearStationInfo();
      identifyFrequency(getEffectiveFreq());
      event |= REMOTE_PREFS;
      break;
    }

    case 'T':
      stream->println(switchThemeEditor(!switchThemeEditor()) ? "Theme editor enabled" : "Theme editor disabled");
      break;
    case '^':
      if(switchThemeEditor()) remoteSetColorTheme(stream);
      break;
    case '@':
      if(switchThemeEditor()) remoteGetColorTheme(stream);
      break;

    default:
      // Command not recognized
      return(event);
  }

  // Command recognized
  return(event | REMOTE_CHANGED);
}

static int serialLoop(Stream* stream, RemoteState* state, uint8_t usbMode)
{
  if(usbMode == USB_OFF) return 0;

  remoteTickTime(stream, state);

  if (stream->available())
    return remoteDoCommand(stream, state, stream->read());
  return 0;
}

int serialLoop(uint8_t usbMode)
{
  return serialLoop(&Serial, &remoteSerialState, usbMode);
}

bool serialConsumeAbortPending(uint8_t usbMode)
{
  if(usbMode == USB_OFF || !Serial.available()) return false;
  Serial.read();
  return true;
}
