#include "ActivityManager.h"

#include <FontCacheManager.h>
#include <HalPowerManager.h>

#include <algorithm>

#include "CasperSettings.h"
#include "CasperState.h"

#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"

#include "OpdsServerStore.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "home/CrashActivity.h"
#include "home/FileBrowserActivity.h"
#include "home/HomeActivity.h"
#include "home/RecentBooksActivity.h"
#include "network/CasperWebServerActivity.h"
#include "reader/ReaderActivity.h"
#include "settings/OpdsServerListActivity.h"
#include "settings/SettingsActivity.h"
#include "util/FrontlightQuickActivity.h"
#include "util/FrontlightUtil.h"
#include "util/FullScreenMessageActivity.h"
#include "util/SystemLog.h"

#include <BoardConfig.h>
#include <HalGPIO.h>

static portMUX_TYPE activityManagerSpinlock = portMUX_INITIALIZER_UNLOCKED;

void ActivityManager::begin() {
#if defined(configNUM_CORES) && configNUM_CORES > 1
  constexpr BaseType_t renderTaskCore = 1;
#else
  constexpr BaseType_t renderTaskCore = 0;
#endif
  xTaskCreatePinnedToCore(&renderTaskTrampoline, "ActivityManagerRender",
                          8192,               // Stack size
                          this,               // Parameters
                          1,                  // Priority
                          &renderTaskHandle,  // Task handle
                          renderTaskCore  // Keep long renders/cover decodes off CPU 0's idle watchdog when available
  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
}

void ActivityManager::renderTaskTrampoline(void* param) {
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

void ActivityManager::waitForRenderIdle() {
  // Main task only — never call from the render task (would deadlock).
  while (renderInProgress.load(std::memory_order_acquire)) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void ActivityManager::renderTaskLoop() {
  while (true) {
    // Wait for at least one paint request. ulTaskNotifyTake(pdTRUE) clears the
    // notification value so stacked wakes coalesce into a single take.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Clear deferred flag so loop() does not immediately re-notify for the same paint.
    requestedUpdate.store(false);

    // Cover the entire paint (including Home multipass after it unlocks the
    // RenderLock) so replace/pop cannot destroy the activity mid-multipass.
    renderInProgress.store(true, std::memory_order_release);

    // Acquire the lock before reading currentActivity to avoid a TOCTOU race
    // where the main task deletes the activity between the null-check and render().
    RenderLock lock;
    if (currentActivity) {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      const uint32_t tRender = millis();
      currentActivity->render(std::move(lock));
      const uint32_t renderMs = millis() - tRender;
      // HALF/full home paints are multi-second; flag only pathological hangs.
      if (renderMs >= 8000) {
        SystemLog::logCritical("RENDER", "slow %lums fre=%u", static_cast<unsigned long>(renderMs),
                               static_cast<unsigned>(ESP.getFreeHeap()));
      }
    }

    renderInProgress.store(false, std::memory_order_release);

    // Notify any task blocked in requestUpdateAndWait() that the render is done.
    TaskHandle_t waiter = nullptr;
    taskENTER_CRITICAL(&activityManagerSpinlock);
    waiter = waitingTaskHandle;
    waitingTaskHandle = nullptr;
    taskEXIT_CRITICAL(&activityManagerSpinlock);
    if (waiter) {
      xTaskNotify(waiter, 1, eSetValueWithOverwrite);
    }
  }
}

void ActivityManager::loop() {
  if (currentActivity) {
    // Capacitive Home pad: hierarchical Back, then Home.
    // Stack: [Home, Reader, Clip] + current Def → pop Def; next pop Clip; …
    // When current is the reader with only Home under it, one pop resumes Home.
    // Never skip intermediate screens (clipping tool used to jump straight home).
    if (gpio.wasHomeKeyTapped()) {
      if (currentActivity->isHomeActivity()) {
        return;
      }
      // Book menu / similar: close overlay first (returns to reader without goHome).
      if (currentActivity->handleHomeGesture()) {
        return;
      }
      if (!stackActivities.empty()) {
        // One level only — same as physical Back through the stack.
        popActivity();
        return;
      }
      goHome();
      return;
    }

    // Configurable edge gestures (Settings → Controls → Gestures).
    // Always run on touch boards — not gated on frontlight (TL menu used to miss
    // when only FRONTLIGHT block ran and handleMenuGesture returned false on Home).
    if (gpio.hasTouch() && currentActivity->name != "FrontlightQuick" &&
        currentActivity->name != "GestureSettings") {
      auto runGesture = [this](const uint8_t action) -> bool {
        using A = CasperSettings::GESTURE_ACTION;
        switch (action) {
          case A::GESTURE_NONE:
            return false;
          case A::GESTURE_MENU:
            if (currentActivity->handleMenuGesture()) return true;
            // Home / other: open home menu via goHome NONE then activity can open menu —
            // HomeActivity overrides handleMenuGesture.
            if (currentActivity->isHomeActivity()) return false;
            // Fallback: open Settings when menu is unavailable on this screen.
            goToSettings();
            return true;
          case A::GESTURE_SETTINGS:
            goToSettings();
            return true;
          case A::GESTURE_LIBRARY:
            goHome(HomeMenuItem::FILE_BROWSER);
            return true;
          case A::GESTURE_RECENTS:
            goHome(HomeMenuItem::RECENTS);
            return true;
          case A::GESTURE_LIGHT:
#if FREEINK_CAP_FRONTLIGHT
            if (frontlight().present()) {
              pushActivity(std::make_unique<FrontlightQuickActivity>(renderer, mappedInput));
              return true;
            }
#endif
            return false;
          case A::GESTURE_HOME:
            if (!currentActivity->isHomeActivity()) goHome();
            return true;
          case A::GESTURE_LIGHT_TOGGLE:
#if FREEINK_CAP_FRONTLIGHT
            if (frontlight().present()) {
              SETTINGS.frontlightOn = (SETTINGS.frontlightOn != 0) ? 0 : 1;
              if (SETTINGS.frontlightPreset < CasperSettings::FL_PRESET_COUNT) {
                SETTINGS.saveActiveToFrontlightPreset(SETTINGS.frontlightPreset);
              }
              applyFrontlightFromSettings();
              SETTINGS.saveToFile();
              return true;
            }
#endif
            return false;
          case A::GESTURE_DARK_TOGGLE: {
            SETTINGS.readerDarkMode = SETTINGS.readerDarkMode ? 0 : 1;
            if (SETTINGS.readerDarkMode) {
              SETTINGS.darkModeReaderOnly = 0;  // whole UI when turned on via gesture
            }
            renderer.setInvertOnDisplay(SETTINGS.readerDarkMode != 0 && SETTINGS.darkModeReaderOnly == 0);
            SETTINGS.saveToFile();
            // Force a full repaint so invert takes effect immediately.
            if (currentActivity) {
              currentActivity->requestUpdate(/*immediate=*/true);
            }
            return true;
          }
          default:
            return false;
        }
      };

      if (mappedInput.wasTopLeftMenuGesture() && runGesture(SETTINGS.gestureTopLeftDown)) return;
      if (mappedInput.wasTopRightLightGesture() && runGesture(SETTINGS.gestureTopRightDown)) return;
      // Bottom swipe-up must not eject the reader (Library/Recents/Home). Users
      // page with sides/horizontal swipes; leave-to-home is the Home pad.
      const bool reading = currentActivity->isReaderActivity();
      auto leavesBook = [](const uint8_t action) {
        using A = CasperSettings::GESTURE_ACTION;
        return action == A::GESTURE_HOME || action == A::GESTURE_LIBRARY || action == A::GESTURE_RECENTS;
      };
      if (mappedInput.wasBottomLeftUpGesture()) {
        if (!(reading && leavesBook(SETTINGS.gestureBottomLeftUp)) &&
            runGesture(SETTINGS.gestureBottomLeftUp)) {
          return;
        }
      }
      if (mappedInput.wasBottomRightUpGesture()) {
        if (!(reading && leavesBook(SETTINGS.gestureBottomRightUp)) &&
            runGesture(SETTINGS.gestureBottomRightUp)) {
          return;
        }
      }
      if (mappedInput.wasTopLeftToRightGesture() && runGesture(SETTINGS.gestureTopLeftToRight)) return;
      if (mappedInput.wasTopRightToLeftGesture() && runGesture(SETTINGS.gestureTopRightToLeft)) return;
    }

#if FREEINK_CAP_FRONTLIGHT

    // Left-edge brightness: long-press to arm (level unchanged), then relative drag.
    // Finger up from arm point → brighter; down → dimmer. No jump to absolute Y.
    // Home clock/cover and the reader page only — menus/lists use that edge for
    // row taps, and re-PWM on those holds flickered the dual-LED mix.
    static bool leftBriActive = false;
    static bool leftBriDragging = false;
    static int leftBriAnchorY = 0;
    static int leftBriAnchor = 40;
    static int leftBriLastApplied = -1;
    if (frontlight().present() && currentActivity->name != "FrontlightQuick" &&
        (currentActivity->isReaderActivity() || currentActivity->allowLeftEdgeFrontlight())) {
      int tx = 0, ty = 0;
      const int pageW = renderer.getScreenWidth();
      const int pageH = renderer.getScreenHeight();
      const int edgeW = leftEdgeFrontlightWidth(pageW);
      constexpr int kDragSlopPx = 12;

      if (!leftBriActive) {
        if (mappedInput.wasTouchLongPress(tx, ty) && tx < edgeW) {
          leftBriActive = true;
          leftBriDragging = false;
          leftBriAnchor = static_cast<int>(SETTINGS.frontlightBrightness);
          leftBriLastApplied = leftBriAnchor;
          int hx = tx, hy = ty;
          if (mappedInput.isScreenTouchHeld(hx, hy)) {
            leftBriAnchorY = hy;
          } else {
            leftBriAnchorY = ty;
          }
          return;
        }
      } else if (mappedInput.isScreenTouchHeld(tx, ty)) {
        if (!leftBriDragging && std::abs(ty - leftBriAnchorY) < kDragSlopPx) {
          return;  // armed — hold level until intentional drag
        }
        leftBriDragging = true;
        // Relative to arm: dy in pixels → percent. fullRangePx ≈ half-screen for
        // smooth control while still reaching 0 and 100 from mid-screen.
        // Same 0–100 scale + apply path as the top light sheet (+/− is 1%).
        const int fullRangePx = std::max(80, (pageH * 45) / 100);
        const int deltaPct = (leftBriAnchorY - ty) * 100 / fullRangePx;
        const int v = std::clamp(leftBriAnchor + deltaPct, 0, 100);
        // Only re-PWM when the integer % changes (cuts LED jitter).
        if (v != leftBriLastApplied) {
          leftBriLastApplied = v;
          setFrontlightBrightnessPercent(v, /*mirrorToActivePreset=*/true);
        }
        return;
      } else {
        if (leftBriDragging) {
          SETTINGS.saveToFile();
        }
        leftBriActive = false;
        leftBriDragging = false;
        leftBriLastApplied = -1;
        return;
      }
    } else if (leftBriActive || leftBriDragging) {
      leftBriActive = false;
      leftBriDragging = false;
      leftBriLastApplied = -1;
    }
#endif

    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    currentActivity->loop();
  }

  while (pendingAction != PendingAction::None) {
    // Always wait for the render task (including Home multipass after it unlocks
    // the RenderLock). Push used to skip this so Settings opened snappily during
    // greys — but multipass still touches heap/SD/BW chunks while Settings builds
    // its list → concurrence lock abort (see crash_report: leave-reader multipass
    // then Entering Settings). Multipass aborts quickly via hasPendingActivityChange,
    // so the wait is typically sub-second after the user navigates, not a full grey pass.
    waitForRenderIdle();

    if (pendingAction == PendingAction::Pop) {
      RenderLock lock;

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction = PendingAction::None;
        continue;
      }

      // Capture before destroy — light sheet soft-dismiss already restored glass.
      const bool lightSheetPop = (currentActivity->name == "FrontlightQuick");
      ActivityResult pendingResult = std::move(currentActivity->result);

      // Destroy the current activity (Frontlight onExit sets skip-parent flag).
      exitActivity(lock);
      pendingAction = PendingAction::None;
      const bool skipParentPaint = lightSheetPop && FrontlightQuickActivity::consumeSkipParentRepaint();

      if (stackActivities.empty()) {
        LOG_DBG("ACT", "No more activities on stack, going home");
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

      } else {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        LOG_DBG("ACT", "Popped from activity stack, new size = %zu", stackActivities.size());
        // Handle result if necessary
        if (currentActivity->resultHandler) {
          LOG_DBG("ACT", "Handling result for popped activity");

          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        } else {
          lock.unlock();
        }

        if (skipParentPaint) {
          // Light menu soft-dismiss: glass already shows parent; do not full-repaint Home.
          LOG_DBG("ACT", "Skip parent onResume/repaint after light sheet dismiss");
        } else {
          // Panel was owned by the child — parent gets a chance to refresh state.
          if (pendingAction == PendingAction::None && currentActivity) {
            currentActivity->onResume();
          }

          // Request an update to ensure the popped activity gets re-rendered
          if (pendingAction == PendingAction::None) {
            requestUpdate();
          }
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingAction == PendingAction::PopToHome) {
      // Phase 2: restore stacked Home instead of allocating a brand-new one.
      RenderLock lock;
      pendingAction = PendingAction::None;

      if (currentActivity) {
        exitActivity(lock);
      }
      // Drop non-home activities above Home (usually stack is just Home).
      while (!stackActivities.empty() && !stackActivities.back()->isHomeActivity()) {
        stackActivities.back()->onExit();
        stackActivities.pop_back();
      }
      if (!stackActivities.empty() && stackActivities.back()->isHomeActivity()) {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        LOG_DBG("ACT", "Restored Home from stack (phase-2 fast path)");
        lock.unlock();
        currentActivity->onResume();
        if (pendingAction == PendingAction::None) {
          requestUpdate();
        }
        continue;
      }
      // No Home on stack — fall through to a fresh replace.
      lock.unlock();
      replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, HomeMenuItem::NONE));
      continue;

    } else if (pendingActivity) {
      // Current activity has requested a new activity to be launched
      RenderLock lock;

      if (pendingAction == PendingAction::Replace) {
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
      } else if (pendingAction == PendingAction::Swap) {
        // Destroy current only — keep Home (or other parents) under the reader.
        exitActivity(lock);
        LOG_DBG("ACT", "Swapped activity, stack size = %zu", stackActivities.size());
      } else if (pendingAction == PendingAction::Push) {
        // Move current activity to stack
        stackActivities.push_back(std::move(currentActivity));
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
      }
      pendingAction = PendingAction::None;
      currentActivity = std::move(pendingActivity);

      lock.unlock();  // onEnter may acquire its own lock
      currentActivity->onEnter();

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    } else {
      // Unknown pending action with no activity — clear to avoid a tight loop.
      LOG_ERR("ACT", "Clearing stale pendingAction=%d", static_cast<int>(pendingAction));
      pendingAction = PendingAction::None;
    }
  }

  if (requestedUpdate.exchange(false)) {
    // One dirty flag → one wake. eSetValueWithOverwrite avoids stacking paint counts.
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eSetValueWithOverwrite);
    }
  }
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    currentActivity->onExit();
    currentActivity.reset();
  }
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    currentActivity = std::move(newActivity);
    currentActivity->onEnter();
  }
}

