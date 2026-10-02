#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$root/.host-pro-tests}"
mkdir -p "$build"
"${CXX:-g++}" -std=c++20 -O0 -g -fno-exceptions -Wall -Wextra \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$root/test/html_to_ir/stubs" -I"$root/lib/Rivulet" \
  -I"$root/lib/Serialization" -I"$root/lib/Utf8" \
  "$root/test/pro_repairs/MemorySafetyTest.cpp" \
  "$root/lib/Rivulet/ChapterIr.cpp" "$root/lib/Rivulet/PageMap.cpp" \
  "$root/lib/Rivulet/LaidOutPage.cpp" "$root/lib/Rivulet/HtmlToIr.cpp" \
  "$root/lib/Utf8/Utf8.cpp" -Wl,--wrap=realloc -o "$build/memory-tests"
ASAN_OPTIONS=detect_leaks=1 "$build/memory-tests"
"${CXX:-g++}" -std=c++20 -O0 -g -fno-exceptions -Wall -Wextra \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$root/test/pro_repairs/stubs" -I"$root/test/html_to_ir/stubs" \
  -I"$root/src" -I"$root/lib/Rivulet" -I"$root/lib/EpdFont" \
  -I"$root/lib/Epub" -I"$root/lib/Serialization" -I"$root/lib/Utf8" -I"$root/lib/JsonParser" \
  "$root/test/pro_repairs/ReaderInteractionTest.cpp" \
  "$root/lib/Rivulet/ChapterIr.cpp" "$root/lib/Rivulet/PageMap.cpp" \
  "$root/lib/Rivulet/LaidOutPage.cpp" "$root/lib/Rivulet/HtmlToIr.cpp" \
  "$root/lib/Rivulet/PageLayouter.cpp" "$root/lib/Rivulet/RivuletEngine.cpp" \
  "$root/lib/Rivulet/FontLadder.cpp" "$root/lib/Epub/Epub/css/StyleResolve.cpp" \
  "$root/lib/JsonParser/ReleaseJsonParser.cpp" "$root/lib/JsonParser/StreamingJsonParser.cpp" \
  "$root/lib/Utf8/Utf8.cpp" -Wl,--wrap=realloc -o "$build/reader-tests"
ASAN_OPTIONS=detect_leaks=1 "$build/reader-tests"
