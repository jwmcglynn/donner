/// @file
/// Exercise the GoogleTest runner in fresh processes with real image-case completion.

#include "tools/svg_suite/GtestSuite.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::svg_suite {
namespace {
std::filesystem::path executable;

int RunFixture(int argc, char** argv) {
  const std::filesystem::path root(argv[2]);
  std::filesystem::create_directories(root / "corpus");
  std::ofstream(root / "corpus" / "input.svg") << "<svg/>";
  const Image image{1, 1, 1, {255, 0, 0, 255}};
  if (!WritePng(root / "corpus" / "input.png", {1, 1, 1, image.data}).empty()) {
    return 3;
  }
  argc -= 2;
  argv += 2;
  testing::InitGoogleTest(&argc, argv);
  const auto discovery = DiscoverCases({{"fixture", root / "corpus"}});
  const auto issue = RegisterTests(
      discovery, [image](const StaticCase&, int, int) -> ImageResult { return {image, {}}; },
      root / "artifacts");
  return issue.empty() ? RunSelectedTests() : 3;
}

class GtestSuiteTest : public testing::Test {
protected:
  void SetUp() override {
    const char* temporary = std::getenv("TEST_TMPDIR");
    ASSERT_NE(temporary, nullptr);
    std::string pattern = (std::filesystem::path(temporary) / "gtest-suite-XXXXXX").string();
    ASSERT_NE(mkdtemp(pattern.data()), nullptr);
    root_ = pattern;
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  int run(std::vector<std::string> flags) {
    std::vector<std::string> arguments{executable.string(), "--fixture-suite", root_.string()};
    arguments.insert(arguments.end(), flags.begin(), flags.end());
    std::vector<char*> pointers;
    for (auto& argument : arguments) {
      pointers.push_back(argument.data());
    }
    pointers.push_back(nullptr);
    const pid_t child = fork();
    if (child < 0) {
      return -1;
    }
    if (child == 0) {
      execv(pointers.front(), pointers.data());
      _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
      return -1;
    }
    return WEXITSTATUS(status);
  }

  std::filesystem::path root_;
};

TEST_F(GtestSuiteTest, ListingCasesCannotCountAsExecution) {
  EXPECT_EQ(run({"--gtest_list_tests"}), 2);
}

TEST_F(GtestSuiteTest, ZeroRepeatsCannotCountAsExecution) {
  EXPECT_EQ(run({"--gtest_repeat=0"}), 2);
}

TEST_F(GtestSuiteTest, EmptyFilterCannotCountAsExecution) {
  EXPECT_EQ(run({"--gtest_filter=NothingMatches.*"}), 2);
}

TEST_F(GtestSuiteTest, MatchingFilterExecutesAndPasses) {
  EXPECT_EQ(run({"--gtest_filter=SvgSuite.*"}), 0);
}

}  // namespace
}  // namespace donner::svg_suite

int main(int argc, char** argv) {
  if (argc >= 3 && std::string_view(argv[1]) == "--fixture-suite") {
    return donner::svg_suite::RunFixture(argc, argv);
  }
  donner::svg_suite::executable = std::filesystem::absolute(argv[0]);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
