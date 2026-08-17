#include "DictionaryWordSelectActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>

#include <HalDisplay.h>

#include <HalGPIO.h>

#include "CasperSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/DictionaryRegistry.h"
#include "util/UiGhostPolicy.h"

// Tiny pad so a highlight box (+2) clears the hint strip without making the
// reserved chrome taller than dashboard / settings buttons (buttonHintsHeight).
constexpr int kHintWordClearance = 2;

namespace {

constexpr unsigned long POPUP_DURATION_MS = 1500;
// Long-press Select to arm multi-word range (~0.4s, same as reader shortcuts).
constexpr unsigned long MULTI_SELECT_HOLD_MS = 400;

// A token is selectable when it has an ASCII alphanumeric or a non-ASCII
// codepoint outside U+2000-U+206F (dashes, bullets and other General
// Punctuation that appear as standalone tokens are not words).
bool isSelectableToken(const char* text) {
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text); *p != 0; p++) {
    if (*p < 0x80) {
      if (std::isalnum(*p)) return true;
    } else if (*p == 0xE2 && (p[1] == 0x80 || p[1] == 0x81)) {
      if (p[2] == 0) break;  // truncated sequence: skipping would step past the NUL
      p += 2;                // skip the 3-byte General Punctuation codepoint
    } else {
      return true;
    }
  }
  return false;
}

void indexBuildYield(void*) { vTaskDelay(1); }

}  // namespace

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  if (fontId == 0) fontId = SETTINGS.getReaderFontId();
  // Match the page's line pitch (Tight default ~0.9× advanceY). Using bare
  // getLineHeight made size-10 highlights spill into the next line.
  lineHeight = std::max(1, renderer.getLineHeight(fontId, SETTINGS.getReaderLineCompression()));
  // No null check: a failed allocation just disables the differential
  // fast path (drawHighlightWithSnapshot skips the read), keeping the
  // full-repaint path as the fallback.
  snapshot = makeUniqueNoThrow<uint8_t[]>(SNAPSHOT_CAPACITY);
  startMarkIdx = -1;
  multiSelectArmedThisHold = false;
  confirmDown = false;
  touchSelectActive = false;
  touchAnchor = -1;
  touchDragged = false;
  // Residual long-press Menu that opened dictionary: wait for Select release.
  ignoreConfirmUntilReleased = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  // Classic path extracts from Page; Rivulet path already has `words` prebuilt.
  if (page) {
    extractWords();
  } else if (!words.empty()) {
    // Measure widths if caller left them zero.
    for (auto& w : words) {
      if (w.width <= 0 && w.text) {
        w.width = static_cast<int16_t>(renderer.getTextAdvanceX(fontId, w.text, w.style));
      }
    }
  }

  // Seed selection: long-press coords from reader (KOReader-like), else mid-page
  // for button/menu open so L/R travel is balanced.
  if (!words.empty()) {
    if (initialTouchX_ >= 0 && initialTouchY_ >= 0) {
      const int hit = wordAtOrNearest(initialTouchX_, initialTouchY_);
      if (hit >= 0) selected = hit;
    } else {
      const int initial = closestInRow(rowCount / 2, renderer.getScreenWidth() / 2);
      if (initial >= 0) selected = initial;
    }
  }

  // Same finger still down after long-press open → keep highlighting / drag range
  // without requiring a second press. Do not call suppressTouchContact on open.
  if (mappedInput.hasTouch() && !words.empty() && selected >= 0) {
    int hx = 0, hy = 0;
    if (mappedInput.isScreenTouchHeld(hx, hy)) {
      touchSelectActive = true;
      touchAnchor = selected;
      touchDragged = false;
      startMarkIdx = -1;
      touchStartX_ = hx;
      touchStartY_ = hy;
      touchMaxAbsDx_ = 0;
      touchMaxAbsDy_ = 0;
      touchSelectStartedMs_ = millis();
      // Finger may have moved while the tool opened — track live position.
      applyTouchSelectionAt(hx, hy, /*isDownEdge=*/false);
    }
  }
  requestUpdate();
}

