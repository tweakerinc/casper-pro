#include "RivuletReaderActivity.h"

#include <FontCacheManager.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "BookAppearanceGeometry.h"
#include "ReaderRenderKey.h"
#include "SdCardFontSystem.h"
#include "fontIds.h"

namespace {
constexpr uint8_t kSizes[] = {8, 10, 12, 14, 16, 18};
constexpr const char* kTabs[] = {"Font", "Spacing", "Layout", "Style"};
constexpr const char* kLines[] = {"Tight", "Normal", "Wide"};
constexpr const char* kAlignments[] = {"Justified", "Left", "Center", "Right", "Book's style"};
const char* familyName(const BookAppearanceState& s) {
  return s.sdFamily[0] ? s.sdFamily : (s.family == CasperSettings::LITERATA ? "Literata" : "Source Serif 4");
}
void label(GfxRenderer& r, int x, int y, const char* text, int maxWidth) {
  // No temporary string/vector for each row. Truncate only the displayed label,
  // at UTF-8 boundaries; the selection keeps the full supported family name.
  char buffer[96];
  size_t n = std::min(std::strlen(text), sizeof(buffer) - 1);
  if (text[n] != '\0') {
    while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xc0) == 0x80) --n;
  }
  std::memcpy(buffer, text, n);
  buffer[n] = '\0';
  while (n > 0 && r.getTextWidth(UI_10_FONT_ID, buffer) > maxWidth) {
    do { --n; } while (n > 0 && (static_cast<unsigned char>(buffer[n]) & 0xc0) == 0x80);
    buffer[n] = '\0';
  }
  r.drawText(UI_10_FONT_ID, x, y, buffer);
}
}  // namespace

bool RivuletReaderActivity::handleBookAppearanceGesture() {
  if (!mappedInput.hasTouch() || !ready_ || error_ || !engine_.hasChapter()) return false;
  RenderLock lock;
  if (appearanceOpen_) return true;
  if (!engine_.ensureLaidOut(renderer)) return true;  // never route failure to generic Settings
  appearanceOriginal_ = appearanceDesired_ = BookAppearanceState::capture();
  appearanceAnchor_ = engine_.page().start;
  appearanceTab_ = 0;
  appearanceFontList_ = false;
  appearanceFontOffset_ = 0;
  appearanceMessage_ = nullptr;
  appearanceOpen_ = true;
  appearanceChangeMs_ = millis();
  pendingConfirmMenuOpen_ = false;
  pageTurnLatch_.waitingRelease = true;
  mappedInput.suppressTouchContact();
  requestUpdate();
  return true;
}

bool RivuletReaderActivity::applyBookAppearance(const BookAppearanceState& desired) {
  RenderLock lock;
  const auto previous = BookAppearanceState::capture();
  if (desired == previous) {
    // Undo must also cancel a coalesced change which has not been previewed yet.
    appearanceDesired_ = desired;
    appearanceMessage_ = nullptr;
    requestUpdate();
    return true;
  }
  desired.apply();
  if (auto* cache = renderer.getFontCacheManager()) cache->clearCache();
  glyphCacheSpine_ = glyphCachePage_ = -1;
  const bool ok = sdFontSystem.tryApplyReaderSelection(renderer, [](void* context) {
    auto* self = static_cast<RivuletReaderActivity*>(context);
    const readerkey::Layout layout = readerkey::compute(self->renderer);
    if (!self->engine_.reflowToCursor(self->renderer, layout.key, layout.lineCompression, self->appearanceAnchor_)) {
      return false;
    }
    self->marginX_ = layout.marginL;
    self->marginY_ = layout.marginT;
    self->marginR_ = layout.marginR;
    self->marginB_ = layout.marginB;
    return true;
  }, this);
  if (!ok) {
    previous.apply();
    appearanceDesired_ = previous;
    appearanceMessage_ = "Could not apply - previous page kept";
  } else {
    appearanceDesired_ = desired;
    appearanceMessage_ = nullptr;
    pageMapDirty_ = true;
    updateBookmarkFlag();
    refreshPageFootnotes();
  }
  requestUpdate();
  return ok;
}

