#pragma once

// Casper product identity — firmware on CrossPoint-compatible SD layout.
// Storage root: /.crosspoint only (settings, stats, book package cache).

namespace CasperProduct {

// Product branding: Casper Pro ships on Xteink X4 Pro (S3 + touch + frontlight).
// Storage root stays /.crosspoint so libraries/settings stay portable with C3 Casper.
inline constexpr const char* kName = "Casper Pro";
inline constexpr const char* kStorageRoot = "/.crosspoint";

inline constexpr bool kHasKOReader = true;
inline constexpr bool kHasOpds = true;
inline constexpr bool kHasFontDownload = true;
inline constexpr bool kHasBootForeignMigrate = false;
inline constexpr bool kRuntimeDualReadForeign = false;
inline constexpr bool kHasLiterataBuiltin = true;
inline constexpr bool kEnglishOnly = false;

}  // namespace CasperProduct
