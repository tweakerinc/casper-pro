#pragma once

#include <cstdint>
#include <cstring>

#include "CasperSettings.h"

// Shared effective appearance snapshot. Deliberately excludes device/light
// controls. Reading or comparing it never mutates SETTINGS or writes the card.
struct BookAppearanceState {
  uint8_t family = 0, size = 0, line = 0, align = 0, margin = 0;
  uint8_t paragraphs = 0, paragraphHeight = 0, embedded = 0;
  uint8_t hyphenation = 0, aa = 0, focus = 0, guide = 0;
  char sdFamily[32]{};

  static BookAppearanceState capture() {
    BookAppearanceState s;
    s.family = SETTINGS.fontFamily;
    s.size = SETTINGS.fontSize;
    s.line = SETTINGS.lineSpacing;
    s.align = SETTINGS.paragraphAlignment;
    s.margin = SETTINGS.screenMargin;
    s.paragraphs = SETTINGS.extraParagraphSpacing;
    s.paragraphHeight = SETTINGS.extraParagraphSpacingHeight;
    s.embedded = SETTINGS.embeddedStyle;
    s.hyphenation = SETTINGS.hyphenationEnabled;
    s.aa = SETTINGS.textAntiAliasing;
    s.focus = SETTINGS.focusReadingEnabled;
    s.guide = SETTINGS.guideReadingEnabled;
    std::memcpy(s.sdFamily, SETTINGS.sdFontFamilyName, sizeof(s.sdFamily));
    s.sdFamily[sizeof(s.sdFamily) - 1] = '\0';
    return s;
  }
  void apply() const {
    SETTINGS.fontFamily = family;
    SETTINGS.fontSize = size;
    SETTINGS.lineSpacing = line;
    SETTINGS.paragraphAlignment = align;
    SETTINGS.screenMargin = margin;
    SETTINGS.extraParagraphSpacing = paragraphs;
    SETTINGS.extraParagraphSpacingHeight = paragraphHeight;
    SETTINGS.embeddedStyle = embedded;
    SETTINGS.hyphenationEnabled = hyphenation;
    SETTINGS.textAntiAliasing = aa;
    SETTINGS.focusReadingEnabled = focus;
    SETTINGS.guideReadingEnabled = guide;
    std::memcpy(SETTINGS.sdFontFamilyName, sdFamily, sizeof(sdFamily));
  }
  bool operator==(const BookAppearanceState& o) const {
    return family == o.family && size == o.size && line == o.line && align == o.align && margin == o.margin &&
           paragraphs == o.paragraphs && paragraphHeight == o.paragraphHeight && embedded == o.embedded &&
           hyphenation == o.hyphenation && aa == o.aa && focus == o.focus && guide == o.guide &&
           std::strcmp(sdFamily, o.sdFamily) == 0;
  }
  bool operator!=(const BookAppearanceState& o) const { return !(*this == o); }
};
