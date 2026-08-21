#pragma once

#include <BoardConfig.h>
#include <HalGPIO.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "CasperSettings.h"
#include "components/themes/minimal/MinimalTheme.h"

class GfxRenderer;
struct RecentBook;
struct BookReadingStats;
struct GlobalReadingStats;
struct Rect;

// Penumbra home theme. No cover art.
// Clock face (devices with RTC — X3 and X4 Pro): large centered clock (top) + under-panel:
//   Title/Author · Recents · Stats · Lifetime — 4 page-dots when tracking is on.
//   X4 Pro uses the same structure scaled for 800×480 (shorter than X3's 792×528).
// Progress face (classic X4, no RTC): Now Reading title/author (top) + Recents under-panel only.
//   No page-dots / stats pages (tracking requires wall clock).
namespace PenumbraMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = MinimalMetrics::values;
  v.homeTopPadding = 10;
  // No cover plate — tile height is unused for layout but kept nonzero for
  // touch-band math that uses homeCoverTileHeight in classic themes.
  v.homeCoverHeight = 1;
  v.homeCoverTileHeight = 1;
  // Cap for under-panel list (progress face uses 5 without View All; clock face ≤4 + View All).
  v.homeRecentBooksCount = 5;
  v.homeContinueReadingInMenu = false;
  v.homeMenuTopOffset = 0;
  // Text-only footer band — see MinimalTheme (drawButtonHints is shared).
  v.buttonHintsHeight = 34;
  return v;
}

constexpr ThemeMetrics values = makeValues();
}  // namespace PenumbraMetrics

