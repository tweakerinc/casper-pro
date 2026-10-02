#include "ReleaseJsonParser.h"

#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

void safeCopy(char* dst, size_t dstSize, const char* src, size_t srcLen) {
  size_t n = srcLen < dstSize - 1 ? srcLen : dstSize - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

// Casper release assets are named Casper-v0.1.0 or Casper-v0.1.0.bin.
// Also accept plain firmware.bin (SD / upstream tooling).
bool isFirmwareAssetName(const char* name, const ReleaseJsonParser::FirmwareTarget target) {
  if (name == nullptr || name[0] == '\0') {
    return false;
  }
  if (target == ReleaseJsonParser::FirmwareTarget::X4Pro) {
    // An ESP32-S3 image is not necessarily for this board (e.g. Sticky).
    // Require a board-specific *application* asset, never a generic/merged bin.
    char lower[96];
    const size_t len = std::strlen(name);
    if (len >= sizeof(lower)) return false;
    for (size_t i = 0; i <= len; ++i) {
      const char c = name[i];
      lower[i] = c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
    }
    if (std::strcmp(lower, "firmware-x4pro.bin") == 0 ||
        std::strcmp(lower, "casper-pro-x4pro.bin") == 0) return true;
    constexpr char prefix[] = "casper-pro-v";
    constexpr char suffix[] = "-x4pro.bin";
    constexpr size_t pn = sizeof(prefix) - 1, sn = sizeof(suffix) - 1;
    if (len <= pn + sn || std::strncmp(lower, prefix, pn) != 0 ||
        std::strcmp(lower + len - sn, suffix) != 0) return false;
    if (lower[pn] < '0' || lower[pn] > '9') return false;
    for (size_t i = pn; i < len - sn; ++i) {
      const char c = lower[i];
      if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'z') && c != '.' && c != '-') return false;
    }
    return true;
  }
  if (strcmp(name, "firmware.bin") == 0) {
    return true;
  }

  // Case-insensitive "Casper-" prefix (7 chars).
  static constexpr char kPrefix[] = "Casper-";
  static constexpr size_t kPrefixLen = 7;
  for (size_t i = 0; i < kPrefixLen; ++i) {
    const char a = name[i];
    const char b = kPrefix[i];
    if (a == '\0') {
      return false;
    }
    const char al = static_cast<char>(a >= 'A' && a <= 'Z' ? a - 'A' + 'a' : a);
    const char bl = static_cast<char>(b >= 'A' && b <= 'Z' ? b - 'A' + 'a' : b);
    if (al != bl) {
      return false;
    }
  }

  const char* rest = name + kPrefixLen;
  // Require a version-looking rest: v0.1.0… or 0.1.0…
  if (!(rest[0] == 'v' || rest[0] == 'V' || (rest[0] >= '0' && rest[0] <= '9'))) {
    return false;
  }

  // Allow no extension or .bin only (reject .zip / .md / source tarballs).
  // Note: names like Casper-v0.1.0 contain dots that are part of the version,
  // not a file extension — only treat a trailing .ext as an extension when the
  // suffix is not purely numeric.
  const char* dot = strrchr(name, '.');
  if (dot == nullptr) {
    return true;
  }
  if (dot == name || dot[1] == '\0') {
    return false;
  }
  // ".bin" case-insensitive
  if ((dot[1] == 'b' || dot[1] == 'B') && (dot[2] == 'i' || dot[2] == 'I') && (dot[3] == 'n' || dot[3] == 'N') &&
      dot[4] == '\0') {
    return true;
  }
  // Pure digit suffix (".0", ".1", ".18") → still a version segment, not an extension.
  bool allDigits = true;
  for (const char* p = dot + 1; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') {
      allDigits = false;
      break;
    }
  }
  return allDigits;
}

}  // namespace

ReleaseJsonParser::ReleaseJsonParser(const FirmwareTarget target)
    : target_(target), parser(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                           sOnArrayStart, sOnArrayEnd}) {
  reset();
}

void ReleaseJsonParser::reset() {
  parser.reset();
  position = Position::TOP_LEVEL;
  lastKey = LastKey::NONE;
  depth = 0;
  assetDepth = 0;
  tagName[0] = '\0';
  firmwareUrl[0] = '\0';
  firmwareSize = 0;
  tagFound = false;
  firmwareFound = false;
  currentAssetName[0] = '\0';
  currentAssetUrl[0] = '\0';
  currentAssetSize = 0;
  currentAssetInvalid = false;
}

void ReleaseJsonParser::feed(const char* data, size_t len) { parser.feed(data, len); }

bool ReleaseJsonParser::foundTag() const { return tagFound; }
bool ReleaseJsonParser::foundFirmware() const { return firmwareFound; }
const char* ReleaseJsonParser::getTagName() const { return tagName; }
const char* ReleaseJsonParser::getFirmwareUrl() const { return firmwareUrl; }
size_t ReleaseJsonParser::getFirmwareSize() const { return firmwareSize; }

