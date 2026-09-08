#include "BookStatsView.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <cstdio>

#include "MappedInputManager.h"
#include "ReadingStatsUtils.h"
#include "components/CompactHeader.h"
#include "components/UITheme.h"
#include "fontIds.h"

// CompactHeader lives under src/components; include path is already on the build.

namespace {
constexpr int kStatsButtonHintTopGap = 10;
constexpr int kStandaloneNoRtcMaxTopCardHeightDivisor = 2;
constexpr int kStandaloneNoRtcMaxVerticalOffset = 32;
constexpr int kPerBookRtcTopCardMaxExtra = 84;

struct StatsLayout {
  int headerHeight;
  int headerDrawHeight;
  int topGap;
  int cardGap;
  int topCardTitleH;
  int topCardH;
  int globalCardH;
  int sectionTitleH;
  int sectionTitleFontId;
  int chartLabelFontId;
  int chartLabelW;
  int barH;
  int barGap;
  int chartTopPadding;
  int chartBottomPadding;
};

constexpr StatsLayout kDefaultLayout = {
    .headerHeight = 78,
    .headerDrawHeight = 67,
    .topGap = 8,
    .cardGap = 26,
    .topCardTitleH = 36,
    .topCardH = 214,
    .globalCardH = 154,
    .sectionTitleH = 34,
    .sectionTitleFontId = UI_10_FONT_ID,
    .chartLabelFontId = UI_10_FONT_ID,
    .chartLabelW = 88,
    .barH = 22,
    .barGap = 12,
    .chartTopPadding = 14,
    .chartBottomPadding = 14,
};

constexpr StatsLayout kCompactLayout = {
    .headerHeight = 67,
    .headerDrawHeight = 67,
    .topGap = 6,
    .cardGap = 8,
    .topCardTitleH = 30,
    .topCardH = 156,
    .globalCardH = 110,
    .sectionTitleH = 30,
    .sectionTitleFontId = UI_10_FONT_ID,
    .chartLabelFontId = SMALL_FONT_ID,
    .chartLabelW = 78,
    .barH = 16,
    .barGap = 8,
    .chartTopPadding = 8,
    .chartBottomPadding = 8,
};

constexpr std::array<StrId, READING_TIME_BUCKET_COUNT> TIME_BUCKET_LABELS = {
    StrId::STR_STATS_MORNING, StrId::STR_STATS_AFTERNOON, StrId::STR_STATS_EVENING, StrId::STR_STATS_NIGHT};
constexpr std::array<StrId, READING_DAY_OF_WEEK_COUNT> DAY_LABELS = {
    StrId::STR_STATS_MON, StrId::STR_STATS_TUE, StrId::STR_STATS_WED, StrId::STR_STATS_THU,
    StrId::STR_STATS_FRI, StrId::STR_STATS_SAT, StrId::STR_STATS_SUN};

const char* dayCountText(const uint16_t days) { return days == 1 ? tr(STR_STATS_DAY) : tr(STR_STATS_DAYS); }

int sectionCardHeight(const StatsLayout& layout, const int rowCount) {
  if (rowCount <= 0) {
    return layout.sectionTitleH + layout.chartTopPadding + layout.chartBottomPadding;
  }
  const int rowStride = layout.barH + layout.barGap;
  return layout.sectionTitleH + layout.chartTopPadding + layout.chartBottomPadding + layout.barH +
         (rowCount - 1) * rowStride;
}

bool shouldShowRtcBasedStats() { return halClock.isAvailable(); }

int noRtcCardBaseHeight(const StatsLayout& layout) { return layout.globalCardH; }

int statsContentHeight(const StatsLayout& layout, const bool globalPage, const bool showRtcStats) {
  const int topCardH = globalPage ? layout.globalCardH : layout.topCardH;
  if (!showRtcStats) {
    return layout.headerHeight + layout.topGap + topCardH;
  }
  const int timeOfDayH = sectionCardHeight(layout, static_cast<int>(TIME_BUCKET_LABELS.size()));
  const int dayOfWeekH = sectionCardHeight(layout, static_cast<int>(DAY_LABELS.size()));
  return layout.headerHeight + layout.topGap + topCardH + layout.cardGap + timeOfDayH + layout.cardGap + dayOfWeekH;
}

int noRtcCombinedContentHeight(const StatsLayout& layout, const bool showAllDevicesStats) {
  const int cardBaseH = noRtcCardBaseHeight(layout);
  return layout.headerHeight + layout.topGap + cardBaseH + layout.cardGap + layout.globalCardH +
         (showAllDevicesStats ? layout.cardGap + layout.globalCardH : 0);
}

int statsBottomInset(const ThemeMetrics& metrics, const bool showButtonHints) {
  return metrics.verticalSpacing + (showButtonHints ? metrics.buttonHintsHeight + kStatsButtonHintTopGap : 0);
}

int perBookRtcTopCardHeight(const StatsLayout& layout, const int extraHeight) {
  return layout.topCardH + std::min(extraHeight, kPerBookRtcTopCardMaxExtra);
}

int globalRtcCardHeightForPerBookRowSpacing(const StatsLayout& layout, const int perBookExtraHeight) {
  constexpr int perBookDataRowCount = 3;
  constexpr int globalDataRowCount = 2;
  const int perBookDataRowH =
      (perBookRtcTopCardHeight(layout, perBookExtraHeight) - layout.topCardTitleH) / perBookDataRowCount;
  return std::max(layout.globalCardH, layout.topCardTitleH + perBookDataRowH * globalDataRowCount);
}

const StatsLayout& getStatsLayout(const GfxRenderer& renderer, const bool globalPage, const bool showButtonHints,
                                  const bool showRtcStats) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int availableHeight =
      renderer.getScreenHeight() - metrics.topPadding - statsBottomInset(metrics, showButtonHints);
  if (statsContentHeight(kDefaultLayout, globalPage, showRtcStats) <= availableHeight) {
    return kDefaultLayout;
  }
  return kCompactLayout;
}