void DictionaryWordSelectActivity::extractWords() {
  if (!page) return;
  words.clear();
  words.reserve(128);
  rowCount = 0;

  // Do not select words under the front-button hint strip. Orientation-aware:
  // portrait = bottom band; landscape = logical side band (same as drawButtonHints).
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, /*hasFrontButtonHints=*/true,
                                                             /*hasSideButtonHints=*/false);
  const int maxWordBottom = safe.y + safe.height - kHintWordClearance;
  const int minWordLeft = safe.x + kHintWordClearance;
  const int maxWordRight = safe.x + safe.width - kHintWordClearance;
  // lineHeight is set in onEnter() before extractWords().
  const int lh = std::max(1, lineHeight);

  // Single walk: collect the selectable words while accumulating their text
  // and styles (~2KB transient string, freed on return). Widths are measured
  // afterwards: merging the page's codepoints into the SD font's persistent
  // advance table first keeps getTextAdvanceX on the in-RAM path instead of
  // loading glyphs from SD one overflow slot at a time.
  std::string pageText;
  pageText.reserve(2048);
  uint8_t styleMask = 0;

  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block || !block->valid()) continue;

    const int lineY = line->yPos + marginTop;
    // Whole line below the safe band: skip without walking tokens.
    if (lineY + lh > maxWordBottom) {
      continue;
    }

    bool rowHasWords = false;
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      const char* text = block->wordText(i);
      if (!isSelectableToken(text)) continue;

      const int wordX = line->xPos + block->wordXpos(i) + marginLeft;
      // Landscape: skip tokens that sit under the side hint strip.
      if (wordX >= maxWordRight) continue;

      WordBox box;
      box.x = static_cast<int16_t>(std::max(wordX, minWordLeft));
      box.y = static_cast<int16_t>(lineY);
      box.style = block->wordStyle(i);
      box.width = 0;  // measured below, once the advance table is ready
      box.row = rowCount;
      box.text = text;
      words.push_back(box);
      rowHasWords = true;

      pageText.append(text);
      pageText.push_back(' ');
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(box.style) & 0x03));
    }
    if (rowHasWords) rowCount++;
  }

  if (styleMask == 0) styleMask = 0x01;  // REGULAR
  renderer.ensureSdCardFontReady(fontId, pageText.c_str(), styleMask);
  const Rect safeClip = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int clipRight = safeClip.x + safeClip.width - kHintWordClearance;
  for (auto& word : words) {
    word.width = static_cast<int16_t>(renderer.getTextAdvanceX(fontId, word.text, word.style));
    // Clamp width so highlight never crosses into the landscape side chrome.
    if (word.x + word.width > clipRight) {
      word.width = static_cast<int16_t>(std::max(0, clipRight - word.x));
    }
  }
}

// Index of the word whose box (with finger-sized slop) contains the touch
// point; -1 when the touch lands on no word. Prefer the closest row by Y when
// vertical slop overlaps two lines (first-hit would steal from the next line).
int DictionaryWordSelectActivity::wordAt(const int x, const int y) const {
  const int slopX = std::max(8, lineHeight / 3);
  const int slopY = std::max(6, lineHeight / 4);
  int best = -1;
  int bestDy = INT_MAX;
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    const WordBox& word = words[i];
    if (x < word.x - slopX || x >= word.x + word.width + slopX) continue;
    if (y < word.y - slopY || y >= word.y + lineHeight + slopY) continue;
    const int cy = word.y + lineHeight / 2;
    const int dy = std::abs(y - cy);
    if (dy < bestDy) {
      bestDy = dy;
      best = i;
    }
  }
  return best;
}

int DictionaryWordSelectActivity::wordAtOrNearest(const int x, const int y) const {
  const int direct = wordAt(x, y);
  if (direct >= 0) return direct;
  if (words.empty()) return -1;

  // Multi-line drag: pick the reading-order row under the finger (by Y), then the
  // word closest in X on that row. Euclidean nearest jumps to wrong words and can
  // feel like "whole next line" when the start slides via phrase caps.
  int bestRow = -1;
  int bestRowDist = INT_MAX;
  for (const auto& w : words) {
    const int cy = w.y + lineHeight / 2;
    const int dy = std::abs(y - cy);
    if (dy < bestRowDist) {
      bestRowDist = dy;
      bestRow = static_cast<int>(w.row);
    }
  }
  // Stay on-page only (all words are current page). Reject far vertical misses.
  if (bestRow < 0 || bestRowDist > lineHeight * 2) {
    return -1;
  }
  const int onRow = closestInRow(static_cast<uint16_t>(bestRow), x);
  return onRow;
}

void DictionaryWordSelectActivity::applyTouchSelectionAt(const int x, const int y, const bool isDownEdge) {
  const int hit = isDownEdge ? wordAt(x, y) : wordAtOrNearest(x, y);
  if (hit < 0) return;

  if (isDownEdge) {
    touchSelectActive = true;
    touchAnchor = hit;
    touchDragged = false;
    selected = hit;
    // Anchor is fixed for the whole gesture — range is always touchAnchor…selected.
    startMarkIdx = -1;
    touchStartX_ = x;
    touchStartY_ = y;
    touchMaxAbsDx_ = 0;
    touchMaxAbsDy_ = 0;
    touchSelectStartedMs_ = millis();
    snapshotIdx = -1;
    requestUpdate();
    return;
  }

  if (!touchSelectActive || touchAnchor < 0) return;

  // Track travel so endTouchSelection can reject page-turn swipes.
  if (touchStartX_ >= 0 && touchStartY_ >= 0) {
    touchMaxAbsDx_ = std::max(touchMaxAbsDx_, std::abs(x - touchStartX_));
    touchMaxAbsDy_ = std::max(touchMaxAbsDy_, std::abs(y - touchStartY_));
  }

  if (hit != selected) {
    selected = hit;
    if (hit != touchAnchor) {
      touchDragged = true;
      // Keep start locked at the original press (never slide with the cursor).
      startMarkIdx = touchAnchor;
    } else {
      // Back on anchor → single word.
      startMarkIdx = -1;
      touchDragged = false;
    }
    snapshotIdx = -1;
    requestUpdate();
  }
}

void DictionaryWordSelectActivity::showActionMenu() {
  popup = Popup::ActionMenu;
  snapshotIdx = -1;
  requestUpdate();
}

