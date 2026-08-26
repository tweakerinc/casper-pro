#include "MappedInputManager.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdlib>

#include <BoardConfig.h>

#include "CasperSettings.h"
#include "components/UITheme.h"

bool MappedInputManager::isNavDirectionSwapped() const {
  // Key the swap on the orientation the screen is *actually* rendered at, not the persisted reader
  // setting. Home/settings force Portrait, so they never swap.
  // When Orient Front Buttons is On:
  //   Portrait 180° — full axis follow (working as intended).
  //   Landscape CCW — front slot 3 (Up func) acts/labels as Down; slot 4 as Up.
  // Portrait and Landscape CW never swap here (Orient defaults Off for those layouts;
  // CW+On would feel inverted for page turn, so leave mapping alone).
  if (!SETTINGS.frontButtonFollowOrientation) {
    return false;
  }
  const auto o = renderer.getOrientation();
  return o == GfxRenderer::PortraitInverted || o == GfxRenderer::LandscapeCounterClockwise;
}

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const auto sideLayout = SETTINGS.sideButtonLayout;
  const bool orientSwap = isNavDirectionSwapped();

  // Front slots are hw 0–3; side slots are hw 4–5 (X3 left/right, X4 upper/lower).
  // Orient Front Buttons must only re-map the front cluster — side polarity stays
  // under Side Button Layout so Landscape CCW does not invert page-turn sides.
  constexpr uint8_t kFrontCount = 4;
  constexpr uint8_t kHwCount = CasperSettings::HW_REMAP_BUTTON_COUNT;

  auto anyFrontWithFunc = [&](const uint8_t func) -> bool {
    for (uint8_t hw = 0; hw < kFrontCount && hw < kHwCount; hw++) {
      if (SETTINGS.hwButtonFunction[hw] == func && (gpio.*fn)(hw)) return true;
    }
    return false;
  };
  auto anySideWithFunc = [&](const uint8_t func) -> bool {
    for (uint8_t hw = kFrontCount; hw < kHwCount; hw++) {
      if (SETTINGS.hwButtonFunction[hw] == func && (gpio.*fn)(hw)) return true;
    }
    return false;
  };
  // Any physical key (front or side) assigned this function.
  auto anyWithFunc = [&](const uint8_t func) -> bool {
    return anyFrontWithFunc(func) || anySideWithFunc(func);
  };

  switch (button) {
    case Button::Back:
      return anyWithFunc(CasperSettings::BTN_FUNC_BACK);
    case Button::Confirm:
      return anyWithFunc(CasperSettings::BTN_FUNC_CONFIRM);
    // Left/Right/Up/Down are *logical* screen directions. When Orient Front
    // Buttons follows Portrait 180° / Landscape CCW, front slots are mirrored
    // so the pill labeled "Up" moves up (mapLabels already swaps captions).
    // Side keys keep their assigned function — side polarity is Side Button Layout.
    case Button::Left:
      if (orientSwap) {
        return anyFrontWithFunc(CasperSettings::BTN_FUNC_RIGHT) ||
               anySideWithFunc(CasperSettings::BTN_FUNC_LEFT);
      }
      return anyWithFunc(CasperSettings::BTN_FUNC_LEFT);
    case Button::Right:
      if (orientSwap) {
        return anyFrontWithFunc(CasperSettings::BTN_FUNC_LEFT) ||
               anySideWithFunc(CasperSettings::BTN_FUNC_RIGHT);
      }
      return anyWithFunc(CasperSettings::BTN_FUNC_RIGHT);
    case Button::Up:
      if (orientSwap) {
        return anyFrontWithFunc(CasperSettings::BTN_FUNC_DOWN) ||
               anySideWithFunc(CasperSettings::BTN_FUNC_UP);
      }
      return anyWithFunc(CasperSettings::BTN_FUNC_UP);
    case Button::Down:
      if (orientSwap) {
        return anyFrontWithFunc(CasperSettings::BTN_FUNC_UP) ||
               anySideWithFunc(CasperSettings::BTN_FUNC_DOWN);
      }
      return anyWithFunc(CasperSettings::BTN_FUNC_DOWN);
    case Button::Power:
      // Power button bypasses remapping.
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack: {
      // Page turn uses Up/Down + Left/Right. Side layout sets base polarity;
      // orientSwap applies to front only so Portrait 180 / Landscape CCW keep
      // front "forward" correct without flipping side prev/next feel.
      const bool frontPrevIsUpLeft = (sideLayout == CasperSettings::PREV_NEXT) != orientSwap;
      const bool sidePrevIsUpLeft = (sideLayout == CasperSettings::PREV_NEXT);
      switch (sideLayout) {
        case CasperSettings::PREV_NEXT:
        case CasperSettings::NEXT_PREV: {
          bool hit = false;
          if (sidePrevIsUpLeft) {
            hit = anySideWithFunc(CasperSettings::BTN_FUNC_UP) || anySideWithFunc(CasperSettings::BTN_FUNC_LEFT);
          } else {
            hit = anySideWithFunc(CasperSettings::BTN_FUNC_DOWN) || anySideWithFunc(CasperSettings::BTN_FUNC_RIGHT);
          }
          if (frontPrevIsUpLeft) {
            hit = hit || anyFrontWithFunc(CasperSettings::BTN_FUNC_UP) ||
                  anyFrontWithFunc(CasperSettings::BTN_FUNC_LEFT);
          } else {
            hit = hit || anyFrontWithFunc(CasperSettings::BTN_FUNC_DOWN) ||
                  anyFrontWithFunc(CasperSettings::BTN_FUNC_RIGHT);
          }
          return hit;
        }
        case CasperSettings::SIDE_BUTTONS_DISABLED:
        default:
          // Still allow remapped front Up/Down/Left/Right for page turn when "sides disabled"
          // only meant the side-layout enum; keep prior behavior (no page from layout).
          return false;
      }
    }
    case Button::PageForward: {
      const bool frontNextIsDownRight = (sideLayout == CasperSettings::PREV_NEXT) != orientSwap;
      const bool sideNextIsDownRight = (sideLayout == CasperSettings::PREV_NEXT);
      switch (sideLayout) {
        case CasperSettings::PREV_NEXT:
        case CasperSettings::NEXT_PREV: {
          bool hit = false;
          if (sideNextIsDownRight) {
            hit = anySideWithFunc(CasperSettings::BTN_FUNC_DOWN) || anySideWithFunc(CasperSettings::BTN_FUNC_RIGHT);
          } else {
            hit = anySideWithFunc(CasperSettings::BTN_FUNC_UP) || anySideWithFunc(CasperSettings::BTN_FUNC_LEFT);
          }
          if (frontNextIsDownRight) {
            hit = hit || anyFrontWithFunc(CasperSettings::BTN_FUNC_DOWN) ||
                  anyFrontWithFunc(CasperSettings::BTN_FUNC_RIGHT);
          } else {
            hit = hit || anyFrontWithFunc(CasperSettings::BTN_FUNC_UP) ||
                  anyFrontWithFunc(CasperSettings::BTN_FUNC_LEFT);
          }
          return hit;
        }
        case CasperSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
    }
    case Button::NavNext: {
      // Logical next = Down | Right (Up/Down/Left/Right already apply front orient follow).
      return mapButton(Button::Down, fn) || mapButton(Button::Right, fn);
    }
    case Button::NavPrevious: {
      return mapButton(Button::Up, fn) || mapButton(Button::Left, fn);
    }
  }

  return false;
}