const StatsLayout& getNoRtcCombinedLayout(const GfxRenderer& renderer, const bool showButtonHints,
                                          const bool showAllDevicesStats) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int availableHeight =
      renderer.getScreenHeight() - metrics.topPadding - statsBottomInset(metrics, showButtonHints);
  if (noRtcCombinedContentHeight(kDefaultLayout, showAllDevicesStats) <= availableHeight) {
    return kDefaultLayout;
  }
  return kCompactLayout;
}

void formatCompactEstimate(const uint32_t seconds, char* buf, const size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "<1m");
    return;
  }
  const uint32_t minutes = (seconds + 30u) / 60u;
  if (minutes < 60) {
    snprintf(buf, len, "%lum", static_cast<unsigned long>(minutes));
    return;
  }
  const uint32_t hours = minutes / 60u;
  const uint32_t remainder = minutes % 60u;
  if (remainder == 0) {
    snprintf(buf, len, "%luh", static_cast<unsigned long>(hours));
  } else {
    snprintf(buf, len, "%luh %lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(remainder));
  }
}

// Prefer live reader ETA, then cached page-based value, then weak progress fallback.
bool resolveEstimatedTimeLeft(const BookReadingStats& stats, const float progressPercent,
                              const bool hasEstimatedTimeLeft, const uint32_t estimatedTimeLeftSeconds,
                              uint32_t& seconds) {
  if (hasEstimatedTimeLeft && estimatedTimeLeftSeconds > 0) {
    seconds = estimatedTimeLeftSeconds;
    return true;
  }
  if (stats.estimatedTimeLeftSeconds > 0) {
    seconds = stats.estimatedTimeLeftSeconds;
    return true;
  }
  return estimateTimeLeftFromProgress(stats.totalReadingSeconds, progressPercent, seconds);
}

bool estimateFinishDateFromDailyPace(const BookReadingStats& stats, const ReadingStatsDateTime& today,
                                     const uint32_t estimatedReadingSeconds, ReadingStatsDate& outDate) {
  outDate = {};
  if (!today.isValid() || !stats.startDate.isValid() || estimatedReadingSeconds == 0 ||
      stats.totalReadingSeconds == 0) {
    return false;
  }

  const uint16_t elapsedDays = readingSpanDaysElapsed(stats.startDate, today.date);
  const uint16_t readingDays = std::max<uint16_t>(1, elapsedDays);

  // Convert remaining reading time into calendar time using the book's average reading seconds per calendar day.
  const uint64_t estimatedCalendarSeconds =
      (static_cast<uint64_t>(estimatedReadingSeconds) * static_cast<uint64_t>(readingDays) * 86400ULL +
       static_cast<uint64_t>(stats.totalReadingSeconds) / 2ULL) /
      static_cast<uint64_t>(stats.totalReadingSeconds);
  if (estimatedCalendarSeconds == 0) {
    return false;
  }

  ReadingStatsDateTime estimatedFinish = today;
  addSecondsToReadingStatsDateTime(estimatedFinish,
                                   static_cast<uint32_t>(std::min<uint64_t>(estimatedCalendarSeconds, UINT32_MAX)));
  outDate = estimatedFinish.date;
  return outDate.isValid();
}

float pagesPerMinute(const uint32_t totalPagesTurned, const uint32_t totalReadingSeconds) {
  if (totalReadingSeconds <= 60) {
    return 0.0f;
  }
  return static_cast<float>(totalPagesTurned) * 60.0f / static_cast<float>(totalReadingSeconds);
}

