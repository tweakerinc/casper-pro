#pragma once

#include <cctype>
#include <cstring>

// SD names that must stay out of the library / web file browsers.
// XTCache is stock Xteink OEM cache (not Casper). Casper uses /.crosspoint only.
// Leading-dot names are hidden separately via showHiddenFiles.
namespace SdHiddenNames {

inline bool equalsIgnoreCase(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    const unsigned char ca = static_cast<unsigned char>(*a++);
    const unsigned char cb = static_cast<unsigned char>(*b++);
    if (std::tolower(ca) != std::tolower(cb)) return false;
  }
  return *a == *b;
}

// Always hide (even when "Show hidden files" is on).
inline bool isAlwaysHidden(const char* name) {
  if (!name || !*name) return false;
  if (equalsIgnoreCase(name, "System Volume Information")) return true;
  // Stock Xteink: cover/thumb cache from OEM firmware. Not used by Casper.
  if (equalsIgnoreCase(name, "XTCache")) return true;
  if (equalsIgnoreCase(name, "xtcache")) return true;
  return false;
}

// WebDAV / web UI list (explicit names; dots handled by callers).
inline constexpr const char* kWebHidden[] = {"System Volume Information", "XTCache", "xtcache"};
inline constexpr size_t kWebHiddenCount = sizeof(kWebHidden) / sizeof(kWebHidden[0]);

}  // namespace SdHiddenNames