void ActivityManager::swapActivity(std::unique_ptr<Activity>&& newActivity) {
  // Same deferral rules as replace, but stack is preserved when the pending action runs.
  if (currentActivity) {
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Swap;
  } else {
    currentActivity = std::move(newActivity);
    currentActivity->onEnter();
  }
}

void ActivityManager::goToFileTransfer() {
  replaceActivity(std::make_unique<CasperWebServerActivity>(renderer, mappedInput));
}

void ActivityManager::goToSettings() {
  // Keep Home under Settings so theme changes resume Home with a real multipass
  // (replace used to tear Home down; a BW-only settle left Stats covers black).
  if (currentActivity && currentActivity->isHomeActivity()) {
    pushActivity(std::make_unique<SettingsActivity>(renderer, mappedInput));
  } else {
    replaceActivity(std::make_unique<SettingsActivity>(renderer, mappedInput));
  }
}

void ActivityManager::goToFileBrowser(std::string path) {
  // Keep Home stacked (like Settings / Reader) so Back can FAST-resume Penumbra
  // instead of allocating a new Home and paying HALF baseline (~1.8s on X4).
  // Also push when Home is already under a parent (e.g. re-open Library).
  const bool homeOnStack = (currentActivity && currentActivity->isHomeActivity()) ||
                           std::any_of(stackActivities.begin(), stackActivities.end(),
                                       [](const std::unique_ptr<Activity>& a) { return a && a->isHomeActivity(); });
  if (homeOnStack) {
    pushActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path)));
  } else {
    replaceActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path)));
  }
}

