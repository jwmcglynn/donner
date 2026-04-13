#pragma once
/// @file
///
/// Structural diff engine for `.rnr` recordings. Compares two decoded command
/// streams (and their headers) using a longest-common-subsequence alignment,
/// producing a unified-diff-style report suitable for regression triage.

#include <cstdint>
#include <string>
#include <vector>

#include "donner/editor/sandbox/FrameInspector.h"
#include "donner/editor/sandbox/RnrFile.h"

namespace donner::editor::sandbox {

/// Result of comparing two `.rnr` recordings.
struct DiffResult {
  /// True when both headers and command streams are identical.
  bool identical = true;
  /// Human-readable diff report. Empty when `identical` is true.
  std::string report;
};

/// Compares two decoded `.rnr` recordings and produces a unified-diff-style
/// report. Header fields are compared first, then the command streams are
/// aligned with a plain O(N*M) longest-common-subsequence and emitted with
/// `+`/`-`/` ` prefixes (indented by each command's depth).
///
/// @param headerA  Header from the first recording.
/// @param cmdsA    Decoded commands from the first recording.
/// @param headerB  Header from the second recording.
/// @param cmdsB    Decoded commands from the second recording.
DiffResult ComputeRnrDiff(const RnrHeader& headerA,
                          const std::vector<DecodedCommand>& cmdsA,
                          const RnrHeader& headerB,
                          const std::vector<DecodedCommand>& cmdsB);

}  // namespace donner::editor::sandbox
