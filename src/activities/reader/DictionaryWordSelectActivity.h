#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <vector>

#include "activities/Activity.h"

// Word selection over the current reader page: Left/Right step through words
// in reading order, Up/Down jump rows (logical directions — Landscape CCW /
// Portrait 180° with Orient Front Buttons follow MappedInputManager).
//
// Dictionary mode: short Select looks up the word (or multi-word range).
// Long-press Select starts a multi-word range; move, then short Select to look
// up the phrase. Back clears a range or returns to the reader.
//
// Clip mode (classic Clipping Tool): short Select marks the start word; move;
// short Select again confirms the end and returns ClippingResult. Back clears
// a mark or cancels. No 6-word phrase cap — range can cover the whole page.
//
// Touch (primary on X4 Pro / KOReader-like):
//   Long-press a word on the reader → open here with that word pre-selected.
//   Keep holding and drag → multi-word range (anchor…finger).
//   Finger up → small action menu: Dictionary Lookup | Highlight (clip).
//   Tap outside → dismiss without looking up (saves power).
// Clip-only mode (menu shortcut): same select, release confirms clip.
// Button path (no initial touch): start mid-page; L/R step; Select looks up.
class DictionaryWordSelectActivity final : public Activity {
 public:
  enum class Mode : uint8_t { Dictionary, Clip };

  // Screen box of one selectable word. `text` points into Page arena or textPool_.
  struct WordBox {
    int16_t x;
    int16_t y;
    int16_t width;
    uint16_t row;
    const char* text;
    EpdFontFamily::Style style;
  };

  // Classic path: words extracted from a Section Page.
  // initialTouchX/Y >= 0: seed selection from long-press (logical coords).
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int marginLeft, int marginTop,
                                        Mode mode = Mode::Dictionary, int initialTouchX = -1, int initialTouchY = -1)
      : Activity(mode == Mode::Clip ? "ClipWordSelect" : "DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop),
        mode_(mode),
        initialTouchX_(initialTouchX),
        initialTouchY_(initialTouchY) {}

  // Rivulet path: prebuilt word boxes + full-framebuffer snapshot of the page
  // (no Section/Page). Coordinates are already screen-absolute.
  DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::vector<WordBox> prebuilt,
                               std::vector<std::string> textPool, int fontId, std::unique_ptr<uint8_t[]> pageFb,
                               size_t pageFbBytes, int marginLeft, int marginTop, Mode mode = Mode::Dictionary,
                               int initialTouchX = -1, int initialTouchY = -1)
      : Activity(mode == Mode::Clip ? "ClipWordSelect" : "DictionaryWordSelect", renderer, mappedInput),
        page(nullptr),
        marginLeft(marginLeft),
        marginTop(marginTop),
        fontId(fontId),
        mode_(mode),
        words(std::move(prebuilt)),
        textPool_(std::move(textPool)),
        pageFb_(std::move(pageFb)),
        pageFbBytes_(pageFbBytes),
        initialTouchX_(initialTouchX),
        initialTouchY_(initialTouchY) {
    // Re-point text pointers into the owned pool (vector reallocation already done).
    for (size_t i = 0; i < words.size() && i < textPool_.size(); ++i) {
      words[i].text = textPool_[i].c_str();
    }
    uint16_t maxRow = 0;
    for (const auto& w : words) {
      if (w.row > maxRow) maxRow = w.row;
    }
    rowCount = words.empty() ? 0 : static_cast<uint16_t>(maxRow + 1);
  }

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Home pad: same as Back (clear multi-select step, then leave tool → reader).
  bool handleHomeGesture() override;

 private:
  static constexpr int kMaxPhraseWords = 6;

  enum class Popup : uint8_t { None, Busy, NotFound, Error, ActionMenu };

  void extractWords();
  int closestInRow(uint16_t row, int centerX) const;
  int wordAt(int x, int y) const;
  // Hit-test with larger slop, then nearest word within a finger-radius (drag).
  int wordAtOrNearest(int x, int y) const;
  void moveVertical(int direction);
  // Apply finger position while a touch selection is active (down / drag).
  void applyTouchSelectionAt(int x, int y, bool isDownEdge);
  void endTouchSelection();
  // Inclusive range bounds for current selection (single word or multi-word).
  void selectionBounds(int& lo, int& hi) const;
  // Join soft-/line-break hyphens; multi-word joins with spaces.
  std::string buildLookupToken() const;
  void performLookup();
  // Clip mode: first Select marks start; second Select builds ClippingResult and finishes.
  void performClipConfirm();
  void handleSelectAction();
  void showActionMenu();
  void layoutActionMenu(int& outX, int& outY, int& outW, int& outH, int& outRowH) const;
  void drawActionMenu() const;
  bool drawHighlightWithSnapshot();
  void drawSelectionHighlights();
  // Highlight box: full line cell (must cover all white redraw ink).
  void highlightBoxFor(const WordBox& word, int& hx, int& hy, int& hw, int& hh) const;
  // Solid black fill + white text (cheapest BW e-ink path).
  void paintWordHighlight(const WordBox& word) const;
  // Compact top title while reader chrome is hidden (mode cue for the tool).
  void drawModeTitle() const;
  void drawHints() const;

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  int fontId = 0;
  // Reader line pitch (advanceY × Tight/Normal/Wide), not bare font advanceY.
  int lineHeight = 0;
  Mode mode_ = Mode::Dictionary;

  std::vector<WordBox> words;
  std::vector<std::string> textPool_;  // owns strings for Rivulet/prebuilt mode
  std::unique_ptr<uint8_t[]> pageFb_;  // full page snapshot for Rivulet mode
  size_t pageFbBytes_ = 0;
  int selected = 0;
  // -1 = single-word mode; otherwise multi-word range anchor index.
  int startMarkIdx = -1;
  uint16_t rowCount = 0;

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;

  // Differential highlight repaint (single-word mode only).
  static constexpr size_t SNAPSHOT_CAPACITY = 4096;
  std::unique_ptr<uint8_t[]> snapshot;
  int16_t snapshotX = 0;
  int16_t snapshotY = 0;
  int16_t snapshotW = 0;
  int16_t snapshotH = 0;
  int snapshotIdx = -1;

  // Confirm/Select state machine (do not use global getHeldTime — it remembers
  // prior nav holds). Ignore residual press that opened dictionary.
  bool ignoreConfirmUntilReleased = true;
  bool confirmDown = false;
  unsigned long confirmDownAtMs = 0;
  bool multiSelectArmedThisHold = false;

  // Touch drag selection (finger still down after a hit on a word).
  bool touchSelectActive = false;
  int touchAnchor = -1;
  bool touchDragged = false;  // left the anchor word at least once this contact
  // Finger path after long-press open — used to reject accidental swipe-as-select.
  int touchStartX_ = -1;
  int touchStartY_ = -1;
  int touchMaxAbsDx_ = 0;
  int touchMaxAbsDy_ = 0;
  unsigned long touchSelectStartedMs_ = 0;

  // Long-press open from reader (logical coords). -1 = menu/button open → mid-page seed.
  int initialTouchX_ = -1;
  int initialTouchY_ = -1;
};
