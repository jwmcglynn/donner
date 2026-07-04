#include "donner/svg/tool/DonnerSvgTool.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::svg {
namespace {

int RunTool(const std::vector<std::string>& args, std::ostringstream* out,
            std::ostringstream* err) {
  std::vector<std::string> storage;
  storage.emplace_back("donner-svg");
  storage.insert(storage.end(), args.begin(), args.end());

  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }

  return RunDonnerSvgTool(static_cast<int>(argv.size()), argv.data(), *out, *err);
}

class DonnerSvgToolFileTest : public ::testing::Test {
protected:
  void SetUp() override {
    const ::testing::TestInfo* const testInfo =
        ::testing::UnitTest::GetInstance()->current_test_info();
    tmpDir_ = std::filesystem::path(::testing::TempDir()) /
              ("donner_svg_tool_" + std::string(testInfo->name()));
    std::error_code error;
    std::filesystem::remove_all(tmpDir_, error);
    std::filesystem::create_directories(tmpDir_);
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(tmpDir_, error);
  }

  std::filesystem::path WriteSvg(std::string_view name, std::string_view source) {
    const std::filesystem::path path = tmpDir_ / std::string(name);
    std::ofstream file(path, std::ios::binary);
    file.write(source.data(), static_cast<std::streamsize>(source.size()));
    return path;
  }

  std::array<unsigned char, 8> ReadPngMagic(const std::filesystem::path& path) {
    std::array<unsigned char, 8> magic{};
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
    return magic;
  }

  std::filesystem::path tmpDir_;
};

constexpr std::string_view kSimpleSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="3" viewBox="0 0 4 3">
         <rect width="4" height="3" fill="#ff0000"/>
       </svg>)";

TEST(DonnerSvgTool, HelpFlagPrintsPreviewAndReturnsZero) {
  char arg0[] = "donner-svg";
  char arg1[] = "--help";
  char* argv[] = {arg0, arg1};

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(RunDonnerSvgTool(2, argv, out, err), 0);
  EXPECT_THAT(out.str(), testing::HasSubstr("--preview"));
  EXPECT_TRUE(err.str().empty());
}

TEST(DonnerSvgTool, UnknownFlagReturnsOne) {
  char arg0[] = "donner-svg";
  char arg1[] = "--bogus";
  char* argv[] = {arg0, arg1};

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(RunDonnerSvgTool(2, argv, out, err), 1);
  EXPECT_THAT(err.str(), testing::HasSubstr("Unknown option: --bogus"));
}

TEST(DonnerSvgTool, MissingInputFileReturnsUsageError) {
  std::ostringstream out;
  std::ostringstream err;

  EXPECT_EQ(RunTool({}, &out, &err), 1);
  EXPECT_TRUE(out.str().empty());
  EXPECT_THAT(err.str(), testing::HasSubstr("Missing input SVG file"));
  EXPECT_THAT(err.str(), testing::HasSubstr("USAGE:"));
}

TEST(DonnerSvgTool, PreviewFlagParsesAndThenFailsOnMissingInputFile) {
  char arg0[] = "donner-svg";
  char arg1[] = "--preview";
  char arg2[] = "does_not_exist.svg";
  char* argv[] = {arg0, arg1, arg2};

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(RunDonnerSvgTool(3, argv, out, err), 2);
  EXPECT_THAT(err.str(), testing::HasSubstr("Failed to read input SVG"));
}

TEST(DonnerSvgTool, MissingFlagValueReturnsUsageError) {
  std::ostringstream out;
  std::ostringstream err;

  EXPECT_EQ(RunTool({"--output"}, &out, &err), 1);
  EXPECT_THAT(err.str(), testing::HasSubstr("Missing value for --output"));
  EXPECT_THAT(err.str(), testing::HasSubstr("USAGE:"));
}

TEST(DonnerSvgTool, InvalidWidthReturnsUsageError) {
  std::ostringstream out;
  std::ostringstream err;

  EXPECT_EQ(RunTool({"--width", "0", "input.svg"}, &out, &err), 1);
  EXPECT_THAT(err.str(), testing::HasSubstr("Invalid --width value: 0"));
}

TEST(DonnerSvgTool, EmptyWidthReturnsUsageError) {
  std::ostringstream out;
  std::ostringstream err;

  EXPECT_EQ(RunTool({"--width", "", "input.svg"}, &out, &err), 1);
  EXPECT_THAT(err.str(), testing::HasSubstr("Invalid --width value:"));
}