namespace {
// Narrow bezel only. Was 0.25 and stole mid-left page-turn L→R swipes as Back
// (closed the book). Home-pad devices disable this entirely — see wasBackGesture.
constexpr float LEFT_EDGE_BACK_GESTURE_FRAC_X = 0.08f;
constexpr float BOTTOM_EDGE_BACK_GESTURE_FRAC_Y = 0.18f;
// Larger top band so TL/TR menu/light swipes register on 480h panels with clock chrome.
constexpr float TOP_EDGE_MENU_GESTURE_FRAC_Y = 0.22f;
constexpr unsigned long TOUCH_DOWN_SELECT_DELAY_MS = 90;
constexpr unsigned long TOUCH_HELD_OVERRIDE_WINDOW_MS = 250;
}  // namespace

bool MappedInputManager::hasTouch() const { return gpio.hasTouch(); }

bool MappedInputManager::needsOnScreenFrontChrome() const { return gpio.needsOnScreenFrontChrome(); }

void MappedInputManager::setSoftFrontChromeEnabled(const bool enabled) { softFrontChromeEnabled = enabled; }

void MappedInputManager::rememberTouchHeldTime() const {
  touchHeldOverrideValid = true;
  touchHeldOverrideMs = gpio.lastTouchHeldMs();
  touchHeldOverrideAt = millis();
}

