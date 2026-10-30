#include "donner/svg/text/GraphemeBreak.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace donner::svg {
namespace {

TEST(GraphemeBreakTest, Unicode17OfficialConformance) {
  std::ifstream input("donner/svg/text/tests/unicode/GraphemeBreakTest.txt");
  ASSERT_THAT(input.is_open(), testing::Eq(true));
  size_t cases = 0;
  for (std::string line; std::getline(input, line);) {
    const std::string content = line.substr(0, line.find('#'));
    std::istringstream tokens(content);
    std::string boundary;
    if (!(tokens >> boundary)) {
      continue;
    }
    SCOPED_TRACE(line);
    GraphemeBreaker breaker;
    std::vector<bool> expected;
    std::vector<bool> actual;
    for (std::string codepoint; tokens >> codepoint;) {
      expected.push_back(boundary == "÷");
      actual.push_back(
          breaker.breakBefore(static_cast<char32_t>(std::stoul(codepoint, nullptr, 16))));
      ASSERT_THAT(static_cast<bool>(tokens >> boundary), testing::Eq(true));
    }
    EXPECT_THAT(actual, testing::ElementsAreArray(expected));
    ++cases;
  }
  EXPECT_THAT(cases, testing::Eq(766u));
}

TEST(GraphemeBreakTest, LamAlefCandidatesIncludeTransparentMarks) {
  LamAlefCandidateBreaker units;
  EXPECT_THAT(units.breakBefore(0x644), testing::Eq(true));
  EXPECT_THAT(units.breakBefore(0x64e), testing::Eq(false));
  EXPECT_THAT(units.breakBefore(0x623), testing::Eq(false));
  EXPECT_THAT(units.breakBefore(0x628), testing::Eq(true));
  LamAlefCandidateBreaker extended;
  EXPECT_THAT(extended.breakBefore(0x6b5), testing::Eq(true));
  EXPECT_THAT(extended.breakBefore(0x627), testing::Eq(false));
}

TEST(GraphemeBreakTest, LamAlefCandidatesHonorNonJoinerAndSpecializedProfile) {
  LamAlefCandidateBreaker nonJoiner;
  EXPECT_THAT(nonJoiner.breakBefore(0x644), testing::Eq(true));
  EXPECT_THAT(nonJoiner.breakBefore(0x200c), testing::Eq(false));
  EXPECT_THAT(nonJoiner.breakBefore(0x627), testing::Eq(true));
  LamAlefCandidateBreaker specialized;
  EXPECT_THAT(specialized.breakBefore(0x644), testing::Eq(true));
  EXPECT_THAT(specialized.breakBefore(0x870), testing::Eq(true));
  GraphemeBreaker defaultUnits;
  EXPECT_THAT(defaultUnits.breakBefore(0x644), testing::Eq(true));
  EXPECT_THAT(defaultUnits.breakBefore(0x627), testing::Eq(true));
}

TEST(GraphemeBreakTest, LamAlefCandidatesDoNotTreatJoinerAsTransparent) {
  LamAlefCandidateBreaker units;
  EXPECT_THAT(units.breakBefore(0x644), testing::Eq(true));
  EXPECT_THAT(units.breakBefore(0x200d), testing::Eq(false));
  EXPECT_THAT(units.breakBefore(0x627), testing::Eq(true));
}

TEST(GraphemeBreakTest, StateContinuesAcrossSpanBoundaries) {
  GraphemeBreaker breaker;
  EXPECT_THAT(breaker.breakBefore(U'e'), testing::Eq(true));
  EXPECT_THAT(breaker.breakBefore(0x301), testing::Eq(false));
  EXPECT_THAT(breaker.breakBefore(0x1f469), testing::Eq(true));
  EXPECT_THAT(breaker.breakBefore(0x200d), testing::Eq(false));
  EXPECT_THAT(breaker.breakBefore(0x1f4bb), testing::Eq(false));
  EXPECT_THAT(breaker.breakBefore(U'A'), testing::Eq(true));
}

}  // namespace
}  // namespace donner::svg
