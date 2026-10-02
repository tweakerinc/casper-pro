#include "ChapterIr.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <Esp.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace rivulet {
namespace {

constexpr size_t kMaxTextBlob = 192 * 1024;
constexpr size_t kMaxBlocks = 4096;
constexpr size_t kMaxRuns = 32768;

// Never split a valid multibyte scalar at a 16-bit run boundary.
size_t runPrefix(const char* text, size_t remaining, size_t limit) {
  size_t n = std::min(remaining, limit);
  if (n == remaining) return n;
  while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;
  return n;
}

bool sourceOffset(const char* data, size_t size, const char* src, size_t& offset) {
  const auto p = reinterpret_cast<uintptr_t>(src);
  const auto begin = reinterpret_cast<uintptr_t>(data);
  if (!data || p < begin || p - begin > size) return false;
  offset = static_cast<size_t>(p - begin);
  return true;
}

}  // namespace

void ChapterIr::freeText() {
  if (textData_) {
    std::free(textData_);
    textData_ = nullptr;
  }
  textLen_ = 0;
  textCap_ = 0;
}

void ChapterIr::clear() {
  openBlock_ = false;
  failed_ = false;
  blocks_.release();
  runs_.release();
  freeText();
}

bool ChapterIr::ensureTextCapacity(const size_t needExtra) {
  if (needExtra > kMaxTextBlob - textLen_) return false;
  const size_t need = textLen_ + needExtra;
  if (need <= textCap_) return true;
  if (need > kMaxTextBlob) return false;
  // Grow by ~2x (min +4KB) so convert does few reallocs. Prefer realloc (may grow
  // in place) over malloc+copy+free — that path fragmented maxAlloc mid-chapter
  // and produced partial IR ("last page" stopped at the totem line).
  size_t newCap = textCap_ ? textCap_ : 4096;
  while (newCap < need) {
    if (newCap > kMaxTextBlob / 2) {
      newCap = need;
      break;
    }
    const size_t grown = newCap + std::max<size_t>(4096, newCap);
    newCap = std::min(kMaxTextBlob, grown);
  }
  if (newCap < need) return false;
  // Prefer realloc (may grow in place). Avoid malloc+copy+free thrash that
  // fragmented maxAlloc mid-convert and truncated chapters.
  char* p = static_cast<char*>(resizeReaderStorage(textData_, newCap + 4));
  if (!p) {
    LOG_ERR("RVIR", "OOM text realloc need=%u cap=%u free=%u maxA=%u", static_cast<unsigned>(need),
            static_cast<unsigned>(newCap), static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return false;
  }
  textData_ = p;
  textCap_ = newCap;
  return true;
}

bool ChapterIr::ensureRunsCapacity(const size_t needExtra) {
  if (needExtra > kMaxRuns - runs_.size()) return false;
  const size_t need = runs_.size() + needExtra;
  if (need <= runs_.capacity()) return true;
  if (need > kMaxRuns) return false;
  size_t newCap = runs_.capacity() ? runs_.capacity() : 64;
  while (newCap < need) {
    newCap = std::min(kMaxRuns, newCap < 256 ? newCap + 64 : newCap * 2);
  }
  if (newCap < need) return false;
  return runs_.reserve(newCap);
}

void ChapterIr::reserveForConvert(const size_t htmlLen) {
  // One-shot pre-size under the caller's FB loan so mid-convert does not thrash
  // realloc and fragment maxAlloc (partial IR → false chapter end).
  // Prose XHTML is typically ~40–60% text; leave room for HTML still in RAM + vectors.
  const size_t maxA = ESP.getMaxAllocHeap();
  if (maxA < 12 * 1024) return;

  size_t textGuess = htmlLen > 0 ? (std::min(htmlLen, kMaxTextBlob) * 3 / 5) + 1024 : 4096;
  if (textGuess > kMaxTextBlob) textGuess = kMaxTextBlob;
  // Cap text pre-size to ~half of max contiguous so the runs/blocks vectors still
  // fit afterwards.
  //
  // Taking the full guess here was tried and measured worse: the same chapter went
  // from `partial=0 text=23006 blocks=158` to `partial=1 text=19826 blocks=120`.
  // Grabbing the whole estimate up front leaves too little contiguous heap for the
  // vector growth that follows, so the convert OOMs later instead of earlier. (That
  // attempt was chasing a misdiagnosis anyway — the real fault was PageLayouter
  // reporting failure for measure-only pages, fixed separately.)
  const size_t textCap = std::min(textGuess, maxA / 2);
  if (textCap >= 2048 && (!textData_ || textCap_ < textCap)) {
    char* p = static_cast<char*>(resizeReaderStorage(textData_, textCap + 4));
    if (p) {
      textData_ = p;
      textCap_ = textCap;
    }
  }

  const size_t blockGuess = std::min(kMaxBlocks, std::max<size_t>(48, htmlLen / 180 + 16));
  const size_t runGuess = std::min(kMaxRuns, std::max<size_t>(96, htmlLen / 90 + 32));
  if (blocks_.capacity() < blockGuess) {
    (void)blocks_.reserve(blockGuess);
  }
  if (runs_.capacity() < runGuess) {
    (void)runs_.reserve(runGuess);
  }
}

void ChapterIr::beginBlock(const BlockKind kind, const Align align, const uint16_t flags) {
  if (failed_) return;
  if (openBlock_) endBlock();
  if (blocks_.size() >= kMaxBlocks) {
    LOG_ERR("RVIR", "block cap %u", static_cast<unsigned>(kMaxBlocks));
    failed_ = true;
    return;
  }
  if (blocks_.size() == blocks_.capacity()) {
    size_t nc = blocks_.capacity() ? blocks_.capacity() + 16 : 32;
    if (nc > kMaxBlocks) nc = kMaxBlocks;
    if (nc <= blocks_.capacity() || !blocks_.reserve(nc)) {
      LOG_ERR("RVIR", "OOM blocks free=%u maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMaxAllocHeap()));
      failed_ = true;
      return;
    }
  }
  Block b;
  b.kind = kind;
  b.align = align;
  b.flags = flags;
  b.runBegin = static_cast<uint16_t>(std::min<size_t>(runs_.size(), 65535));
  b.runCount = 0;
  if (kind >= BlockKind::Heading1 && kind <= BlockKind::Heading6) {
    b.flags |= kBlockNoIndent;
    b.align = Align::Center;
    b.marginTopEmQ4 = 4;
    b.marginBottomEmQ4 = 6;
  } else if (kind == BlockKind::Paragraph) {
    b.indentEmQ4 = 16;
    b.marginTopEmQ4 = 0;
    b.marginBottomEmQ4 = 0;
  } else if (kind == BlockKind::HorizontalRule || kind == BlockKind::Spacer) {
    b.flags |= kBlockNoIndent;
    b.marginTopEmQ4 = 8;
    b.marginBottomEmQ4 = 8;
  } else if (kind == BlockKind::Image) {
    b.flags |= kBlockNoIndent;
    b.align = Align::Center;
    b.marginTopEmQ4 = 4;
    b.marginBottomEmQ4 = 4;
    b.indentEmQ4 = 0;
  }
  if (!blocks_.push_back(b)) { failed_ = true; return; }
  openBlock_ = true;
}

void ChapterIr::endBlock() {
  if (!openBlock_ || blocks_.empty()) {
    openBlock_ = false;
    return;
  }
  Block& b = blocks_.back();
  const size_t end = runs_.size();
  b.runCount = static_cast<uint16_t>(end - b.runBegin);
  if (b.runCount == 0 &&
      (b.kind == BlockKind::Paragraph ||
       (b.kind >= BlockKind::Heading1 && b.kind <= BlockKind::Heading6))) {
    blocks_.pop_back();
  }
  openBlock_ = false;
}

bool ChapterIr::appendRun(const RunStyle style, const SizeStep step, const char* utf8, const size_t len) {
  if (failed_) return false;
  if (len == 0) return true;
  if (!openBlock_ || !utf8 || len > kMaxTextBlob - textLen_) {
    failed_ = true;
    return false;
  }
  size_t aliasOffset = 0;
  const bool aliased = sourceOffset(textData_, textLen_, utf8, aliasOffset);
  if (aliased && len > textLen_ - aliasOffset) { failed_ = true; return false; }
  constexpr size_t maxRun = UINT16_MAX;
  size_t merge = 0;
  if (!runs_.empty() && blocks_.back().runCount > 0) {
    const Run& last = runs_.back();
    if (last.style == style && last.sizeStep == step &&
        static_cast<size_t>(last.textOff) + last.textLen == textLen_) {
      merge = runPrefix(utf8, len, maxRun - last.textLen);
    }
  }
  // Preflight every record before touching the visible text. The old code copied
  // all bytes but saturated textLen at 65535, silently making the tail invisible.
  size_t extraRuns = 0;
  for (size_t offset = merge; offset < len;) {
    const size_t n = runPrefix(utf8 + offset, len - offset, maxRun);
    if (n == 0) { failed_ = true; return false; }
    offset += n;
    ++extraRuns;
  }
  if (!ensureRunsCapacity(extraRuns) || !ensureTextCapacity(len)) {
    failed_ = true;
    return false;
  }
  if (aliased) utf8 = textData_ + aliasOffset;
  const size_t start = textLen_;
  std::memmove(textData_ + start, utf8, len);
  textLen_ += len;
  // UTF-8 helpers accept NUL-terminated buffers. Keep a sentinel beyond the blob
  // even when a malformed final scalar is encountered (not counted or persisted).
  std::memset(textData_ + textLen_, 0, 4);
  if (merge > 0) runs_.back().textLen = static_cast<uint16_t>(runs_.back().textLen + merge);
  for (size_t offset = merge; offset < len;) {
    const size_t n = runPrefix(textData_ + start + offset, len - offset, maxRun);
    Run r;
    r.textOff = static_cast<uint32_t>(start + offset);
    r.textLen = static_cast<uint16_t>(n);
    r.style = style;
    r.sizeStep = step;
    // Capacity was reserved above; this cannot allocate.
    if (!runs_.push_back(r)) { failed_ = true; return false; }
    ++blocks_.back().runCount;
    offset += n;
  }
  return true;
}

void ChapterIr::setCurrentIndentEmQ4(const uint8_t v) {
  if (openBlock_ && !blocks_.empty()) blocks_.back().indentEmQ4 = v;
}

void ChapterIr::setCurrentMarginsEmQ4(const int8_t top, const int8_t bottom) {
  if (openBlock_ && !blocks_.empty()) {
    blocks_.back().marginTopEmQ4 = top;
    blocks_.back().marginBottomEmQ4 = bottom;
  }
}

void ChapterIr::markDropCapOnCurrent() {
  if (openBlock_ && !blocks_.empty()) {
    blocks_.back().flags = static_cast<uint16_t>(blocks_.back().flags | kBlockDropCap | kBlockNoIndent);
  }
}

const char* ChapterIr::runText(const Run& r) const {
  if (!textData_ || r.textOff > textLen_ || r.textLen > textLen_ - r.textOff) return "";
  return textData_ + r.textOff;
}

std::string ChapterIr::runString(const Run& r) const {
  if (!textData_ || r.textOff > textLen_ || r.textLen > textLen_ - r.textOff) return {};
  return std::string(textData_ + r.textOff, r.textLen);
}

bool ChapterIr::setRunText(const size_t runIndex, const char* utf8, const size_t len) {
  if (failed_ || !utf8 || len == 0 || len > UINT16_MAX || runIndex >= runs_.size()) return false;
  Run& r = runs_[runIndex];
  if (r.textOff > textLen_ || r.textLen > textLen_ - r.textOff) return false;
  size_t aliasOffset = 0;
  const bool aliased = sourceOffset(textData_, textLen_, utf8, aliasOffset);
  if (aliased && len > textLen_ - aliasOffset) return false;
  // In-place replacement must remain possible even with a full blob/low heap.
  if (r.textLen == len && textData_) {
    std::memmove(textData_ + r.textOff, utf8, len);
    return true;
  }
  if (!ensureTextCapacity(len)) return false;
  if (aliased) utf8 = textData_ + aliasOffset;
  std::memmove(textData_ + textLen_, utf8, len);
  r.textOff = static_cast<uint32_t>(textLen_);
  r.textLen = static_cast<uint16_t>(len);
  textLen_ += len;
  std::memset(textData_ + textLen_, 0, 4);
  return true;
}

bool ChapterIr::writeTo(HalFile& f) const {
  if (!serialization::tryWritePod(f, kIrMagic)) return false;
  if (!serialization::tryWritePod(f, kIrFormatVersion)) return false;
  const uint32_t nBlocks = static_cast<uint32_t>(blocks_.size());
  const uint32_t nRuns = static_cast<uint32_t>(runs_.size());
  const uint32_t nText = static_cast<uint32_t>(textLen_);
  if (!serialization::tryWritePod(f, nBlocks)) return false;
  if (!serialization::tryWritePod(f, nRuns)) return false;
  if (!serialization::tryWritePod(f, nText)) return false;
  for (const Block& b : blocks_) {
    const uint8_t kind = static_cast<uint8_t>(b.kind);
    const uint8_t align = static_cast<uint8_t>(b.align);
    if (!serialization::tryWritePod(f, kind)) return false;
    if (!serialization::tryWritePod(f, align)) return false;
    if (!serialization::tryWritePod(f, b.flags)) return false;
    if (!serialization::tryWritePod(f, b.indentEmQ4)) return false;
    if (!serialization::tryWritePod(f, b.marginTopEmQ4)) return false;
    if (!serialization::tryWritePod(f, b.marginBottomEmQ4)) return false;
    if (!serialization::tryWritePod(f, b.runBegin)) return false;
    if (!serialization::tryWritePod(f, b.runCount)) return false;
    if (!serialization::tryWritePod(f, b.imageW)) return false;
    if (!serialization::tryWritePod(f, b.imageH)) return false;
  }
  for (const Run& r : runs_) {
    if (!serialization::tryWritePod(f, r.textOff)) return false;
    if (!serialization::tryWritePod(f, r.textLen)) return false;
    const uint8_t st = static_cast<uint8_t>(r.style);
    const int8_t step = static_cast<int8_t>(r.sizeStep);
    if (!serialization::tryWritePod(f, st)) return false;
    if (!serialization::tryWritePod(f, step)) return false;
  }
  if (nText > 0 && textData_) {
    if (f.write(reinterpret_cast<const uint8_t*>(textData_), nText) != nText) return false;
  }
  return true;
}

bool ChapterIr::readFrom(HalFile& f) {
  clear();
  char magic[4] = {};
  if (!serialization::tryReadPod(f, magic)) return false;
  if (std::memcmp(magic, kIrMagic, 4) != 0) {
    LOG_ERR("RVIR", "bad magic");
    return false;
  }
  uint16_t ver = 0;
  if (!serialization::tryReadPod(f, ver) || ver < kIrFormatVersionMin || ver > kIrFormatVersionMax) {
    LOG_ERR("RVIR", "bad version %u", ver);
    return false;
  }
  uint32_t nBlocks = 0, nRuns = 0, nText = 0;
  if (!serialization::tryReadPod(f, nBlocks)) return false;
  if (!serialization::tryReadPod(f, nRuns)) return false;
  if (!serialization::tryReadPod(f, nText)) return false;
  if (nBlocks > kMaxBlocks || nRuns > kMaxRuns || nText > kMaxTextBlob) {
    LOG_ERR("RVIR", "corrupt counts b=%u r=%u t=%u", nBlocks, nRuns, nText);
    return false;
  }
  // Disk records are 15 bytes/block and 8 bytes/run (not sizeof, which includes
  // native padding). Reject truncated/corrupt files BEFORE any large allocation.
  const uint64_t required = uint64_t{nBlocks} * 15 + uint64_t{nRuns} * 8 + nText;
  if (f.position() > f.size() || required != f.size() - f.position()) return false;
  if (!blocks_.resize(nBlocks) || !runs_.resize(nRuns)) return false;
  for (uint32_t i = 0; i < nBlocks; ++i) {
    uint8_t kind = 0, align = 0;
    if (!serialization::tryReadPod(f, kind)) return false;
    if (!serialization::tryReadPod(f, align)) return false;
    Block& b = blocks_[i];
    if (kind > static_cast<uint8_t>(BlockKind::Image) || align > static_cast<uint8_t>(Align::Justify)) return false;
    b.kind = static_cast<BlockKind>(kind);
    b.align = static_cast<Align>(align);
    if (!serialization::tryReadPod(f, b.flags)) return false;
    if (!serialization::tryReadPod(f, b.indentEmQ4)) return false;
    if (!serialization::tryReadPod(f, b.marginTopEmQ4)) return false;
    if (!serialization::tryReadPod(f, b.marginBottomEmQ4)) return false;
    if (!serialization::tryReadPod(f, b.runBegin)) return false;
    if (!serialization::tryReadPod(f, b.runCount)) return false;
    if (!serialization::tryReadPod(f, b.imageW)) return false;
    if (!serialization::tryReadPod(f, b.imageH)) return false;
    if (b.runBegin > nRuns || b.runCount > nRuns - b.runBegin || (b.flags & ~uint16_t{0x3F}) != 0) return false;
  }
  for (uint32_t i = 0; i < nRuns; ++i) {
    Run& r = runs_[i];
    if (!serialization::tryReadPod(f, r.textOff)) return false;
    if (!serialization::tryReadPod(f, r.textLen)) return false;
    uint8_t st = 0;
    int8_t step = 2;
    if (!serialization::tryReadPod(f, st)) return false;
    if (!serialization::tryReadPod(f, step)) return false;
    if (r.textOff > nText || r.textLen > nText - r.textOff || (st & 0xC0) != 0 || step < 0 || step > 4) return false;
    r.style = static_cast<RunStyle>(st);
    r.sizeStep = static_cast<SizeStep>(step);
  }
  if (nText > 0) {
    textData_ = static_cast<char*>(resizeReaderStorage(nullptr, static_cast<size_t>(nText) + 4));
    if (!textData_) return false;
    textCap_ = nText;
    textLen_ = nText;
    if (f.read(reinterpret_cast<uint8_t*>(textData_), nText) != static_cast<int>(nText)) return false;
    std::memset(textData_ + textLen_, 0, 4);
  }
  return true;
}

bool ChapterIr::saveToFile(const char* path) const {
  if (!path || !*path || failed_) return false;
  HalFile f;
  if (!Storage.openFileForWrite("RVIR", path, f)) {
    LOG_ERR("RVIR", "save open failed %s", path);
    return false;
  }
  const bool ok = writeTo(f);
  f.close();
  if (!ok) Storage.remove(path);
  return ok;
}

bool ChapterIr::loadFromFile(const char* path) {
  if (!path || !*path) return false;
  HalFile f;
  if (!Storage.openFileForRead("RVIR", path, f)) return false;
  const bool ok = readFrom(f);
  f.close();
  if (!ok) clear();
  return ok;
}

int ChapterIr::estimatePageCount(const int viewportW, const int viewportH, const int bodyEmPx,
                                 const float lineCompression) const {
  if (viewportW < 16 || viewportH < 16 || bodyEmPx < 4) return 1;
  if (textLen_ == 0 && blocks_.empty()) return 1;

  // Heuristic that knows about Rivulet block kinds — not a classic section rebuild,
  // but far closer than "chars / fixed CPL" for chapters with images, HRs, and
  // large headings (the old estimate undercounted plate-heavy spines badly).
  const float lc = lineCompression > 0.1f ? lineCompression : 1.0f;
  const int bodyLine = std::max(bodyEmPx + 2, static_cast<int>(bodyEmPx * 1.2f * lc + 0.5f));
  const int linesPerPage = std::max(1, viewportH / bodyLine);
  // Typical Latin serif advance is ~0.5–0.55em; the old 0.5em (2*W/em) CPL was still
  // too optimistic on e-ink margins and produced first-load ETAs like "7 pages" for
  // a 40-page DCC chapter. Use ~0.62em and floor CPL so we over-estimate slightly
  // (status "~" is better high than low until the idle map catches up).
  const int charsPerLine = std::max(28, (viewportW * 100) / std::max(1, bodyEmPx * 62));

  int contentLines = 0;
  int paraCount = 0;
  for (const Block& b : blocks_) {
    switch (b.kind) {
      case BlockKind::HorizontalRule:
        contentLines += 1;
        break;
      case BlockKind::Spacer: {
        // marginBottomEmQ4 is in 1/16 em.
        const int gap = std::max(1, (static_cast<int>(b.marginBottomEmQ4) * bodyEmPx) / 16);
        contentLines += std::max(1, (gap + bodyLine - 1) / bodyLine);
        break;
      }
      case BlockKind::Image: {
        int h = b.imageH > 0 ? static_cast<int>(b.imageH) : bodyEmPx * 4;
        // Floats share vertical space with wrapping text — charge ~half height.
        if ((b.flags & (kBlockFloatLeft | kBlockFloatRight)) != 0) {
          h = std::max(bodyLine, h / 2);
        }
        // Cap a single plate at one page so a cover-like image cannot explode ETA.
        const int imgLines = std::min(linesPerPage, std::max(1, (h + bodyLine - 1) / bodyLine));
        contentLines += imgLines;
        break;
      }
      default: {
        // Paragraph / heading: count UTF-8 bytes in the block's runs.
        size_t bytes = 0;
        const uint16_t runEnd = static_cast<uint16_t>(b.runBegin + b.runCount);
        for (uint16_t ri = b.runBegin; ri < runEnd && ri < runs_.size(); ++ri) {
          bytes += runs_[ri].textLen;
        }
        int stepBoost = 0;
        if (b.kind == BlockKind::Heading1) stepBoost = bodyLine;  // ~extra line of air
        else if (b.kind == BlockKind::Heading2)
          stepBoost = bodyLine / 2;
        else if (b.kind >= BlockKind::Heading3 && b.kind <= BlockKind::Heading6)
          stepBoost = bodyLine / 4;
        // Larger faces use fewer chars per line.
        int cpl = charsPerLine;
        if (b.kind == BlockKind::Heading1)
          cpl = std::max(8, charsPerLine * 2 / 3);
        else if (b.kind == BlockKind::Heading2)
          cpl = std::max(10, charsPerLine * 4 / 5);
        const int textLines =
            bytes == 0 ? 1
                       : static_cast<int>((bytes + static_cast<size_t>(cpl) - 1) / static_cast<size_t>(cpl));
        contentLines += textLines + (stepBoost + bodyLine - 1) / bodyLine;
        if (b.kind == BlockKind::Paragraph) ++paraCount;
        break;
      }
    }
  }
  contentLines += paraCount / 2;

  // Empty-run chapters (image-only): still at least the image lines above.
  if (contentLines <= 0) {
    contentLines = static_cast<int>((textLen_ + static_cast<size_t>(charsPerLine) - 1) /
                                    static_cast<size_t>(charsPerLine)) +
                   static_cast<int>(blocks_.size()) / 2;
  }

  int pages = std::max(1, (contentLines + linesPerPage - 1) / linesPerPage);
  // Floor from raw text length so a sparse block list cannot under-count a prose chapter.
  if (textLen_ > 0 && charsPerLine > 0 && linesPerPage > 0) {
    const int charsPerPage = charsPerLine * linesPerPage;
    const int fromText =
        static_cast<int>((textLen_ + static_cast<size_t>(charsPerPage) - 1) / static_cast<size_t>(charsPerPage));
    pages = std::max(pages, fromText);
  }
  if (pages == 1 && (textLen_ > 400 || blocks_.size() > 3)) {
    pages = 2;
  }
  // Slight padding while the map is still cold — UI shows "~N"; idle map replaces it.
  if (pages >= 4) {
    pages = pages + std::max(1, pages / 12);
  }
  return pages;
}

}  // namespace rivulet
