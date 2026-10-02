#pragma once
#include <algorithm>

// One allocation-free geometry shared by rendering and hit-testing. All
// coordinates are logical (already transformed for the current orientation).
struct BookAppearanceGeometry {
  int width, height, top, header, tabs, rowsTop, rowHeight, footer, rows;
  static BookAppearanceGeometry make(int width, int height) {
    const int h = std::min(height, height >= 600 ? 350 : 264);
    const int top = height - h;
    const int header = 46, tabs = 44, footer = 42;
    const int rowH = std::max(38, (h - header - tabs - footer) / 3);
    return {width, height, top, header, tabs, top + header + tabs, rowH,
            top + header + tabs + 3 * rowH, 3};
  }
  int rowAt(int y) const {
    return y >= rowsTop && y < footer ? (y - rowsTop) / rowHeight : -1;
  }
  int tabAt(int x, int y) const {
    return x >= 0 && x < width && y >= top + header && y < rowsTop
               ? std::min(3, x * 4 / width) : -1;
  }
};
