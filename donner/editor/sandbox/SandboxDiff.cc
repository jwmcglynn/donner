#include "donner/editor/sandbox/SandboxDiff.h"

#include <sstream>

namespace donner::editor::sandbox {

namespace {

/// Returns the BackendHint as a display string for header diffs.
const char* BackendHintName(BackendHint hint) {
  switch (hint) {
    case BackendHint::kUnspecified: return "unspecified";
    case BackendHint::kTinySkia:   return "tiny_skia";
    case BackendHint::kGeode:      return "geode";
  }
  return "unknown";
}

/// Two commands are considered equal for LCS purposes when their opcode and
/// summary string both match. Depth is intentionally excluded — it's a
/// rendering-time property, not a semantic one.
bool CommandsEqual(const DecodedCommand& a, const DecodedCommand& b) {
  return a.opcode == b.opcode && a.summary == b.summary;
}

/// Appends a single diff line, indented by `depth` levels (2 spaces each),
/// prefixed with `prefix` (one of ' ', '-', '+').
void AppendDiffLine(std::ostringstream& os, char prefix, int32_t depth,
                    const std::string& summary) {
  os << prefix << ' ';
  for (int i = 0; i < depth; ++i) {
    os << "  ";
  }
  os << summary << '\n';
}

/// Compares header fields and appends any differences to `os`. Returns true
/// if any header field differs.
bool DiffHeaders(std::ostringstream& os, const RnrHeader& a, const RnrHeader& b) {
  bool differs = false;
  auto field = [&](const char* name, auto va, auto vb) {
    if (va != vb) {
      differs = true;
      os << "header " << name << ": ";
      if constexpr (std::is_same_v<decltype(va), BackendHint>) {
        os << BackendHintName(va) << " -> " << BackendHintName(vb) << '\n';
      } else if constexpr (std::is_same_v<decltype(va), std::string>) {
        os << '"' << va << "\" -> \"" << vb << "\"\n";
      } else {
        os << va << " -> " << vb << '\n';
      }
    }
  };

  field("fileVersion", a.fileVersion, b.fileVersion);
  field("timestampNanos", a.timestampNanos, b.timestampNanos);
  field("width", a.width, b.width);
  field("height", a.height, b.height);
  field("backend", a.backend, b.backend);
  field("uri", a.uri, b.uri);
  return differs;
}

}  // namespace

DiffResult ComputeRnrDiff(const RnrHeader& headerA,
                          const std::vector<DecodedCommand>& cmdsA,
                          const RnrHeader& headerB,
                          const std::vector<DecodedCommand>& cmdsB) {
  DiffResult result;
  std::ostringstream os;

  const bool headerDiffers = DiffHeaders(os, headerA, headerB);
  if (headerDiffers) {
    result.identical = false;
  }

  // --- LCS over (opcode, summary) pairs ---
  const auto n = cmdsA.size();
  const auto m = cmdsB.size();

  // dp[i][j] = length of LCS of cmdsA[0..i-1] and cmdsB[0..j-1].
  // Use a flat vector for cache-friendliness.
  std::vector<uint32_t> dp((n + 1) * (m + 1), 0);
  auto idx = [cols = m + 1](std::size_t i, std::size_t j) { return i * cols + j; };

  for (std::size_t i = 1; i <= n; ++i) {
    for (std::size_t j = 1; j <= m; ++j) {
      if (CommandsEqual(cmdsA[i - 1], cmdsB[j - 1])) {
        dp[idx(i, j)] = dp[idx(i - 1, j - 1)] + 1;
      } else {
        dp[idx(i, j)] = std::max(dp[idx(i - 1, j)], dp[idx(i, j - 1)]);
      }
    }
  }

  // Back-trace to emit the unified diff.
  // Collect diff entries in reverse, then print in order.
  struct DiffEntry {
    char prefix;      // ' ', '-', '+'
    int32_t depth;
    std::string summary;
  };
  std::vector<DiffEntry> entries;

  std::size_t i = n;
  std::size_t j = m;
  while (i > 0 || j > 0) {
    if (i > 0 && j > 0 && CommandsEqual(cmdsA[i - 1], cmdsB[j - 1])) {
      entries.push_back({' ', cmdsA[i - 1].depth, cmdsA[i - 1].summary});
      --i;
      --j;
    } else if (j > 0 && (i == 0 || dp[idx(i, j - 1)] >= dp[idx(i - 1, j)])) {
      entries.push_back({'+', cmdsB[j - 1].depth, cmdsB[j - 1].summary});
      --j;
    } else {
      entries.push_back({'-', cmdsA[i - 1].depth, cmdsA[i - 1].summary});
      --i;
    }
  }

  // Check if there are any non-context lines.
  bool streamDiffers = false;
  for (const auto& e : entries) {
    if (e.prefix != ' ') {
      streamDiffers = true;
      break;
    }
  }

  if (streamDiffers) {
    result.identical = false;
    // Emit entries in forward order.
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
      AppendDiffLine(os, it->prefix, it->depth, it->summary);
    }
  }

  result.report = os.str();
  return result;
}

}  // namespace donner::editor::sandbox
