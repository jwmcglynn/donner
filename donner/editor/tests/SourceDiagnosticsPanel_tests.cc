#include "donner/editor/SourceDiagnosticsPanel.h"

#include <gtest/gtest.h>

#include <array>

namespace donner::editor {
namespace {

TEST(SourceDiagnosticsPanel, CountsSeverityAndFindsStableId) {
  const std::array diagnostics = {
      SourceDiagnostic{.id = 11, .severity = DiagnosticSeverity::Warning, .message = "warning"},
      SourceDiagnostic{.id = 12, .severity = DiagnosticSeverity::Error, .message = "error"},
      SourceDiagnostic{.id = 13, .severity = DiagnosticSeverity::Error, .message = "error"},
  };

  EXPECT_EQ(CountSourceDiagnostics(diagnostics), (SourceDiagnosticCounts{2, 1}));
  ASSERT_NE(FindSourceDiagnostic(diagnostics, 12), nullptr);
  EXPECT_EQ(FindSourceDiagnostic(diagnostics, 12)->severity, DiagnosticSeverity::Error);
  EXPECT_EQ(FindSourceDiagnostic(diagnostics, 99), nullptr);
}

}  // namespace
}  // namespace donner::editor
