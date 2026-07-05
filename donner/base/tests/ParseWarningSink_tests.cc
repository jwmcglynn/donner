#include "donner/base/ParseWarningSink.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using testing::ElementsAre;
using testing::IsEmpty;

namespace donner {
namespace {

MATCHER_P2(WarningAt, expectedReason, expectedOffset, "") {
  const FileOffset expectedLocation = FileOffset::Offset(expectedOffset);
  return testing::ExplainMatchResult(testing::Eq(DiagnosticSeverity::Warning), arg.severity,
                                     result_listener) &&
         testing::ExplainMatchResult(testing::Eq(std::string(expectedReason)),
                                     std::string(arg.reason.str()), result_listener) &&
         testing::ExplainMatchResult(testing::Eq(expectedLocation), arg.range.start,
                                     result_listener) &&
         testing::ExplainMatchResult(testing::Eq(expectedLocation), arg.range.end, result_listener);
}

}  // namespace

TEST(ParseWarningSink, DefaultIsEnabled) {
  ParseWarningSink sink;
  EXPECT_TRUE(sink.isEnabled());
  EXPECT_FALSE(sink.hasWarnings());
  EXPECT_THAT(sink.warnings(), IsEmpty());
}

TEST(ParseWarningSink, DisabledSink) {
  auto sink = ParseWarningSink::Disabled();
  EXPECT_FALSE(sink.isEnabled());
  EXPECT_FALSE(sink.hasWarnings());
}

TEST(ParseWarningSink, AddDiagnosticDirect) {
  ParseWarningSink sink;
  sink.add(ParseDiagnostic::Warning("test warning", FileOffset::Offset(5)));

  EXPECT_TRUE(sink.hasWarnings());
  EXPECT_THAT(sink.warnings(), ElementsAre(WarningAt("test warning", 5)));
}

TEST(ParseWarningSink, AddDiagnosticViaFactory) {
  ParseWarningSink sink;
  sink.add([&] { return ParseDiagnostic::Warning("lazy warning", FileOffset::Offset(10)); });

  EXPECT_THAT(sink.warnings(), ElementsAre(WarningAt("lazy warning", 10)));
}

TEST(ParseWarningSink, DisabledSinkDropsDirect) {
  auto sink = ParseWarningSink::Disabled();
  sink.add(ParseDiagnostic::Warning("dropped", FileOffset::Offset(0)));

  EXPECT_FALSE(sink.hasWarnings());
  EXPECT_THAT(sink.warnings(), IsEmpty());
}

TEST(ParseWarningSink, DisabledSinkSkipsFactory) {
  auto sink = ParseWarningSink::Disabled();
  bool factoryInvoked = false;
  sink.add([&] {
    factoryInvoked = true;
    return ParseDiagnostic::Warning("should not be created", FileOffset::Offset(0));
  });

  EXPECT_FALSE(factoryInvoked);
  EXPECT_FALSE(sink.hasWarnings());
}

TEST(ParseWarningSink, DisabledSinkSkipsFormattingOverhead) {
  auto sink = ParseWarningSink::Disabled();
  bool formatCalled = false;
  sink.add([&] {
    formatCalled = true;
    // Simulate expensive formatting.
    return ParseDiagnostic::Warning(
        RcString::fromFormat("Unknown attribute '{}'", std::string_view("expensive")),
        FileOffset::Offset(0));
  });

  EXPECT_FALSE(formatCalled);
}

TEST(ParseWarningSink, Merge) {
  ParseWarningSink sink1;
  sink1.add(ParseDiagnostic::Warning("warning 1", FileOffset::Offset(0)));

  ParseWarningSink sink2;
  sink2.add(ParseDiagnostic::Warning("warning 2", FileOffset::Offset(5)));
  sink2.add(ParseDiagnostic::Warning("warning 3", FileOffset::Offset(10)));

  sink1.merge(std::move(sink2));

  EXPECT_THAT(sink1.warnings(), ElementsAre(WarningAt("warning 1", 0), WarningAt("warning 2", 5),
                                            WarningAt("warning 3", 10)));
}

TEST(ParseWarningSink, MergeIntoDisabledDrops) {
  auto sink1 = ParseWarningSink::Disabled();

  ParseWarningSink sink2;
  sink2.add(ParseDiagnostic::Warning("warning", FileOffset::Offset(0)));

  sink1.merge(std::move(sink2));

  EXPECT_THAT(sink1.warnings(), IsEmpty());
}

TEST(ParseWarningSink, MultipleWarnings) {
  ParseWarningSink sink;
  sink.add(ParseDiagnostic::Warning("first", FileOffset::Offset(0)));
  sink.add(ParseDiagnostic::Warning("second", FileOffset::Offset(5)));
  sink.add(ParseDiagnostic::Warning("third", FileOffset::Offset(10)));

  EXPECT_THAT(sink.warnings(),
              ElementsAre(WarningAt("first", 0), WarningAt("second", 5), WarningAt("third", 10)));
}

}  // namespace donner
