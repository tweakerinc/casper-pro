#include "GestureSettingsActivity.h"

#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include <HalGPIO.h>

#include "CasperSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/UiGhostPolicy.h"

namespace {
constexpr int kSlots = static_cast<int>(GestureSettingsActivity::Slot::Count);

// Compact direction marks drawn with lines (no dependency on glyph coverage).
void drawDirectionMark(const GfxRenderer& r, const GestureSettingsActivity::Slot s, const int cx, const int cy) {
  constexpr int arm = 7;
  constexpr int head = 3;
  switch (s) {
    case GestureSettingsActivity::Slot::TopLeftDown:
      r.drawLine(cx - arm, cy - arm, cx - arm, cy + arm, true);
      r.drawLine(cx - arm, cy + arm, cx - arm + head, cy + arm - head, true);
      r.drawLine(cx - arm, cy + arm, cx - arm - head, cy + arm - head, true);
      r.drawLine(cx - arm, cy - arm, cx + 2, cy - arm, true);
      break;
    case GestureSettingsActivity::Slot::TopRightDown:
      r.drawLine(cx + arm, cy - arm, cx + arm, cy + arm, true);
      r.drawLine(cx + arm, cy + arm, cx + arm + head, cy + arm - head, true);
      r.drawLine(cx + arm, cy + arm, cx + arm - head, cy + arm - head, true);
      r.drawLine(cx + arm, cy - arm, cx - 2, cy - arm, true);
      break;
    case GestureSettingsActivity::Slot::BottomLeftUp:
      r.drawLine(cx - arm, cy + arm, cx - arm, cy - arm, true);
      r.drawLine(cx - arm, cy - arm, cx - arm + head, cy - arm + head, true);
      r.drawLine(cx - arm, cy - arm, cx - arm - head, cy - arm + head, true);
      r.drawLine(cx - arm, cy + arm, cx + 2, cy + arm, true);
      break;
    case GestureSettingsActivity::Slot::BottomRightUp:
      r.drawLine(cx + arm, cy + arm, cx + arm, cy - arm, true);
      r.drawLine(cx + arm, cy - arm, cx + arm + head, cy - arm + head, true);
      r.drawLine(cx + arm, cy - arm, cx + arm - head, cy - arm + head, true);
      r.drawLine(cx + arm, cy + arm, cx - 2, cy + arm, true);
      break;
    case GestureSettingsActivity::Slot::TopLeftToRight:
      r.drawLine(cx - arm, cy, cx + arm, cy, true);
      r.drawLine(cx + arm, cy, cx + arm - head, cy - head, true);
      r.drawLine(cx + arm, cy, cx + arm - head, cy + head, true);
      break;
    case GestureSettingsActivity::Slot::TopRightToLeft:
      r.drawLine(cx + arm, cy, cx - arm, cy, true);
      r.drawLine(cx - arm, cy, cx - arm + head, cy - head, true);
      r.drawLine(cx - arm, cy, cx - arm + head, cy + head, true);
      break;
    default:
      break;
  }
}
}  // namespace

void GestureSettingsActivity::onEnter() {
  Activity::onEnter();
  SETTINGS.sanitizeGestures();
  requestUpdate();
}

void GestureSettingsActivity::onExit() {
  SETTINGS.sanitizeGestures();
  SETTINGS.saveToFile();
  Activity::onExit();
}

uint8_t& GestureSettingsActivity::slotRef(const Slot s) {
  switch (s) {
    case Slot::TopLeftDown:
      return SETTINGS.gestureTopLeftDown;
    case Slot::TopRightDown:
      return SETTINGS.gestureTopRightDown;
    case Slot::BottomLeftUp:
      return SETTINGS.gestureBottomLeftUp;
    case Slot::BottomRightUp:
      return SETTINGS.gestureBottomRightUp;
    case Slot::TopLeftToRight:
      return SETTINGS.gestureTopLeftToRight;
    case Slot::TopRightToLeft:
    default:
      return SETTINGS.gestureTopRightToLeft;
  }
}

uint8_t GestureSettingsActivity::slotValue(const Slot s) const {
  return const_cast<GestureSettingsActivity*>(this)->slotRef(s);
}

