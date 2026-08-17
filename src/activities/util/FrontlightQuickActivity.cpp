#include "FrontlightQuickActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CasperSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/FrontlightUtil.h"
#include "util/UiGhostPolicy.h"
#include "activities/reader/ReaderUtils.h"

#if !FREEINK_CAP_FRONTLIGHT
void FrontlightQuickActivity::onEnter() { finish(); }
void FrontlightQuickActivity::onExit() { Activity::onExit(); }
void FrontlightQuickActivity::loop() {}
void FrontlightQuickActivity::render(RenderLock&&) {}
void FrontlightQuickActivity::layoutGeometry() {}
void FrontlightQuickActivity::applyLive() const {}
void FrontlightQuickActivity::persistActiveToPreset() {}
void FrontlightQuickActivity::selectPreset(uint8_t) {}
void FrontlightQuickActivity::setBrightness(int) {}
void FrontlightQuickActivity::setWarm(int) {}
void FrontlightQuickActivity::setBrightnessFromX(int) {}
void FrontlightQuickActivity::setWarmFromX(int) {}
void FrontlightQuickActivity::nudgeBrightness(int) {}
void FrontlightQuickActivity::nudgeWarm(int) {}
void FrontlightQuickActivity::toggleOnOff() {}
void FrontlightQuickActivity::toggleDarkMode() {}
void FrontlightQuickActivity::toggleReaderOnly() {}
void FrontlightQuickActivity::drawCardChrome() const {}
void FrontlightQuickActivity::drawPresetChips() const {}
void FrontlightQuickActivity::drawMinusPlus(int, int, int, int, int) const {}
void FrontlightQuickActivity::drawSliderTrack(int, int, int, int, uint8_t) const {}
void FrontlightQuickActivity::drawToggleRow(int, int, const char*, bool, int&, int&) const {}
void FrontlightQuickActivity::drawOnOffButton() const {}
bool FrontlightQuickActivity::hitSheet(int, int) const { return false; }
bool FrontlightQuickActivity::hitBtn(int, int, int, int, int) const { return false; }
bool FrontlightQuickActivity::hitBar(int, int, int, int, int, int) const { return false; }
void FrontlightQuickActivity::blitBackground() const {}
#else

