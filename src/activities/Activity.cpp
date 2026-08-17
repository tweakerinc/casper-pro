#include "Activity.h"

#include <HalGPIO.h>

#include "ActivityManager.h"
#include "components/UITheme.h"
#include "util/SystemLog.h"

void Activity::onEnter() {
  LOG_DBG("ACT", "Entering activity: %s", name.c_str());
  SystemLog::logVerbose("ACT", "enter %s", name.c_str());
}

void Activity::onExit() {
  LOG_DBG("ACT", "Exiting activity: %s", name.c_str());
  SystemLog::logVerbose("ACT", "exit %s", name.c_str());
}

void Activity::requestUpdate(bool immediate) { activityManager.requestUpdate(immediate); }

void Activity::requestUpdateAndWait() { activityManager.requestUpdateAndWait(); }

void Activity::onGoHome(HomeMenuItem item) { activityManager.goHome(item); }

void Activity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  this->resultHandler = std::move(resultHandler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finish() { activityManager.popActivity(); }

Activity::ListTouchResult Activity::handleListTouch(int& selectedIndex, const int itemCount, const int listTop,
                                                    const int listHeight, const bool hasSubtitle) {
  int touched = -1;
  int tx = 0, ty = 0;

  // Long-press before tap so a hold is not also treated as Activated on lift.
  if (mappedInput.wasTouchLongPress(tx, ty)) {
    if (mappedInput.listItemAtPoint(tx, ty, touched, itemCount, listTop, listHeight, hasSubtitle)) {
      selectedIndex = touched;
      mappedInput.suppressTouchContact();
      return ListTouchResult::LongPress;
    }
  }

  if (mappedInput.wasListItemTouchedDown(touched, itemCount, selectedIndex, listTop, listHeight, hasSubtitle)) {
    // Touch: update index without bold-highlight repaint; buttons still highlight.
    selectedIndex = touched;
    if (!gpio.hasTouch()) {
      requestUpdate();
    }
    return ListTouchResult::Consumed;
  }
  if (mappedInput.wasListItemTapped(touched, itemCount, selectedIndex, listTop, listHeight, hasSubtitle)) {
    selectedIndex = touched;
    return ListTouchResult::Activated;
  }
  return ListTouchResult::None;
}