void ActivityManager::goToRecentBooks() {
  const bool homeOnStack = (currentActivity && currentActivity->isHomeActivity()) ||
                           std::any_of(stackActivities.begin(), stackActivities.end(),
                                       [](const std::unique_ptr<Activity>& a) { return a && a->isHomeActivity(); });
  if (homeOnStack) {
    pushActivity(std::make_unique<RecentBooksActivity>(renderer, mappedInput));
  } else {
    replaceActivity(std::make_unique<RecentBooksActivity>(renderer, mappedInput));
  }
}

void ActivityManager::goToBrowser() {
  const auto& servers = OPDS_STORE.getServers();
  // Skip the server picker when there's only one server configured
  if (servers.size() == 1) {
    replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, servers[0]));
  } else {
    replaceActivity(std::make_unique<OpdsServerListActivity>(renderer, mappedInput, true));
  }
}

void ActivityManager::goToReader(std::string path) {
  // Keep Home (and Library/Recents above it) on the stack so Back → goHome can
  // PopToHome with a snappy FAST resume. Replacing from FileBrowser used to clear
  // the stack, force a brand-new HomeActivity on Back, and cost ~3s HALF on X3
  // (felt like multi-press / frozen return from the book).
  //
  // QR / cold open: stack was empty (log: "Swapped activity, stack size = 0"), so
  // leaveReaderToHome built a *new* Home (~5s multipass / HALF on X3). Seed Home
  // under the reader without painting it first.
  //
  // Free Home cover snapshot RAM while reading — Home stays stacked for snappy
  // Back, but a full cover buffer does not need to compete with section build.
  auto releaseHomeCover = [](Activity* a) {
    if (a && a->isHomeActivity()) {
      static_cast<HomeActivity*>(a)->releaseHeavyResourcesForReader();
    }
  };
  releaseHomeCover(currentActivity.get());
  for (const auto& stacked : stackActivities) {
    releaseHomeCover(stacked.get());
  }

  const bool homeOnStack = (currentActivity && currentActivity->isHomeActivity()) ||
                           std::any_of(stackActivities.begin(), stackActivities.end(),
                                       [](const std::unique_ptr<Activity>& a) { return a && a->isHomeActivity(); });
  if (homeOnStack) {
    pushActivity(std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(path)));
    return;
  }

  // No Home yet: exit whatever is current (Boot, etc.), seed Home on the stack
  // without SD/recents work (QR critical path). Full Home load runs on first Back.
  if (currentActivity) {
    RenderLock lock;
    exitActivity(lock);
    while (!stackActivities.empty()) {
      stackActivities.back()->onExit();
      stackActivities.pop_back();
    }
  }
  auto home = std::make_unique<HomeActivity>(renderer, mappedInput, HomeMenuItem::NONE);
  home->seedUnderReader();  // no loadRecentBooks / GlobalStats / requestUpdate
  stackActivities.push_back(std::move(home));
  LOG_DBG("ACT", "Seeded Home under reader (QR/cold open snappy Back path)");
  currentActivity = std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(path));
  currentActivity->onEnter();
}