namespace {
constexpr int kPad = 24;
constexpr int kCardRadius = 20;
constexpr int kKnobR = 12;
constexpr int kBarH = 7;
constexpr int kBtnS = 44;
constexpr int kTrackHitPad = 24;
constexpr int kNudgeStep = 1;
// Nearly full width on 800px landscape Pro — less squish.
constexpr int kCardMaxW = 520;

// Soft disc (rounded rect) — e-ink has no true circle primitive.
void drawDisc(const GfxRenderer& r, const int cx, const int cy, const int rad, const bool fill) {
  const int d = rad * 2;
  if (fill) {
    r.fillRoundedRect(cx - rad, cy - rad, d, d, rad, Color::Black);
  } else {
    r.drawRoundedRect(cx - rad, cy - rad, d, d, 2, rad, true);
  }
}

void drawSunIcon(const GfxRenderer& r, const int cx, const int cy, const int s) {
  const int rad = std::max(5, s / 4);
  drawDisc(r, cx, cy, rad, /*fill=*/false);
  const int dx[] = {0, 1, 1, 1, 0, -1, -1, -1};
  const int dy[] = {-1, -1, 0, 1, 1, 1, 0, -1};
  for (int i = 0; i < 8; ++i) {
    r.drawLine(cx + dx[i] * (rad + 3), cy + dy[i] * (rad + 3), cx + dx[i] * (rad + 8), cy + dy[i] * (rad + 8),
               2, true);
  }
}

void drawMoonIcon(const GfxRenderer& r, const int cx, const int cy, const int s) {
  const int rad = std::max(7, s / 3);
  drawDisc(r, cx, cy, rad, /*fill=*/true);
  r.fillRoundedRect(cx - rad / 3, cy - rad + 1, rad * 2 - 1, rad * 2 - 2, rad - 1, Color::White);
}

// Simple side-view bed: complete outline (headboard + mattress + foot + legs).
void drawBedIcon(const GfxRenderer& r, const int cx, const int cy, const int s) {
  const int w = std::max(18, (s * 9) / 10);
  const int h = std::max(12, (s * 6) / 10);
  const int left = cx - w / 2;
  const int top = cy - h / 2;
  const int right = left + w;
  const int bottom = top + h;
  const int matTop = top + h / 3;
  const int matBot = bottom - 3;

  // Headboard (full vertical post + top cap)
  r.drawLine(left, top, left, bottom, 2, true);
  r.drawLine(left, top, left + w / 4, top, 2, true);
  // Mattress outline (closed rectangle — not a partial bar)
  r.drawRect(left, matTop, w, matBot - matTop, 2, true);
  // Pillow (small closed rect at head end, inset)
  const int pw = std::max(6, w / 4);
  const int ph = std::max(4, (matBot - matTop) / 2);
  r.drawRect(left + 3, matTop + 2, pw, ph, 1, true);
  // Footboard
  r.drawLine(right, matTop, right, bottom, 2, true);
  // Legs
  r.drawLine(left + 2, matBot, left + 2, bottom, 2, true);
  r.drawLine(right - 2, matBot, right - 2, bottom, 2, true);
}

void drawCustomIcon(const GfxRenderer& r, const int cx, const int cy, const int s) {
  const int gap = std::max(5, s / 5);
  const int rad = std::max(2, s / 10);
  for (int i = -1; i <= 1; ++i) {
    drawDisc(r, cx + i * gap, cy, rad, /*fill=*/true);
  }
}

void drawPresetIcon(const GfxRenderer& r, const int preset, const int cx, const int cy, const int s) {
  switch (preset) {
    case 0:
      drawSunIcon(r, cx, cy, s);
      break;
    case 1:
      drawMoonIcon(r, cx, cy, s);
      break;
    case 2:
      drawBedIcon(r, cx, cy, s);
      break;
    default:
      drawCustomIcon(r, cx, cy, s);
      break;
  }
}

// Lightbulb: ON = solid bulb + rays; OFF = outline bulb, no rays (asleep).
void drawLightbulb(const GfxRenderer& r, const int cx, const int cy, const int size, const bool on) {
  const int bodyR = std::max(6, size / 3);
  const int bodyCy = cy - 2;
  if (on) {
    drawDisc(r, cx, bodyCy, bodyR, /*fill=*/true);
    // Soft halo rays
    const int dx[] = {0, 1, 1, 1, 0, -1, -1, -1};
    const int dy[] = {-1, -1, 0, 1, 1, 1, 0, -1};
    for (int i = 0; i < 8; ++i) {
      // Skip downward rays into the base
      if (dy[i] > 0 && dx[i] == 0) continue;
      r.drawLine(cx + dx[i] * (bodyR + 3), bodyCy + dy[i] * (bodyR + 3), cx + dx[i] * (bodyR + 7),
                 bodyCy + dy[i] * (bodyR + 7), true);
    }
  } else {
    drawDisc(r, cx, bodyCy, bodyR, /*fill=*/false);
  }
  // Screw base (two horizontal bars)
  const int baseTop = bodyCy + bodyR - 1;
  r.drawLine(cx - 4, baseTop + 3, cx + 4, baseTop + 3, true);
  r.drawLine(cx - 3, baseTop + 6, cx + 3, baseTop + 6, true);
  r.drawLine(cx - 2, baseTop + 9, cx + 2, baseTop + 9, true);
  if (!on) {
    // Quiet "asleep" slash through the glass
    r.drawLine(cx - bodyR + 2, bodyCy + bodyR - 3, cx + bodyR - 2, bodyCy - bodyR + 3, true);
  }
}

void formatPct(char* buf, const size_t n, const int v) {
  snprintf(buf, n, "%d%%", std::clamp(v, 0, 100));
}
}  // namespace

