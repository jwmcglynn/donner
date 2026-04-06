#include "donner/base/DiagnosticRenderer.h"

#include <gtest/gtest.h>

namespace donner {

TEST(DiagnosticRenderer, SingleCharError) {
  const std::string_view source = R"(<path d="M 100 100 h 2!" />)";
  auto diag = ParseDiagnostic::Error("Unexpected character",
                                     SourceRange{FileOffset::Offset(23), FileOffset::Offset(24)});

  std::string result = DiagnosticRenderer::format(source, diag);
  EXPECT_EQ(result,
            "error: Unexpected character\n"
            "  --> 1:24\n"
            "  |\n"
            " 1 | <path d=\"M 100 100 h 2!\" />\n"
            "  |                        ^\n");
}

TEST(DiagnosticRenderer, MultiCharError) {
  const std::string_view source = R"(<path d="Inf" />)";
  auto diag = ParseDiagnostic::Error("Not finite",
                                     SourceRange{FileOffset::Offset(9), FileOffset::Offset(12)});

  std::string result = DiagnosticRenderer::format(source, diag);
  EXPECT_EQ(result,
            "error: Not finite\n"
            "  --> 1:10\n"
            "  |\n"
            " 1 | <path d=\"Inf\" />\n"
            "  |          ^~~\n");
}

TEST(DiagnosticRenderer, WarningLabel) {
  const std::string_view source = "fill: url(#bad)";
  auto diag = ParseDiagnostic::Warning("Invalid paint server",
                                       SourceRange{FileOffset::Offset(6), FileOffset::Offset(15)});

  std::string result = DiagnosticRenderer::format(source, diag);
  EXPECT_EQ(result,
            "warning: Invalid paint server\n"
            "  --> 1:7\n"
            "  |\n"
            " 1 | fill: url(#bad)\n"
            "  |       ^~~~~~~~~\n");
}

TEST(DiagnosticRenderer, WithFilename) {
  const std::string_view source = "hello";
  auto diag = ParseDiagnostic::Error("Bad input",
                                     SourceRange{FileOffset::Offset(0), FileOffset::Offset(1)});

  std::string result =
      DiagnosticRenderer::format(source, diag, {.filename = "test.svg"});
  EXPECT_EQ(result,
            "error: Bad input\n"
            "  --> test.svg:1:1\n"
            "  |\n"
            " 1 | hello\n"
            "  | ^\n");
}

TEST(DiagnosticRenderer, EndOfString) {
  const std::string_view source = "abc";
  auto diag = ParseDiagnostic::Error("Unexpected end",
                                     SourceRange{FileOffset::EndOfString(),
                                                 FileOffset::EndOfString()});

  // EndOfString has no offset, so only the message is shown.
  std::string result = DiagnosticRenderer::format(source, diag);
  EXPECT_EQ(result, "error: Unexpected end\n");
}

TEST(DiagnosticRenderer, MultiLineSource) {
  const std::string_view source = "<svg>\n  <rect x=\"abc\" />\n</svg>";
  //                                0123456 789...
  // Line 1: "<svg>" (0..5)
  // Line 2: "  <rect x=\"abc\" />" (6..24)
  // Error at "abc" which starts at offset 16
  auto diag = ParseDiagnostic::Error("Invalid number",
                                     SourceRange{FileOffset::Offset(16), FileOffset::Offset(19)});

  std::string result = DiagnosticRenderer::format(source, diag);
  EXPECT_EQ(result,
            "error: Invalid number\n"
            "  --> 2:10\n"
            "  |\n"
            " 2 |   <rect x=\"abc\" />\n"
            "  |          ^~~\n");
}

TEST(DiagnosticRenderer, PointRange) {
  const std::string_view source = "test";
  auto diag = ParseDiagnostic::Error("Point error",
                                     SourceRange{FileOffset::Offset(2), FileOffset::Offset(2)});

  std::string result = DiagnosticRenderer::format(source, diag);
  EXPECT_EQ(result,
            "error: Point error\n"
            "  --> 1:3\n"
            "  |\n"
            " 1 | test\n"
            "  |   ^\n");
}

TEST(DiagnosticRenderer, FormatAll) {
  const std::string_view source = "abc";
  ParseWarningSink sink;
  sink.add(ParseDiagnostic::Warning("warn1",
                                    SourceRange{FileOffset::Offset(0), FileOffset::Offset(1)}));
  sink.add(ParseDiagnostic::Warning("warn2",
                                    SourceRange{FileOffset::Offset(2), FileOffset::Offset(3)}));

  std::string result = DiagnosticRenderer::formatAll(source, sink);
  EXPECT_EQ(result,
            "warning: warn1\n"
            "  --> 1:1\n"
            "  |\n"
            " 1 | abc\n"
            "  | ^\n"
            "warning: warn2\n"
            "  --> 1:3\n"
            "  |\n"
            " 1 | abc\n"
            "  |   ^\n");
}

TEST(DiagnosticRenderer, FormatAllEmpty) {
  ParseWarningSink sink;
  EXPECT_EQ(DiagnosticRenderer::formatAll("test", sink), "");
}

TEST(DiagnosticRenderer, DoubleDigitLineNumber) {
  // Create source with 10+ lines to test alignment.
  std::string source;
  for (int i = 1; i <= 11; ++i) {
    source += "line " + std::to_string(i) + "\n";
  }
  // "line 11\n" starts at some offset. Let's compute:
  // Each line is "line N\n" (7 chars for 1-digit, 8 for 2-digit).
  // Lines 1-9: 9 * 7 = 63 chars. Line 10 starts at 63, "line 10\n" is 8 chars.
  // Line 11 starts at 71.
  auto diag = ParseDiagnostic::Error("Error on line 11",
                                     SourceRange{FileOffset::Offset(71), FileOffset::Offset(72)});

  std::string result = DiagnosticRenderer::format(source, diag);
  EXPECT_EQ(result,
            "error: Error on line 11\n"
            "  --> 11:1\n"
            "   |\n"
            " 11 | line 11\n"
            "   | ^\n");
}

}  // namespace donner