void DictionaryWordSelectActivity::layoutActionMenu(int& outX, int& outY, int& outW, int& outH, int& outRowH) const {
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  outW = std::min(300, pageW - 36);
  // Taller rows so fat fingers hit Dictionary vs Highlight cleanly.
  outRowH = 52;
  outH = outRowH * 2 + 12;
  // Prefer just below the selection; fall back above if near bottom.
  int lo = 0, hi = 0;
  selectionBounds(lo, hi);
  int selBottom = 0;
  int selTop = pageH;
  for (int i = lo; i <= hi && i < static_cast<int>(words.size()); ++i) {
    const auto& w = words[static_cast<size_t>(i)];
    selBottom = std::max(selBottom, w.y + lineHeight);
    selTop = std::min(selTop, static_cast<int>(w.y));
  }
  outX = (pageW - outW) / 2;
  outY = selBottom + 12;
  if (outY + outH > pageH - 16) {
    outY = std::max(12, selTop - outH - 12);
  }
}

void DictionaryWordSelectActivity::drawActionMenu() const {
  int mx = 0, my = 0, mw = 0, mh = 0, rowH = 0;
  layoutActionMenu(mx, my, mw, mh, rowH);
  // Clean card — no offset drop-shadow (shadow made the bottom edge look thicker).
  renderer.fillRoundedRect(mx, my, mw, mh, 10, Color::White);
  renderer.drawRoundedRect(mx, my, mw, mh, 1, 10, true);
  // Divider between rows (mid-gap for easier targeting)
  renderer.drawLine(mx + 12, my + rowH + 6, mx + mw - 12, my + rowH + 6, true);

  // Short label — "Dictionary Lookup" was long; "Dictionary" is clearer.
  const char* dictLabel = tr(STR_DICTIONARY);
  const char* hiLabel = "Highlight";
  const int dictW = renderer.getTextWidth(UI_10_FONT_ID, dictLabel);
  const int hiW = renderer.getTextWidth(UI_10_FONT_ID, hiLabel);
  const int th = renderer.getLineHeight(UI_10_FONT_ID);
  renderer.drawText(UI_10_FONT_ID, mx + (mw - dictW) / 2, my + (rowH - th) / 2, dictLabel, true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, mx + (mw - hiW) / 2, my + rowH + 6 + (rowH - th) / 2, hiLabel, true,
                    EpdFontFamily::BOLD);
}

void DictionaryWordSelectActivity::endTouchSelection() {
  if (!touchSelectActive) return;
  touchSelectActive = false;
  const int anchor = touchAnchor;
  const bool dragged = touchDragged;
  const int maxDx = touchMaxAbsDx_;
  const int maxDy = touchMaxAbsDy_;
  const unsigned long heldMs =
      touchSelectStartedMs_ != 0 ? (millis() - touchSelectStartedMs_) : 0;
  touchAnchor = -1;
  touchDragged = false;
  touchStartX_ = touchStartY_ = -1;
  touchMaxAbsDx_ = touchMaxAbsDy_ = 0;
  touchSelectStartedMs_ = 0;
  if (words.empty() || selected < 0 || selected >= static_cast<int>(words.size())) return;

  // Accidental page-turn swipe that briefly held still (long-press fired):
  // mostly horizontal travel, little vertical, short hold after arm → leave
  // without Dictionary|Highlight so the reader can take the next swipe.
  constexpr int kSwipeRejectPx = 56;
  constexpr unsigned long kQuickSwipeMs = 900;
  if (maxDx >= kSwipeRejectPx && maxDx > maxDy * 3 / 2 && heldMs < kQuickSwipeMs && !dragged) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  // Same idea when the finger raced across one line (many words) as a swipe
  // rather than a deliberate multi-word highlight: large dx, short hold.
  if (mode_ == Mode::Dictionary && dragged && maxDx >= kSwipeRejectPx && maxDx > maxDy * 2 &&
      heldMs < kQuickSwipeMs) {
    int lo = 0, hi = 0;
    selectionBounds(lo, hi);
    // Wide horizontal span in a short time = swipe, not highlight.
    if (hi - lo >= 3) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
      return;
    }
  }

  if (mode_ == Mode::Dictionary) {
    // Show Dictionary | Highlight — do not auto-lookup (saves power / SD).
    if (dragged && startMarkIdx < 0 && anchor >= 0) {
      startMarkIdx = anchor;
    }
    showActionMenu();
    return;
  }

  // Clip-only mode: drag multi-word in one gesture → set range ends and confirm.
  if (dragged && startMarkIdx < 0 && anchor >= 0) {
    startMarkIdx = anchor;
  }
  if (dragged && startMarkIdx >= 0 && selected != startMarkIdx) {
    handleSelectAction();  // confirm end
    return;
  }
  // Single-word lift: classic first mark (or second mark if start already set).
  handleSelectAction();
}

