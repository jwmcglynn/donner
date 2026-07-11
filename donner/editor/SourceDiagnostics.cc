#include "donner/editor/SourceDiagnostics.h"

#include <algorithm>
#include <limits>

namespace donner::editor {
namespace {

constexpr std::size_t kMaxPublishedDiagnostics = 256;
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void HashByte(std::uint64_t* hash, std::uint8_t byte) {
  *hash ^= byte;
  *hash *= kFnvPrime;
}

void HashSize(std::uint64_t* hash, std::size_t value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    HashByte(hash, static_cast<std::uint8_t>(value & 0xffu));
    value >>= 8u;
  }
}

std::uint64_t DiagnosticId(const ParseDiagnostic& diagnostic, SourceByteRange range,
                           std::uint64_t revision, std::size_t index) {
  std::uint64_t hash = kFnvOffsetBasis;
  HashSize(&hash, static_cast<std::size_t>(revision));
  HashSize(&hash, index);
  HashSize(&hash, static_cast<std::size_t>(diagnostic.severity));
  HashSize(&hash, range.start);
  HashSize(&hash, range.end);
  for (const char value : std::string_view(diagnostic.reason)) {
    HashByte(&hash, static_cast<std::uint8_t>(value));
  }
  return hash;
}

SourceByteRange NormalizeRange(const SourceRange& range, std::string_view source) {
  const std::size_t start =
      std::min(range.start.resolveOffset(source).offset.value(), source.size());
  std::size_t end = std::min(range.end.resolveOffset(source).offset.value(), source.size());
  end = std::max(start, end);

  if (start == end && !source.empty()) {
    if (end < source.size()) {
      ++end;
    } else {
      return SourceByteRange{start - 1, start};
    }
  }

  return SourceByteRange{start, end};
}

std::vector<std::size_t> BuildLineStarts(std::string_view source) {
  std::vector<std::size_t> lineStarts{0};
  for (std::size_t i = 0; i < source.size(); ++i) {
    if (source[i] == '\n') {
      lineStarts.push_back(i + 1);
    }
  }
  return lineStarts;
}

FileOffset::LineInfo RecoverLineInfo(std::span<const std::size_t> lineStarts, std::size_t offset) {
  const auto nextLine = std::upper_bound(lineStarts.begin(), lineStarts.end(), offset);
  const std::size_t lineIndex = static_cast<std::size_t>(nextLine - lineStarts.begin() - 1);
  return FileOffset::LineInfo{lineIndex + 1, offset - lineStarts[lineIndex]};
}

}  // namespace

SourceDiagnosticSnapshot BuildSourceDiagnosticSnapshot(std::span<const ParseDiagnostic> diagnostics,
                                                       std::string_view source,
                                                       std::uint64_t revision) {
  SourceDiagnosticSnapshot snapshot{.revision = revision};
  snapshot.diagnostics.reserve(std::min(diagnostics.size(), kMaxPublishedDiagnostics));
  const std::vector<std::size_t> lineStarts = BuildLineStarts(source);

  for (std::size_t i = 0; i < diagnostics.size() && i < kMaxPublishedDiagnostics; ++i) {
    const ParseDiagnostic& diagnostic = diagnostics[i];
    const SourceByteRange range = NormalizeRange(diagnostic.range, source);
    const std::size_t sourceOffset = std::min(
        diagnostic.range.start.resolveOffset(source).offset.value(), source.size());
    const FileOffset::LineInfo lineInfo = diagnostic.range.start.lineInfo.value_or(
        RecoverLineInfo(lineStarts, sourceOffset));
    snapshot.diagnostics.push_back(SourceDiagnostic{
        .id = DiagnosticId(diagnostic, range, revision, i),
        .severity = diagnostic.severity,
        .range = range,
        .line = lineInfo.line,
        .column = lineInfo.offsetOnLine,
        .message = std::string(std::string_view(diagnostic.reason)),
    });
  }

  return snapshot;
}

}  // namespace donner::editor
