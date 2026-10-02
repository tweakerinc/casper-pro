#pragma once
// Deterministic host metrics, NOT an e-paper/SD-font/hardware simulation.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <EpdFontFamily.h>
inline uint32_t fakeMillis = 0;
inline uint32_t millis() { return fakeMillis++; }
inline void yield() {}
class GfxRenderer {
 public:
  int getFontAscenderSize(int id) const { return id == -9002 ? 24 : 12; }
  int getLineHeight(int id, float lc) const { return int((getFontAscenderSize(id) + 4) * lc); }
  int getTextAdvanceX(int id, const char* s, EpdFontFamily::Style = EpdFontFamily::REGULAR, uint32_t = 0) const {
    int n=0; for (; *s; ++s) if ((static_cast<unsigned char>(*s) & 0xc0) != 0x80) ++n;
    return n * (id == -9002 ? 12 : 6);
  }
  int getSpaceWidth(int, EpdFontFamily::Style = EpdFontFamily::REGULAR) const { return 6; }
  template<class... Args> int getSpaceAdvance(Args...) const { return 6; }
  const std::map<int, EpdFontFamily>& getFontMap() const { static const std::map<int,EpdFontFamily> fonts; return fonts; }
  template<class... Args> void drawText(Args...) const {}
  template<class... Args> void drawLine(Args...) const {}
  template<class... Args> void fillRect(Args...) const {}
};