void FrontlightQuickActivity::layoutGeometry() {
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const bool showReaderOnly = SETTINGS.readerDarkMode != 0;

  sheetW = std::min(kCardMaxW, pageW - 16);
  sheetLeft = (pageW - sheetW) / 2;
  // Slightly higher so the taller card still fits with margin.
  sheetTop = std::max(4, pageH / 18);

  briBtnS = kBtnS;
  const int contentL = sheetLeft + kPad;
  const int contentR = sheetLeft + sheetW - kPad;
  const int contentW = contentR - contentL;

  // Presets: text labels only (Day | Night | Bed).
  chipY = sheetTop + 16;
  chipH = 36;
  chipGap = 0;
  chipLeft = contentL;
  chipW = contentW / 3;

  // Brightness — extra gap between label/% and the slider row
  briLabelY = chipY + chipH + 18;
  const int row1Y = briLabelY + 34;
  briMinusX = contentL;
  briMinusY = row1Y;
  briPlusX = contentR - briBtnS;
  briPlusY = row1Y;
  briBarH = kBarH;
  briBarX = briMinusX + briBtnS + 14;
  briBarW = std::max(56, briPlusX - 14 - briBarX);
  briBarY = row1Y + (briBtnS - briBarH) / 2;

  // Color — same breathing room under the label row
  warmLabelY = row1Y + briBtnS + 20;
  const int row2Y = warmLabelY + 34;
  warmMinusX = contentL;
  warmMinusY = row2Y;
  warmPlusX = contentR - briBtnS;
  warmPlusY = row2Y;
  warmBarH = kBarH;
  warmBarX = warmMinusX + briBtnS + 14;
  warmBarW = std::max(56, warmPlusX - 14 - warmBarX);
  warmBarY = row2Y + (briBtnS - warmBarH) / 2;

  // Dark mode
  darkH = 40;
  darkY = row2Y + briBtnS + 20;
  readerOnlyH = 36;
  readerOnlyY = darkY + darkH + 8;
  // Lightbulb power control — round hit target, not a full-width bar.
  toggleH = 52;
  toggleW = 60;
  toggleX = sheetLeft + (sheetW - toggleW) / 2;
  const int afterDark = showReaderOnly ? (readerOnlyY + readerOnlyH + 16) : (darkY + darkH + 16);
  toggleY = afterDark;
  sheetH = (toggleY + toggleH + kPad) - sheetTop;
  (void)pageH;
}

bool FrontlightQuickActivity::s_skipParentRepaint = false;

bool FrontlightQuickActivity::consumeSkipParentRepaint() {
  const bool v = s_skipParentRepaint;
  s_skipParentRepaint = false;
  return v;
}

void FrontlightQuickActivity::onEnter() {
  Activity::onEnter();
  firstPaint_ = true;
  skipParentRepaint_ = true;
  s_skipParentRepaint = false;
  bgSnapOk_ = false;
  bgSnap_.reset();
  bgSnapBytes_ = 0;
  if (renderer.hasFrameBuffer()) {
    uint8_t* fb = renderer.getFrameBuffer();
    const size_t n = static_cast<size_t>(HalDisplay::BUFFER_SIZE);
    if (fb && n > 0) {
      bgSnap_ = makeUniqueNoThrow<uint8_t[]>(n);
      if (bgSnap_) {
        memcpy(bgSnap_.get(), fb, n);
        bgSnapBytes_ = n;
        bgSnapOk_ = true;
      }
    }
  }
  layoutGeometry();
  // Match global dark-mode invert so a reopen is not stuck light.
  renderer.setInvertOnDisplay(SETTINGS.readerDarkMode != 0 && SETTINGS.darkModeReaderOnly == 0);
  // Do NOT reload the preset here. Live SETTINGS (including left-edge brightness)
  // are the source of truth; reloading wiped side-gesture levels and made the
  // sheet feel stuck at a higher preset floor. Presets load only when a chip is tapped.
  applyLive();
  requestUpdate();
}

