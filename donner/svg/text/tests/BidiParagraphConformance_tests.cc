#include <gtest/gtest.h>

#include <array>
#include <charconv>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/tests/Runfiles.h"
#include "donner/svg/text/BidiParagraph.h"

namespace donner::svg {
namespace {

constexpr size_t kShardCount = 64;
constexpr size_t kCharacterRows = 91707;
constexpr size_t kPropertyRows = 490846;
constexpr std::array<size_t, kShardCount> kPropertyDirections = {
    12022, 12039, 12036, 12075, 12060, 12043, 12008, 11999, 12018, 12018, 12030, 12078, 12054,
    12059, 12041, 12029, 12051, 12046, 12046, 12048, 12044, 12017, 12014, 12033, 12044, 12017,
    12007, 12015, 12018, 12034, 12015, 12009, 12017, 12000, 11997, 12019, 12057, 12048, 12038,
    12025, 12015, 12019, 12036, 12008, 12038, 11993, 12001, 12005, 12038, 12069, 12053, 12037,
    12040, 12047, 12029, 12056, 12052, 12059, 12077, 12075, 12074, 12074, 12053, 12025,
};

std::vector<std::string> ReadCorpus(const std::string& filename) {
  std::ifstream input(Runfiles::instance().RlocationExternal("sheenbidi_unicode_data",
                                                             "Tools/Unicode/" + filename));
  std::vector<std::string> result;
  for (std::string line; std::getline(input, line);) {
    result.push_back(std::move(line));
  }
  return result;
}

std::vector<std::string> Fields(std::string_view text) {
  std::vector<std::string> result;
  size_t start = 0;
  for (size_t end = text.find(';'); end != std::string_view::npos; end = text.find(';', start)) {
    result.emplace_back(text.substr(start, end - start));
    start = end + 1;
  }
  result.emplace_back(text.substr(start));
  return result;
}

int Number(std::string_view text, int base = 10) {
  const size_t start = text.find_first_not_of(" \t\r");
  const size_t end = text.find_last_not_of(" \t\r");
  if (start == std::string_view::npos) {
    ADD_FAILURE() << "Missing integer";
    return -1;
  }
  text = text.substr(start, end - start + 1);
  int result = -1;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result, base);
  EXPECT_EQ(parsed.ec, std::errc{});
  EXPECT_EQ(parsed.ptr, text.data() + text.size());
  return result;
}

std::vector<int> Numbers(std::string_view text, int base = 10) {
  std::istringstream input{std::string(text)};
  std::vector<int> result;
  for (std::string token; input >> token;) {
    result.push_back(token == "x" ? -1 : Number(token, base));
  }
  return result;
}

// Representatives have the named Bidi_Class and no paired-bracket property.
char32_t Representative(std::string_view bidiClass) {
  constexpr std::array<std::pair<std::string_view, char32_t>, 23> kRepresentatives = {{
      {"L", U'a'},     {"R", 0x05d0},   {"AL", 0x0627},  {"EN", U'0'},    {"ES", U'+'},
      {"ET", U'$'},    {"AN", 0x0660},  {"CS", U','},    {"NSM", 0x0300}, {"BN", 0x00ad},
      {"B", U'\n'},    {"S", U'\t'},    {"WS", U' '},    {"ON", U'!'},    {"LRE", 0x202a},
      {"LRO", 0x202d}, {"RLE", 0x202b}, {"RLO", 0x202e}, {"PDF", 0x202c}, {"LRI", 0x2066},
      {"RLI", 0x2067}, {"FSI", 0x2068}, {"PDI", 0x2069},
  }};
  for (const auto& [name, codepoint] : kRepresentatives) {
    if (name == bidiClass) {
      return codepoint;
    }
  }
  ADD_FAILURE() << "Unknown Bidi_Class: " << bidiClass;
  return 0xfffd;
}

void CheckCase(std::vector<char32_t> codepoints, uint8_t base, int resolvedBase,
               const std::vector<int>& expectedLevels, const std::vector<int>& expectedOrder) {
  ASSERT_EQ(codepoints.size(), expectedLevels.size());
  BidiResourceBudget budget;
  const auto paragraph = BidiParagraph::Analyze(std::move(codepoints), budget, base, true);
  ASSERT_TRUE(paragraph.has_value());
  if (resolvedBase >= 0) {
    EXPECT_EQ(paragraph->paragraphLevelAt(0), resolvedBase);
  }
  const auto runs = paragraph->visualRuns(0, static_cast<uint32_t>(expectedLevels.size()), budget);
  ASSERT_TRUE(runs.has_value());
  std::vector<int> actualLevels(expectedLevels.size(), -1);
  std::vector<int> actualOrder;
  for (const auto& run : *runs) {
    ASSERT_LT(run.start, run.end);
    ASSERT_LE(run.end, expectedLevels.size());
    for (uint32_t index = run.start; index < run.end; ++index) {
      ASSERT_EQ(actualLevels[index], -1) << "Overlapping visual runs";
      actualLevels[index] = run.level;
    }
    for (uint32_t offset = 0; offset < run.end - run.start; ++offset) {
      const uint32_t index = (run.level & 1) ? run.end - offset - 1 : run.start + offset;
      if (expectedLevels[index] >= 0) {
        actualOrder.push_back(static_cast<int>(index));
      }
    }
  }
  for (size_t index = 0; index < expectedLevels.size(); ++index) {
    if (expectedLevels[index] >= 0) {
      // Unicode expects levels after L1; levelAt() intentionally reports levels before L1.
      EXPECT_EQ(actualLevels[index], expectedLevels[index]) << "Logical index " << index;
    }
  }
  EXPECT_EQ(actualOrder, expectedOrder);
  EXPECT_FALSE(budget.rejected);
}

class BidiParagraphConformance : public testing::TestWithParam<size_t> {};

TEST_P(BidiParagraphConformance, Unicode17Characters) {
  static const auto kLines = ReadCorpus("BidiCharacterTest.txt");
  ASSERT_FALSE(kLines.empty());
  ASSERT_EQ(kLines.front(), "# BidiCharacterTest-17.0.0.txt");
  size_t rows = 0;
  size_t executed = 0;
  for (const std::string& line : kLines) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    if (rows++ % kShardCount != GetParam()) {
      continue;
    }
    SCOPED_TRACE(line);
    const auto fields = Fields(line);
    ASSERT_EQ(fields.size(), 5u);
    std::vector<char32_t> codepoints;
    for (const int codepoint : Numbers(fields[0], 16)) {
      ASSERT_GE(codepoint, 0);
      codepoints.push_back(static_cast<char32_t>(codepoint));
    }
    const int base = Number(fields[1]);
    ASSERT_GE(base, 0);
    ASSERT_LE(base, 2);
    CheckCase(std::move(codepoints), static_cast<uint8_t>(base), Number(fields[2]),
              Numbers(fields[3]), Numbers(fields[4]));
    ASSERT_FALSE(HasFailure());
    ++executed;
  }
  EXPECT_EQ(rows, kCharacterRows);
  EXPECT_EQ(executed, (kCharacterRows + kShardCount - 1 - GetParam()) / kShardCount);
}