void drawCenteredLabel(const GfxRenderer& renderer, const int fontId, const int x, const int w, const int y,
                       const char* text, const bool bold = false) {
  const int textWidth = renderer.getTextWidth(fontId, text, bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  renderer.drawText(fontId, x + (w - textWidth) / 2, y, text, true,
                    bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
}

// Match dashboard book-cover chrome: rounded corners + slightly thicker stroke.
constexpr int kStatsCardCornerRadius = 10;
constexpr int kStatsCardStroke = 2;

void drawStatsCardChrome(const GfxRenderer& renderer, const int x, const int y, const int w, const int h,
                         const int titleH, const bool black = true) {
  if (w < 8 || h < 8) return;
  const int radius = std::min(kStatsCardCornerRadius, std::min(w, h) / 4);
  renderer.drawRoundedRect(x, y, w, h, kStatsCardStroke, radius, black);
  // Title divider inset so ends sit inside the rounded sides.
  const int inset = std::max(2, radius / 2);
  const int divY = y + titleH;
  if (divY > y && divY < y + h - 1) {
    for (int t = 0; t < kStatsCardStroke; ++t) {
      renderer.drawLine(x + inset, divY + t, x + w - 1 - inset, divY + t, black);
    }
  }
}

void drawStatCell(const GfxRenderer& renderer, const int x, const int w, const int y, const int h, const char* value,
                  const char* label) {
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelLineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int totalTextH = valueLineH + 4 + labelLineH;
  const int textY = y + (h - totalTextH) / 2;
  drawCenteredLabel(renderer, UI_12_FONT_ID, x, w, textY, value, true);
  drawCenteredLabel(renderer, SMALL_FONT_ID, x, w, textY + valueLineH + 4, label);
}

void drawSectionCard(const GfxRenderer& renderer, const int x, const int y, const int w, const int h, const char* title,
                     const StatsLayout& layout) {
  drawStatsCardChrome(renderer, x, y, w, h, layout.sectionTitleH);
  drawCenteredLabel(renderer, layout.sectionTitleFontId, x, w,
                    y + (layout.sectionTitleH - renderer.getLineHeight(layout.sectionTitleFontId)) / 2, title, true);
}

template <size_t N>
void drawHorizontalBars(GfxRenderer& renderer, const int x, const int y, const int w, const int h,
                        const std::array<uint32_t, N>& values, const std::array<StrId, N>& labels,
                        const StatsLayout& layout) {
  constexpr int labelLeftPadding = 10;
  constexpr int labelRightPadding = 18;
  constexpr int barLeftGap = 8;
  constexpr int rightPadding = 18;
  const uint32_t maxValue = *std::max_element(values.begin(), values.end());
  const int labelLineH = renderer.getLineHeight(layout.chartLabelFontId);
  const int rowContentH = std::max(labelLineH, layout.barH);
  const int n = static_cast<int>(N);
  // Fit all rows (incl. last label) inside the card so "Night"/"Sun" never clip
  // the bottom chrome — per-book page was overflowing on the first stats screen.
  const int bodyH = std::max(0, h - layout.sectionTitleH);
  const int usable = std::max(0, bodyH - layout.chartTopPadding - layout.chartBottomPadding);
  int rowGap = layout.barGap;
  if (n > 1) {
    const int minNeeded = n * rowContentH;
    if (minNeeded + (n - 1) * rowGap > usable) {
      rowGap = std::max(2, (usable - minNeeded) / (n - 1));
    }
  }
  const int usedRowsH = n * rowContentH + (n > 1 ? (n - 1) * rowGap : 0);
  const int leftover = std::max(0, usable - usedRowsH);
  const int topPadding = layout.chartTopPadding + leftover / 2;
  const int contentTop = y + layout.sectionTitleH + topPadding;
  const int rowStride = rowContentH + rowGap;
  int maxLabelW = 0;
  for (size_t i = 0; i < N; ++i) {
    maxLabelW = std::max(maxLabelW, renderer.getTextWidth(layout.chartLabelFontId, I18N.get(labels[i])));
  }
  const int labelColumnW = std::max(layout.chartLabelW, labelLeftPadding + maxLabelW + labelRightPadding);
  const int barX = x + labelColumnW + barLeftGap;
  const int barW = std::max(0, w - labelColumnW - barLeftGap - rightPadding);
  const int cardBottom = y + h - 2;
  for (size_t i = 0; i < N; ++i) {
    const int rowTop = contentTop + static_cast<int>(i) * rowStride;
    int labelY = rowTop + (rowContentH - labelLineH) / 2;
    if (labelY + labelLineH > cardBottom) {
      labelY = std::max(rowTop, cardBottom - labelLineH);
    }
    const int barY = rowTop + (rowContentH - layout.barH) / 2;
    renderer.drawText(layout.chartLabelFontId, x + labelLeftPadding, labelY, I18N.get(labels[i]));
    if (maxValue > 0 && values[i] > 0) {
      const int fillW = std::max(2, static_cast<int>((static_cast<uint64_t>(barW) * values[i]) / maxValue));
      renderer.fillRect(barX, barY, fillW, layout.barH, true);
    }
  }
}

void drawPerBookStatsCard(GfxRenderer& renderer, const int x, const int y, const int w, const int h,
                          const std::string& bookTitle, const BookReadingStats& stats, const float progressPercent,
                          const bool hasEstimatedTimeLeft, const uint32_t estimatedTimeLeftSeconds,
                          const StatsLayout& layout) {
  drawStatsCardChrome(renderer, x, y, w, h, layout.topCardTitleH);
  const std::string visibleTitle =
      renderer.truncatedText(UI_10_FONT_ID, bookTitle.c_str(), w - 20, EpdFontFamily::BOLD);
  drawCenteredLabel(renderer, UI_10_FONT_ID, x, w,
                    y + (layout.topCardTitleH - renderer.getLineHeight(UI_10_FONT_ID)) / 2, visibleTitle.c_str(), true);

  const bool showRtcStats = shouldShowRtcBasedStats();
  const int thirdW = w / 3;
  const int halfW = w / 2;
  const int rowCount = showRtcStats ? 3 : 2;
  const int rowH = (h - layout.topCardTitleH) / rowCount;
  char buf[40];

  snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(stats.sessionCount));
  drawStatCell(renderer, x, thirdW, y + layout.topCardTitleH, rowH, buf, tr(STR_STATS_SESSIONS_LBL));

  BookReadingStats::formatDuration(stats.totalReadingSeconds, buf, sizeof(buf));
  drawStatCell(renderer, x + thirdW, thirdW, y + layout.topCardTitleH, rowH, buf, tr(STR_STATS_TIME_LBL));

  if (progressPercent >= 0.0f) {
    snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(progressPercent + 0.5f));
  } else {
    snprintf(buf, sizeof(buf), "-");
  }
  drawStatCell(renderer, x + thirdW * 2, thirdW, y + layout.topCardTitleH, rowH, buf, tr(STR_STATS_PROGRESS_LBL));

  const uint32_t avgSecs = stats.sessionCount > 0 ? stats.totalReadingSeconds / stats.sessionCount : 0;
  BookReadingStats::formatDuration(avgSecs, buf, sizeof(buf));
  drawStatCell(renderer, x, thirdW, y + layout.topCardTitleH + rowH, rowH, buf, tr(STR_STATS_AVG_SESSION_LBL));

  uint32_t resolvedEstimateSeconds = 0;
  const bool hasResolvedEstimate =
      !stats.isCompleted && resolveEstimatedTimeLeft(stats, progressPercent, hasEstimatedTimeLeft,
                                                     estimatedTimeLeftSeconds, resolvedEstimateSeconds);
  if (hasResolvedEstimate) {
    formatCompactEstimate(resolvedEstimateSeconds, buf, sizeof(buf));
  } else {
    snprintf(buf, sizeof(buf), "-");
  }
  drawStatCell(renderer, x + thirdW, thirdW, y + layout.topCardTitleH + rowH, rowH, buf, tr(STR_TIME_LEFT));

  snprintf(buf, sizeof(buf), "%.1f", pagesPerMinute(stats.totalPagesTurned, stats.totalReadingSeconds));
  drawStatCell(renderer, x + thirdW * 2, thirdW, y + layout.topCardTitleH + rowH, rowH, buf,
               tr(STR_STATS_PAGES_PER_MIN));

  if (!showRtcStats) {
    return;
  }

  ReadingStatsDateTime today;
  const bool hasToday = getCurrentLocalReadingStatsDateTime(today);
  const ReadingStatsDate endDate = stats.isCompleted && stats.finishedDate.isValid()
                                       ? stats.finishedDate
                                       : (hasToday ? today.date : ReadingStatsDate{});
  const bool hasDaySpan = stats.startDate.isValid() && endDate.isValid();
  const uint16_t daysReading = hasDaySpan ? readingSpanDaysElapsed(stats.startDate, endDate) : 0;
  if (hasDaySpan) {
    snprintf(buf, sizeof(buf), "%u %s", static_cast<unsigned>(daysReading), dayCountText(daysReading));
  } else {
    snprintf(buf, sizeof(buf), "-");
  }
  char startedLabel[32];
  char dateBuf[24];
  formatReadingStatsShortDate(stats.startDate, dateBuf, sizeof(dateBuf));
  snprintf(startedLabel, sizeof(startedLabel), "%s %s", tr(STR_STATS_STARTED), dateBuf);
  drawStatCell(renderer, x, halfW, y + layout.topCardTitleH + rowH * 2, rowH, buf, startedLabel);

  ReadingStatsDate finishDisplayDate;
  bool finished = stats.isCompleted;
  if (finished) {
    finishDisplayDate = stats.finishedDate;
  } else if (hasToday && hasResolvedEstimate) {
    if (!estimateFinishDateFromDailyPace(stats, today, resolvedEstimateSeconds, finishDisplayDate)) {
      ReadingStatsDateTime estimatedFinish = today;
      addSecondsToReadingStatsDateTime(estimatedFinish, resolvedEstimateSeconds);
      finishDisplayDate = estimatedFinish.date;
    }
  }
  formatReadingStatsShortDate(finishDisplayDate, buf, sizeof(buf));
  drawStatCell(renderer, x + halfW, halfW, y + layout.topCardTitleH + rowH * 2, rowH, buf,
               finished ? tr(STR_STATS_FINISHED_DATE) : tr(STR_STATS_EST_FINISH_DATE));
}

