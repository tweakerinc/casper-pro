#include "BootActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include "fontIds.h"
#include "images/Logo120.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  // Centered Casper sheet-ghost logo + name (no "BOOTING" caption).
  constexpr int kLogoSize = 120;
  const int logoY = pageHeight / 2 - kLogoSize / 2 - 24;
  renderer.drawImage(Logo120, (pageWidth - kLogoSize) / 2, logoY, kLogoSize, kLogoSize);

  const int wordY = logoY + kLogoSize + 12;
  renderer.drawCenteredText(UI_12_FONT_ID, wordY, tr(STR_CASPER), true, EpdFontFamily::BOLD);

  const int versionY = pageHeight - renderer.getLineHeight(SMALL_FONT_ID) - 20;
  renderer.drawCenteredText(SMALL_FONT_ID, versionY, CASPER_VERSION, true);

  // Pro FULL/HALF is OEM 0xF7 and often develops nothing after USB/reset
  // (no logo, then Home flashes in). FAST 0xC7 redrives every pixel.
  renderer.displayBuffer(BoardConfig::isX4Pro() ? HalDisplay::FAST_REFRESH : HalDisplay::FULL_REFRESH);
}
