#pragma once

#include <FrontlightManager.h>

#include <algorithm>

#include "CasperSettings.h"

// Global frontlight (inert when FREEINK_CAP_FRONTLIGHT=0 / no hardware).
inline FrontlightManager& frontlight() {
  static FrontlightManager fl;
  return fl;
}

inline int leftEdgeFrontlightWidth(int pageW) { return std::max(24, pageW / 8); }

// Push SETTINGS → LEDs (on/off + brightness + CT mix). Single source of truth.
inline void applyFrontlightFromSettings() {
  auto& fl = frontlight();
  if (!fl.present()) return;
  fl.setColorTemperature(SETTINGS.frontlightWarmPercent);
  if (SETTINGS.frontlightOn != 0) {
    fl.setBrightness(SETTINGS.frontlightBrightness);
  } else {
    fl.off();
  }
}

// Shared by the light sheet, left-edge gesture, and any future control so all
// paths stay on the same 0–100 scale and the same apply() path.
inline void setFrontlightBrightnessPercent(int pct01to100, const bool mirrorToActivePreset = true) {
  const int v = std::clamp(pct01to100, 0, 100);
  SETTINGS.frontlightBrightness = static_cast<uint8_t>(v);
  if (v > 0) {
    SETTINGS.frontlightOn = 1;
  }
  if (mirrorToActivePreset && SETTINGS.frontlightPreset < CasperSettings::FL_PRESET_COUNT) {
    SETTINGS.saveActiveToFrontlightPreset(SETTINGS.frontlightPreset);
  }
  applyFrontlightFromSettings();
}

inline void setFrontlightWarmPercent(int pct01to100, const bool mirrorToActivePreset = true) {
  const int v = std::clamp(pct01to100, 0, 100);
  SETTINGS.frontlightWarmPercent = static_cast<uint8_t>(v);
  if (mirrorToActivePreset && SETTINGS.frontlightPreset < CasperSettings::FL_PRESET_COUNT) {
    SETTINGS.saveActiveToFrontlightPreset(SETTINGS.frontlightPreset);
  }
  applyFrontlightFromSettings();
}