void ReleaseJsonParser::commitAsset() {
  if (!firmwareFound && !currentAssetInvalid && currentAssetSize > 0 &&
      std::strncmp(currentAssetUrl, "https://", 8) == 0 &&
      isFirmwareAssetName(currentAssetName, target_)) {
    memcpy(firmwareUrl, currentAssetUrl, sizeof(firmwareUrl));
    firmwareSize = currentAssetSize;
    firmwareFound = true;
  }
  currentAssetName[0] = '\0';
  currentAssetUrl[0] = '\0';
  currentAssetSize = 0;
  currentAssetInvalid = false;
}

// -- SAX callbacks (static trampolines) -------------------------------------

void ReleaseJsonParser::sOnKey(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth == 1) {
        if (len == 8 && memcmp(key, "tag_name", 8) == 0)
          self->lastKey = LastKey::TAG_NAME;
        else if (len == 6 && memcmp(key, "assets", 6) == 0)
          self->lastKey = LastKey::ASSETS;
        else
          self->lastKey = LastKey::NONE;
      }
      break;
    case Position::IN_ASSET_OBJECT:
      if (self->assetDepth == 1) {
        if (len == 4 && memcmp(key, "name", 4) == 0)
          self->lastKey = LastKey::ASSET_NAME;
        else if (len == 20 && memcmp(key, "browser_download_url", 20) == 0)
          self->lastKey = LastKey::ASSET_URL;
        else if (len == 4 && memcmp(key, "size", 4) == 0)
          self->lastKey = LastKey::ASSET_SIZE;
        else
          self->lastKey = LastKey::NONE;
      }
      break;
    default:
      break;
  }
}

void ReleaseJsonParser::sOnString(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->lastKey) {
    case LastKey::TAG_NAME:
      if (self->position == Position::TOP_LEVEL && self->depth == 1) {
        self->tagFound = len > 0 && len < sizeof(self->tagName);
        if (self->tagFound) safeCopy(self->tagName, sizeof(self->tagName), value, len);
      }
      break;
    case LastKey::ASSET_NAME:
      if (self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1) {
        if (len >= sizeof(self->currentAssetName)) self->currentAssetInvalid = true;
        else safeCopy(self->currentAssetName, sizeof(self->currentAssetName), value, len);
      }
      break;
    case LastKey::ASSET_URL:
      if (self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1) {
        // A full SAX token may already have been truncated by the generic
        // parser. Refuse it rather than downloading from a partial URL.
        if (len >= sizeof(self->currentAssetUrl) - 1) self->currentAssetInvalid = true;
        else safeCopy(self->currentAssetUrl, sizeof(self->currentAssetUrl), value, len);
      }
      break;
    default:
      break;
  }
  self->lastKey = LastKey::NONE;
}

void ReleaseJsonParser::sOnNumber(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  if (self->lastKey == LastKey::ASSET_SIZE && self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1) {
    size_t n = 0;
    if (len == 0) self->currentAssetInvalid = true;
    for (size_t i = 0; i < len; ++i) {
      if (value[i] < '0' || value[i] > '9' ||
          n > (std::numeric_limits<size_t>::max() - static_cast<size_t>(value[i] - '0')) / 10) {
        self->currentAssetInvalid = true;
        return;
      }
      n = n * 10 + static_cast<size_t>(value[i] - '0');
    }
    self->currentAssetSize = n;
  }
  self->lastKey = LastKey::NONE;
}

void ReleaseJsonParser::sOnBool(void* ctx, bool /*value*/) {
  static_cast<ReleaseJsonParser*>(ctx)->lastKey = LastKey::NONE;
}

void ReleaseJsonParser::sOnNull(void* ctx) { static_cast<ReleaseJsonParser*>(ctx)->lastKey = LastKey::NONE; }

void ReleaseJsonParser::sOnObjectStart(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      self->depth++;
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_ASSETS_ARRAY:
      self->position = Position::IN_ASSET_OBJECT;
      self->assetDepth = 1;
      self->currentAssetName[0] = '\0';
      self->currentAssetUrl[0] = '\0';
      self->currentAssetSize = 0;
      self->currentAssetInvalid = false;
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_ASSET_OBJECT:
      self->assetDepth++;
      self->lastKey = LastKey::NONE;
      break;
  }
}

void ReleaseJsonParser::sOnObjectEnd(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth > 0) self->depth--;
      break;
    case Position::IN_ASSET_OBJECT:
      self->assetDepth--;
      if (self->assetDepth == 0) {
        self->commitAsset();
        self->position = Position::IN_ASSETS_ARRAY;
      }
      self->lastKey = LastKey::NONE;
      break;
    default:
      break;
  }
}

void ReleaseJsonParser::sOnArrayStart(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->lastKey == LastKey::ASSETS && self->depth == 1) {
        self->position = Position::IN_ASSETS_ARRAY;
      } else {
        self->depth++;
      }
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_ASSET_OBJECT:
      self->assetDepth++;
      self->lastKey = LastKey::NONE;
      break;
    default:
      break;
  }
}

void ReleaseJsonParser::sOnArrayEnd(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth > 0) self->depth--;
      break;
    case Position::IN_ASSETS_ARRAY:
      self->position = Position::TOP_LEVEL;
      break;
    case Position::IN_ASSET_OBJECT:
      self->assetDepth--;
      self->lastKey = LastKey::NONE;
      break;
  }
}