void drawGlobalStatsCard(GfxRenderer& renderer, const int x, const int y, const int w, const int h, const char* title,
                         const GlobalReadingStats& stats, const StatsLayout& layout) {
  // Match Dashboard lifetime 2x3 (no Pages Turned / Days Read):
  //   Sessions | Reading Time | Pages/Min
  //   Avg. Session | Books Read | Streak
  drawStatsCardChrome(renderer, x, y, w, h, layout.topCardTitleH);
  drawCenteredLabel(renderer, UI_10_FONT_ID, x, w,
                    y + (layout.topCardTitleH - renderer.getLineHeight(UI_10_FONT_ID)) / 2, title, true);

  constexpr int kColCount = 3;
  constexpr int kBodyInsetX = 2;
  const int bodyY = y + layout.topCardTitleH;
  const int bodyH = h - layout.topCardTitleH;
  const int gridW = std::max(kColCount, w - kBodyInsetX * 2);
  const int baseColW = gridW / kColCount;
  const int colRem = gridW % kColCount;
  int colW[kColCount];
  int colX[kColCount];
  int cx = x + kBodyInsetX;
  for (int i = 0; i < kColCount; ++i) {
    colW[i] = baseColW + (i < colRem ? 1 : 0);
    colX[i] = cx;
    cx += colW[i];
  }
  const int baseRowH = bodyH / 2;
  const int rowRem = bodyH % 2;
  const int rowH0 = baseRowH + (rowRem > 0 ? 1 : 0);
  const int rowH1 = baseRowH + (rowRem > 1 ? 1 : 0);
  const int rowY0 = bodyY;
  const int rowY1 = bodyY + rowH0;

  char buf[40];
  auto cell = [&](const int col, const int rowY, const int rowH, const char* value, const char* label) {
    drawStatCell(renderer, colX[col], colW[col], rowY, rowH, value, label);
  };

  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(stats.totalSessions));
  cell(0, rowY0, rowH0, buf, tr(STR_STATS_SESSIONS_LBL));

  BookReadingStats::formatDuration(stats.totalReadingSeconds, buf, sizeof(buf));
  cell(1, rowY0, rowH0, buf, tr(STR_STATS_TIME_LBL));

  snprintf(buf, sizeof(buf), "%.1f", pagesPerMinute(stats.totalPagesTurned, stats.totalReadingSeconds));
  cell(2, rowY0, rowH0, buf, tr(STR_STATS_PAGES_PER_MIN));

  const uint32_t avgSecs = stats.totalSessions > 0 ? stats.totalReadingSeconds / stats.totalSessions : 0;
  BookReadingStats::formatDuration(avgSecs, buf, sizeof(buf));
  cell(0, rowY1, rowH1, buf, tr(STR_STATS_AVG_SESSION_LBL));

  if (stats.completedBooks > 0) {
    snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(stats.completedBooks));
  } else {
    snprintf(buf, sizeof(buf), "-");
  }
  cell(1, rowY1, rowH1, buf, tr(STR_STATS_COMPLETED_LBL));

  const uint16_t longest = stats.displayLongestReadingStreak();
  if (longest > 0) {
    snprintf(buf, sizeof(buf), "%u %s", static_cast<unsigned>(longest), dayCountText(longest));
  } else {
    snprintf(buf, sizeof(buf), "-");
  }
  cell(2, rowY1, rowH1, buf, tr(STR_STATS_LONGEST_STREAK_LBL));
}