TEST(DonnerSvgTool, InvalidHeightReturnsUsageError) {
  std::ostringstream out;
  std::ostringstream err;

  EXPECT_EQ(RunTool({"--height", "wide", "input.svg"}, &out, &err), 1);
  EXPECT_THAT(err.str(), testing::HasSubstr("Invalid --height value: wide"));
}

TEST(DonnerSvgTool, RejectsMultipleInputFiles) {
  std::ostringstream out;
  std::ostringstream err;

  EXPECT_EQ(RunTool({"first.svg", "second.svg"}, &out, &err), 1);
  EXPECT_THAT(err.str(), testing::HasSubstr("Only one input SVG is supported"));
}

TEST_F(DonnerSvgToolFileTest, EmptyInputFileReturnsParseError) {
  const std::filesystem::path inputPath = WriteSvg("empty.svg", "");

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(RunTool({inputPath.string()}, &out, &err), 3);

  EXPECT_THAT(err.str(), testing::HasSubstr("Parse error:"));
}

TEST_F(DonnerSvgToolFileTest, RendersSvgToPngWithCanvasOverrides) {
  const std::filesystem::path inputPath = WriteSvg("input.svg", kSimpleSvg);
  const std::filesystem::path outputPath = tmpDir_ / "render.png";

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(RunTool({"--width", "7", "--height", "5", "--output", outputPath.string(),
                     inputPath.string()},
                    &out, &err),
            0);

  EXPECT_TRUE(err.str().empty());
  EXPECT_THAT(out.str(), testing::HasSubstr("Saved PNG:"));
  EXPECT_THAT(out.str(), testing::HasSubstr("Rendered size: 7x5"));
  ASSERT_TRUE(std::filesystem::exists(outputPath));
  EXPECT_THAT(ReadPngMagic(outputPath),
              testing::ElementsAre(0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'));
}

TEST_F(DonnerSvgToolFileTest, HeightOnlyOverrideIsAccepted) {
  const std::filesystem::path inputPath = WriteSvg("input.svg", kSimpleSvg);
  const std::filesystem::path outputPath = tmpDir_ / "height-only.png";

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(
      RunTool({"--height", "9", "--output", outputPath.string(), inputPath.string()}, &out, &err),
      0);

  EXPECT_TRUE(err.str().empty());
  EXPECT_THAT(out.str(), testing::HasSubstr("Rendered size: 4x3"));
  ASSERT_TRUE(std::filesystem::exists(outputPath));
  EXPECT_THAT(ReadPngMagic(outputPath),
              testing::ElementsAre(0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'));
}

TEST_F(DonnerSvgToolFileTest, WidthOnlyOverrideIsAccepted) {
  const std::filesystem::path inputPath = WriteSvg("input.svg", kSimpleSvg);
  const std::filesystem::path outputPath = tmpDir_ / "width-only.png";

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(
      RunTool({"--width", "8", "--output", outputPath.string(), inputPath.string()}, &out, &err),
      0);

  EXPECT_TRUE(err.str().empty());
  EXPECT_THAT(out.str(), testing::HasSubstr("Rendered size: 4x3"));
  ASSERT_TRUE(std::filesystem::exists(outputPath));
  EXPECT_THAT(ReadPngMagic(outputPath),
              testing::ElementsAre(0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'));
}

TEST_F(DonnerSvgToolFileTest, ParseErrorReturnsDistinctExitCode) {
  const std::filesystem::path inputPath = WriteSvg("bad.svg", "<svg><");

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(RunTool({inputPath.string()}, &out, &err), 3);

  EXPECT_THAT(err.str(), testing::HasSubstr("Parse error:"));
}

TEST_F(DonnerSvgToolFileTest, ParseWarningsArePrintedUnlessQuiet) {
  const std::filesystem::path inputPath =
      WriteSvg("warning.svg",
               R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
                    <rect id="r" x="bad" width="2" height="2"/>
                  </svg>)");

  std::ostringstream noisyOut;
  std::ostringstream noisyErr;
  EXPECT_EQ(RunTool({"--preview", inputPath.string()}, &noisyOut, &noisyErr), 0);
  EXPECT_TRUE(noisyErr.str().empty());
  EXPECT_THAT(noisyOut.str(), testing::HasSubstr("Parse warnings:"));
  EXPECT_THAT(noisyOut.str(), testing::HasSubstr("warning.svg"));

  std::ostringstream quietOut;
  std::ostringstream quietErr;
  EXPECT_EQ(RunTool({"--quiet", "--preview", inputPath.string()}, &quietOut, &quietErr), 0);
  EXPECT_TRUE(quietErr.str().empty());
  EXPECT_THAT(quietOut.str(), testing::Not(testing::HasSubstr("Parse warnings:")));
  EXPECT_THAT(quietOut.str(), testing::HasSubstr("Rendered size: 4x4"));
}

TEST_F(DonnerSvgToolFileTest, SaveFailureReturnsDistinctExitCode) {
  const std::filesystem::path inputPath = WriteSvg("input.svg", kSimpleSvg);
  const std::filesystem::path outputDirectory = tmpDir_ / "output-directory";
  std::filesystem::create_directories(outputDirectory);

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(RunTool({"--output", outputDirectory.string(), inputPath.string()}, &out, &err), 4);

  EXPECT_THAT(err.str(), testing::HasSubstr("Failed to save PNG:"));
}

TEST_F(DonnerSvgToolFileTest, OutputAndPreviewSavesThenRendersTerminalPreview) {
  const std::filesystem::path inputPath = WriteSvg("input.svg", kSimpleSvg);
  const std::filesystem::path outputPath = tmpDir_ / "preview-and-save.png";

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(RunTool({"--preview", "--output", outputPath.string(), inputPath.string()}, &out, &err),
            0);

  EXPECT_TRUE(err.str().empty());
  EXPECT_THAT(out.str(), testing::HasSubstr("Saved PNG:"));
  EXPECT_THAT(out.str(), testing::HasSubstr("Rendered size: 4x3"));
  ASSERT_TRUE(std::filesystem::exists(outputPath));
}

TEST_F(DonnerSvgToolFileTest, PreviewRendersWithoutWritingDefaultPng) {
  const std::filesystem::path inputPath = WriteSvg("input.svg", kSimpleSvg);

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(RunTool({"--preview", inputPath.string()}, &out, &err), 0);

  EXPECT_TRUE(err.str().empty());
  EXPECT_THAT(out.str(), testing::HasSubstr("Rendered size: 4x3"));
  EXPECT_THAT(out.str(), testing::Not(testing::HasSubstr("Saved PNG:")));
}

TEST_F(DonnerSvgToolFileTest, ExperimentalFlagIsForwardedToParser) {
  const std::filesystem::path inputPath = WriteSvg("filter.svg", R"SVG(
    <svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
      <defs>
        <filter id="f"><feGaussianBlur stdDeviation="0.2"/></filter>
      </defs>
      <rect width="4" height="4" fill="red" filter="url(#f)"/>
    </svg>
  )SVG");

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(RunTool({"--experimental", "--preview", inputPath.string()}, &out, &err), 0);

  EXPECT_TRUE(err.str().empty());
  EXPECT_THAT(out.str(), testing::HasSubstr("Rendered size: 4x4"));
}

TEST_F(DonnerSvgToolFileTest, VerboseFlagIsAcceptedBeforeInputReadFailure) {
  std::ostringstream out;
  std::ostringstream err;

  EXPECT_EQ(RunTool({"--verbose", (tmpDir_ / "missing.svg").string()}, &out, &err), 2);

  EXPECT_TRUE(out.str().empty());
  EXPECT_THAT(err.str(), testing::HasSubstr("Failed to read input SVG"));
}

TEST_F(DonnerSvgToolFileTest, InteractiveModeFallsBackWhenStdinIsNotATty) {
  const std::filesystem::path inputPath = WriteSvg("input.svg", kSimpleSvg);

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(RunTool({"--interactive", inputPath.string()}, &out, &err), 0);

  EXPECT_TRUE(err.str().empty());
  EXPECT_THAT(out.str(), testing::HasSubstr("Rendered size: 4x3"));
  EXPECT_THAT(out.str(), testing::HasSubstr("Interactive mode needs a TTY terminal."));
  EXPECT_THAT(out.str(), testing::Not(testing::HasSubstr("Saved PNG:")));
}

}  // namespace
}  // namespace donner::svg