void ActivityManager::goToSleep(bool fromTimeout, bool useQuickResume) {
  // Flag while reader (etc.) onExit runs so leave chrome ("Saving...") is not painted
  // over the page before Quick Resume / wallpaper sleep art.
  sleepTransition_ = true;
  replaceActivity(std::make_unique<SleepActivity>(renderer, mappedInput, fromTimeout, useQuickResume));
  loop();  // Important: sleep screen must be rendered immediately, the caller will go to sleep right after this returns
  sleepTransition_ = false;
}

void ActivityManager::goToBoot() { replaceActivity(std::make_unique<BootActivity>(renderer, mappedInput)); }

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivity(std::make_unique<FullScreenMessageActivity>(renderer, mappedInput, std::move(message), style));
}

void ActivityManager::goHome(HomeMenuItem initialMenuItem) {
  // Phase 2: if Home is already under the stack (typical Read → Back), pop to it.
  // Skips allocating a new HomeActivity and re-running full onEnter book/thumb work.
  const bool homeOnStack = std::any_of(stackActivities.begin(), stackActivities.end(),
                                       [](const std::unique_ptr<Activity>& a) { return a && a->isHomeActivity(); });
  if (homeOnStack) {
    if (pendingActivity) {
      pendingActivity.reset();
    }
    pendingAction = PendingAction::PopToHome;
    return;
  }

  if (initialMenuItem == HomeMenuItem::NONE && currentActivity) {
    const auto& activityName = currentActivity->name;
    if (activityName == "FileBrowser") {
      initialMenuItem = HomeMenuItem::FILE_BROWSER;
    } else if (activityName == "RecentBooks") {
      initialMenuItem = HomeMenuItem::RECENTS;
    } else if (activityName == "OpdsBookBrowser") {
      initialMenuItem = HomeMenuItem::OPDS_BROWSER;
    } else if (activityName == "CasperWebServer") {
      initialMenuItem = HomeMenuItem::FILE_TRANSFER;
    }
    // Do not map Settings → SETTINGS_MENU: that selected the classic bottom-row
    // Settings item and felt like "Back opened the menu" after leaving Settings.
  }
  replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, initialMenuItem));
}
void ActivityManager::goToCrashReport() { replaceActivity(std::make_unique<CrashActivity>(renderer, mappedInput)); }

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
}