constexpr int kDateHitPad = 12;
constexpr int kDateBtnRadius = 8;

struct DateStepperCol {
  Rect up;
  Rect value;
  Rect down;
};

struct DateEditLayout {
  DateStepperCol start[3];
  DateStepperCol finished[3];
  Rect done;
  Rect clearStart;
  Rect clearFinished;
};

Rect gPerBookEditHit{};

bool containsPadded(const Rect& r, const int x, const int y, const int pad = kDateHitPad) {
  return r.width > 0 && r.height > 0 && x >= r.x - pad && x < r.x + r.width + pad && y >= r.y - pad &&
         y < r.y + r.height + pad;
}

void placeStepperRow(DateStepperCol cols[3], const int x0, const int y0, const int colW, const int colGap,
                     const int stepH, const int valueH) {
  for (int i = 0; i < 3; ++i) {
    const int x = x0 + i * (colW + colGap);
    cols[i].up = Rect{x, y0, colW, stepH};
    cols[i].value = Rect{x, y0 + stepH, colW, valueH};
    cols[i].down = Rect{x, y0 + stepH + valueH, colW, stepH};
  }
}

DateEditLayout makeDateEditLayout(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, /*hasFrontButtonHints=*/true, false);
  const int pad = metrics.contentSidePadding;
  const int x0 = safe.x + pad;
  const int usableW = std::max(1, safe.width - pad * 2);
  const int bottom = safe.y + safe.height;
  const int doneH = 48;
  const int doneGap = 8;
  DateEditLayout layout{};
  layout.done = Rect{x0, bottom - doneH, usableW, doneH};

  const int titleY = CompactHeader::contentTop(metrics);
  const int titleH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelH = renderer.getLineHeight(UI_10_FONT_ID);
  const int labelBand = std::max(36, labelH + 8);
  const int groupGap = 16;
  const int groupTop = titleY + titleH + 8;
  const int avail = std::max(1, layout.done.y - groupTop - doneGap);
  const int colGap = 8;
  const bool sideBySide = safe.width >= 640;

  int stepH = sideBySide ? 48 : 56;
  int valueH = sideBySide ? 44 : 52;
  auto groupBodyH = [&]() { return labelBand + stepH + valueH + stepH; };

  int groupW = usableW;
  if (sideBySide) {
    groupW = (usableW - groupGap) / 2;
    while (groupBodyH() > avail && stepH > 36) {
      --stepH;
    }
    while (groupBodyH() > avail && valueH > 32) {
      --valueH;
    }
  } else {
    while (groupBodyH() * 2 + groupGap > avail && stepH > 40) {
      --stepH;
    }
    while (groupBodyH() * 2 + groupGap > avail && valueH > 36) {
      --valueH;
    }
  }

  int colW = (groupW - colGap * 2) / 3;
  if (colW < 56) colW = std::max(48, groupW / 3);
  const int clearW = std::min(groupW / 3, std::max(56, renderer.getTextWidth(UI_10_FONT_ID, tr(STR_CLEAR_BUTTON)) + 20));

  auto placeGroup = [&](DateStepperCol cols[3], Rect& clearRect, const int x, const int y) {
    placeStepperRow(cols, x, y + labelBand, colW, colGap, stepH, valueH);
    clearRect = Rect{x + groupW - clearW, y, clearW, labelBand};
  };

  if (sideBySide) {
    placeGroup(layout.start, layout.clearStart, x0, groupTop);
    placeGroup(layout.finished, layout.clearFinished, x0 + groupW + groupGap, groupTop);
  } else {
    placeGroup(layout.start, layout.clearStart, x0, groupTop);
    placeGroup(layout.finished, layout.clearFinished, x0, groupTop + groupBodyH() + groupGap);
  }
  return layout;
}

void drawTouchBtn(const GfxRenderer& renderer, const Rect& r, const char* text, const int fontId, const bool selected) {
  if (r.width <= 0 || r.height <= 0 || text == nullptr) return;
  renderer.fillRoundedRect(r.x, r.y, r.width, r.height, kDateBtnRadius, selected ? Color::LightGray : Color::White);
  renderer.drawRoundedRect(r.x, r.y, r.width, r.height, selected ? 2 : 1, kDateBtnRadius, true);
  const int tw = renderer.getTextWidth(fontId, text, EpdFontFamily::BOLD);
  const int th = renderer.getLineHeight(fontId);
  renderer.drawText(fontId, r.x + (r.width - tw) / 2, r.y + (r.height - th) / 2, text, true, EpdFontFamily::BOLD);
}