// Index of the word in `row` whose horizontal center is closest to centerX;
// -1 when the row has no words.
int DictionaryWordSelectActivity::closestInRow(const uint16_t row, const int centerX) const {
  int best = -1;
  int bestDistance = INT_MAX;
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    if (words[i].row != row) continue;
    const int distance = std::abs(words[i].x + words[i].width / 2 - centerX);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

void DictionaryWordSelectActivity::moveVertical(const int direction) {
  const WordBox& current = words[selected];
  const int targetRow = static_cast<int>(current.row) + direction;
  if (targetRow < 0 || targetRow >= static_cast<int>(rowCount)) return;

  const int best = closestInRow(static_cast<uint16_t>(targetRow), current.x + current.width / 2);
  if (best >= 0 && best != selected) {
    selected = best;
    requestUpdate();
  }
}

void DictionaryWordSelectActivity::selectionBounds(int& lo, int& hi) const {
  // Full reading-order range on this page only (words[] is current-page boxes).
  // Do NOT slide the start for a phrase cap — that made multi-line drag look like
  // the anchor jumped to the next line. Dictionary lookup caps later in buildLookupToken.
  lo = selected;
  hi = selected;
  if (startMarkIdx >= 0) {
    lo = std::min(startMarkIdx, selected);
    hi = std::max(startMarkIdx, selected);
  } else if (touchSelectActive && touchAnchor >= 0) {
    lo = std::min(touchAnchor, selected);
    hi = std::max(touchAnchor, selected);
  }
  if (lo < 0) lo = 0;
  if (hi >= static_cast<int>(words.size())) hi = static_cast<int>(words.size()) - 1;
}

// Build a lookup token from the selection. Soft-hyphen / end-of-line hyphen
// splits are glued; multi-word ranges join with spaces.
std::string DictionaryWordSelectActivity::buildLookupToken() const {
  if (words.empty() || selected < 0 || selected >= static_cast<int>(words.size())) {
    return {};
  }

  auto endsWithBreakHyphen = [](const std::string& s) -> bool {
    if (s.empty()) return false;
    if (s.back() == '-') return true;
    // UTF-8 soft hyphen U+00AD = C2 AD
    return s.size() >= 2 && static_cast<unsigned char>(s[s.size() - 2]) == 0xC2 &&
           static_cast<unsigned char>(s[s.size() - 1]) == 0xAD;
  };
  auto stripTrailingBreakHyphen = [](std::string& s) {
    if (s.empty()) return;
    if (s.back() == '-') {
      s.pop_back();
      return;
    }
    if (s.size() >= 2 && static_cast<unsigned char>(s[s.size() - 2]) == 0xC2 &&
        static_cast<unsigned char>(s[s.size() - 1]) == 0xAD) {
      s.resize(s.size() - 2);
    }
  };

  int lo = 0;
  int hi = 0;
  selectionBounds(lo, hi);
  // Dictionary lookup only: limit key size. Highlight/clip keeps the full page range.
  if (mode_ == Mode::Dictionary && hi - lo + 1 > kMaxPhraseWords) {
    if (selected >= lo) {
      lo = hi - (kMaxPhraseWords - 1);
    } else {
      hi = lo + (kMaxPhraseWords - 1);
    }
    if (lo < 0) lo = 0;
  }

  std::string phrase;
  for (int i = lo; i <= hi; ++i) {
    std::string token = words[static_cast<size_t>(i)].text ? words[static_cast<size_t>(i)].text : "";
    // Join continuation fragments on the same page (hyphenation / soft-hyphen).
    while (i + 1 <= hi && endsWithBreakHyphen(token)) {
      stripTrailingBreakHyphen(token);
      const char* next = words[static_cast<size_t>(i + 1)].text;
      if (next && *next) token += next;
      ++i;
    }
    if (token.empty()) continue;
    if (!phrase.empty()) phrase += ' ';
    phrase += token;
  }
  return phrase;
}

void DictionaryWordSelectActivity::performLookup() {
  std::vector<std::string> dictNames;
  SETTINGS.getEnabledDictionaries(dictNames);
  if (dictNames.empty()) {
    // Safety net: auto-use every installed pack so EN + bilingual cascade still
    // works if settings were cleared or never multi-selected.
    std::vector<DictionaryEntry> installed;
    DictionaryRegistry::discover(installed);
    dictNames.reserve(installed.size());
    for (const auto& e : installed) {
      dictNames.push_back(e.name);
    }
  }
  if (dictNames.empty()) {
    popup = Popup::Error;
    popupMsg = StrId::STR_DICT_NONE_SELECTED;
    popupTime = millis();
    requestUpdate();
    return;
  }

  popup = Popup::Busy;
  popupMsg = StrId::STR_DICT_LOOKING_UP;
  requestUpdateAndWait();

  // One pass per pack: open → index if needed → lookup. Avoids a second open
  // cycle (resolve + exists probes) before searching. SD still only holds one
  // reader at a time, so packs are sequential.
  //
  // buildLookupToken joins a multi-word range with spaces. Dictionary::lookup
  // expands that into collocation windows + per-token stems (so "Ayudame por
  // favor" can hit "por favor" / "ayudar" even when the full phrase is absent).
  const std::string rawToken = buildLookupToken();
  std::string combined;
  std::string headword;
  bool anyOpened = false;
  bool anyFound = false;
  const bool multi = dictNames.size() > 1;
  bool showedIndexing = false;

  auto runLookup = [&](const char* token) {
    for (const std::string& name : dictNames) {
      Dictionary dict;
      if (!dict.open(name.c_str())) {
        continue;
      }
      anyOpened = true;

      if (dict.needsIndex()) {
        if (!showedIndexing) {
          popupMsg = StrId::STR_DICT_INDEXING;
          requestUpdateAndWait();
          showedIndexing = true;
        }
        (void)dict.buildIndex(&indexBuildYield);
        popupMsg = StrId::STR_DICT_LOOKING_UP;
        requestUpdateAndWait();
      }

      std::string definition;
      std::string matched;
      if (!dict.lookup(token, definition, matched)) {
        continue;
      }
      anyFound = true;
      if (headword.empty()) {
        headword = matched.empty() ? Dictionary::cleanWord(token) : matched;
      }
      if (multi) {
        if (!combined.empty()) combined += "\n\n";
        // '@' → bold pack header in DictionaryDefinitionActivity.
        combined += '@';
        combined += name;
        combined += '\n';
        combined += definition;
      } else {
        if (combined.empty()) {
          combined = std::move(definition);
        } else {
          // Multi-word token fallback may hit several keys in one pack.
          combined += "\n\n";
          combined += definition;
        }
      }
    }
  };

  runLookup(rawToken.c_str());

  // Extra safety for multi-word ranges: if the phrase path still misses, try
  // each word alone and merge hits. Dictionary::lookup already expands windows
  // and stems for a single call; this covers packs/candidates that still miss.
  if (!anyFound) {
    const std::string cleaned = Dictionary::cleanWord(rawToken.c_str());
    if (cleaned.find(' ') != std::string::npos) {
      size_t start = 0;
      while (start < cleaned.size()) {
        while (start < cleaned.size() && cleaned[start] == ' ') ++start;
        if (start >= cleaned.size()) break;
        size_t end = cleaned.find(' ', start);
        if (end == std::string::npos) end = cleaned.size();
        if (end > start) {
          const std::string tok = cleaned.substr(start, end - start);
          runLookup(tok.c_str());
        }
        start = (end < cleaned.size()) ? end + 1 : end;
      }
    }
  }

  if (anyFound) {
    // Clear "Looking up…" and repaint a clean page *before* the definition
    // activity snapshots the framebuffer — otherwise the busy popup and the
    // word-select button chrome get baked into the background (double buttons
    // + stuck "Looking up…" under the definition card).
    popup = Popup::None;
    snapshotIdx = -1;
    requestUpdateAndWait();
    startActivityForResult(
        std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(headword), std::move(combined)),
        [this](const ActivityResult& result) {
          popup = Popup::None;
          snapshotIdx = -1;
          // Done (or tap): leave dictionary mode entirely → reader.
          // Back: keep word-select so the user can look up another word.
          if (!result.isCancelled) {
            finish();
            return;
          }
          requestUpdate();
        });
    return;
  }
  popup = anyOpened ? Popup::NotFound : Popup::Error;
  popupMsg = anyOpened ? StrId::STR_DICT_NOT_FOUND : StrId::STR_DICT_ERROR;
  popupTime = millis();
  requestUpdate();
}

