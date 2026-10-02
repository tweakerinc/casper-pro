#pragma once
// Hyphenation pattern correctness is tested separately by the existing suite.
#include <string>
#include <vector>
class Hyphenator {
 public:
  struct BreakInfo { size_t byteOffset; bool requiresInsertedHyphen; };
  static std::vector<BreakInfo> breakOffsets(const std::string&, bool) { return {}; }
};