void formatDateColumnTokens(const ReadingStatsDate& date, char monthBuf[8], char dayBuf[8], char yearBuf[8]) {
  formatReadingStatsMonthToken(date, monthBuf, 8);
  if (date.isValid()) {
    snprintf(dayBuf, 8, "%02u", static_cast<unsigned>(date.day));
    snprintf(yearBuf, 8, "%u", static_cast<unsigned>(date.year));
  } else {
    snprintf(dayBuf, 8, "-");
    snprintf(yearBuf, 8, "-");
  }
}

void drawDateStepperGroup(const GfxRenderer& renderer, const DateStepperCol cols[3], const Rect& clearRect,
                           const char* title, const ReadingStatsDate& date, const int selectedField,
                           const int fieldBase) {
  const int labelH = renderer.getLineHeight(UI_10_FONT_ID);
  const int titleMaxW = std::max(1, clearRect.x - cols[0].up.x - 8);
  const std::string titleText = renderer.truncatedText(UI_10_FONT_ID, title, titleMaxW, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, cols[0].up.x, clearRect.y + (clearRect.height - labelH) / 2, titleText.c_str(), true,
                    EpdFontFamily::BOLD);
  drawTouchBtn(renderer, clearRect, tr(STR_CLEAR_BUTTON), UI_10_FONT_ID, false);

  char monthBuf[8];
  char dayBuf[8];
  char yearBuf[8];
  formatDateColumnTokens(date, monthBuf, dayBuf, yearBuf);
  const char* tokens[3] = {monthBuf, dayBuf, yearBuf};
  for (int i = 0; i < 3; ++i) {
    drawTouchBtn(renderer, cols[i].up, "+", UI_12_FONT_ID, false);
    drawTouchBtn(renderer, cols[i].value, tokens[i], UI_12_FONT_ID, selectedField == fieldBase + i);
    drawTouchBtn(renderer, cols[i].down, "-", UI_12_FONT_ID, false);
  }
}
}  // namespace

DateEditHit editBookDateHitAt(const GfxRenderer& renderer, const int tx, const int ty) {
  const DateEditLayout layout = makeDateEditLayout(renderer);
  if (containsPadded(layout.done, tx, ty)) return {DateEditHitKind::Done, 0};
  if (containsPadded(layout.clearStart, tx, ty)) return {DateEditHitKind::ClearStart, 0};
  if (containsPadded(layout.clearFinished, tx, ty)) return {DateEditHitKind::ClearFinished, 0};
  for (int i = 0; i < 3; ++i) {
    if (containsPadded(layout.start[i].up, tx, ty) || containsPadded(layout.start[i].value, tx, ty)) {
      return {DateEditHitKind::Inc, static_cast<uint8_t>(i)};
    }
    if (containsPadded(layout.start[i].down, tx, ty)) {
      return {DateEditHitKind::Dec, static_cast<uint8_t>(i)};
    }
    if (containsPadded(layout.finished[i].up, tx, ty) || containsPadded(layout.finished[i].value, tx, ty)) {
      return {DateEditHitKind::Inc, static_cast<uint8_t>(3 + i)};
    }
    if (containsPadded(layout.finished[i].down, tx, ty)) {
      return {DateEditHitKind::Dec, static_cast<uint8_t>(3 + i)};
    }
  }
  return {};
}

bool perBookEditDatesHitAt(const int tx, const int ty) { return containsPadded(gPerBookEditHit, tx, ty); }