void ActivityManager::popActivity() {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  pendingAction = PendingAction::Pop;
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::isCurrentActivity(const Activity* activity) const {
  return activity != nullptr && currentActivity.get() == activity;
}

bool ActivityManager::hasPendingActivityChange() const { return pendingAction != PendingAction::None; }

bool ActivityManager::isReaderActivity() const {
  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity->isReaderActivity(); }) ||
         (currentActivity && currentActivity->isReaderActivity());
}

bool ActivityManager::isSettingsActivity() const {
  return (currentActivity && currentActivity->isSettingsActivity()) ||
         std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity && activity->isSettingsActivity(); });
}

bool ActivityManager::isReaderMenuActivity() const {
  return currentActivity && currentActivity->isReaderMenuActivity();
}

void ActivityManager::persistForSleep() {
  // Walk stack + current so a pushed menu still lets the reader under it save.
  for (const auto& stacked : stackActivities) {
    if (stacked) stacked->persistProgressForSleep();
  }
  if (currentActivity) currentActivity->persistProgressForSleep();
}

uint8_t ActivityManager::classifySleepResumeTarget() const {
  if (isReaderMenuActivity() && isReaderActivity()) {
    return CasperState::RESUME_READER_MENU;
  }
  if (isReaderActivity()) {
    return CasperState::RESUME_READER;
  }
  if (isSettingsActivity()) {
    return CasperState::RESUME_SETTINGS;
  }
  return CasperState::RESUME_HOME;
}

