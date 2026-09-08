#include "BookStatsActivity.h"

#include <I18n.h>

#include <algorithm>

#include "BookStatsView.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "util/UiGhostPolicy.h"

BookStatsActivity::BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                     const std::string& bookCachePath, const BookReadingStats& stats,
                                     const float progressPercent, const bool hasEstimatedTimeLeft,
                                     const uint32_t estimatedTimeLeftSeconds, const GlobalReadingStats& globalStats,
                                     const bool returnToHomeOnExit)
    : Activity("BookStats", renderer, mappedInput),
      bookTitle(title),
      bookCachePath(bookCachePath),
      stats(stats),
      globalStats(globalStats),
      returnToHomeOnExit(returnToHomeOnExit),
      progressPercent(progressPercent),
      hasEstimatedTimeLeft(hasEstimatedTimeLeft),
      estimatedTimeLeftSeconds(estimatedTimeLeftSeconds) {}

BookStatsActivity::BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                     const std::string& bookCachePath, const BookReadingStats& stats,
                                     const float progressPercent, const bool hasEstimatedTimeLeft,
                                     const uint32_t estimatedTimeLeftSeconds, const GlobalReadingStats& globalStats,
                                     const GlobalReadingStats& allDevicesStats, const bool returnToHomeOnExit)
    : Activity("BookStats", renderer, mappedInput),
      bookTitle(title),
      bookCachePath(bookCachePath),
      stats(stats),
      globalStats(globalStats),
      allDevicesStats(allDevicesStats),
      showAllDevicesStats(true),
      returnToHomeOnExit(returnToHomeOnExit),
      progressPercent(progressPercent),
      hasEstimatedTimeLeft(hasEstimatedTimeLeft),
      estimatedTimeLeftSeconds(estimatedTimeLeftSeconds) {}

void BookStatsActivity::refreshAllDevicesStats() {
  if (showAllDevicesStats) {
    allDevicesStats = GlobalReadingStats::loadAggregated(globalStats);
  }
}

void BookStatsActivity::saveStats() {
  if (!didChangeStats || !hasEditableBook()) {
    return;
  }

  stats.save(bookCachePath);
  globalStats.save();
  refreshAllDevicesStats();
  didChangeStats = false;
}

ReadingStatsDate BookStatsActivity::defaultDateForField(const bool finishedField) const {
  if (finishedField && stats.finishedDate.isValid()) {
    return stats.finishedDate;
  }
  if (!finishedField && stats.startDate.isValid()) {
    return stats.startDate;
  }
  if (finishedField && stats.startDate.isValid()) {
    return stats.startDate;
  }
  if (!finishedField && stats.finishedDate.isValid()) {
    return stats.finishedDate;
  }

  ReadingStatsDateTime now;
  if (getCurrentLocalReadingStatsDateTime(now)) {
    return now.date;
  }
  return {2000, 1, 1};
}

void BookStatsActivity::applyCompletedState(const bool completed) {
  if (stats.isCompleted == completed) {
    return;
  }

  stats.isCompleted = completed;
  if (completed) {
    globalStats.completedBooks++;
    if (!stats.finishedDateManual && !stats.finishedDate.isValid()) {
      ReadingStatsDateTime now;
      if (getCurrentLocalReadingStatsDateTime(now)) {
        stats.finishedDate = now.date;
      }
    }
  } else if (globalStats.completedBooks > 0) {
    globalStats.completedBooks--;
  }
}

void BookStatsActivity::normalizeEditedDates(const bool editedFinishedField) {
  if (!stats.startDate.isValid() || !stats.finishedDate.isValid()) {
    return;
  }
  if (compareReadingStatsDate(stats.finishedDate, stats.startDate) >= 0) {
    return;
  }

  if (editedFinishedField) {
    stats.startDate = stats.finishedDate;
  } else {
    stats.finishedDate = stats.startDate;
  }
}

void BookStatsActivity::clearEditedDate(const bool finishedField) {
  ReadingStatsDate& date = finishedField ? stats.finishedDate : stats.startDate;
  date.clear();

  if (finishedField) {
    stats.finishedDateManual = false;
    applyCompletedState(false);
  } else {
    stats.startDateManual = false;
  }

  didChangeStats = true;
  requestUpdate();
}