bool MappedInputManager::wasScreenTapped(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  rememberTouchHeldTime();
  return true;
}

bool MappedInputManager::wasScreenTouchDown(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  unsigned long heldMs = 0;
  if (!gpio.isTouchTapCandidate(nx, ny, heldMs)) return false;
  if (heldMs < TOUCH_DOWN_SELECT_DELAY_MS) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::isScreenTouchHeld(int& x, int& y) const {
  // Live contact position while the finger is down (no tap-slop gate) — drag tracking.
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.isTouchHeldAt(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasTouchLongPress(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchLongPress(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  rememberTouchHeldTime();
  return true;
}

void MappedInputManager::suppressTouchContact() const { gpio.suppressTouchContact(); }

bool MappedInputManager::listItemAtPoint(const int x, const int y, int& index, const int itemCount, const int listTop,
                                         const int listHeight, const bool hasSubtitle) const {
  return listItemFromPoint(x, y, index, itemCount, /*selectedIndex=*/0, listTop, listHeight, hasSubtitle);
}

bool MappedInputManager::wasTapInRect(const int x, const int y, const int width, const int height) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) && tx >= x && tx < x + width && ty >= y && ty < y + height;
}

bool MappedInputManager::listItemFromPoint(const int x, const int y, int& index, const int itemCount,
                                           const int selectedIndex, const int listTop, const int listHeight,
                                           const bool hasSubtitle) const {
  (void)x;
  if (itemCount <= 0) return false;
  if (y < listTop || y >= listTop + listHeight) return false;

  const auto& theme = UITheme::getInstance().getTheme();
  const int rowStep = theme.getListRowStep(hasSubtitle);
  if (rowStep <= 0) return false;

  const int pageItems = theme.getListPageItems(listHeight, hasSubtitle);
  if (pageItems <= 0) return false;
  const int pageStart = std::max(0, selectedIndex / pageItems) * pageItems;
  const int row = (y - listTop) / rowStep;
  const int tapped = pageStart + row;
  if (row < 0 || row >= pageItems || tapped >= itemCount) return false;
  index = tapped;
  return true;
}

bool MappedInputManager::wasListItemTapped(int& index, const int itemCount, const int selectedIndex, const int listTop,
                                           const int listHeight, const bool hasSubtitle) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) &&
         listItemFromPoint(tx, ty, index, itemCount, selectedIndex, listTop, listHeight, hasSubtitle);
}

bool MappedInputManager::wasListItemTouchedDown(int& index, const int itemCount, const int selectedIndex,
                                                const int listTop, const int listHeight, const bool hasSubtitle) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTouchDown(tx, ty) &&
         listItemFromPoint(tx, ty, index, itemCount, selectedIndex, listTop, listHeight, hasSubtitle);
}

