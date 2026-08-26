#include "MinimalTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <Utf8.h>

#include <algorithm>
#include <string>
#include <vector>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Match BaseTheme landscape stack: ALL CAPS letters only, 10 pt regular, left column.
constexpr int kLandscapeStackFontId = SOURCESERIF4_10_FONT_ID;

std::string stackedLettersOnly(const char* text) {
  std::string out;
  if (!text) return out;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&p)) != 0) {
    if (utf8IsCombiningMark(cp)) continue;
    if (cp >= 'a' && cp <= 'z') cp = cp - ('a' - 'A');
    const bool keep = (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9');
    if (keep) utf8AppendCodepoint(cp, out);
  }
  return out;
}

int landscapeLabelYBias(const char* text) {
  if (!text) return 0;
  const std::string u = stackedLettersOnly(text);
  if (u == "UP" || u == "NEXT") return -10;
  if (u == "SELECT" || u == "LOOKUP" || u == "DONE") return -10;
  if (u == "DOWN") return -4;
  return 0;
}

void drawStackedVerticalLabel(const GfxRenderer& renderer, const int fontId, const int stripX, const int stripW,
                              const int areaY, const int areaH, const char* text, const int yBias = 0) {
  if (!text || !*text || areaH <= 0 || stripW <= 0) return;

  const std::string upper = stackedLettersOnly(text);
  if (upper.empty()) return;

  std::vector<std::string> letters;
  letters.reserve(8);
  int maxCw = 0;
  {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(upper.c_str());
    uint32_t cp;
    while ((cp = utf8NextCodepoint(&p)) != 0) {
      std::string one;
      utf8AppendCodepoint(cp, one);
      maxCw = std::max(maxCw, renderer.getTextWidth(fontId, one.c_str(), EpdFontFamily::REGULAR));
      letters.push_back(std::move(one));
    }
  }
  const int nChars = static_cast<int>(letters.size());
  if (nChars <= 0) return;

  constexpr EpdFontFamily::Style kStyle = EpdFontFamily::REGULAR;
  const int glyphH = std::max(8, renderer.getTextHeight(fontId));
  int step = glyphH + 1;
  int totalH = nChars * step;
  if (totalH > areaH) {
    step = std::max(glyphH, areaH / nChars);
    totalH = nChars * step;
  }
  constexpr int kTopPad = 2;
  int y = areaY + kTopPad + yBias;
  if (y + totalH > areaY + areaH) {
    y = areaY + std::max(0, areaH - totalH);
  }
  if (y < areaY) y = areaY;

  const int colLeft = stripX + std::max(2, (stripW - maxCw) / 2);

  for (const std::string& one : letters) {
    const int baseline = y + glyphH;
    renderer.drawText(fontId, colLeft, baseline, one.c_str(), true, kStyle);
    y += step;
  }
}

}  // namespace