void DictionaryWordSelectActivity::performClipConfirm() {
  if (words.empty() || selected < 0 || selected >= static_cast<int>(words.size())) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  // First Select: set range start (classic Clipping Tool).
  if (startMarkIdx < 0) {
    startMarkIdx = selected;
    snapshotIdx = -1;
    requestUpdate();
    return;
  }

  // Second Select: confirm end and return the selected text.
  int lo = 0;
  int hi = 0;
  selectionBounds(lo, hi);
  std::string text = buildLookupToken();
  if (text.empty()) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  ClippingResult clip;
  clip.text = std::move(text);
  clip.fromWordIdx = lo;
  clip.toWordIdx = hi;
  clip.startPageWordIndex = static_cast<uint16_t>(std::max(0, lo));
  clip.endPageWordIndex = static_cast<uint16_t>(std::max(0, hi));
  clip.wordCount = static_cast<uint16_t>(std::max(0, hi - lo + 1));
  // Page indices filled by the reader (single-page Rivulet harvest).
  clip.sectionPage = 0;
  clip.endSectionPage = 0;
  clip.sectionPageCount = 1;
  if (words[static_cast<size_t>(lo)].text) clip.startText = words[static_cast<size_t>(lo)].text;
  if (words[static_cast<size_t>(hi)].text) clip.endText = words[static_cast<size_t>(hi)].text;
  setResult(ActivityResult{std::move(clip)});
  finish();
}

void DictionaryWordSelectActivity::handleSelectAction() {
  if (words.empty()) return;
  if (mode_ == Mode::Clip) {
    performClipConfirm();
  } else {
    performLookup();
  }
}

bool DictionaryWordSelectActivity::handleHomeGesture() {
  // Hierarchical Home: always leave the tool and return to the book (one tap).
  // Clearing range on first Home and tool on second felt like "Home does nothing".
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
  return true;
}