bool RivuletReaderActivity::closeBookAppearance() {
  if (!appearanceOpen_) return true;
  // Commit the last coalesced change, not the penultimate visible preview.
  if (!applyBookAppearance(appearanceDesired_)) return false;
  RenderLock lock;
  if (BookAppearanceState::capture() != appearanceOriginal_ && !SETTINGS.saveToFile()) {
    appearanceMessage_ = "Could not save - tap Done to retry";
    requestUpdate();
    return false;
  }
  appearanceOpen_ = false;
  appearanceFontList_ = false;
  appearanceMessage_ = nullptr;
  mappedInput.suppressTouchContact();
  pageTurnLatch_.waitingRelease = true;
  pendingConfirmMenuOpen_ = false;
  // New typography changes page numbers even when the anchored text did not move.
  (void)saveProgress();
  requestUpdate();
  return true;
}

void RivuletReaderActivity::setAppearanceFamily(const int index) {
  if (index < 0) return;
  if (index < CasperSettings::BUILTIN_FONT_COUNT) {
    appearanceDesired_.family = static_cast<uint8_t>(index);
    appearanceDesired_.sdFamily[0] = '\0';
    appearanceDesired_.size = std::clamp<uint8_t>(appearanceDesired_.size, CasperSettings::SIZE_10,
                                                 CasperSettings::SIZE_16);
  } else {
    const auto& families = sdFontSystem.registry().getFamilies();
    const size_t sdIndex = static_cast<size_t>(index - CasperSettings::BUILTIN_FONT_COUNT);
    if (sdIndex >= families.size()) return;
    const auto& family = families[sdIndex];
    if (family.name.size() >= sizeof(appearanceDesired_.sdFamily) || family.files.empty()) {
      appearanceMessage_ = "Font name too long or no usable sizes";
      return;
    }
    std::snprintf(appearanceDesired_.sdFamily, sizeof(appearanceDesired_.sdFamily), "%s", family.name.c_str());
  }
  appearanceFontList_ = false;
  appearanceChangeMs_ = millis();
  appearanceMessage_ = nullptr;
}

void RivuletReaderActivity::adjustAppearanceSize(const int direction) {
  const auto* family = appearanceDesired_.sdFamily[0]
      ? sdFontSystem.registry().findFamily(appearanceDesired_.sdFamily) : nullptr;
  const int old = std::min<int>(appearanceDesired_.size, 5);
  const auto* oldFile = family ? family->findClosestReaderSize(static_cast<uint8_t>(old)) : nullptr;
  for (int i = old + direction; i >= 0 && i < 6; i += direction) {
    if (!family && (i < CasperSettings::SIZE_10 || i > CasperSettings::SIZE_16)) continue;
    const auto* file = family ? family->findClosestReaderSize(static_cast<uint8_t>(i)) : nullptr;
    // Do not offer duplicate labels for aliases to the very same SD face.
    if (family && (!file || (oldFile && file->pointSize == oldFile->pointSize))) continue;
    appearanceDesired_.size = static_cast<uint8_t>(i);
    appearanceChangeMs_ = millis();
    appearanceMessage_ = nullptr;
    break;
  }
}