void FrontlightQuickActivity::onExit() {
  persistActiveToPreset();
  SETTINGS.saveToFile();
  applyLive();
  // Soft dismiss: put the underlay back on glass via a card-sized window refresh.
  // Tell ActivityManager to skip Home onResume+full repaint (was flashing Home
  // every time the light sheet closed).
  UiGhostPolicy::clearHardScrub();
  bool restoredGlass = false;
  if (bgSnapOk_ && bgSnap_ && renderer.hasFrameBuffer()) {
    layoutGeometry();
    blitBackground();
    const int wx = std::max(0, sheetLeft - 4);
    const int wy = std::max(0, sheetTop - 4);
    const int ww = std::min(renderer.getScreenWidth() - wx, sheetW + 10);
    const int wh = std::min(renderer.getScreenHeight() - wy, sheetH + 10);
    renderer.displayWindow(wx, wy, ww, wh);
    restoredGlass = true;
  }
  s_skipParentRepaint = skipParentRepaint_ && restoredGlass;
  bgSnap_.reset();
  bgSnapOk_ = false;
  Activity::onExit();
}

void FrontlightQuickActivity::applyLive() const { applyFrontlightFromSettings(); }

void FrontlightQuickActivity::persistActiveToPreset() {
  SETTINGS.saveActiveToFrontlightPreset(SETTINGS.frontlightPreset);
}

void FrontlightQuickActivity::selectPreset(const uint8_t preset) {
  persistActiveToPreset();
  SETTINGS.loadFrontlightPreset(preset);
  applyLive();
  requestUpdate();
}

void FrontlightQuickActivity::setBrightness(const int v01to100) {
  // Same helper as left-edge gesture: one 0–100 scale, one apply path.
  setFrontlightBrightnessPercent(v01to100, /*mirrorToActivePreset=*/true);
  requestUpdate();
}

void FrontlightQuickActivity::setWarm(const int v01to100) {
  setFrontlightWarmPercent(v01to100, /*mirrorToActivePreset=*/true);
  requestUpdate();
}

void FrontlightQuickActivity::setBrightnessFromX(const int x) {
  const int span = std::max(1, briBarW - 1);
  // Live PWM only while dragging — full e-ink repaint every pixel made the sheet
  // lag so users never reached the low end (side-edge felt "lower" as a result).
  const int v = std::clamp(x - briBarX, 0, span) * 100 / span;
  setFrontlightBrightnessPercent(v, /*mirrorToActivePreset=*/true);
}

void FrontlightQuickActivity::setWarmFromX(const int x) {
  const int span = std::max(1, warmBarW - 1);
  const int v = std::clamp(x - warmBarX, 0, span) * 100 / span;
  setFrontlightWarmPercent(v, /*mirrorToActivePreset=*/true);
}

void FrontlightQuickActivity::nudgeBrightness(const int delta) {
  setBrightness(static_cast<int>(SETTINGS.frontlightBrightness) + delta);
}

void FrontlightQuickActivity::nudgeWarm(const int delta) {
  setWarm(static_cast<int>(SETTINGS.frontlightWarmPercent) + delta);
}

void FrontlightQuickActivity::toggleOnOff() {
  SETTINGS.frontlightOn = (SETTINGS.frontlightOn != 0) ? 0 : 1;
  persistActiveToPreset();
  applyLive();
  requestUpdate();
}

void FrontlightQuickActivity::toggleDarkMode() {
  SETTINGS.readerDarkMode = SETTINGS.readerDarkMode ? 0 : 1;
  if (SETTINGS.readerDarkMode) {
    // Reader Only defaults OFF → whole UI inverts immediately (including this sheet).
    SETTINGS.darkModeReaderOnly = 0;
  }
  skipParentRepaint_ = false;  // invert needs full parent redraw on dismiss
  // Apply invert now so the next paint is dark (do not wait for main-loop re-sync).
  renderer.setInvertOnDisplay(SETTINGS.readerDarkMode != 0 && SETTINGS.darkModeReaderOnly == 0);
  SETTINGS.saveToFile();
  forceFullPaint_ = true;  // backdrop must invert too
  layoutGeometry();
  requestUpdate();
}

