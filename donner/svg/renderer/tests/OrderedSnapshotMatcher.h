#pragma once
/// @file

#include <gmock/gmock.h>

#include <algorithm>
#include <functional>
#include <iterator>

namespace donner::svg::tests {

// Keep successful comparisons allocation-free. On a mismatch, report the first unequal entry
// without constructing a separate gMock matcher for every record in each corpus frame.
MATCHER_P(OrderedSnapshotEquals, expected, "matches the complete ordered snapshot") {
  const auto& reference = expected.get();
  if (arg.size() != reference.size()) {
    *result_listener << "has " << arg.size() << " entries, expected " << reference.size();
    return false;
  }
  const auto [actualEntry, expectedEntry] =
      std::mismatch(arg.begin(), arg.end(), reference.begin());
  if (actualEntry == arg.end()) {
    return true;
  }
  *result_listener << "first differs at index " << std::distance(arg.begin(), actualEntry)
                   << ": expected " << testing::PrintToString(*expectedEntry) << ", actual "
                   << testing::PrintToString(*actualEntry);
  return false;
}

template <typename Entries>
auto SnapshotEq(const Entries& expected) {
  return OrderedSnapshotEquals(std::cref(expected));
}

}  // namespace donner::svg::tests