static void drawStatsFrontChrome(GfxRenderer& renderer, const MappedInputManager* mappedInput, const char* back,
                                  const char* confirm, const char* left, const char* right) {
  if (mappedInput == nullptr) return;
  if (mappedInput->needsOnScreenFrontChrome()) {
    GUI.drawButtonHints(renderer, back, confirm, left, right);
    return;
  }
  const auto labels = mappedInput->mapLabels(back, confirm, left, right);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void renderPerBookStatsPage(GfxRenderer& renderer, const MappedInputManager* mappedInput, const std::string& bookTitle,
                            const BookReadingStats& stats, const float progressPercent, const bool hasEstimatedTimeLeft,
                            const uint32_t estimatedTimeLeftSeconds, const bool showButtonHints,
                            const bool showEditButton, const bool showMoreButton) {
  gPerBookEditHit = Rect(0, 0, 0, 0);
  renderer.clearScreen();
  const bool showRtcStats = shouldShowRtcBasedStats();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& layout = getStatsLayout(renderer, false, showButtonHints, showRtcStats);
  CompactHeader::drawTitle(renderer, tr(STR_READING_STATS), true);
  const int screenW = renderer.getScreenWidth();
  const int cardX = metrics.contentSidePadding;
  const int cardW = screenW - metrics.contentSidePadding * 2;
  const int availableHeight =
      renderer.getScreenHeight() - metrics.topPadding - statsBottomInset(metrics, showButtonHints);
  int topCardH = layout.topCardH;
  int y = metrics.topPadding + std::min(metrics.headerHeight, layout.headerHeight) + layout.topGap;

  if (showRtcStats) {
    const int timeOfDayH = sectionCardHeight(layout, static_cast<int>(TIME_BUCKET_LABELS.size()));
    const int dayOfWeekH = sectionCardHeight(layout, static_cast<int>(DAY_LABELS.size()));
    const int compactContentHeight = std::min(metrics.headerHeight, layout.headerHeight) + layout.topGap +
                                     layout.topCardH + layout.cardGap + timeOfDayH + layout.cardGap + dayOfWeekH;
    const int extraHeight = std::max(0, availableHeight - compactContentHeight);
    const int extraTopCardHeight = std::min(extraHeight, kPerBookRtcTopCardMaxExtra);
    const int remainingExtraHeight = extraHeight - extraTopCardHeight;
    const int timeOfDayExtraHeight = (remainingExtraHeight * 4) / 11;
    const int dayOfWeekExtraHeight = remainingExtraHeight - timeOfDayExtraHeight;
    const int timeOfDayCardH = timeOfDayH + timeOfDayExtraHeight;
    const int dayOfWeekCardH = dayOfWeekH + dayOfWeekExtraHeight;
    topCardH += extraTopCardHeight;

    drawPerBookStatsCard(renderer, cardX, y, cardW, topCardH, bookTitle, stats, progressPercent, hasEstimatedTimeLeft,
                         estimatedTimeLeftSeconds, layout);
    if (showEditButton) {
      gPerBookEditHit = Rect{cardX, y, cardW, topCardH};
    }
    y += topCardH + layout.cardGap;

    drawSectionCard(renderer, cardX, y, cardW, timeOfDayCardH, tr(STR_STATS_TIME_OF_DAY), layout);
    drawHorizontalBars(renderer, cardX, y, cardW, timeOfDayCardH, stats.timeOfDaySeconds, TIME_BUCKET_LABELS, layout);
    y += timeOfDayCardH + layout.cardGap;

    drawSectionCard(renderer, cardX, y, cardW, dayOfWeekCardH, tr(STR_STATS_DAY_OF_WEEK), layout);
    drawHorizontalBars(renderer, cardX, y, cardW, dayOfWeekCardH, stats.dayOfWeekSeconds, DAY_LABELS, layout);
  } else {
    const int compactContentHeight =
        std::min(metrics.headerHeight, layout.headerHeight) + layout.topGap + layout.topCardH;
    const int extraHeight = std::max(0, availableHeight - compactContentHeight);
    if (showButtonHints) {
      topCardH += extraHeight;
    } else {
      // The sleep-screen variant has no footer controls, so on tall portrait displays the
      // single card can balloon and create huge internal gaps between the two stat rows.
      // Cap the card growth and spend the rest as outer margin instead.
      const int maxStandaloneCardHeight =
          std::max(layout.topCardH, renderer.getScreenHeight() / kStandaloneNoRtcMaxTopCardHeightDivisor);
      topCardH = std::min(layout.topCardH + extraHeight, maxStandaloneCardHeight);
      const int unusedExtraHeight = extraHeight - (topCardH - layout.topCardH);
      y += std::min(unusedExtraHeight / 3, kStandaloneNoRtcMaxVerticalOffset);
    }
    drawPerBookStatsCard(renderer, cardX, y, cardW, topCardH, bookTitle, stats, progressPercent, hasEstimatedTimeLeft,
                         estimatedTimeLeftSeconds, layout);
    if (showEditButton) {
      gPerBookEditHit = Rect{cardX, y, cardW, topCardH};
    }
  }

  if (showButtonHints && mappedInput) {
    drawStatsFrontChrome(renderer, mappedInput, tr(STR_BACK), "", showEditButton ? tr(STR_EDIT) : "",
                         showMoreButton ? tr(STR_MORE) : "");
  }
}

void renderGlobalStatsPage(GfxRenderer& renderer, const MappedInputManager* mappedInput, const char* screenTitle,
                           const GlobalReadingStats& stats, const bool showButtonHints, const bool showMoreButton) {
  renderer.clearScreen();
  const bool showRtcStats = shouldShowRtcBasedStats();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& layout = getStatsLayout(renderer, true, showButtonHints, showRtcStats);
  CompactHeader::drawTitle(renderer, screenTitle);
  const int screenW = renderer.getScreenWidth();
  const int cardX = metrics.contentSidePadding;
  const int cardW = screenW - metrics.contentSidePadding * 2;
  const int availableHeight =
      renderer.getScreenHeight() - metrics.topPadding - statsBottomInset(metrics, showButtonHints);
  int globalCardH = layout.globalCardH;
  int y = metrics.topPadding + std::min(metrics.headerHeight, layout.headerHeight) + layout.topGap;

  if (showRtcStats) {
    const int timeOfDayH = sectionCardHeight(layout, static_cast<int>(TIME_BUCKET_LABELS.size()));
    const int dayOfWeekH = sectionCardHeight(layout, static_cast<int>(DAY_LABELS.size()));
    const int compactContentHeight = std::min(metrics.headerHeight, layout.headerHeight) + layout.topGap +
                                     layout.globalCardH + layout.cardGap + timeOfDayH + layout.cardGap + dayOfWeekH;
    const int extraHeight = std::max(0, availableHeight - compactContentHeight);
    const int perBookCompactContentHeight = std::min(metrics.headerHeight, layout.headerHeight) + layout.topGap +
                                            layout.topCardH + layout.cardGap + timeOfDayH + layout.cardGap + dayOfWeekH;
    const int perBookExtraHeight = std::max(0, availableHeight - perBookCompactContentHeight);
    const int targetGlobalCardH = globalRtcCardHeightForPerBookRowSpacing(layout, perBookExtraHeight);
    const int extraTopCardHeight = std::min(extraHeight, std::max(0, targetGlobalCardH - layout.globalCardH));
    const int remainingExtraHeight = extraHeight - extraTopCardHeight;
    const int timeOfDayExtraHeight = (remainingExtraHeight * 4) / 11;
    const int dayOfWeekExtraHeight = remainingExtraHeight - timeOfDayExtraHeight;
    const int timeOfDayCardH = timeOfDayH + timeOfDayExtraHeight;
    const int dayOfWeekCardH = dayOfWeekH + dayOfWeekExtraHeight;
    globalCardH += extraTopCardHeight;

    drawGlobalStatsCard(renderer, cardX, y, cardW, globalCardH, tr(STR_STATS_ALL_TIME), stats, layout);
    y += globalCardH + layout.cardGap;

    drawSectionCard(renderer, cardX, y, cardW, timeOfDayCardH, tr(STR_STATS_TIME_OF_DAY), layout);
    drawHorizontalBars(renderer, cardX, y, cardW, timeOfDayCardH, stats.timeOfDaySeconds, TIME_BUCKET_LABELS, layout);
    y += timeOfDayCardH + layout.cardGap;

    drawSectionCard(renderer, cardX, y, cardW, dayOfWeekCardH, tr(STR_STATS_DAY_OF_WEEK), layout);
    drawHorizontalBars(renderer, cardX, y, cardW, dayOfWeekCardH, stats.dayOfWeekSeconds, DAY_LABELS, layout);
  } else {
    const int compactContentHeight =
        std::min(metrics.headerHeight, layout.headerHeight) + layout.topGap + layout.globalCardH;
    globalCardH += std::max(0, availableHeight - compactContentHeight);
    drawGlobalStatsCard(renderer, cardX, y, cardW, globalCardH, tr(STR_STATS_ALL_TIME), stats, layout);
  }

  if (showButtonHints && mappedInput) {
    drawStatsFrontChrome(renderer, mappedInput, tr(STR_BACK), tr(STR_HOME), "", showMoreButton ? tr(STR_MORE) : "");
  }
}

void renderNoRtcCombinedStatsPage(GfxRenderer& renderer, const MappedInputManager* mappedInput,
                                  const std::string& bookTitle, const BookReadingStats& bookStats,
                                  const float progressPercent, const bool hasEstimatedTimeLeft,
                                  const uint32_t estimatedTimeLeftSeconds, const GlobalReadingStats& deviceStats,
                                  const GlobalReadingStats* allDevicesStats, const bool showButtonHints) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& layout = getNoRtcCombinedLayout(renderer, showButtonHints, allDevicesStats != nullptr);
  CompactHeader::drawTitle(renderer, tr(STR_READING_STATS));
  const int screenW = renderer.getScreenWidth();
  const int cardX = metrics.contentSidePadding;
  const int cardW = screenW - metrics.contentSidePadding * 2;
  const int availableHeight =
      renderer.getScreenHeight() - metrics.topPadding - statsBottomInset(metrics, showButtonHints);
  const int compactContentHeight = noRtcCombinedContentHeight(layout, allDevicesStats != nullptr);
  const int extraHeight = std::max(0, availableHeight - compactContentHeight);
  const int visibleCardCount = allDevicesStats ? 3 : 2;
  const int extraPerCard = visibleCardCount > 0 ? extraHeight / visibleCardCount : 0;
  const int extraRemainder = visibleCardCount > 0 ? extraHeight % visibleCardCount : 0;
  const int perBookExtraHeight = extraPerCard + (extraRemainder > 0 ? 1 : 0);
  const int deviceExtraHeight = extraPerCard + (extraRemainder > 1 ? 1 : 0);
  const int allDevicesExtraHeight = allDevicesStats ? extraPerCard : 0;
  const int perBookCardH = noRtcCardBaseHeight(layout) + perBookExtraHeight;
  const int deviceCardH = layout.globalCardH + deviceExtraHeight;
  const int allDevicesCardH = layout.globalCardH + allDevicesExtraHeight;

  int y = metrics.topPadding + std::min(metrics.headerHeight, layout.headerHeight) + layout.topGap;
  drawPerBookStatsCard(renderer, cardX, y, cardW, perBookCardH, bookTitle, bookStats, progressPercent,
                       hasEstimatedTimeLeft, estimatedTimeLeftSeconds, layout);
  y += perBookCardH + layout.cardGap;

  drawGlobalStatsCard(renderer, cardX, y, cardW, deviceCardH, tr(STR_STATS_THIS_DEVICE_SCREEN), deviceStats, layout);
  y += deviceCardH;

  if (allDevicesStats) {
    y += layout.cardGap;
    drawGlobalStatsCard(renderer, cardX, y, cardW, allDevicesCardH, tr(STR_STATS_ALL_DEVICES_SCREEN), *allDevicesStats,
                        layout);
  }

  if (showButtonHints && mappedInput) {
    drawStatsFrontChrome(renderer, mappedInput, tr(STR_BACK), "", "", "");
  }
}

