#pragma once

#include <cstdint>
#include <string>
#include <array>
#include <algorithm>
#include <utility>

class GfxRenderer;
class SdCardFont;
struct SdCardFontFamilyInfo;
struct SdCardFontFileInfo;

class SdCardFontManager {
 public:
  SdCardFontManager() = default;
  ~SdCardFontManager();
  SdCardFontManager(const SdCardFontManager&) = delete;
  SdCardFontManager& operator=(const SdCardFontManager&) = delete;

  // Load the font file whose physical point size is closest to the reader
  // fontSizeEnum (0..5 = 8/10/12/14/16/18 pt). Only one .cpfont file is loaded
  // for body text; other sizes remain on disk (ladder hook may load extras).
  // Returns true on success.
  bool loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t fontSizeEnum);

  // Additively load the .cpfont of `family` at the exact physical `pointSize`
  // (used for size-matched CJK UI fallback alongside the reader-size font).
  // Does not unload anything. If a font of that size is already loaded its id
  // is reused. Returns the font id, or 0 if the family has no file at that size
  // or loading failed.
  int loadFamilyExtraSize(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t pointSize);

  // Unload everything, unregister from renderer.
  void unloadAll(GfxRenderer& renderer);

  // Look up the font ID for the loaded family. Returns 0 if nothing loaded
  // or familyName doesn't match.
  int getFontId(const std::string& familyName) const;

  // True if `fontId` is one of the currently loaded SD faces (reader or extra size).
  bool ownsFontId(int fontId) const;

  // Fill sizeStep 0..4 ladder relative to `baseFontId` using available .cpfont
  // files near the base point size (reader ladder: 10/12/14/16/18). Loads missing
  // sizes via loadFamilyExtraSize. Returns true if at least two distinct sizes
  // were filled (multi-size family); false → StyleResolve collapses to one face.
  bool fillRelativeLadder(int baseFontId, const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                          int outFontIdByStep[5]);

  // Reuse/load a size without unloading the old face; used by live appearance.
  bool selectReaderSize(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t fontSizeEnum);
  bool selectLoadedPointSize(uint8_t pointSize);
  void swap(SdCardFontManager& other) noexcept {
    loaded_.swap(other.loaded_);
    loadedFamilyName_.swap(other.loadedFamilyName_);
    std::swap(loadedPointSize_, other.loadedPointSize_);
  }

  // Get name of currently loaded family (empty if none).
  const std::string& currentFamilyName() const { return loadedFamilyName_; };

  // Point size that was actually loaded.
  // 0 if nothing loaded.
  uint8_t currentPointSize() const { return loadedPointSize_; };

 private:
  struct LoadedFont {
    SdCardFont* font;  // heap-allocated, owned
    int fontId;
    uint8_t size;
  };
  static int computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize);

  // Load+register a single .cpfont file and append it to loaded_.
  // Returns the font id, or 0 on failure (allocation, read, or id collision).
  int loadFile(const SdCardFontFileInfo& file, const char* familyName, GfxRenderer& renderer);

  std::string loadedFamilyName_;
  uint8_t loadedPointSize_ = 0;
  struct FontSlots {
    static constexpr size_t capacity = 12;  // six reader rungs + UI/CJK fallback sizes
    std::array<LoadedFont, capacity> slots{};
    size_t count = 0;
    LoadedFont* begin() { return slots.data(); }
    const LoadedFont* begin() const { return slots.data(); }
    LoadedFont* end() { return slots.data() + count; }
    const LoadedFont* end() const { return slots.data() + count; }
    bool empty() const { return count == 0; }
    size_t size() const { return count; }
    LoadedFont& front() { return slots[0]; }
    const LoadedFont& front() const { return slots[0]; }
    bool push_back(LoadedFont f) { if (count == capacity) return false; slots[count++] = f; return true; }
    void clear() { count = 0; }
    void swap(FontSlots& other) noexcept { slots.swap(other.slots); std::swap(count, other.count); }
  } loaded_;
};