void BookStatsActivity::leaveEditDates() {
  saveStats();
  page = Page::PerBook;
  requestUpdate();
}

void BookStatsActivity::adjustSelectedDateField(const int delta) {
  const bool finishedField = selectedEditField >= 3;
  ReadingStatsDate& date = finishedField ? stats.finishedDate : stats.startDate;
  const int fieldIndex = selectedEditField % 3;

  if (!date.isValid()) {
    date = defaultDateForField(finishedField);
  }

  switch (fieldIndex) {
    case 0: {
      int month = static_cast<int>(date.month) + delta;
      while (month < 1) {
        month += 12;
      }
      while (month > 12) {
        month -= 12;
      }
      date.month = static_cast<uint8_t>(month);
      break;
    }
    case 1: {
      const int monthDays = daysInMonth(date.year, date.month);
      int day = static_cast<int>(date.day) + delta;
      while (day < 1) {
        day += monthDays;
      }
      while (day > monthDays) {
        day -= monthDays;
      }
      date.day = static_cast<uint8_t>(day);
      break;
    }
    case 2: {
      int year = static_cast<int>(date.year) + delta;
      if (year < 2000) {
        year = 2099;
      } else if (year > 2099) {
        year = 2000;
      }
      date.year = static_cast<uint16_t>(year);
      break;
    }
  }

  const uint8_t monthDays = daysInMonth(date.year, date.month);
  if (date.day > monthDays) {
    date.day = monthDays;
  }

  if (finishedField) {
    stats.finishedDateManual = true;
    applyCompletedState(true);
  } else {
    stats.startDateManual = true;
  }
  normalizeEditedDates(finishedField);

  didChangeStats = true;
  requestUpdate();
}

void BookStatsActivity::onEnter() {
  Activity::onEnter();
  // Stats pages are portrait layouts. From a landscape reader the chrome sat
  // on the side while the date fields stayed tiny — force a tall touch UI.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  requestUpdate();
}

void BookStatsActivity::onExit() {
  saveStats();
  Activity::onExit();
}

void BookStatsActivity::exitStatsActivity(const bool /*viaBack*/) {
  if (returnToHomeOnExit) {
    onGoHome();
    return;
  }

  finish();
}