void DictionaryWordSelectActivity::loop() {
  if (popup == Popup::NotFound || popup == Popup::Error) {
    if (millis() - popupTime >= POPUP_DURATION_MS) {
      popup = Popup::None;
      requestUpdate();
    }
    return;
  }

  // Action menu: Dictionary Lookup | Highlight — tap outside dismisses.
  if (popup == Popup::ActionMenu) {
    int tx = 0, ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      popup = Popup::None;
      startMarkIdx = -1;
      finish();
      return;
    }
    // Side page keys / swipe: leave the tool so page-turn works again.
    if (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
        mappedInput.wasPressed(MappedInputManager::Button::PageForward) ||
        mappedInput.wasSwipe() != MappedInputManager::SwipeDir::None) {
      popup = Popup::None;
      startMarkIdx = -1;
      finish();
      return;
    }
    if (mappedInput.wasScreenTapped(tx, ty)) {
      int mx = 0, my = 0, mw = 0, mh = 0, rowH = 0;
      layoutActionMenu(mx, my, mw, mh, rowH);
      if (tx >= mx && tx < mx + mw && ty >= my && ty < my + mh) {
        const int row = (ty - my) / std::max(1, rowH + 4);
        if (row <= 0) {
          popup = Popup::None;
          performLookup();
        } else {
          popup = Popup::None;
          // Highlight = save clipping for selection
          mode_ = Mode::Clip;
          if (startMarkIdx < 0) startMarkIdx = selected;
          performClipConfirm();
        }
      } else {
        // Outside → close without lookup
        finish();
      }
      return;
    }
    return;
  }

  using Button = MappedInputManager::Button;
  const bool selectDown = mappedInput.isPressed(Button::Confirm);
  const bool selectReleased = mappedInput.wasReleased(Button::Confirm);

  if (ignoreConfirmUntilReleased) {
    if (!selectDown) {
      ignoreConfirmUntilReleased = false;
      confirmDown = false;
      multiSelectArmedThisHold = false;
    }
    if (mappedInput.wasReleased(Button::Back)) {
      finish();
    }
    return;
  }

  if (selectDown && !confirmDown) {
    confirmDown = true;
    confirmDownAtMs = millis();
    multiSelectArmedThisHold = false;
  }
  // Dictionary only: long-press Select arms multi-word range.
  // Clip mode uses two short Selects (start mark → confirm end), like classic.
  if (mode_ == Mode::Dictionary && confirmDown && selectDown && !multiSelectArmedThisHold && startMarkIdx < 0 &&
      !words.empty() && (millis() - confirmDownAtMs) >= MULTI_SELECT_HOLD_MS) {
    startMarkIdx = selected;
    multiSelectArmedThisHold = true;
    snapshotIdx = -1;  // force full repaint for range highlight
    requestUpdate();
  }

  if (selectReleased) {
    const bool armedMulti = multiSelectArmedThisHold;
    confirmDown = false;
    multiSelectArmedThisHold = false;
    if (armedMulti) {
      // Release after arming range — user extends with Left/Right next.
      return;
    }
    if (mode_ == Mode::Dictionary && mappedInput.hasTouch()) {
      showActionMenu();
    } else {
      handleSelectAction();
    }
    return;
  }

  if (mappedInput.wasReleased(Button::Back)) {
    if (startMarkIdx >= 0) {
      startMarkIdx = -1;
      confirmDown = false;
      multiSelectArmedThisHold = false;
      snapshotIdx = -1;
      requestUpdate();
      return;
    }
    finish();
    return;
  }

  if (words.empty()) return;

  // Page-turn intent while the tool is open (side keys or swipe): exit so the
  // next gesture turns pages. Do not steal long-press+drag selection (only when
  // not mid-touch-select).
  if (!touchSelectActive) {
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Right ||
        mappedInput.wasPressed(Button::PageBack) || mappedInput.wasPressed(Button::PageForward)) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
      return;
    }
  }

  // Touch-first selection: long-press open keeps hold; drag extends; release → menu.
  // Fresh short taps on a word also open the menu (button-less Pro path).
  if (mappedInput.hasTouch()) {
    int tx = 0;
    int ty = 0;
    if (touchSelectActive) {
      if (mappedInput.isScreenTouchHeld(tx, ty)) {
        applyTouchSelectionAt(tx, ty, /*isDownEdge=*/false);
        return;
      }
      (void)mappedInput.wasScreenTapped(tx, ty);
      endTouchSelection();
      return;
    }
    // Do not arm selection on ordinary touch-down — that steals page-turn swipes.
    // Only continue a contact that was already active (long-press open path), or
    // a completed tap on a word (explicit pick).
    if (mappedInput.wasScreenTapped(tx, ty)) {
      const int hit = wordAt(tx, ty);
      if (hit >= 0) {
        selected = hit;
        if (mode_ == Mode::Dictionary) {
          startMarkIdx = -1;
          showActionMenu();
        } else {
          handleSelectAction();
        }
      } else {
        // Tap empty space → leave selection tool
        finish();
      }
      return;
    }
  }

  // Button::Up/Down/Left/Right are logical screen directions (MappedInputManager
  // applies Orient Front Buttons for Portrait 180° / Landscape CCW), matching
  // the captions from mapLabels().
  if (mappedInput.wasPressed(Button::Left) && selected > 0) {
    selected--;
    requestUpdate();
  } else if (mappedInput.wasPressed(Button::Right) && selected + 1 < static_cast<int>(words.size())) {
    selected++;
    requestUpdate();
  } else if (mappedInput.wasPressed(Button::Up)) {
    moveVertical(-1);
  } else if (mappedInput.wasPressed(Button::Down)) {
    moveVertical(1);
  }
}

