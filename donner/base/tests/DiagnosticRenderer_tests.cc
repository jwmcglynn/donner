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

  std::string result = DiagnosticRenderer::format(source, diag, {.filename = "test.svg"});
  EXPECT_EQ(result,
            "error: Bad input\n"
            "  --> test.svg:1:1\n"
            "  |\n"
            " 1 | hello\n"
            "  | ^\n");
}

TEST(DiagnosticRenderer, EndOfString) {
  const std::string_view source = "abc";
  auto diag = ParseDiagnostic::Error(
      "Unexpected end", SourceRange{FileOffset::EndOfString(), FileOffset::EndOfString()});

  // EndOfString has no offset, so only the message is shown.
  std::string result = DiagnosticRenderer::format(source, diag);
  EXPECT_EQ(result, "error: Unexpected end\n");
}

TEST(DiagnosticRenderer, MultiLineSource) {
  const std::string_view source = "<svg>\n  <rect x=\"abc\" />\n</svg>";
  //                                0123456 789...
  // Line 1: "<svg>" (0..5)
  // Line 2: "  <rect x=\"abc\" />" (6..24)
  // Error at "abc" which starts at offset 17 (after the opening quote)
  auto diag = ParseDiagnostic::Error("Invalid number",
                                     SourceRange{FileOffset::Offset(17), FileOffset::Offset(20)});

  std::string result = DiagnosticRenderer::format(source, diag);
  EXPECT_EQ(result,
            "error: Invalid number\n"
            "  --> 2:12\n"
            "  |\n"
            " 2 |   <rect x=\"abc\" />\n"
            "  |            ^~~\n");
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
  sink.add(
      ParseDiagnostic::Warning("warn1", SourceRange{FileOffset::Offset(0), FileOffset::Offset(1)}));
  sink.add(
      ParseDiagnostic::Warning("warn2", SourceRange{FileOffset::Offset(2), FileOffset::Offset(3)}));

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

TEST(DiagnosticRenderer, ColorizedWarningIncludesAnsiStyling) {
  const std::string_view source = "abc\ndef";
  auto diag =
      ParseDiagnostic::Warning("Styled", SourceRange{FileOffset::Offset(4), FileOffset::Offset(7)});

  std::string result =
      DiagnosticRenderer::format(source, diag, {.filename = "test.svg", .colorize = true});

  EXPECT_EQ(result,
            "\033[1;33mwarning\033[1;37m: Styled\033[0m\n"
            "\033[1;34m  --> \033[0mtest.svg:2:1\n"
            "\033[1;34m  |\033[0m\n"
            "\033[1;34m 2 | \033[0mdef\n"
            "\033[1;34m  | \033[0m\033[1;33m^~~\033[0m\n");
}

TEST(DiagnosticRenderer, ColorizedErrorUsesRedSeverityAndCaret) {
  const std::string_view source = "abc";
  auto diag = ParseDiagnostic::Error("Styled error",
                                     SourceRange{FileOffset::Offset(1), FileOffset::Offset(2)});

  std::string result = DiagnosticRenderer::format(source, diag, {.colorize = true});

  EXPECT_NE(result.find("\033[1;31merror"), std::string::npos);
  EXPECT_NE(result.find("\033[1;31m^"), std::string::npos);
}

TEST(DiagnosticRenderer, OffsetPastSourceOnlyPrintsMessage) {
  const std::string_view source = "abc";
  auto diag = ParseDiagnostic::Error("Past source",
                                     SourceRange{FileOffset::Offset(4), FileOffset::Offset(5)});

  std::string result = DiagnosticRenderer::format(source, diag);

  EXPECT_EQ(result, "error: Past source\n");
}

TEST(DiagnosticRenderer, CarriageReturnTerminatesRenderedLine) {
  const std::string_view source = "abc\rdef";
  auto diag =
      ParseDiagnostic::Error("CR line", SourceRange{FileOffset::Offset(1), FileOffset::Offset(3)});

  std::string result = DiagnosticRenderer::format(source, diag);

  EXPECT_EQ(result,
            "error: CR line\n"
            "  --> 1:2\n"
            "  |\n"
            " 1 | abc\n"
            "  |  ^~\n");
}

TEST(DiagnosticRenderer, MissingEndOffsetUsesSingleCaret) {
  const std::string_view source = "abc";
  auto diag = ParseDiagnostic::Error("No end",
                                     SourceRange{FileOffset::Offset(1), FileOffset::EndOfString()});

  std::string result = DiagnosticRenderer::format(source, diag);

  EXPECT_EQ(result,
            "error: No end\n"
            "  --> 1:2\n"
            "  |\n"
            " 1 | abc\n"
            "  |  ^\n");
}

TEST(DiagnosticRenderer, RangeStartingAtLineEndUsesSingleCaret) {
  const std::string_view source = "abc\nnext";
  auto diag =
      ParseDiagnostic::Error("Line end", SourceRange{FileOffset::Offset(3), FileOffset::Offset(4)});

  std::string result = DiagnosticRenderer::format(source, diag);

  EXPECT_EQ(result,
            "error: Line end\n"
            "  --> 1:4\n"
            "  |\n"
            " 1 | abc\n"
            "  |    ^\n");
}

}  // namespace donner
