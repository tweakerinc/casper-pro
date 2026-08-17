#pragma once

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

// Settings → Controls → Gestures: map edge swipes to actions.
// Tap a row (or Confirm) to pick from a popup — no cycle-through enums.
// At least one gesture must open Menu or Settings (escape hatch).
class GestureSettingsActivity final : public Activity {
 public:
  GestureSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("GestureSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 public:
  enum class Slot : uint8_t {
    TopLeftDown = 0,
    TopRightDown,
    BottomLeftUp,
    BottomRightUp,
    TopLeftToRight,
    TopRightToLeft,
    Count
  };

  int selectorIndex = 0;
  ButtonNavigator buttonNavigator;
  OptionPopup actionPopup;

  uint8_t& slotRef(Slot s);
  uint8_t slotValue(Slot s) const;
  void setSlot(Slot s, uint8_t action);
  void ensureEscape();
  void openActionPicker(Slot s);
  bool tryApplyAction(Slot s, uint8_t action);
  static const char* slotTitle(Slot s);
  static const char* slotHint(Slot s);
  static const char* actionLabel(uint8_t action);
  // Row hit geometry shared by touch + render.
  void rowGeometry(int pageW, int pageH, int& bandTop, int& rowH, int& contentLeft, int& contentRight) const;
};