void DictionaryWordSelectActivity::highlightBoxFor(const WordBox& word, int& hx, int& hy, int& hw, int& hh) const {
  // Horizontal pad only. Fill must cover every white drawText pixel or ink
  // outside the black rect is painted white (vanishes — bad in dark mode).
  const int padX = std::max(1, std::min(2, lineHeight / 16));
  hx = word.x - padX;
  hw = word.width + padX * 2;

  // One layout line: compressed pitch, or exact gap to the next row.
  int cellH = lineHeight;
  int nextRowY = INT_MAX;
  for (const auto& w : words) {
    if (w.row == static_cast<uint16_t>(word.row + 1) && w.y < nextRowY) {
      nextRowY = w.y;
    }
  }
  if (nextRowY != INT_MAX && nextRowY > word.y) {
    cellH = nextRowY - word.y;
  }

  // drawText: baseline = word.y + ascender. Bottom of the band needs to clear
  // descenders (g/y/p). Top of the line box is usually empty above Latin caps
  // (ascender covers accents / max extent) — pull the top down so the bar looks
  // centered on the word (still enough black above caps / accents).
  const int asc = std::max(1, renderer.getFontAscenderSize(fontId));
  const int minInk = asc + std::max(2, asc / 6);
  const int bottom = word.y + std::max(cellH, minInk);
  // ~30% of ascender is typical empty headroom above body caps.
  const int topInset = std::max(2, (asc * 3) / 10);
  hy = word.y + topInset;
  hh = bottom - hy;
  if (hh < 6) {
    hy = word.y;
    hh = bottom - hy;
  }

  // Landscape: front-button chrome sits on a logical side. Clamp so the black
  // bar does not run under the hint strip (looked like the menu overlapping).
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, /*hasFrontButtonHints=*/true,
                                                             /*hasSideButtonHints=*/false);
  if (hx < safe.x) {
    hw -= (safe.x - hx);
    hx = safe.x;
  }
  if (hy < safe.y) {
    hh -= (safe.y - hy);
    hy = safe.y;
  }
  const int safeRight = safe.x + safe.width;
  const int safeBottom = safe.y + safe.height;
  if (hx + hw > safeRight) hw = safeRight - hx;
  if (hy + hh > safeBottom) hh = safeBottom - hy;
  if (hw < 0) hw = 0;
  if (hh < 0) hh = 0;
}

void DictionaryWordSelectActivity::paintWordHighlight(const WordBox& word) const {
  if (!word.text) return;
  int hx = 0, hy = 0, hw = 0, hh = 0;
  highlightBoxFor(word, hx, hy, hw, hh);
  if (hw <= 0 || hh <= 0) return;
  // Fill first so every white glyph pixel lands on black (no erased halves).
  renderer.fillRect(hx, hy, hw, hh, true);
  renderer.drawText(fontId, word.x, word.y, word.text, false, word.style);
}

void DictionaryWordSelectActivity::drawSelectionHighlights() {
  int lo = 0;
  int hi = 0;
  selectionBounds(lo, hi);
  for (int i = lo; i <= hi; ++i) {
    paintWordHighlight(words[static_cast<size_t>(i)]);
  }
}

// Saves the pixels under words[selected]'s highlight box, then draws the
// highlight over them. Returns false when the pixels could not be saved
// (no buffer / oversize box / multi-word range) — the highlight is drawn
// regardless, but the next cursor move must do a full repaint.
bool DictionaryWordSelectActivity::drawHighlightWithSnapshot() {
  if (startMarkIdx >= 0) {
    drawSelectionHighlights();
    snapshotIdx = -1;
    return false;
  }

  const WordBox& word = words[selected];
  int hx = 0, hy = 0, hw = 0, hh = 0;
  highlightBoxFor(word, hx, hy, hw, hh);

  bool saved = false;
  if (snapshot && hw > 0 && hh > 0) {
    saved = renderer.readFramebufferRegion(hx, hy, hw, hh, snapshot.get(), SNAPSHOT_CAPACITY) > 0;
  }
  snapshotX = static_cast<int16_t>(hx);
  snapshotY = static_cast<int16_t>(hy);
  snapshotW = static_cast<int16_t>(hw);
  snapshotH = static_cast<int16_t>(hh);
  snapshotIdx = saved ? selected : -1;

  paintWordHighlight(word);
  return saved;
}