MappedInputManager::RowTouch MappedInputManager::rowTouch(int& row, const int top, const int rowStep,
                                                          const int rowCount, const int xStart, const int xEnd,
                                                          const int rowHeight) const {
  if (rowStep <= 0 || rowCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (x < xStart || x >= xEnd || y < top) return false;
    const int r = (y - top) / rowStep;
    if (r >= rowCount) return false;
    if (rowHeight > 0 && (y - top) % rowStep >= rowHeight) return false;
    row = r;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

MappedInputManager::RowTouch MappedInputManager::colTouch(int& col, const int left, const int colStep,
                                                          const int colCount, const int yStart, const int yEnd,
                                                          const int colWidth) const {
  if (colStep <= 0 || colCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (y < yStart || y >= yEnd || x < left) return false;
    const int c = (x - left) / colStep;
    if (c >= colCount) return false;
    if (colWidth > 0 && (x - left) % colStep >= colWidth) return false;
    col = c;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

bool MappedInputManager::decodeSwipe(int& sx, int& sy, int& ex, int& ey) const {
  float nxs = 0.0f;
  float nys = 0.0f;
  float nxe = 0.0f;
  float nye = 0.0f;
  if (!gpio.wasSwipe(nxs, nys, nxe, nye)) return false;
  renderer.tapToLogical(nxs, nys, sx, sy);
  renderer.tapToLogical(nxe, nye, ex, ey);
  return true;
}

MappedInputManager::SwipeDir MappedInputManager::wasSwipe() const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return SwipeDir::None;
  const int dx = ex - sx;
  const int dy = ey - sy;
  if (std::abs(dx) >= std::abs(dy)) {
    return dx < 0 ? SwipeDir::Left : SwipeDir::Right;
  }
  return dy < 0 ? SwipeDir::Up : SwipeDir::Down;
}

MappedInputManager::SwipeDir MappedInputManager::wasHorizontalSwipeInY(const int yTop, const int yBottom) const {
  if (yBottom <= yTop) return SwipeDir::None;
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return SwipeDir::None;
  const int dx = ex - sx;
  const int dy = ey - sy;
  if (std::abs(dx) < std::abs(dy)) return SwipeDir::None;
  const int midY = (sy + ey) / 2;
  if (midY < yTop || midY >= yBottom) return SwipeDir::None;
  return dx < 0 ? SwipeDir::Left : SwipeDir::Right;
}

bool MappedInputManager::wasBackGesture() const {
  // Back = left-to-right swipe starting near the left edge. Edge-anchored so that
  // mid-screen horizontal swipes stay available to activities that consume
  // SwipeDir::Left/Right (e.g. page turn, percent selection, image viewer).
  //
  // X4 Pro (and any board with a capacitive Home pad): never synthesize Back from
  // L→R. Home is leave-to-home; L→R must page previous. Edge-back closed books.
  if (BoardConfig::hasHomeKey()) return false;
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const bool hit = sx <= renderer.getScreenWidth() * LEFT_EDGE_BACK_GESTURE_FRAC_X && ex > sx &&
                   std::abs(ex - sx) > std::abs(ey - sy);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasMenuGesture() const {
  // Downward swipe starting at the top edge (mirror of the bottom-edge home gesture).
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const int topEdgeBottom = static_cast<int>(renderer.getScreenHeight() * TOP_EDGE_MENU_GESTURE_FRAC_Y);
  const bool hit = sy <= topEdgeBottom && ey > sy && std::abs(ey - sy) > std::abs(ex - sx);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasTopLeftMenuGesture() const {
  int sx = 0, sy = 0, ex = 0, ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const int topEdgeBottom = static_cast<int>(renderer.getScreenHeight() * TOP_EDGE_MENU_GESTURE_FRAC_Y);
  const int midX = renderer.getScreenWidth() / 2;
  const bool hit =
      sy <= topEdgeBottom && sx < midX && ey > sy && std::abs(ey - sy) > std::abs(ex - sx);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasTopRightLightGesture() const {
  int sx = 0, sy = 0, ex = 0, ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const int topEdgeBottom = static_cast<int>(renderer.getScreenHeight() * TOP_EDGE_MENU_GESTURE_FRAC_Y);
  const int midX = renderer.getScreenWidth() / 2;
  const bool hit =
      sy <= topEdgeBottom && sx >= midX && ey > sy && std::abs(ey - sy) > std::abs(ex - sx);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasHomeGesture() const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (decodeSwipe(sx, sy, ex, ey)) {
    const int bottomEdgeTop =
        renderer.getScreenHeight() - static_cast<int>(renderer.getScreenHeight() * BOTTOM_EDGE_BACK_GESTURE_FRAC_Y);
    if (sy >= bottomEdgeTop && ey < sy && std::abs(ey - sy) > std::abs(ex - sx)) {
      rememberTouchHeldTime();
      return true;
    }
  }
  return false;
}

bool MappedInputManager::wasBottomLeftUpGesture() const {
  int sx = 0, sy = 0, ex = 0, ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int bottomEdgeTop = pageH - static_cast<int>(pageH * BOTTOM_EDGE_BACK_GESTURE_FRAC_Y);
  const bool hit = sy >= bottomEdgeTop && sx < pageW / 2 && ey < sy && std::abs(ey - sy) > std::abs(ex - sx);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasBottomRightUpGesture() const {
  int sx = 0, sy = 0, ex = 0, ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int bottomEdgeTop = pageH - static_cast<int>(pageH * BOTTOM_EDGE_BACK_GESTURE_FRAC_Y);
  const bool hit = sy >= bottomEdgeTop && sx >= pageW / 2 && ey < sy && std::abs(ey - sy) > std::abs(ex - sx);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasTopLeftToRightGesture() const {
  int sx = 0, sy = 0, ex = 0, ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const int topEdgeBottom = static_cast<int>(renderer.getScreenHeight() * TOP_EDGE_MENU_GESTURE_FRAC_Y);
  const int midX = renderer.getScreenWidth() / 2;
  // Start left half of top band, primarily horizontal rightward.
  const bool hit = sy <= topEdgeBottom && sx < midX && ex > sx && std::abs(ex - sx) > std::abs(ey - sy);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasTopRightToLeftGesture() const {
  int sx = 0, sy = 0, ex = 0, ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const int topEdgeBottom = static_cast<int>(renderer.getScreenHeight() * TOP_EDGE_MENU_GESTURE_FRAC_Y);
  const int midX = renderer.getScreenWidth() / 2;
  const bool hit = sy <= topEdgeBottom && sx >= midX && ex < sx && std::abs(ex - sx) > std::abs(ey - sy);
  if (hit) rememberTouchHeldTime();
  return hit;
}

namespace {
// Soft front chrome (X4 Pro): map strip taps to the same slots BaseTheme paints
// so Menu/Library/Recents/Read work without physical front keys.
int softChromeReleasedSlot(const MappedInputManager& input, HalGPIO& gpio, const GfxRenderer& renderer) {
  if (!gpio.needsOnScreenFrontChrome() || !input.isSoftFrontChromeEnabled()) return -1;
  int tx = 0;
  int ty = 0;
  if (!input.wasScreenTapped(tx, ty)) return -1;

  constexpr int kButtonW = 80;
  constexpr int kX4Positions[] = {58, 146, 254, 342};
  constexpr int kX3Positions[] = {65, 157, 291, 383};
  constexpr uint8_t kSlotToBtn[] = {HalGPIO::BTN_BACK, HalGPIO::BTN_CONFIRM, HalGPIO::BTN_LEFT, HalGPIO::BTN_RIGHT};
  const int* positions = gpio.deviceIsX3() ? kX3Positions : kX4Positions;
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int stripDepth = BaseTheme::frontButtonHintReserve(renderer);
  const auto orient = renderer.getOrientation();
  const bool landscapeCw = orient == GfxRenderer::LandscapeClockwise;
  const bool landscapeCcw = orient == GfxRenderer::LandscapeCounterClockwise;
  const bool inverted = orient == GfxRenderer::PortraitInverted;

  if (landscapeCw || landscapeCcw) {
    const int stripX = landscapeCcw ? (pageW - stripDepth) : 0;
    if (tx < stripX || tx >= stripX + stripDepth) return -1;
    const int portraitSpan = gpio.deviceIsX3() ? 528 : 480;
    constexpr int kClusterNudgeUp = 8;
    for (int i = 0; i < 4; ++i) {
      const int portraitCenterX = positions[i] + kButtonW / 2;
      const int scaled = (portraitCenterX * pageH + portraitSpan / 2) / portraitSpan;
      int yCenter = landscapeCcw ? (pageH - 1 - scaled) : scaled;
      yCenter -= kClusterNudgeUp;
      const int pillY = yCenter - kButtonW / 2;
      if (ty >= pillY && ty < pillY + kButtonW) return static_cast<int>(kSlotToBtn[i]);
    }
    return -1;
  }

  // Portrait home is 480×800 on X4/X4 Pro (X3 is 528×792). Bare/Penumbra/Minimal
  // draw four equal-width text columns across that width — hit-test the same
  // quarters so a finger tap matches the label, not the 80px X4 key cutouts.
  const int barY = inverted ? 0 : (pageH - stripDepth);
  if (ty < barY || ty >= barY + stripDepth) return -1;
  const int visualCol = std::clamp(tx * 4 / std::max(1, pageW), 0, 3);
  const int slot = inverted ? (3 - visualCol) : visualCol;
  return static_cast<int>(kSlotToBtn[slot]);
}

bool softChromeIsLogical(const MappedInputManager& input, HalGPIO& gpio, const GfxRenderer& renderer,
                         const MappedInputManager::Button button) {
  const int hw = softChromeReleasedSlot(input, gpio, renderer);
  if (hw < 0 || hw >= CasperSettings::HW_REMAP_BUTTON_COUNT) return false;
  const uint8_t func = SETTINGS.hwButtonFunction[static_cast<uint8_t>(hw)];
  switch (button) {
    case MappedInputManager::Button::Back:
      return func == CasperSettings::BTN_FUNC_BACK;
    case MappedInputManager::Button::Confirm:
      return func == CasperSettings::BTN_FUNC_CONFIRM;
    case MappedInputManager::Button::Left:
      return func == CasperSettings::BTN_FUNC_LEFT;
    case MappedInputManager::Button::Right:
      return func == CasperSettings::BTN_FUNC_RIGHT;
    case MappedInputManager::Button::Up:
      return func == CasperSettings::BTN_FUNC_UP;
    case MappedInputManager::Button::Down:
      return func == CasperSettings::BTN_FUNC_DOWN;
    case MappedInputManager::Button::Power:
    case MappedInputManager::Button::PageBack:
    case MappedInputManager::Button::PageForward:
    case MappedInputManager::Button::NavNext:
    case MappedInputManager::Button::NavPrevious:
      return false;
  }
  return false;
}
}  // namespace

bool MappedInputManager::wasPressed(const Button button) const {
  if (button == Button::Back && wasBackGesture()) return true;
  // Soft chrome is release-style (tap); treat as press too so Settings Back works.
  if (softChromeIsLogical(*this, gpio, renderer, button)) return true;
  return mapButton(button, &HalGPIO::wasPressed);
}

bool MappedInputManager::wasReleased(const Button button) const {
  if (button == Button::Back && wasBackGesture()) return true;
  if (softChromeIsLogical(*this, gpio, renderer, button)) return true;
  return mapButton(button, &HalGPIO::wasReleased);
}

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasAnyPressed() const { return gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const {
  if (!gpio.wasAnyPressed() && !gpio.wasAnyReleased() && touchHeldOverrideValid &&
      millis() - touchHeldOverrideAt <= TOUCH_HELD_OVERRIDE_WINDOW_MS) {
    return touchHeldOverrideMs;
  }
  touchHeldOverrideValid = false;
  return gpio.getHeldTime();
}

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Orientation swap flips each pair so physical feel matches a rotated reader.
  const bool swapLabels = isNavDirectionSwapped();
  // Vertical pair: callers usually pass Up/Down as previous/next for list menus.
  const char* upLabel = swapLabels ? next : previous;
  const char* downLabel = swapLabels ? previous : next;
  // Horizontal pair: always show true Left/Right names (not Up/Down aliases).
  // Previously Left/Right reused previous/next, so remapped "Left" still read "Up".
  const char* leftLabel = swapLabels ? tr(STR_DIR_RIGHT) : tr(STR_DIR_LEFT);
  const char* rightLabel = swapLabels ? tr(STR_DIR_LEFT) : tr(STR_DIR_RIGHT);

  // Label each physical front slot by the function assigned to it.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    if (hw >= CasperSettings::HW_REMAP_BUTTON_COUNT) return "";
    switch (SETTINGS.hwButtonFunction[hw]) {
      case CasperSettings::BTN_FUNC_BACK:
        return back;
      case CasperSettings::BTN_FUNC_CONFIRM:
        return confirm;
      case CasperSettings::BTN_FUNC_LEFT:
        return leftLabel;
      case CasperSettings::BTN_FUNC_RIGHT:
        return rightLabel;
      case CasperSettings::BTN_FUNC_UP:
        return upLabel;
      case CasperSettings::BTN_FUNC_DOWN:
        return downLabel;
      default:
        return "";
    }
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

namespace {
const char* directionFuncCaption(const uint8_t func, const char* backAction, const char* confirmAction) {
  using F = CasperSettings::BUTTON_FUNCTION;
  switch (func) {
    case F::BTN_FUNC_BACK:
      return backAction ? backAction : tr(STR_BACK);
    case F::BTN_FUNC_CONFIRM:
      return confirmAction ? confirmAction : tr(STR_SELECT);
    case F::BTN_FUNC_LEFT:
      return tr(STR_DIR_LEFT);
    case F::BTN_FUNC_RIGHT:
      return tr(STR_DIR_RIGHT);
    case F::BTN_FUNC_UP:
      return tr(STR_DIR_UP);
    case F::BTN_FUNC_DOWN:
      return tr(STR_DIR_DOWN);
    default:
      return "";
  }
}
}  // namespace

MappedInputManager::Labels MappedInputManager::mapDirectionLabels(const char* backAction,
                                                                  const char* confirmAction) const {
  // Physical front L→R (hw 0–3). When orient-follow is active, captions track
  // logical directions (same mirror as mapButton / mapLabels) so keyboard chrome
  // stays consistent with Up/Down/Left/Right actions.
  const bool swap = isNavDirectionSwapped();
  auto logicalFunc = [swap](uint8_t func) -> uint8_t {
    if (!swap) return func;
    using F = CasperSettings::BUTTON_FUNCTION;
    switch (func) {
      case F::BTN_FUNC_UP:
        return F::BTN_FUNC_DOWN;
      case F::BTN_FUNC_DOWN:
        return F::BTN_FUNC_UP;
      case F::BTN_FUNC_LEFT:
        return F::BTN_FUNC_RIGHT;
      case F::BTN_FUNC_RIGHT:
        return F::BTN_FUNC_LEFT;
      default:
        return func;
    }
  };
  const auto& map = SETTINGS.hwButtonFunction;
  return {directionFuncCaption(logicalFunc(map[CasperSettings::FRONT_HW_BACK]), backAction, confirmAction),
          directionFuncCaption(logicalFunc(map[CasperSettings::FRONT_HW_CONFIRM]), backAction, confirmAction),
          directionFuncCaption(logicalFunc(map[CasperSettings::FRONT_HW_LEFT]), backAction, confirmAction),
          directionFuncCaption(logicalFunc(map[CasperSettings::FRONT_HW_RIGHT]), backAction, confirmAction)};
}

void MappedInputManager::mapSideDirectionLabels(const char*& sideA, const char*& sideB) const {
  // hw 4 = X3 left / X4 upper; hw 5 = X3 right / X4 lower.
  const auto& map = SETTINGS.hwButtonFunction;
  sideA = directionFuncCaption(map[4], tr(STR_BACK), tr(STR_SELECT));
  sideB = directionFuncCaption(map[5], tr(STR_BACK), tr(STR_SELECT));
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}

int MappedInputManager::getReleasedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // Bypasses remapping for screens whose labels are fixed to physical slots.
  if (gpio.wasReleased(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasReleased(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasReleased(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasReleased(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }

  // X4 Pro soft bottom strip (Home pad is global goHome — not Menu).
  if (needsOnScreenFrontChrome()) {
    const int slot = softChromeReleasedSlot(*this, gpio, renderer);
    if (slot >= 0) return slot;
  }
  return -1;
}

bool MappedInputManager::isFrontButtonPressed(const uint8_t buttonIndex) const { return gpio.isPressed(buttonIndex); }
