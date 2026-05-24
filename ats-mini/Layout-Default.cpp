#include "Common.h"
#include "Themes.h"
#include "Utils.h"
#include "Menu.h"
#include "MenuDraw.h"
#include "Draw.h"

void drawLayoutDefault(const char *statusLine1, const char *statusLine2)
{
  // Draw common top bar
  drawTopBar();

  // Draw left-side menu/info bar
  drawSideBar(radioState.cmd, MENU_OFFSET_X, MENU_OFFSET_Y, MENU_DELTA_X);

  // Draw S-meter
  drawSMeter(getStrength(rssi), METER_OFFSET_X, METER_OFFSET_Y);

  // Indicate FM pilot detection (stereo indicator)
  drawStereoIndicator(METER_OFFSET_X, METER_OFFSET_Y, (radioState.mode==FM) && rx.getCurrentPilot());

  if(radioState.cmd == CMD_SCAN)
  {
    drawScanGraphs(isSSB() ? getEffectiveFreq() : radioState.frequency);
  }
  else if(!drawWiFiStatus(statusLine1, statusLine2, STATUS_OFFSET_X, STATUS_OFFSET_Y))
  {
    // Show radio text if present, else show frequency scale
    if(*getRadioText() || *getProgramInfo())
      drawRadioText(STATUS_OFFSET_Y, STATUS_OFFSET_Y + 25);
    else
      drawScale(isSSB() ? getEffectiveFreq() : radioState.frequency);
  }
}