void GestureSettingsActivity::setSlot(const Slot s, const uint8_t action) {
  slotRef(s) = action;
  ensureEscape();
}

void GestureSettingsActivity::ensureEscape() {
  if (!SETTINGS.gesturesKeepSettingsEscape()) {
    SETTINGS.gestureTopLeftDown = CasperSettings::GESTURE_MENU;
  }
}

const char* GestureSettingsActivity::slotTitle(const Slot s) {
  switch (s) {
    case Slot::TopLeftDown:
      return "Top Left";
    case Slot::TopRightDown:
      return "Top Right";
    case Slot::BottomLeftUp:
      return "Bottom Left";
    case Slot::BottomRightUp:
      return "Bottom Right";
    case Slot::TopLeftToRight:
      return "Top Edge";
    case Slot::TopRightToLeft:
      return "Top Edge";
    default:
      return "";
  }
}

const char* GestureSettingsActivity::slotHint(const Slot s) {
  switch (s) {
    case Slot::TopLeftDown:
    case Slot::TopRightDown:
      return "Swipe Down";
    case Slot::BottomLeftUp:
    case Slot::BottomRightUp:
      return "Swipe Up";
    case Slot::TopLeftToRight:
      return "Left To Right";
    case Slot::TopRightToLeft:
      return "Right To Left";
    default:
      return "";
  }
}

const char* GestureSettingsActivity::actionLabel(const uint8_t action) {
  using A = CasperSettings::GESTURE_ACTION;
  switch (action) {
    case A::GESTURE_NONE:
      return "None";
    case A::GESTURE_MENU:
      return "Menu";
    case A::GESTURE_SETTINGS:
      return "Settings";
    case A::GESTURE_LIBRARY:
      return "Library";
    case A::GESTURE_RECENTS:
      return "Recents";
    case A::GESTURE_LIGHT:
      return "Light Menu";
    case A::GESTURE_HOME:
      return "Home";
    case A::GESTURE_LIGHT_TOGGLE:
      return "Light On/Off";
    case A::GESTURE_DARK_TOGGLE:
      return "Dark Mode On/Off";
    default:
      return "None";
  }
}

bool GestureSettingsActivity::tryApplyAction(const Slot s, const uint8_t action) {
  const uint8_t prev = slotValue(s);
  setSlot(s, action);
  if (!SETTINGS.gesturesKeepSettingsEscape()) {
    setSlot(s, prev);
    return false;
  }
  return true;
}

void GestureSettingsActivity::openActionPicker(const Slot s) {
  selectorIndex = static_cast<int>(s);
  // Order matches GESTURE_ACTION enum (append-only).
  static const char* const kActions[] = {
      "None",           "Menu",        "Settings", "Library", "Recents", "Light Menu", "Home",
      "Light On/Off",   "Dark Mode On/Off",
  };
  static_assert(sizeof(kActions) / sizeof(kActions[0]) == CasperSettings::GESTURE_ACTION_COUNT);

  const int current = std::clamp(static_cast<int>(slotValue(s)), 0,
                                 static_cast<int>(CasperSettings::GESTURE_ACTION_COUNT) - 1);
  char title[48];
  snprintf(title, sizeof(title), "%s", slotTitle(s));

  actionPopup.show(title, kActions, CasperSettings::GESTURE_ACTION_COUNT, current, [this, s](const int idx) {
    if (idx < 0 || idx >= static_cast<int>(CasperSettings::GESTURE_ACTION_COUNT)) return;
    (void)tryApplyAction(s, static_cast<uint8_t>(idx));
  });
  requestUpdate();
}

void GestureSettingsActivity::rowGeometry(const int /*pageW*/, const int pageH, int& bandTop, int& rowH,
                                         int& contentLeft, int& contentRight) const {
  const auto& m = UITheme::getInstance().getMetrics();
  // Pull content up: short cue under header, escape note sits above button hints.
  const int headerH = m.headerHeight + m.topPadding;
  const int footerH = m.buttonHintsHeight;
  const int cueH = 14;
  const int escapeH = 16;
  bandTop = headerH + cueH;
  const int bandBottom = pageH - footerH - escapeH - 4;
  const int bandH = std::max(1, bandBottom - bandTop);
  rowH = bandH / kSlots;
  contentLeft = 18;
  contentRight = renderer.getScreenWidth() - 18;
}