// Session-only under-panel mode (not persisted). HomeActivity cycles on side L/R.
// Numeric values match page-dot order so Recents is always a stable slot.
namespace PenumbraThemeUi {
enum class UnderMode : uint8_t {
  Recents = 0,      // Clock face + progress face.
  BookStats = 1,    // Clock face when tracking is on.
  Lifetime = 2,     // Clock face when tracking is on.
  TitleAuthor = 3,  // Clock face only (title under clock).
  Count = 4
};

// True when Penumbra draws the giant home clock (needs wall time → RTC).
// X3 and X4 Pro: clock face. Classic X4: progress / Now Reading face.
inline bool usesClockFace() { return BoardConfig::hasRtc(); }
inline bool usesProgressFace() { return !usesClockFace(); }

// First under-panel page (clock face → Title/Author; progress face → Recents).
inline UnderMode defaultUnderMode() { return usesClockFace() ? UnderMode::TitleAuthor : UnderMode::Recents; }

inline UnderMode& underMode() {
  // Lazy default: re-evaluated only once. Always force-clamp on paint / home enter.
  static UnderMode mode = UnderMode::Recents;
  return mode;
}

// Reset to the device default page (call on theme enter / theme switch / home enter).
inline void resetUnderModeToDefault() { underMode() = defaultUnderMode(); }

// --- Progress face: Recents only (no multi-page under-panel) ---
inline int x4PageIndex(const UnderMode m) {
  (void)m;
  return 0;
}

inline UnderMode x4ModeFromPage(const int page) {
  (void)page;
  return UnderMode::Recents;
}

// --- Clock-face page-dot / cycle order: Title (0) → Recents (1) → Stats (2) → Lifetime (3) ---
inline int x3PageIndex(const UnderMode m) {
  switch (m) {
    case UnderMode::TitleAuthor:
      return 0;
    case UnderMode::Recents:
      return 1;
    case UnderMode::BookStats:
      return 2;
    case UnderMode::Lifetime:
      return 3;
    default:
      return 0;
  }
}

inline UnderMode x3ModeFromPage(const int page) {
  switch (page) {
    case 1:
      return UnderMode::Recents;
    case 2:
      return UnderMode::BookStats;
    case 3:
      return UnderMode::Lifetime;
    case 0:
    default:
      return UnderMode::TitleAuthor;
  }
}

// True when under-panel is the recents list (either face).
// Progress face TitleAuthor is not a real page — treat as Recents for Down / Read.
inline bool isRecentsUnderPanel() {
  const UnderMode m = underMode();
  if (m == UnderMode::Recents) return true;
  return usesProgressFace() && m == UnderMode::TitleAuthor;
}

// Legacy name used by HomeActivity (progress-face Recents under-panel).
inline bool isX4RecentsUnderPanel() { return usesProgressFace() && isRecentsUnderPanel(); }

// Collapse invalid / stats pages when tracking is disabled.
// Progress face (no RTC): Recents only. Clock face (RTC): multi-page when tracking on.
inline void clampUnderModeToTracking() {
  if (!SETTINGS.readingStatsTrackingEnabled()) {
    underMode() = defaultUnderMode();
    return;
  }
  // Progress face: no TitleAuthor under-clock page.
  if (usesProgressFace() && underMode() == UnderMode::TitleAuthor) {
    underMode() = UnderMode::Recents;
  }
}

// Returns true if the mode changed (caller can window-repaint).
// Clock face: Title → Recents → Stats → Lifetime when tracking is on.
// Progress face (no RTC): no cycle.
inline bool cycleUnderMode(const int delta) {
  if (!SETTINGS.readingStatsTrackingEnabled() || !usesClockFace()) {
    underMode() = defaultUnderMode();
    return false;
  }
  constexpr int n = 4;
  int page = x3PageIndex(underMode());
  page = (page + delta) % n;
  if (page < 0) page += n;
  const UnderMode next = x3ModeFromPage(page);
  if (next == underMode()) return false;
  underMode() = next;
  return true;
}

// Last-drawn Recents under-panel hit geometry (for touch — not equal row slices).
struct RecentsHitLayout {
  int booksTop = 0;
  int rowStep = 0;  // row height + gap
  int bookCount = 0;
  int viewAllTop = 0;
  int viewAllBottom = 0;
  int underTop = 0;     // midY + rule
  int underBottom = 0;  // above dots / footer
  bool valid = false;
};
RecentsHitLayout& recentsHitLayout();

// White-fill + redraw the band below the center rule. listFocusIndex picks the
// highlighted row in the Recents under-panel only (does NOT change upper title).
Rect redrawUnderPanel(GfxRenderer& renderer, const std::vector<RecentBook>& recentBooks, int listFocusIndex,
                      const BookReadingStats* stats, float progressPercent, const GlobalReadingStats* globalStats);

// Ensure Recents progress % is in RAM. SD load only for paths not already cached
// (or when the list is empty). Path order changes reuse prior % by path match —
// does not re-read all N books from SD. Call after loadRecentBooks on resume.
void warmRecentsProgressCache(const std::vector<RecentBook>& books);

// Update one book's micro-bar % after reader exit (or mark finished). No SD I/O.
// Prefer this over force-reloading every recent book.
void updateRecentsProgressForPath(const char* bookPath, float progressPercent);

// Drop the RAM progress cache (clear cache / heavy book actions). Next warm reloads SD.
void invalidateRecentsProgressCache();

// White-fill only the clock digit band (not weekday/hairline) and redraw time.
// prevTime: last drawn string ("H:MM") so the dirty rect can be the union of old/new
// and, when only the minutes change, only the changing suffix is cleared.
// Returns the tight dirty rect for a windowed/soft panel update.
Rect redrawClockBlock(GfxRenderer& renderer, const char* prevTime = nullptr, char* outTime = nullptr,
                      size_t outTimeSize = 0);

// Clock-face only: 72pt clock is 2-bit AA, but BW home paints drop light fringe → jagged.
// Call after BW home is in the framebuffer (full paint or after redrawClockBlock).
// baseMode = HALF/FAST for greyscale base; window greys over the clock digit band.
// Returns false if not clock-face / storeBw failed (caller should plain-display BW).
bool displayClockAntiAliased(GfxRenderer& renderer, int baseRefreshMode, const Rect* dirtyOverride = nullptr);

bool formatHeroTimeNow(char* buf, size_t bufSize);
}  // namespace PenumbraThemeUi

class PenumbraTheme : public MinimalTheme {
 public:
  // selectorIndex: list focus for Recents under-panel ONLY.
  // Upper "Now Reading" / clock-face title / Stats always use last-read book (index 0).
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           StoreCoverBufferFn storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f, const GlobalReadingStats* globalStats = nullptr,
                           const char* currentChapterTitle = nullptr) const override;
};