void RivuletReaderActivity::loopBookAppearance() {
  // Collapse rapid +/- taps into one resident-chapter layout; no queued reloads.
  if (appearanceDesired_ != BookAppearanceState::capture() &&
      static_cast<uint32_t>(millis() - appearanceChangeMs_) >= 180) {
    (void)applyBookAppearance(appearanceDesired_);
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    (void)closeBookAppearance();
    return;
  }
  const auto g = BookAppearanceGeometry::make(renderer.getScreenWidth(), renderer.getScreenHeight());
  int x = 0, y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) {
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::None) return;
    RenderLock lock;
    // Vertical font-list scrolling is never a dismissal or a global edge action.
    if (appearanceFontList_ && (swipe == MappedInputManager::SwipeDir::Up ||
                                swipe == MappedInputManager::SwipeDir::Down)) {
      const int count = CasperSettings::BUILTIN_FONT_COUNT + sdFontSystem.registry().getFamilyCount();
      appearanceFontOffset_ = std::clamp(appearanceFontOffset_ +
          (swipe == MappedInputManager::SwipeDir::Up ? g.rows : -g.rows), 0, std::max(0, count - g.rows));
    } else if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Right) {
      appearanceTab_ = static_cast<uint8_t>((appearanceTab_ +
          (swipe == MappedInputManager::SwipeDir::Left ? 1 : 3)) % 4);
      appearanceFontList_ = false;
    }
    mappedInput.suppressTouchContact();
    requestUpdate();
    return;
  }
  if (y < g.top || (y < g.top + g.header && x >= g.width - 100)) {
    (void)closeBookAppearance();  // outside tap closes ONLY; never reaches page-turn dispatch
    return;
  }
  if (y >= g.footer && x < 100) {
    (void)applyBookAppearance(appearanceOriginal_);
    return;
  }
  RenderLock lock;
  if (y < g.top + g.header) {
    appearanceFontList_ = false;
  } else if (const int tab = g.tabAt(x, y); tab >= 0) {
    appearanceTab_ = static_cast<uint8_t>(tab);
    appearanceFontList_ = false;
  } else if (y >= g.footer && appearanceFontList_) {
    const int count = CasperSettings::BUILTIN_FONT_COUNT + sdFontSystem.registry().getFamilyCount();
    appearanceFontOffset_ = std::clamp(appearanceFontOffset_ + (x < g.width / 2 ? -g.rows : g.rows),
                                      0, std::max(0, count - g.rows));
  } else {
    const int row = g.rowAt(y);
    if (row < 0) return;
    const int direction = x < g.width / 2 ? -1 : 1;
    if (appearanceFontList_) {
      setAppearanceFamily(appearanceFontOffset_ + row);
    } else if (appearanceTab_ == 0) {
      if (row == 0) {
        appearanceFontList_ = true;
        int index = appearanceDesired_.family;
        if (appearanceDesired_.sdFamily[0]) {
          index = CasperSettings::BUILTIN_FONT_COUNT + sdFontSystem.registry().getFamilyIndex(appearanceDesired_.sdFamily);
        }
        appearanceFontOffset_ = std::max(0, index - 1);
      } else if (row == 1) {
        adjustAppearanceSize(direction);
      }
    } else if (appearanceTab_ == 1) {
      if (row == 0) appearanceDesired_.line = static_cast<uint8_t>((appearanceDesired_.line + (direction > 0 ? 1 : 2)) % 3);
      if (row == 1) {
        int value = !appearanceDesired_.paragraphs ? 0 :
            (appearanceDesired_.paragraphHeight == CasperSettings::SPACING_QUARTER ? 1 :
             appearanceDesired_.paragraphHeight == CasperSettings::SPACING_FULL ? 3 : 2);
        value = (value + (direction > 0 ? 1 : 3)) % 4;
        appearanceDesired_.paragraphs = value != 0;
        appearanceDesired_.paragraphHeight = value == 1 ? CasperSettings::SPACING_QUARTER :
            value == 3 ? CasperSettings::SPACING_FULL : CasperSettings::SPACING_HALF;
      }
    } else if (appearanceTab_ == 2) {
      if (row == 0) appearanceDesired_.margin = static_cast<uint8_t>(std::clamp<int>(
          appearanceDesired_.margin + direction * CasperSettings::SCREEN_MARGIN_STEP,
          CasperSettings::SCREEN_MARGIN_MIN, CasperSettings::SCREEN_MARGIN_MAX));
      if (row == 1) {
        appearanceDesired_.align = static_cast<uint8_t>((appearanceDesired_.align + (direction > 0 ? 1 : 4)) % 5);
        appearanceDesired_.embedded = appearanceDesired_.align == CasperSettings::BOOK_STYLE;
      }
    } else {
      if (row == 0) appearanceDesired_.hyphenation = !appearanceDesired_.hyphenation;
      if (row == 1) appearanceDesired_.aa = !appearanceDesired_.aa;
    }
    appearanceChangeMs_ = millis();
  }
  mappedInput.suppressTouchContact();
  requestUpdate();
}

