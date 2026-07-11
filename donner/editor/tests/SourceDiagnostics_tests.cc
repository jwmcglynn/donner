#include "donner/editor/SourceDiagnostics.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/FileOffset.h"
#include "donner/base/ParseDiagnostic.h"

namespace donner::editor {
namespace {

TEST(SourceDiagnostics, NormalizesRangesAndRecoversLineInformation) {
  constexpr std::string_view kSource = "<svg>\n  <rect/>\n</svg>";
  const std::array diagnostics = {
      ParseDiagnostic::Warning("point", FileOffset::Offset(8)),
      ParseDiagnostic::Error(
          "past end", SourceRange{FileOffset::Offset(9), FileOffset::Offset(kSource.size() + 20)}),
  };

  const SourceDiagnosticSnapshot snapshot = BuildSourceDiagnosticSnapshot(diagnostics, kSource, 7);

  ASSERT_EQ(snapshot.diagnostics.size(), 2u);
  EXPECT_EQ(snapshot.revision, 7u);
  EXPECT_EQ(snapshot.diagnostics[0].range, (SourceByteRange{8, 9}));
  EXPECT_EQ(snapshot.diagnostics[0].line, 2u);
  EXPECT_EQ(snapshot.diagnostics[0].column, 2u);
  EXPECT_EQ(snapshot.diagnostics[1].range, (SourceByteRange{9, kSource.size()}));
}

TEST(SourceDiagnostics, UsesRevisionScopedStableIds) {
  constexpr std::string_view kSource = "<svg/>";
  const std::array diagnostics = {
      ParseDiagnostic::Error("invalid", FileOffset::Offset(2)),
  };

  const SourceDiagnosticSnapshot first = BuildSourceDiagnosticSnapshot(diagnostics, kSource, 3);
  const SourceDiagnosticSnapshot repeated = BuildSourceDiagnosticSnapshot(diagnostics, kSource, 3);
  const SourceDiagnosticSnapshot reparsed = BuildSourceDiagnosticSnapshot(diagnostics, kSource, 4);

  ASSERT_EQ(first.diagnostics.size(), 1u);
  EXPECT_EQ(first.diagnostics[0].id, repeated.diagnostics[0].id);
  EXPECT_NE(first.diagnostics[0].id, reparsed.diagnostics[0].id);
}

TEST(SourceDiagnostics, PreservesSeverityAndMessage) {
  constexpr std::string_view kSource = "<svg/>";
  const std::array diagnostics = {
      ParseDiagnostic::Warning("deprecated", FileOffset::Offset(1)),
      ParseDiagnostic::Error("invalid", FileOffset::Offset(2)),
  };

  const SourceDiagnosticSnapshot snapshot = BuildSourceDiagnosticSnapshot(diagnostics, kSource, 1);

  ASSERT_EQ(snapshot.diagnostics.size(), 2u);
  EXPECT_EQ(snapshot.diagnostics[0].severity, DiagnosticSeverity::Warning);
  EXPECT_EQ(snapshot.diagnostics[0].message, "deprecated");
  EXPECT_EQ(snapshot.diagnostics[1].severity, DiagnosticSeverity::Error);
  EXPECT_EQ(snapshot.diagnostics[1].message, "invalid");
}

TEST(SourceDiagnostics, CapsPublishedDiagnosticsAndRecoversLocationsInLargeSource) {
  std::string source;
  std::vector<ParseDiagnostic> diagnostics;
  for (std::size_t i = 0; i < 300; ++i) {
    source += "line\n";
    diagnostics.push_back(ParseDiagnostic::Warning("warning", FileOffset::Offset(i * 5)));
  }

  const SourceDiagnosticSnapshot snapshot = BuildSourceDiagnosticSnapshot(diagnostics, source, 1);

  ASSERT_EQ(snapshot.diagnostics.size(), 256u);
  EXPECT_EQ(snapshot.diagnostics.front().line, 1u);
  EXPECT_EQ(snapshot.diagnostics.front().column, 0u);
  EXPECT_EQ(snapshot.diagnostics.back().line, 256u);
  EXPECT_EQ(snapshot.diagnostics.back().column, 0u);
}

}  // namespace
}  // namespace donner::editor
