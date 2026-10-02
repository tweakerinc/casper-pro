#pragma once
#include <EpdFontData.h>
class EpdFontFamily {
 public:
  enum Style : uint8_t { REGULAR=0,BOLD=1,ITALIC=2,BOLD_ITALIC=3,UNDERLINE=4,
    STRIKETHROUGH=8,SUP=16,SUB=32,DROP_CAP=64,TEXT_DECORATION_MASK=12 };
  const EpdGlyph* getGlyph(uint32_t, Style) const { return nullptr; }
};