void FrontlightQuickActivity::toggleReaderOnly() {
  SETTINGS.darkModeReaderOnly = SETTINGS.darkModeReaderOnly ? 0 : 1;
  skipParentRepaint_ = false;
  renderer.setInvertOnDisplay(SETTINGS.readerDarkMode != 0 && SETTINGS.darkModeReaderOnly == 0);
  SETTINGS.saveToFile();
  forceFullPaint_ = true;
  layoutGeometry();
  requestUpdate();
}

bool FrontlightQuickActivity::hitSheet(const int x, const int y) const {
  return x >= sheetLeft && x < sheetLeft + sheetW && y >= sheetTop && y < sheetTop + sheetH;
}

bool FrontlightQuickActivity::hitBtn(const int x, const int y, const int bx, const int by, const int s) const {
  return x >= bx && x < bx + s && y >= by && y < by + s;
}

bool FrontlightQuickActivity::hitBar(const int x, const int y, const int barX, const int barY, const int barW,
                                     const int barH) const {
  return y >= barY - kTrackHitPad && y < barY + barH + kTrackHitPad && x >= barX - 4 && x < barX + barW + 4;
}

void FrontlightQuickActivity::loop() {
  layoutGeometry();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasTopRightLightGesture() ||
      mappedInput.wasMenuGesture()) {
    finish();
    return;
  }

  int tx = 0, ty = 0;
  if (mappedInput.isScreenTouchHeld(tx, ty)) {
    if (drag == DragTarget::Brightness ||
        (drag == DragTarget::None && hitBar(tx, ty, briBarX, briBarY, briBarW, briBarH))) {
      drag = DragTarget::Brightness;
      setBrightnessFromX(tx);  // PWM live; ink updates on finger-up
      return;
    }
    if (drag == DragTarget::Warm ||
        (drag == DragTarget::None && hitBar(tx, ty, warmBarX, warmBarY, warmBarW, warmBarH))) {
      drag = DragTarget::Warm;
      setWarmFromX(tx);
      return;
    }
  } else if (drag != DragTarget::None) {
    // One paint after the drag so the knob/labels match the final level.
    drag = DragTarget::None;
    requestUpdate();
    return;
  }

  if (mappedInput.wasScreenTapped(tx, ty)) {
    if (!hitSheet(tx, ty)) {
      finish();
      return;
    }
    // Presets (full column hit targets — Day | Night | Bed)
    if (ty >= chipY && ty < chipY + chipH) {
      for (int i = 0; i < 3; ++i) {
        const int cx = chipLeft + i * chipW;
        if (tx >= cx && tx < cx + chipW) {
          selectPreset(static_cast<uint8_t>(i));
          return;
        }
      }
    }
    if (hitBtn(tx, ty, briMinusX, briMinusY, briBtnS)) {
      nudgeBrightness(-kNudgeStep);
      return;
    }
    if (hitBtn(tx, ty, briPlusX, briPlusY, briBtnS)) {
      nudgeBrightness(kNudgeStep);
      return;
    }
    if (hitBtn(tx, ty, warmMinusX, warmMinusY, briBtnS)) {
      nudgeWarm(-kNudgeStep);
      return;
    }
    if (hitBtn(tx, ty, warmPlusX, warmPlusY, briBtnS)) {
      nudgeWarm(kNudgeStep);
      return;
    }
    if (hitBar(tx, ty, briBarX, briBarY, briBarW, briBarH)) {
      setBrightnessFromX(tx);
      requestUpdate();  // tap (not drag): paint knob once
      return;
    }
    if (hitBar(tx, ty, warmBarX, warmBarY, warmBarW, warmBarH)) {
      setWarmFromX(tx);
      requestUpdate();
      return;
    }
    // Dark mode row (toggle on right half of row)
    if (ty >= darkY && ty < darkY + darkH) {
      toggleDarkMode();
      return;
    }
    if (SETTINGS.readerDarkMode != 0 && ty >= readerOnlyY && ty < readerOnlyY + readerOnlyH) {
      toggleReaderOnly();
      return;
    }
    if (tx >= toggleX && tx < toggleX + toggleW && ty >= toggleY && ty < toggleY + toggleH) {
      toggleOnOff();
      return;
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    toggleOnOff();
    return;
  }
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { nudgeBrightness(-kNudgeStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { nudgeBrightness(kNudgeStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { nudgeWarm(-kNudgeStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { nudgeWarm(kNudgeStep); });
}

void FrontlightQuickActivity::blitBackground() const {
  if (!bgSnapOk_ || !bgSnap_ || !renderer.hasFrameBuffer()) {
    renderer.clearScreen(0xFF);
    return;
  }
  uint8_t* fb = renderer.getFrameBuffer();
  if (!fb) {
    renderer.clearScreen(0xFF);
    return;
  }
  memcpy(fb, bgSnap_.get(), std::min(bgSnapBytes_, static_cast<size_t>(HalDisplay::BUFFER_SIZE)));
}

void FrontlightQuickActivity::drawCardChrome() const {
  // Soft single-stroke card — no double outline / shadow (less jagged on e-ink).
  renderer.fillRoundedRect(sheetLeft, sheetTop, sheetW, sheetH, kCardRadius, Color::White);
  renderer.drawRoundedRect(sheetLeft, sheetTop, sheetW, sheetH, 2, kCardRadius, true);
}

void FrontlightQuickActivity::drawPresetChips() const {
  static const StrId kLabels[3] = {StrId::STR_FL_DAY, StrId::STR_FL_NIGHT, StrId::STR_FL_BED};
  constexpr int kCount = 3;
  const uint8_t active = SETTINGS.frontlightPreset;
  const int fontId = UI_12_FONT_ID;
  const int lh = renderer.getLineHeight(fontId);
  const int ty = chipY + (chipH - lh) / 2;
  // Even columns across the card: Day | Night | Bed.
  for (int i = 0; i < kCount; ++i) {
    const int colL = chipLeft + i * chipW;
    const bool sel = (active == static_cast<uint8_t>(i));
    const char* lab = I18n::getInstance().get(kLabels[i]);
    const auto style = sel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int tw = renderer.getTextWidth(fontId, lab, style);
    renderer.drawText(fontId, colL + (chipW - tw) / 2, ty, lab, true, style);
    if (i < kCount - 1) {
      const char* sep = "|";
      const int sw = renderer.getTextWidth(fontId, sep, EpdFontFamily::REGULAR);
      renderer.drawText(fontId, colL + chipW - sw / 2, ty, sep, true, EpdFontFamily::REGULAR);
    }
  }
}

void FrontlightQuickActivity::drawMinusPlus(const int minusX, const int minusY, const int plusX, const int plusY,
                                            const int btnS) const {
  // Plain − / + glyphs only (no circles).
  const int midY = minusY + btnS / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
  const int minusW = renderer.getTextWidth(UI_12_FONT_ID, "-", EpdFontFamily::BOLD);
  const int plusW = renderer.getTextWidth(UI_12_FONT_ID, "+", EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, minusX + (btnS - minusW) / 2, midY, "-", true, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, plusX + (btnS - plusW) / 2, midY, "+", true, EpdFontFamily::BOLD);
}

void FrontlightQuickActivity::drawSliderTrack(const int barX, const int barY, const int barW, const int barH,
                                              const uint8_t value01to100) const {
  // Soft track: thicker fill, single-stroke knob (no double ring).
  const int trackH = barH + 4;
  const int trackY = barY - 2;
  const int rad = trackH / 2;
  renderer.drawRoundedRect(barX, trackY, barW, trackH, 2, rad, true);
  const int fillW = std::max(0, (barW * static_cast<int>(value01to100)) / 100);
  if (fillW > rad) {
    renderer.fillRoundedRect(barX, trackY, fillW, trackH, rad, Color::Black);
  }
  const int kx = std::clamp(barX + fillW, barX + kKnobR, barX + barW - kKnobR);
  const int ky = barY + barH / 2;
  const int r = kKnobR;
  renderer.fillRoundedRect(kx - r, ky - r, r * 2, r * 2, r, Color::White);
  renderer.drawRoundedRect(kx - r, ky - r, r * 2, r * 2, 2, r, true);
}

void FrontlightQuickActivity::drawToggleRow(const int y, const int h, const char* label, const bool on,
                                            int& outToggleX, int& outToggleW) const {
  const int lh = renderer.getLineHeight(UI_10_FONT_ID);
  const int textY = y + (h - lh) / 2;
  renderer.drawText(UI_10_FONT_ID, sheetLeft + kPad, textY, label, true, EpdFontFamily::REGULAR);
  // Plain On/Off text — no pill/circle chrome.
  const char* state = on ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
  const auto st = on ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const int tw = renderer.getTextWidth(UI_10_FONT_ID, state, st);
  outToggleW = std::max(40, tw + 8);
  outToggleX = sheetLeft + sheetW - kPad - outToggleW;
  renderer.drawText(UI_10_FONT_ID, outToggleX + (outToggleW - tw) / 2, textY, state, true, st);
}

void FrontlightQuickActivity::drawOnOffButton() const {
  const bool on = SETTINGS.frontlightOn != 0;
  // Bare lightbulb only — no circle chrome around it.
  drawLightbulb(renderer, toggleX + toggleW / 2, toggleY + toggleH / 2 - 1, 30, on);
}

void FrontlightQuickActivity::render(RenderLock&&) {
  layoutGeometry();
  blitBackground();
  drawCardChrome();
  drawPresetChips();

  // Brightness label + live percent (right-aligned).
  {
    const char* briTitle = tr(STR_BRIGHTNESS);
    renderer.drawText(UI_10_FONT_ID, sheetLeft + kPad, briLabelY, briTitle, true, EpdFontFamily::REGULAR);
    char pct[8];
    formatPct(pct, sizeof(pct), SETTINGS.frontlightBrightness);
    const int pw = renderer.getTextWidth(UI_12_FONT_ID, pct, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, sheetLeft + sheetW - kPad - pw, briLabelY - 2, pct, true, EpdFontFamily::BOLD);
  }
  drawMinusPlus(briMinusX, briMinusY, briPlusX, briPlusY, briBtnS);
  drawSliderTrack(briBarX, briBarY, briBarW, briBarH, SETTINGS.frontlightBrightness);

  // Color: label left + live percent right (same pattern as Brightness).
  {
    renderer.drawText(UI_10_FONT_ID, sheetLeft + kPad, warmLabelY, "Color", true, EpdFontFamily::REGULAR);
    char pct[8];
    formatPct(pct, sizeof(pct), SETTINGS.frontlightWarmPercent);
    const int pw = renderer.getTextWidth(UI_12_FONT_ID, pct, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, sheetLeft + sheetW - kPad - pw, warmLabelY - 2, pct, true, EpdFontFamily::BOLD);
  }
  drawMinusPlus(warmMinusX, warmMinusY, warmPlusX, warmPlusY, briBtnS);
  drawSliderTrack(warmBarX, warmBarY, warmBarW, warmBarH, SETTINGS.frontlightWarmPercent);

  drawToggleRow(darkY, darkH, tr(STR_READER_DARK_MODE), SETTINGS.readerDarkMode != 0, darkToggleX, darkToggleW);
  if (SETTINGS.readerDarkMode != 0) {
    drawToggleRow(readerOnlyY, readerOnlyH, tr(STR_DARK_MODE_READER_ONLY), SETTINGS.darkModeReaderOnly != 0,
                  readerOnlyToggleX, readerOnlyToggleW);
  }

  drawOnOffButton();

  firstPaint_ = false;
  if (forceFullPaint_) {
    forceFullPaint_ = false;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  // Card-only update (open + live edits) — no full Home flash.
  const int wx = std::max(0, sheetLeft - 4);
  const int wy = std::max(0, sheetTop - 4);
  const int ww = std::min(renderer.getScreenWidth() - wx, sheetW + 10);
  const int wh = std::min(renderer.getScreenHeight() - wy, sheetH + 10);
  renderer.displayWindow(wx, wy, ww, wh);
}

#endif