// Shared by Stats / Bare / Penumbra. Physical-key devices: text-only columns.
// X4 Pro: outlined pills — 10pt labels were invisible on the clock-face home.
void MinimalTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                   const char* btn4) const {
  // X4 Pro: soft front chrome is the only Menu/Library/Recents/Read surface.
  if (gpio.hasTouch() && !gpio.needsOnScreenFrontChrome()) {
    return;
  }

  const GfxRenderer::Orientation orient = renderer.getOrientation();
  const bool landscapeCw = orient == GfxRenderer::LandscapeClockwise;
  const bool landscapeCcw = orient == GfxRenderer::LandscapeCounterClockwise;
  const bool landscape = landscapeCw || landscapeCcw;
  const bool inverted = orient == GfxRenderer::PortraitInverted;
  const int barH = BaseTheme::frontButtonHintReserve(renderer);

  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const char* labels[] = {btn1, btn2, btn3, btn4};

  constexpr int buttonWidth = 80;
  constexpr int x4ButtonPositions[] = {58, 146, 254, 342};
  constexpr int x3ButtonPositions[] = {65, 157, 291, 383};
  const int* buttonPositions = gpio.deviceIsX3() ? x3ButtonPositions : x4ButtonPositions;

  if (landscape) {
    // Pro has no physical front keys — use BaseTheme outlined pills so Home/Menu
    // on the reader edge is actually visible. Hit-test uses the same slots.
    if (gpio.needsOnScreenFrontChrome()) {
      BaseTheme::drawButtonHints(renderer, btn1, btn2, btn3, btn4);
      return;
    }
    const int stripX = landscapeCcw ? (pageW - barH) : 0;
    renderer.fillRect(stripX, 0, barH, pageH, false);

    const int portraitSpan = gpio.deviceIsX3() ? 528 : 480;
    constexpr int kClusterNudgeUp = 8;

    for (int i = 0; i < 4; ++i) {
      if (labels[i] == nullptr || labels[i][0] == '\0') continue;
      const int portraitCenterX = buttonPositions[i] + buttonWidth / 2;
      const int scaled = (portraitCenterX * pageH + portraitSpan / 2) / portraitSpan;
      int yCenter = landscapeCcw ? (pageH - 1 - scaled) : scaled;
      yCenter -= kClusterNudgeUp;
      const int pillY = yCenter - buttonWidth / 2;
      drawStackedVerticalLabel(renderer, kLandscapeStackFontId, stripX, barH, pillY + 2, buttonWidth - 4, labels[i],
                               landscapeLabelYBias(labels[i]));
    }
    return;
  }

  const int barY = inverted ? 0 : (pageH - barH);
  if (barH > 0) {
    renderer.fillRect(0, barY, pageW, barH, false);
  }

  constexpr int kSlots = 4;
  const int slotW = pageW / kSlots;

  // X4 Pro: 10pt text-only labels vanished on the 480-wide Penumbra clock face.
  // Draw X3-style outlined pills in the same four equal columns the hit-test uses.
  if (gpio.needsOnScreenFrontChrome()) {
    constexpr int kGap = 6;
    constexpr int kCorner = 6;
    constexpr int kFontId = UI_12_FONT_ID;
    const bool roundTop = !inverted;
    const bool roundBottom = inverted;
    const int lineH = renderer.getLineHeight(kFontId);
    const int textY = barY + (barH - lineH) / 2;
    for (int i = 0; i < kSlots; ++i) {
      if (labels[i] == nullptr || labels[i][0] == '\0') continue;
      const int col = inverted ? (kSlots - 1 - i) : i;
      const int pillX = col * slotW + kGap / 2;
      const int pillW = slotW - kGap;
      renderer.fillRoundedRect(pillX, barY, pillW, barH, kCorner, roundTop, roundTop, roundBottom, roundBottom,
                               Color::White);
      renderer.drawRoundedRect(pillX, barY, pillW, barH, 1, kCorner, roundTop, roundTop, roundBottom, roundBottom,
                               true);
      const int maxLabelW = std::max(8, pillW - 8);
      const std::string label = renderer.truncatedText(kFontId, labels[i], maxLabelW, EpdFontFamily::BOLD);
      const int tw = renderer.getTextWidth(kFontId, label.c_str(), EpdFontFamily::BOLD);
      const int tx = pillX + (pillW - tw) / 2;
      renderer.drawText(kFontId, tx, textY, label.c_str(), true, EpdFontFamily::BOLD);
    }
    return;
  }

  constexpr int kFooterFontId = UI_10_FONT_ID;
  const int lineH = renderer.getLineHeight(kFooterFontId);
  const int textY = barY + (barH - lineH) / 2;

  for (int i = 0; i < kSlots; ++i) {
    if (labels[i] == nullptr || labels[i][0] == '\0') continue;
    const int col = inverted ? (kSlots - 1 - i) : i;
    const int maxLabelW = slotW - 8;
    const std::string label = renderer.truncatedText(kFooterFontId, labels[i], maxLabelW, EpdFontFamily::REGULAR);
    const int tw = renderer.getTextWidth(kFooterFontId, label.c_str(), EpdFontFamily::REGULAR);
    const int tx = col * slotW + (slotW - tw) / 2;
    renderer.drawText(kFooterFontId, tx, textY, label.c_str(), true, EpdFontFamily::REGULAR);
  }
}