// Front-button pills only — no full-width white strip (that blanked the last
// lines of page text). Page layout now reserves bottom chrome so words end
// above this zone; drawButtonHints paints white only inside each pill.
void DictionaryWordSelectActivity::drawHints() const {
  // Touch devices: no soft L/R/Select chrome — press/drag/release is the UI.
  if (mappedInput.hasTouch() && !gpio.needsOnScreenFrontChrome()) {
    return;
  }
  if (words.empty()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  // previous/next args label UP/DOWN functions after remap. LEFT/RIGHT functions
  // still get hardcoded Left/Right from mapLabels (word-step actions).
  // Passing Left/Right here made remapped Up/Down buttons read "Left"/"Right".
  // Clip mode mirrors classic: Select (mark start) then Done (confirm end).
  const char* confirmLabel =
      (mode_ == Mode::Clip && startMarkIdx >= 0) ? tr(STR_DONE) : tr(STR_SELECT);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// Reader top chrome (battery/clock) is gone in this activity — use that band
// for a quiet mode title so users know they left the reader.
void DictionaryWordSelectActivity::drawModeTitle() const {
  const char* title;
  if (mode_ == Mode::Clip) {
    title = (startMarkIdx >= 0) ? tr(STR_MULTI_WORD_SELECTION) : tr(STR_CLIPPING_TOOL);
  } else {
    title = (startMarkIdx >= 0) ? tr(STR_MULTI_WORD_SELECTION) : tr(STR_DICTIONARY_LOOKUP);
  }
  // UI_10 bold: a step up from SMALL_FONT so the mode label is easy to spot
  // without colliding with body text (page still has top chrome headroom).
  constexpr int kTitleFont = UI_10_FONT_ID;
  const int lineH = renderer.getLineHeight(kTitleFont);
  const int titleY = std::max(2, (marginTop - lineH) / 2);
  // Light wipe so page ink under the title band does not show through.
  renderer.fillRect(0, 0, renderer.getScreenWidth(), std::max(lineH + titleY + 2, marginTop - 2), false);
  renderer.drawCenteredText(kTitleFont, titleY, title, true, EpdFontFamily::BOLD);
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  // Differential fast path (single-word only): restore old highlight pixels,
  // paint the new cursor, skip full page re-render.
  if (popup == Popup::None && startMarkIdx < 0 && snapshotIdx >= 0 && !words.empty() && selected != snapshotIdx) {
    renderer.writeFramebufferRegion(snapshotX, snapshotY, snapshotW, snapshotH, snapshot.get());
    // The full path's PrewarmScope cleared the glyph cache on exit; batch-load
    // just the highlighted word's glyphs before redrawing white-on-black.
    renderer.getFontCacheManager()->prewarmCache(
        fontId, words[selected].text, static_cast<uint8_t>(1u << (static_cast<uint8_t>(words[selected].style) & 0x03)));
    if (drawHighlightWithSnapshot()) {
      drawModeTitle();
      drawHints();
      ReaderUtils::displayWithDarkMode(renderer, HalDisplay::FAST_REFRESH);
      return;
    }
    // Snapshot failed (oversize box) — fall through to a full repaint.
  }

  renderer.clearScreen(0xFF);

  if (pageFb_ && pageFbBytes_ > 0 && renderer.getFrameBuffer() && pageFbBytes_ <= renderer.getBufferSize()) {
    // Rivulet: restore the page snapshot captured when the tool opened.
    std::memcpy(renderer.getFrameBuffer(), pageFb_.get(), pageFbBytes_);
  } else if (page) {
    // Same prewarm-scan-then-render pass the reader uses, so SD-card fonts hit
    // the in-RAM glyph cache during the real draw.
    auto* fcm = renderer.getFontCacheManager();
    auto scope = fcm->createPrewarmScope();
    page->render(renderer, fontId, marginLeft, marginTop);
    scope.endScanAndPrewarm();
    page->render(renderer, fontId, marginLeft, marginTop);
  } else if (!words.empty()) {
    // No snapshot (OOM) and no classic Page: redraw selectable tokens as body
    // ink so the tool is still usable instead of a blank plate.
    auto* fcm = renderer.getFontCacheManager();
    if (fcm) {
      std::string warm;
      warm.reserve(words.size() * 8);
      uint8_t styleMask = 0;
      for (const auto& w : words) {
        if (!w.text) continue;
        warm += w.text;
        warm += ' ';
        styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(w.style) & 0x03));
      }
      if (styleMask == 0) styleMask = 0x01;
      fcm->prewarmCache(fontId, warm.c_str(), styleMask);
    }
    for (const auto& w : words) {
      if (!w.text) continue;
      renderer.drawText(fontId, w.x, w.y, w.text, true, w.style);
    }
  }

  if (!words.empty()) {
    drawHighlightWithSnapshot();
  }

  // No soft Back/Done chrome on touch — tap outside / action menu is enough.
  if (!(mappedInput.hasTouch() && !gpio.needsOnScreenFrontChrome())) {
    drawModeTitle();
    drawHints();
  }

  if (popup == Popup::Busy) {
    snapshotIdx = -1;
    GUI.drawTopLeftStatus(renderer, I18N.get(popupMsg), /*refresh=*/false);
    ReaderUtils::displayWithDarkMode(renderer, HalDisplay::FAST_REFRESH);
    return;
  }
  if (popup == Popup::ActionMenu) {
    drawActionMenu();
    ReaderUtils::displayWithDarkMode(renderer, HalDisplay::FAST_REFRESH);
    return;
  }
  if (popup != Popup::None) {
    snapshotIdx = -1;
    GUI.drawPopup(renderer, I18N.get(popupMsg));
    ReaderUtils::displayWithDarkMode(renderer, HalDisplay::FAST_REFRESH);
    return;
  }
  ReaderUtils::displayWithDarkMode(renderer, HalDisplay::FAST_REFRESH);
}