void renderEditBookDatesPage(GfxRenderer& renderer, const MappedInputManager* mappedInput, const std::string& bookTitle,
                             const BookReadingStats& stats, const int selectedField, const bool showButtonHints) {
  renderer.clearScreen();
  CompactHeader::drawTitle(renderer, tr(STR_READING_STATS));

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int titleY = CompactHeader::contentTop(metrics);
  const std::string visibleTitle =
      renderer.truncatedText(UI_12_FONT_ID, bookTitle.c_str(), pageWidth - metrics.contentSidePadding * 2,
                            EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, titleY, visibleTitle.c_str(), true, EpdFontFamily::BOLD);

  const DateEditLayout layout = makeDateEditLayout(renderer);
  drawDateStepperGroup(renderer, layout.start, layout.clearStart, tr(STR_STATS_START_DATE), stats.startDate,
                       selectedField, 0);
  drawDateStepperGroup(renderer, layout.finished, layout.clearFinished, tr(STR_STATS_FINISHED_DATE), stats.finishedDate,
                       selectedField, 3);
  drawTouchBtn(renderer, layout.done, tr(STR_DONE), UI_12_FONT_ID, false);

  if (showButtonHints && mappedInput) {
    drawStatsFrontChrome(renderer, mappedInput, tr(STR_BACK), "", "", "");
  }
}