void GestureSettingsActivity::loop() {
  if (actionPopup.isActive()) {
    if (actionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) {
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  int bandTop = 0, rowH = 1, contentLeft = 0, contentRight = 0;
  rowGeometry(renderer.getScreenWidth(), renderer.getScreenHeight(), bandTop, rowH, contentLeft, contentRight);

  int tx = 0, ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    if (ty >= bandTop && ty < bandTop + rowH * kSlots) {
      const int row = std::clamp((ty - bandTop) / std::max(1, rowH), 0, kSlots - 1);
      openActionPicker(static_cast<Slot>(row));
      return;
    }
  }

  buttonNavigator.onNextRelease([this] {
    selectorIndex = (selectorIndex + 1) % kSlots;
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectorIndex = (selectorIndex + kSlots - 1) % kSlots;
    requestUpdate();
  });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    openActionPicker(static_cast<Slot>(std::clamp(selectorIndex, 0, kSlots - 1)));
    return;
  }
}

void GestureSettingsActivity::render(RenderLock&&) {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  renderer.clearScreen(0xFF);

  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageW, m.headerHeight}, "Gestures");

  // Compact cue tucked under the header (saves vertical room for the escape line).
  const char* cue = "Tap A Gesture To Assign";
  const int cueW = renderer.getTextWidth(SMALL_FONT_ID, cue, EpdFontFamily::REGULAR);
  renderer.drawText(SMALL_FONT_ID, (pageW - cueW) / 2, m.topPadding + m.headerHeight + 2, cue, true,
                    EpdFontFamily::REGULAR);

  int bandTop = 0, rowH = 1, contentLeft = 0, contentRight = 0;
  rowGeometry(pageW, pageH, bandTop, rowH, contentLeft, contentRight);

  for (int i = 0; i < kSlots; ++i) {
    const Slot s = static_cast<Slot>(i);
    const int rowTop = bandTop + i * rowH;
    const int midY = rowTop + rowH / 2;
    const bool focused = (i == selectorIndex) && !actionPopup.isActive();
    const auto titleStyle = focused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

    const int markCx = contentLeft + 12;
    drawDirectionMark(renderer, s, markCx, midY - 2);

    const int textX = contentLeft + 32;
    const int titleLh = renderer.getLineHeight(UI_12_FONT_ID);
    const int hintLh = renderer.getLineHeight(SMALL_FONT_ID);
    const int blockH = titleLh + 1 + hintLh;
    const int textTop = midY - blockH / 2;
    renderer.drawText(UI_12_FONT_ID, textX, textTop, slotTitle(s), true, titleStyle);
    renderer.drawText(SMALL_FONT_ID, textX, textTop + titleLh + 1, slotHint(s), true, EpdFontFamily::REGULAR);

    const char* act = actionLabel(slotValue(s));
    const int aw = renderer.getTextWidth(UI_12_FONT_ID, act, titleStyle);
    // Keep long labels ("Dark Mode On/Off") from colliding with the title block.
    const int actX = std::max(textX + 120, contentRight - aw);
    renderer.drawText(UI_12_FONT_ID, actX, midY - titleLh / 2, act, true, titleStyle);

    if (i < kSlots - 1) {
      const int lineY = rowTop + rowH - 1;
      renderer.drawLine(contentLeft, lineY, contentRight, lineY, true);
    }
  }

  // Escape hatch — above the button-hint strip (was clipped off-screen).
  const char* escapeHint = "One Gesture Must Open Menu Or Settings";
  const int ehW = renderer.getTextWidth(SMALL_FONT_ID, escapeHint, EpdFontFamily::REGULAR);
  const int footerY = pageH - m.buttonHintsHeight - 14;
  renderer.drawText(SMALL_FONT_ID, (pageW - ehW) / 2, footerY, escapeHint, true, EpdFontFamily::REGULAR);

  if (gpio.hasTouch()) {
    GUI.drawButtonHints(renderer, tr(STR_BACK), "Tap To Set", "", "");
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  if (actionPopup.isActive()) {
    actionPopup.render(renderer);
  }
  UiGhostPolicy::displayMenuFrame(renderer);
}
