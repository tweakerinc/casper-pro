#pragma once
#include <algorithm>

// The contact owner, not a recognizer: the HAL supplies the stationary hold.
// Once armed it consumes the complete contact, including release. Pure state
// makes lower-left conflicts and interruption behavior host-testable.
class LeftBrightnessGesture {
 public:
  struct Result { bool consumed = false, changed = false, save = false; int level = 0; };
  Result update(bool enabled, int width, int height, bool longPress, int startX,
                bool held, int y, int currentLevel) {
    if (!enabled) return cancel();
    if (!active_) {
      if (!longPress || !held || startX < 0 || startX >= std::max(24, width / 8)) return {};
      active_ = true;
      dragging_ = false;
      anchorY_ = y;
      anchorLevel_ = lastLevel_ = std::clamp(currentLevel, 0, 100);
      return {true, false, false, lastLevel_};
    }
    if (!held) return cancel();
    const int delta = anchorY_ - y;
    if (!dragging_ && delta > -12 && delta < 12) return {true, false, false, lastLevel_};
    dragging_ = true;
    const int level = std::clamp(anchorLevel_ + delta * 100 / std::max(80, height * 45 / 100), 0, 100);
    const bool changed = level != lastLevel_;
    lastLevel_ = level;
    return {true, changed, false, level};
  }
  Result cancel() {
    const Result out{active_, false, active_ && dragging_, lastLevel_};
    active_ = dragging_ = false;
    return out;
  }
  bool active() const { return active_; }
 private:
  bool active_ = false, dragging_ = false;
  int anchorY_ = 0, anchorLevel_ = 40, lastLevel_ = 40;
};
