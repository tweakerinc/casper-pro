# Casper Pro reader / memory / touch repair candidate

Baseline: `cursor/x4pro-sleep-reader-menu-6b98` at
`53797361f451242b8cd4e2cea19ac449c83c1ef5`. No merge to main or device flashing.

## Implemented

- Bottom-edge up opens a reader-owned Book Appearance overlay: family, actual
  available size, line and paragraph spacing, margins, alignment, hyphenation,
  anti-aliasing. No brightness/warmth controls. Uses full book viewport under the
  overlay; opening/closing does not release/re-extract the chapter. Settings are
  GLOBAL defaults, explicitly labelled; per-book overrides are not implemented.
- Coalesced edits reflow resident IR around the original content cursor, with
  bounded work and rollback of the old page/map/settings/font ownership on
  failure. Undo cancels pending edits too. Done/outside/Back/Home close the panel
  without also turning a page. Portrait and landscape share hit-test geometry.
- Preserve relative left hold/drag brightness. A stationary hold does not jump
  brightness; the owned contact includes release. Immediate bottom-up motion
  does not arm brightness. Lighting remains separate from appearance.
- Input dispatch no longer returns past pending transitions/render notification.
- Selected SD font is no longer silently replaced by a built-in. Layout and
  paint use the shared family ladder. Fix clamped Literata rung and negative
  hashed font IDs; cap owned font slots and remove only owned registrations.
- Fallible PSRAM-preferred IR, page-index and token storage; checked sizes and
  allocation failures. Keep old allocations/data on failure. No token OOM may
  be interpreted as permission to skip a block during indexing.
- Preserve text across 64-KiB run boundaries, respecting UTF-8 boundaries; reject
  oversized replacement runs, invalid aliases and corrupt IR offsets/ranges.
- Include spacing/image flags in render-key equality. Invalidate old IR/page/map
  versions. Check serialized record sizes, cursor ordering, counts, strings,
  trailing data and long temporary paths before trusting caches. Never save a
  failed/partial IR or page. Release retained page/IR storage on clear.
- Validate cached page/map cursors against actual block/run bounds and UTF-8
  boundaries, require the true chapter start, reject false chapter-end flags and
  map/paint disagreements, and validate reflow anchors before mutating state.
  Seven new reproductions failed before these guards and passed after them.
- The legacy HTML-prefix limit is now explicitly partial and is not cached as a
  complete chapter. This is NOT a complete streaming solution for large chapters.
- X4 Pro uses its own OTA release feed and accepts board-labelled application
  assets only (`firmware-x4pro.bin`, `casper-pro-x4pro.bin`, or
  `Casper-Pro-v<version>-x4pro.bin`). Generic, merged and bootloader names are
  not auto-selected. Reject truncated asset metadata, invalid sizes/URLs and
  integer overflow. Existing MCU validation now fails closed if device identity
  cannot be read; segment lengths use overflow-safe bounds. No OTA was installed.
- Fix pre-existing C3 linker failure: missing non-frontlight implementation of
  `FrontlightQuickActivity::consumeSkipParentRepaint()`.

## Validation

`bash test/pro_repairs/run.sh /tmp/casper-pro-tests`

58 native regression tests passed locally with AddressSanitizer,
UndefinedBehaviorSanitizer, leak checking and exceptions disabled. Includes
forced realloc failures, corrupt files, long UTF-8 text, brightness ownership,
font ladders, layout measure/paint cursor agreement, real-engine same-passage
reflow and rollback, and OTA asset filtering. Engine tests use deterministic
renderer metrics, a memory filesystem and a no-pattern hyphenation fixture.
They do NOT validate real font glyph files, display waveforms, I2C touch,
physical brightness, sleep current, flash writes or book-format fidelity.

The main repair commit `1ac65e3210a3d531e0788cd8c7cbc1d7d14b8cad` passed
51 native tests on GitHub Actions and compiled successfully for both X4 Pro
and shared C3 in run `37051902219`. The seven additional cursor tests bring
the local suite to 58; final follow-up CI must validate this exact revision.
No hardware has been flashed or tested. The earlier unchanged C3 baseline
failed to link; the repaired C3 build confirms the frontlight stub resolves it.

## Not a finished release

No physical X4 Pro testing has occurred. This patch does not claim every screen
is fully touch-complete, every allocation is fallible, all rendering bugs are
fixed, or upstream Casper changes have been exhaustively merged. Page paint
strings/spans, font parsers, third-party decoders and other legacy paths still
use STL allocations and require further low-memory/device coverage. Large
chapters exceeding existing conversion limits require a streaming follow-up.
Global appearance defaults are retained; per-book style storage and a full
bookmark/clipping location migration are not added. Typography changes need
real EPUB/font comparison and long-session hardware testing before release.

Acceptance on hardware: bottom drawer open/close with no page jump; change
font/spacing while staying on the same passage; family/size match actual text;
left hold+drag from lower-left then release without any navigation; immediate
bottom-left up opens appearance; modal taps never reach the book; font-list
paging and landscape targets; repeated changes/cancel/sleep/reopen; large and
malformed EPUBs; no corrupt cache or low-heap reset. Keep recovery firmware and
settings/bookmark backups before any test flash. Do not use a C3 binary on Pro.