TEST_P(BidiParagraphConformance, Unicode17BidiClasses) {
  static const auto kLines = ReadCorpus("BidiTest.txt");
  ASSERT_FALSE(kLines.empty());
  ASSERT_EQ(kLines.front(), "# BidiTest-17.0.0.txt");
  std::vector<int> expectedLevels;
  std::vector<int> expectedOrder;
  size_t rows = 0;
  size_t executed = 0;
  size_t directions = 0;
  for (const std::string& line : kLines) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    if (line.starts_with("@Levels:")) {
      expectedLevels = Numbers(std::string_view(line).substr(8));
      continue;
    }
    if (line.starts_with("@Reorder:")) {
      expectedOrder = Numbers(std::string_view(line).substr(9));
      continue;
    }
    if (line.front() == '@' || rows++ % kShardCount != GetParam()) {
      continue;
    }
    SCOPED_TRACE(line);
    const auto fields = Fields(line);
    ASSERT_EQ(fields.size(), 2u);
    std::istringstream input{std::string(fields[0])};
    std::vector<char32_t> codepoints;
    for (std::string bidiClass; input >> bidiClass;) {
      codepoints.push_back(Representative(bidiClass));
    }
    const int mask = Number(fields[1], 16);
    ASSERT_GE(mask, 1);
    ASSERT_LE(mask, 7);
    constexpr std::array<uint8_t, 3> kBases = {2, 0, 1};
    for (size_t bit = 0; bit < kBases.size(); ++bit) {
      if (mask & (1 << bit)) {
        SCOPED_TRACE(testing::Message() << "Paragraph base " << static_cast<int>(kBases[bit]));
        CheckCase(codepoints, kBases[bit], -1, expectedLevels, expectedOrder);
        ASSERT_FALSE(HasFailure());
        ++directions;
      }
    }
    ++executed;
  }
  EXPECT_EQ(rows, kPropertyRows);
  EXPECT_EQ(executed, (kPropertyRows + kShardCount - 1 - GetParam()) / kShardCount);
  EXPECT_EQ(directions, kPropertyDirections[GetParam()]);
}

INSTANTIATE_TEST_SUITE_P(AllRows, BidiParagraphConformance, testing::Range(size_t{0}, kShardCount));

}  // namespace
}  // namespace donner::svg