void RivuletReaderActivity::paintBookAppearance() {
  const auto g = BookAppearanceGeometry::make(renderer.getScreenWidth(), renderer.getScreenHeight());
  const auto& s = appearanceDesired_;
  renderer.fillRect(0, g.top, g.width, g.height - g.top, false);
  renderer.drawLine(0, g.top, g.width - 1, g.top, 2, true);
  renderer.drawLine(0, g.top + 5, g.width - 1, g.top + 5);
  label(renderer, 16, g.top + 14, "Book Appearance", g.width - 130);
  label(renderer, g.width - 84, g.top + 14, "Done", 76);
  for (int i = 0; i < 4; ++i) {
    const int left = i * g.width / 4, right = (i + 1) * g.width / 4;
    label(renderer, left + 12, g.top + g.header + 10, kTabs[i], right - left - 24);
    if (i == appearanceTab_) renderer.fillRect(left + 10, g.rowsTop - 5, right - left - 20, 3);
  }
  renderer.drawLine(0, g.rowsTop, g.width - 1, g.rowsTop);
  auto row = [&](int index, const char* title, const char* value, bool adjust) {
    const int y = g.rowsTop + index * g.rowHeight;
    // A single baseline remains readable in landscape (44px rows). The
    // entire half-row is the target, rather than just the +/- glyph itself.
    const int inset = adjust ? 44 : 16;
    label(renderer, inset, y + 12, title, g.width / 2 - inset - 12);
    label(renderer, g.width / 2 + 12, y + 12, value, g.width / 2 - inset - 12);
    if (adjust) {
      label(renderer, 14, y + 12, "-", 25);
      label(renderer, g.width - 30, y + 12, "+", 25);
    }
    renderer.drawLine(12, y + g.rowHeight - 1, g.width - 12, y + g.rowHeight - 1);
  };
  if (appearanceFontList_) {
    const auto& families = sdFontSystem.registry().getFamilies();
    for (int i = 0; i < g.rows; ++i) {
      const int index = appearanceFontOffset_ + i;
      const char* title = nullptr;
      if (index == CasperSettings::SOURCESERIF4) title = "Source Serif 4";
      else if (index == CasperSettings::LITERATA) title = "Literata";
      else if (index >= CasperSettings::BUILTIN_FONT_COUNT &&
               static_cast<size_t>(index - CasperSettings::BUILTIN_FONT_COUNT) < families.size())
        title = families[index - CasperSettings::BUILTIN_FONT_COUNT].name.c_str();
      if (title) {
        const int y = g.rowsTop + i * g.rowHeight;
        label(renderer, 16, y + 14, title, g.width - 36);
        renderer.drawLine(12, y + g.rowHeight - 1, g.width - 12, y + g.rowHeight - 1);
      }
    }
  } else if (appearanceTab_ == 0) {
    row(0, "Family", familyName(s), false);
    uint8_t points = kSizes[std::min<int>(s.size, 5)];
    if (const auto* family = s.sdFamily[0] ? sdFontSystem.registry().findFamily(s.sdFamily) : nullptr) {
      if (const auto* file = family->findClosestReaderSize(s.size)) points = file->pointSize;
    }
    char size[24]; std::snprintf(size, sizeof(size), "%u pt", static_cast<unsigned>(points));
    row(1, "Font size", size, true);
    row(2, "Preview", "Current book", false);
  } else if (appearanceTab_ == 1) {
    row(0, "Line spacing", kLines[std::min<int>(s.line, 2)], true);
    row(1, "Paragraph gap", !s.paragraphs ? "Off" : s.paragraphHeight == CasperSettings::SPACING_FULL ? "Full" :
        s.paragraphHeight == CasperSettings::SPACING_QUARTER ? "Quarter" : "Half", true);
    row(2, "Applies to", "Current layout", false);
  } else if (appearanceTab_ == 2) {
    char margin[24]; std::snprintf(margin, sizeof(margin), "%u px", static_cast<unsigned>(s.margin));
    row(0, "Margins", margin, true);
    row(1, "Alignment", kAlignments[std::min<int>(s.align, 4)], true);
    row(2, "Publisher style", s.align == CasperSettings::BOOK_STYLE ? "On" : "Overridden", false);
  } else {
    row(0, "Hyphenation", s.hyphenation ? "On" : "Off", true);
    row(1, "Anti-aliasing", s.aa ? "On" : "Off", true);
    row(2, "AA preview", "After closing", false);
  }
  label(renderer, 16, g.footer + 10, "Undo", 76);
  const char* footer = appearanceMessage_ ? appearanceMessage_ :
      appearanceFontList_ ? "< Previous          Next >" : "Defaults for all books";
  label(renderer, 106, g.footer + 10, footer, g.width - 120);
}