void BookStatsActivity::loop() {
  // Soft footer slots (L→R). Per-book: Back | empty | Edit | More.
  // Equal quarters match Bare/Penumbra painted captions (not the 80px key cutouts).
  auto footerSlotAt = [this](const int tx, const int ty) -> int {
    const int pageW = renderer.getScreenWidth();
    const int pageH = renderer.getScreenHeight();
    const int stripH = std::max(40, pageH / 12);
    if (ty < pageH - stripH) return -1;
    return std::clamp(tx * 4 / std::max(1, pageW), 0, 3);
  };

  if (usesNoRtcSingleScreenLayout()) {
    int tx = 0, ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      const int slot = footerSlotAt(tx, ty);
      if (slot == 0 || slot == 1) {
        exitStatsActivity(true);
        return;
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      exitStatsActivity(true);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      exitStatsActivity(false);
      return;
    }
    return;
  }

  // Edit dates: hit-test the on-screen +/− steppers and Done first. The painted
  // footer Back/Next/Up/Down strip is not reliable on Pro (equal-column
  // captions vs 80px pills, landscape side chrome).
  if (page == Page::EditDates) {
    int tx = 0, ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      const DateEditHit hit = editBookDateHitAt(renderer, tx, ty);
      switch (hit.kind) {
        case DateEditHitKind::Inc:
          selectedEditField = hit.field;
          adjustSelectedDateField(1);
          return;
        case DateEditHitKind::Dec:
          selectedEditField = hit.field;
          adjustSelectedDateField(-1);
          return;
        case DateEditHitKind::Done:
          leaveEditDates();
          return;
        case DateEditHitKind::ClearStart:
          clearEditedDate(false);
          return;
        case DateEditHitKind::ClearFinished:
          clearEditedDate(true);
          return;
        case DateEditHitKind::None:
          break;
      }
      const int pageW = renderer.getScreenWidth();
      const int pageH = renderer.getScreenHeight();
      const int stripH = UITheme::getInstance().getMetrics().buttonHintsHeight;
      if (ty >= pageH - stripH) {
        const int slot = std::clamp(tx * 4 / std::max(1, pageW), 0, 3);
        if (slot == 0) {
          leaveEditDates();
        }
        return;
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      leaveEditDates();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      adjustSelectedDateField(-1);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
        mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      adjustSelectedDateField(1);
      return;
    }
    return;
  }

  const bool editShortcutPressed = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                                   mappedInput.wasPressed(MappedInputManager::Button::Left);
  const bool moreShortcutPressed = mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                                   mappedInput.wasPressed(MappedInputManager::Button::Right);

  // Touch: Edit (slot 2) / More (slot 3) on the painted hint strip.
  int tx = 0, ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const int slot = footerSlotAt(tx, ty);
    if (slot >= 0) {
      if (slot == 0) {
        if (page == Page::PerBook) {
          exitStatsActivity(true);
        } else if (page == Page::ThisDevice) {
          page = Page::PerBook;
          requestUpdate();
        } else if (page == Page::AllDevices) {
          page = Page::ThisDevice;
          requestUpdate();
        }
        return;
      }
      if (page == Page::PerBook && slot == 2 && hasEditableBook()) {
        page = Page::EditDates;
        requestUpdate();
        return;
      }
      if (page == Page::PerBook && slot == 3) {
        page = Page::ThisDevice;
        requestUpdate();
        return;
      }
      if (page == Page::ThisDevice && slot == 3 && showAllDevicesStats) {
        page = Page::AllDevices;
        requestUpdate();
        return;
      }
      if (page == Page::ThisDevice && slot == 1) {
        exitStatsActivity(false);  // Home label on lifetime page
        return;
      }
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (page == Page::PerBook) {
      exitStatsActivity(true);
    } else if (page == Page::ThisDevice) {
      page = Page::PerBook;
      requestUpdate();
    } else if (page == Page::AllDevices) {
      page = Page::ThisDevice;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    exitStatsActivity(false);
    return;
  }

  if (page == Page::PerBook) {
    if (hasEditableBook() && editShortcutPressed) {
      page = Page::EditDates;
      requestUpdate();
      return;
    }
    if (moreShortcutPressed) {
      page = Page::ThisDevice;
      requestUpdate();
      return;
    }
    return;
  }

  if (page == Page::ThisDevice && showAllDevicesStats && moreShortcutPressed) {
    page = Page::AllDevices;
    requestUpdate();
  }
}

void BookStatsActivity::render(RenderLock&&) {
  if (usesNoRtcSingleScreenLayout()) {
    renderNoRtcCombinedStatsPage(renderer, &mappedInput, bookTitle, stats, progressPercent, hasEstimatedTimeLeft,
                                 estimatedTimeLeftSeconds, globalStats,
                                 showAllDevicesStats ? &allDevicesStats : nullptr, true);
    UiGhostPolicy::displayMenuFrame(renderer);
    return;
  }

  switch (page) {
    case Page::PerBook:
      renderPerBookStatsPage(renderer, &mappedInput, bookTitle, stats, progressPercent, hasEstimatedTimeLeft,
                             estimatedTimeLeftSeconds, true, hasEditableBook(), true);
      break;
    case Page::ThisDevice:
      renderGlobalStatsPage(renderer, &mappedInput, tr(STR_STATS_THIS_DEVICE_SCREEN), globalStats, true,
                            showAllDevicesStats);
      break;
    case Page::AllDevices:
      renderGlobalStatsPage(renderer, &mappedInput, tr(STR_STATS_ALL_DEVICES_SCREEN), allDevicesStats, true, false);
      break;
    case Page::EditDates:
      renderEditBookDatesPage(renderer, &mappedInput, bookTitle, stats, selectedEditField, true);
      break;
  }
  UiGhostPolicy::displayMenuFrame(renderer);
}