bool ActivityManager::handleForcedRefresh() { return currentActivity && currentActivity->handleForcedRefresh(); }

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

ScreenshotInfo ActivityManager::getScreenshotInfo() const {
  if (currentActivity) {
    return currentActivity->getScreenshotInfo();
  }
  return {};
}

void ActivityManager::requestUpdate(bool immediate) {
  // Coalesce: many requestUpdate() calls collapse to one paint.
  // Deferred path: set dirty; loop() wakes the render task once per main tick.
  // Immediate path: set dirty and wake now (overwrite, do not stack counts).
  requestedUpdate.store(true);
  if (immediate && renderTaskHandle) {
    xTaskNotify(renderTaskHandle, 1, eSetValueWithOverwrite);
  }
}
bool ActivityManager::requestUpdateAndWait(const uint32_t timeoutMs) {
  if (!renderTaskHandle) {
    return false;
  }

  // Atomic section to perform checks
  taskENTER_CRITICAL(&activityManagerSpinlock);
  auto currTaskHandler = xTaskGetCurrentTaskHandle();
  auto mutexHolder = xSemaphoreGetMutexHolder(renderingMutex);
  bool isRenderTask = (currTaskHandler == renderTaskHandle);
  bool alreadyWaiting = (waitingTaskHandle != nullptr);
  bool holdingRenderLock = (mutexHolder == currTaskHandler);
  if (!alreadyWaiting && !isRenderTask && !holdingRenderLock) {
    waitingTaskHandle = currTaskHandler;
  }
  taskEXIT_CRITICAL(&activityManagerSpinlock);

  // Render task cannot call requestUpdateAndWait() or it will cause a deadlock
  assert(!isRenderTask && "Render task cannot call requestUpdateAndWait()");

  // There should never be the case where 2 tasks are waiting for a render at the same time
  assert(!alreadyWaiting && "Already waiting for a render to complete");

  // Cannot call while holding RenderLock or it will cause a deadlock
  assert(!holdingRenderLock && "Cannot call requestUpdateAndWait() while holding RenderLock");

  requestedUpdate.store(true);
  xTaskNotify(renderTaskHandle, 1, eSetValueWithOverwrite);
  const TickType_t ticks = (timeoutMs == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs);
  const uint32_t got = ulTaskNotifyTake(pdTRUE, ticks);
  if (got == 0) {
    // Timed out: clear waiter so a late render does not notify the wrong task.
    taskENTER_CRITICAL(&activityManagerSpinlock);
    if (waitingTaskHandle == currTaskHandler) {
      waitingTaskHandle = nullptr;
    }
    taskEXIT_CRITICAL(&activityManagerSpinlock);
    LOG_ERR("ACT", "requestUpdateAndWait timeout %lums (render still running=%d)",
            static_cast<unsigned long>(timeoutMs), renderInProgress.load() ? 1 : 0);
    return false;
  }
  return true;
}

// RenderLock

RenderLock::RenderLock() {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::RenderLock([[maybe_unused]] Activity&) {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::~RenderLock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

void RenderLock::unlock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

/**
 *
 * Checks if renderingMutex is busy.
 *
 * @return true if renderingMutex is busy, otherwise false.
 *
 */
bool RenderLock::peek() { return xQueuePeek(activityManager.renderingMutex, NULL, 0) != pdTRUE; };
