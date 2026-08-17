#pragma once

#include <cstdint>
#include <memory>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Light Menu card over the previous screen:
//   Day | Night | Bed (words, bold when selected)
//   Brightness / color with live %
//   Dark mode (+ nested Reader Only)
//   Bare lightbulb power toggle
// Top-right swipe-down opens this; tap outside dismisses.
class FrontlightQuickActivity final : public Activity {
 public:
  explicit FrontlightQuickActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FrontlightQuick", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // When true, ActivityManager should not onResume+full-repaint the parent
  // (glass already holds the underlay from soft dismiss).
  static bool consumeSkipParentRepaint();

 private:
  // Cleared to false if Dark Mode (etc.) needs a full parent redraw on exit.
  bool skipParentRepaint_ = true;
  static bool s_skipParentRepaint;
  enum class DragTarget : uint8_t { None = 0, Brightness = 1, Warm = 2 };
  DragTarget drag = DragTarget::None;
  ButtonNavigator buttonNavigator;

  int sheetTop = 0, sheetH = 0, sheetLeft = 0, sheetW = 0;

  // Preset chips
  int chipY = 0, chipH = 0, chipW = 0, chipGap = 0, chipLeft = 0;

  // Brightness row
  int briLabelY = 0;
  int briMinusX = 0, briMinusY = 0, briBtnS = 0;
  int briBarX = 0, briBarY = 0, briBarW = 0, briBarH = 0;
  int briPlusX = 0, briPlusY = 0;

  // Color-temp row
  int warmLabelY = 0;
  int warmMinusX = 0, warmMinusY = 0;
  int warmBarX = 0, warmBarY = 0, warmBarW = 0, warmBarH = 0;
  int warmPlusX = 0, warmPlusY = 0;

  // Dark mode row + nested Reader Only
  int darkY = 0, darkH = 0;
  int darkToggleX = 0, darkToggleW = 0;
  int readerOnlyY = 0, readerOnlyH = 0;
  int readerOnlyToggleX = 0, readerOnlyToggleW = 0;

  // On/Off
  int toggleX = 0, toggleY = 0, toggleW = 0, toggleH = 0;

  std::unique_ptr<uint8_t[]> bgSnap_;
  size_t bgSnapBytes_ = 0;
  bool bgSnapOk_ = false;
  bool firstPaint_ = true;
  // After invert changes, one full FAST so the backdrop matches (not card-window only).
  bool forceFullPaint_ = false;

  void layoutGeometry();
  void applyLive() const;
  void persistActiveToPreset();
  void selectPreset(uint8_t preset);
  void setBrightness(int v01to100);
  void setWarm(int v01to100);
  void setBrightnessFromX(int x);
  void setWarmFromX(int x);
  void nudgeBrightness(int delta);
  void nudgeWarm(int delta);
  void toggleOnOff();
  void toggleDarkMode();
  void toggleReaderOnly();
  void drawCardChrome() const;
  void drawPresetChips() const;
  void drawMinusPlus(int minusX, int minusY, int plusX, int plusY, int btnS) const;
  void drawSliderTrack(int barX, int barY, int barW, int barH, uint8_t value01to100) const;
  void drawToggleRow(int y, int h, const char* label, bool on, int& outToggleX, int& outToggleW) const;
  void drawOnOffButton() const;
  bool hitSheet(int x, int y) const;
  bool hitBtn(int x, int y, int bx, int by, int s) const;
  bool hitBar(int x, int y, int barX, int barY, int barW, int barH) const;
  void blitBackground() const;
};
