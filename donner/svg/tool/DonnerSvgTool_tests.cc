#include "donner/svg/tool/DonnerSvgTool.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "donner/base/PackagedResource.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/tests/GifTestDecoder.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::svg {
namespace {

RendererBitmap ToBitmap(const std::vector<uint8_t>& rgba, int width, int height) {
  RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(width, height);
  bitmap.pixels = rgba;
  bitmap.rowBytes = static_cast<size_t>(width) * 4;
  bitmap.alphaType = AlphaType::Unpremultiplied;
  return bitmap;
}

void ExpectFrameIdentical(const DecodedGif& gif, size_t frameIndex,
                          const std::vector<uint8_t>& expected) {
  ASSERT_LT(frameIndex, gif.frames.size());
  const std::string label =
      std::string(testing::UnitTest::GetInstance()->current_test_info()->name()) + "_frame" +
      std::to_string(frameIndex);
  editor::tests::CompareBitmapToBitmap(ToBitmap(gif.frames[frameIndex].rgba, gif.width, gif.height),
                                       ToBitmap(expected, gif.width, gif.height), label,
                                       editor::tests::PixelmatchIdentityParams());
}

int RunTool(const std::vector<std::string>& args, std::ostringstream* out,
            std::ostringstream* err) {
  std::vector<std::string> storage;
  storage.emplace_back("donner-cli");
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

  std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }

  std::filesystem::path tmpDir_;
};

class ScopedPseudoTerminalStdin {
public:
  ScopedPseudoTerminalStdin() {
    savedStdin_ = dup(STDIN_FILENO);
    masterFd_ = posix_openpt(O_RDWR | O_NOCTTY);
    if (masterFd_ < 0 || grantpt(masterFd_) != 0 || unlockpt(masterFd_) != 0) {
      return;
    }

    const char* slaveName = ptsname(masterFd_);
    if (slaveName == nullptr) {
      return;
    }

    slaveFd_ = open(slaveName, O_RDWR | O_NOCTTY);
    if (savedStdin_ < 0 || slaveFd_ < 0 || dup2(slaveFd_, STDIN_FILENO) < 0) {
      return;
    }

    active_ = true;
  }

  ~ScopedPseudoTerminalStdin() {
    if (active_) {
      (void)dup2(savedStdin_, STDIN_FILENO);
    }
    if (slaveFd_ >= 0) {
      close(slaveFd_);
    }
    if (masterFd_ >= 0) {
      close(masterFd_);
    }
    if (savedStdin_ >= 0) {
      close(savedStdin_);
    }
  }

  bool active() const { return active_; }
  int masterFd() const { return masterFd_; }

private:
  int savedStdin_ = -1;
  int masterFd_ = -1;
  int slaveFd_ = -1;
  bool active_ = false;
};

class ScopedWorkingDirectory {
public:
  explicit ScopedWorkingDirectory(const std::filesystem::path& path)
      : previous_(std::filesystem::current_path()) {
    std::filesystem::current_path(path);
  }

  ~ScopedWorkingDirectory() {
    std::error_code error;
    std::filesystem::current_path(previous_, error);
  }

private:
  std::filesystem::path previous_;
};

constexpr std::string_view kSimpleSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="3" viewBox="0 0 4 3">
         <rect width="4" height="3" fill="#ff0000"/>
       </svg>)";

TEST_F(DonnerSvgToolFileTest, ResourceStartupRequiresOnlyEnabledTextResources) {
  const auto input = WriteSvg("input.svg", kSimpleSvg);
  const auto output = tmpDir_ / "output.png";
#ifdef DONNER_TEXT_ENABLED
  constexpr int kExpectedExitCode = 2;
  constexpr char kExpectedError[] = "Failed to load packaged resources:";
#else
  constexpr int kExpectedExitCode = 0;
  constexpr char kExpectedError[] = "";
#endif
  EXPECT_EXIT(
      {
        auto unavailable = std::make_shared<PackagedResourceSession>(
            [](const PackagedResource&) -> ResourceBytes { return {}; });
        if (!SetPackagedResourceSession(std::move(unavailable))) {
          std::_Exit(3);
        }
        std::ostringstream out;
        std::ostringstream err;
        const int result = RunTool({input.string(), "--output", output.string()}, &out, &err);
        const std::string message = err.str();
        (void)write(STDERR_FILENO, message.data(), message.size());
        std::_Exit(result);
      },
      testing::ExitedWithCode(kExpectedExitCode), kExpectedError);
#ifndef DONNER_TEXT_ENABLED
  EXPECT_THAT(ReadPngMagic(output),
              testing::ElementsAre(0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A));
#endif
}

TEST(DonnerSvgTool, HelpFlagPrintsPreviewAndReturnsZero) {
  char arg0[] = "donner-cli";
  char arg1[] = "--help";
  char* argv[] = {arg0, arg1};

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(RunDonnerSvgTool(2, argv, out, err), 0);
  EXPECT_THAT(out.str(), testing::HasSubstr("--preview"));
  EXPECT_TRUE(err.str().empty());
}

TEST(DonnerSvgTool, UnknownFlagReturnsOne) {
  char arg0[] = "donner-cli";
  char arg1[] = "--bogus";
  char* argv[] = {arg0, arg1};

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(RunDonnerSvgTool(2, argv, out, err), 1);
  EXPECT_THAT(err.str(), testing::HasSubstr("Unknown option: --bogus"));
}

TEST(DonnerSvgTool, EscapesTerminalControlsInRejectedArguments) {
  std::ostringstream out;
  std::ostringstream err;

  const std::string argument =
      std::string("--bad\x1b]8;;https://example.test\a") + "\xe2\x80\xae" + "txt";
  EXPECT_EQ(RunTool({argument}, &out, &err), 1);
  EXPECT_THAT(err.str(), testing::HasSubstr(R"(--bad\x1b]8;;https://example.test\x07\u202etxt)"));
  EXPECT_THAT(err.str(), testing::Not(testing::HasSubstr("\x1b]8")));
  EXPECT_THAT(err.str(), testing::Not(testing::HasSubstr(std::string("\xe2\x80\xae", 3))));
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
  char arg0[] = "donner-cli";
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

TEST(DonnerSvgTool, GifRequiresEnd) {
  std::ostringstream out;
  std::ostringstream err;

  EXPECT_EQ(RunTool({"--gif", "out.gif", "input.svg"}, &out, &err), 1);
  EXPECT_THAT(err.str(), testing::HasSubstr("--gif requires --end"));
}

TEST(DonnerSvgTool, GifRejectsEndBeforeStart) {
  std::ostringstream out;
  std::ostringstream err;

  EXPECT_EQ(RunTool({"--gif", "out.gif", "--start", "2", "--end", "1", "input.svg"}, &out, &err),
            1);
  EXPECT_THAT(err.str(), testing::HasSubstr("--end must be greater than --start"));
}

TEST(DonnerSvgTool, GifRejectsPngOutputAndPreview) {
  std::ostringstream out;
  std::ostringstream err;

  EXPECT_EQ(
      RunTool({"--gif", "out.gif", "--end", "1", "--output", "out.png", "input.svg"}, &out, &err),
      1);
  EXPECT_THAT(err.str(), testing::HasSubstr("--gif cannot be combined with --output"));
}

TEST(DonnerSvgTool, AnimationOptionsRequireGif) {
  std::ostringstream out;
  std::ostringstream err;

  EXPECT_EQ(RunTool({"--fps", "10", "input.svg"}, &out, &err), 1);
  EXPECT_THAT(err.str(), testing::HasSubstr("require --gif"));

  // Values equal to the defaults are still rejected without --gif.
  err.str("");
  EXPECT_EQ(RunTool({"--start", "0", "input.svg"}, &out, &err), 1);
  EXPECT_THAT(err.str(), testing::HasSubstr("require --gif"));
}

TEST(DonnerSvgTool, GifRejectsInteractive) {
  std::ostringstream out;
  std::ostringstream err;

  EXPECT_EQ(RunTool({"--gif", "out.gif", "--end", "1", "--interactive", "input.svg"}, &out, &err),
            1);
  EXPECT_THAT(err.str(), testing::HasSubstr("--gif cannot be combined"));
}

TEST(DonnerSvgTool, RejectsInvalidAnimationValues) {
  std::ostringstream out;
  std::ostringstream err;

  EXPECT_EQ(RunTool({"--gif", "out.gif", "--end", "1", "--fps", "0", "input.svg"}, &out, &err), 1);
  EXPECT_THAT(err.str(), testing::HasSubstr("Invalid --fps value: 0"));

  err.str("");
  EXPECT_EQ(RunTool({"--gif", "out.gif", "--end", "nan", "input.svg"}, &out, &err), 1);
  EXPECT_THAT(err.str(), testing::HasSubstr("Invalid --end value: nan"));

  err.str("");
  EXPECT_EQ(RunTool({"--gif", "out.gif", "--end", "1", "--background", "notacolor", "input.svg"},
                    &out, &err),
            1);
  EXPECT_THAT(err.str(), testing::HasSubstr("Invalid --background color (must be an opaque CSS "
                                            "color): notacolor"));

  err.str("");
  EXPECT_EQ(RunTool({"--gif", "out.gif", "--end", "1", "--background", "rgba(255, 0, 0, 0.5)",
                     "input.svg"},
                    &out, &err),
            1);
  EXPECT_THAT(err.str(), testing::HasSubstr("must be an opaque CSS color"));

  err.str("");
  EXPECT_EQ(RunTool({"--gif", "out.gif", "--end", "100000", "input.svg"}, &out, &err), 1);
  EXPECT_THAT(err.str(), testing::HasSubstr("more than 10000 frames"));

  err.str("");
  EXPECT_EQ(
      RunTool({"--gif", "out.gif", "--end", "1000", "--fps", "0.001", "input.svg"}, &out, &err), 1);
  EXPECT_THAT(err.str(), testing::HasSubstr("--fps must be at least"));
}

TEST(DonnerSvgTool, ScaleCannotCombineWithWidth) {
  std::ostringstream out;
  std::ostringstream err;

  EXPECT_EQ(RunTool({"--scale", "2", "--width", "10", "input.svg"}, &out, &err), 1);
  EXPECT_THAT(err.str(), testing::HasSubstr("--scale cannot be combined"));
}

constexpr std::string_view kAnimatedSvg = R"(
<svg xmlns="http://www.w3.org/2000/svg" width="20" height="10" viewBox="0 0 20 10">
  <rect width="20" height="10" fill="white"/>
  <rect x="0" y="2" width="4" height="6" fill="blue">
    <animate attributeName="x" from="0" to="16" dur="1s" fill="freeze"/>
  </rect>
</svg>
)";

TEST_F(DonnerSvgToolFileTest, RendersAnimationToLoopingGif) {
  const std::filesystem::path inputPath = WriteSvg("animated.svg", kAnimatedSvg);
  const std::filesystem::path gifPath = tmpDir_ / "animated.gif";

  std::ostringstream out;
  std::ostringstream err;
  ASSERT_EQ(RunTool({"--gif", gifPath.string(), "--end", "1", "--fps", "4", "--scale", "2",
                     "--background", "white", inputPath.string()},
                    &out, &err),
            0)
      << err.str();

  EXPECT_TRUE(err.str().empty());
  EXPECT_THAT(out.str(), testing::HasSubstr("Saved GIF:"));
  EXPECT_THAT(out.str(), testing::HasSubstr("Rendered size: 40x20"));
  EXPECT_THAT(out.str(), testing::HasSubstr("Frames: 4 rendered, 4 stored"));

  const std::string bytes = ReadFile(gifPath);
  std::string error;
  const std::optional<DecodedGif> gif = DecodeGifForTesting(
      std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), &error);
  ASSERT_TRUE(gif.has_value()) << error;

  ASSERT_EQ(gif->width, 40);
  ASSERT_EQ(gif->height, 20);
  EXPECT_THAT(gif->loopCount, testing::Optional(0));
  ASSERT_THAT(gif->frames, testing::SizeIs(4));
  EXPECT_THAT(gif->frames, testing::Each(testing::Field(&DecodedGifFrame::delayCentiseconds, 25)));
  for (size_t frameIndex = 0; frameIndex < gif->frames.size(); ++frameIndex) {
    std::vector<uint8_t> expected(static_cast<size_t>(gif->width * gif->height * 4), 255);
    const int rectLeft = static_cast<int>(frameIndex) * 8;
    for (int y = 4; y < 16; ++y) {
      for (int x = rectLeft; x < rectLeft + 8; ++x) {
        const size_t offset = static_cast<size_t>((y * gif->width + x) * 4);
        expected[offset] = 0;
        expected[offset + 1] = 0;
        expected[offset + 2] = 255;
      }
    }
    ExpectFrameIdentical(*gif, frameIndex, expected);
  }
}

TEST_F(DonnerSvgToolFileTest, GifFrameCountCoversEveryFrameBeforeEnd) {
  const std::filesystem::path inputPath = WriteSvg("animated.svg", kAnimatedSvg);
  const std::filesystem::path gifPath = tmpDir_ / "animated.gif";

  std::ostringstream out;
  std::ostringstream err;
  // Frames at 0, 0.4 and 0.8 s fall before the 1 s end.
  ASSERT_EQ(RunTool({"--gif", gifPath.string(), "--end", "1", "--fps", "2.5", "--background",
                     "white", inputPath.string()},
                    &out, &err),
            0)
      << err.str();
  EXPECT_THAT(out.str(), testing::HasSubstr("Frames: 3 rendered"));

  // The sample at t = 1 s is still before an end of 1.0000005 s.
  out.str("");
  ASSERT_EQ(RunTool({"--gif", gifPath.string(), "--end", "1.0000005", "--fps", "1", "--background",
                     "white", inputPath.string()},
                    &out, &err),
            0)
      << err.str();
  EXPECT_THAT(out.str(), testing::HasSubstr("Frames: 2 rendered"));

  // An exact multiple of the frame period does not gain a frame at the end time.
  out.str("");
  ASSERT_EQ(RunTool({"--gif", gifPath.string(), "--end", "1", "--fps", "30", "--background",
                     "white", inputPath.string()},
                    &out, &err),
            0)
      << err.str();
  EXPECT_THAT(out.str(), testing::HasSubstr("Frames: 30 rendered"));
}

TEST_F(DonnerSvgToolFileTest, GifRejectsExcessiveTotalRenderWork) {
  const std::filesystem::path inputPath = WriteSvg("animated.svg", kAnimatedSvg);
  const std::filesystem::path gifPath = tmpDir_ / "huge.gif";

  std::ostringstream out;
  std::ostringstream err;
  // 66 frames of a 2048x2048 canvas exceed the 2^28 sampled-pixel limit.
  EXPECT_EQ(RunTool({"--gif", gifPath.string(), "--end", "66", "--fps", "1", "--width", "2048",
                     "--height", "2048", inputPath.string()},
                    &out, &err),
            4);
  EXPECT_THAT(err.str(), testing::HasSubstr("the limit is 268435456"));
  EXPECT_FALSE(std::filesystem::exists(gifPath));
}

TEST_F(DonnerSvgToolFileTest, GifRejectsEncoderWorkingSetAboveLimit) {
  const std::filesystem::path inputPath = WriteSvg("animated.svg", kAnimatedSvg);
  const std::filesystem::path gifPath = tmpDir_ / "large-working-set.gif";

  std::ostringstream out;
  std::ostringstream err;
  // One 4096x4096 RGBA frame fits the renderer surface limit, but the renderer snapshot,
  // fitted frame and encoder scratch cannot coexist inside the 256 MiB GIF workspace limit.
  EXPECT_EQ(RunTool({"--gif", gifPath.string(), "--end", "1", "--fps", "1", "--width", "4096",
                     "--height", "4096", inputPath.string()},
                    &out, &err),
            4);
  EXPECT_THAT(err.str(), testing::HasSubstr("GIF working set"));
  EXPECT_THAT(err.str(), testing::HasSubstr("268435456 bytes"));
  EXPECT_FALSE(std::filesystem::exists(gifPath));
}

TEST_F(DonnerSvgToolFileTest, GifRejectsCanvasAboveTheRendererSurfaceBudget) {
  const std::filesystem::path inputPath = WriteSvg("animated.svg", kAnimatedSvg);
  const std::filesystem::path gifPath = tmpDir_ / "huge.gif";

  std::ostringstream out;
  std::ostringstream err;
  // One 20000x10000 frame needs 800 MB, within the GIF side and total pixel limits.
  EXPECT_EQ(RunTool({"--gif", gifPath.string(), "--end", "1", "--fps", "1", "--width", "20000",
                     "--height", "10000", inputPath.string()},
                    &out, &err),
            4);
  EXPECT_THAT(err.str(), testing::HasSubstr("above the renderer's surface budget"));
  EXPECT_FALSE(std::filesystem::exists(gifPath));
}

TEST_F(DonnerSvgToolFileTest, GifKeepsCanvasSizeWhenRootSizeAnimates) {
  const std::filesystem::path inputPath = WriteSvg("growing.svg", R"(
<svg xmlns="http://www.w3.org/2000/svg" width="20" height="10" viewBox="0 0 20 10">
  <animate attributeName="width" from="30" to="40" dur="1s" fill="freeze"/>
  <rect width="20" height="10" fill="blue"/>
</svg>
)");
  const std::filesystem::path gifPath = tmpDir_ / "growing.gif";

  std::ostringstream out;
  std::ostringstream err;
  ASSERT_EQ(RunTool({"--gif", gifPath.string(), "--end", "1", "--fps", "4", inputPath.string()},
                    &out, &err),
            0)
      << err.str();

  const std::string bytes = ReadFile(gifPath);
  std::string error;
  const std::optional<DecodedGif> gif = DecodeGifForTesting(
      std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), &error);
  ASSERT_TRUE(gif.has_value()) << error;
  // The size at --start, where the animated width is 30.
  EXPECT_EQ(gif->width, 30);
  EXPECT_EQ(gif->height, 10);
  EXPECT_THAT(out.str(), testing::HasSubstr("Frames: 4 rendered"));
  // Frames whose fitted pixels match their predecessor are merged; the loop still lasts 1 s.
  int totalDelay = 0;
  for (const DecodedGifFrame& frame : gif->frames) {
    totalDelay += frame.delayCentiseconds;
  }
  EXPECT_EQ(totalDelay, 100);
}

TEST_F(DonnerSvgToolFileTest, GifPreservesCanvasWhenAnimatedExtentRoundsToZero) {
  for (const std::string axis : {"width", "height"}) {
    for (const bool opaqueBackground : {false, true}) {
      const std::string label = axis + (opaqueBackground ? "_white" : "_transparent");
      SCOPED_TRACE(label);
      const std::string source =
          R"(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="10" viewBox="0 0 20 10">
  <set attributeName=")" +
          axis + R"(" to="1000" begin="0.25s" dur="0.5s"/>
  <rect width="20" height="10" fill="blue"/>
</svg>)";
      const auto inputPath = WriteSvg(label + ".svg", source);
      const auto gifPath = tmpDir_ / (label + ".gif");
      std::vector<std::string> args = {"--gif", gifPath.string(), "--end", "1", "--fps", "4"};
      if (opaqueBackground) {
        args.insert(args.end(), {"--background", "white"});
      }
      args.push_back(inputPath.string());
      std::ostringstream out;
      std::ostringstream err;
      ASSERT_EQ(RunTool(args, &out, &err), 0) << err.str();
      EXPECT_TRUE(err.str().empty());
      EXPECT_THAT(out.str(), testing::HasSubstr("Frames: 4 rendered, 3 stored"));

      const std::string bytes = ReadFile(gifPath);
      std::string error;
      const auto gif = DecodeGifForTesting(
          std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), &error);
      ASSERT_TRUE(gif.has_value()) << error;
      ASSERT_EQ(gif->width, 20);
      ASSERT_EQ(gif->height, 10);
      ASSERT_THAT(gif->frames, testing::SizeIs(3));
      EXPECT_THAT(gif->frames,
                  testing::ElementsAre(testing::Field(&DecodedGifFrame::delayCentiseconds, 25),
                                       testing::Field(&DecodedGifFrame::delayCentiseconds, 50),
                                       testing::Field(&DecodedGifFrame::delayCentiseconds, 25)));
      std::vector<uint8_t> blue(20 * 10 * 4, 255);
      for (size_t offset = 0; offset < blue.size(); offset += 4) {
        blue[offset] = 0;
        blue[offset + 1] = 0;
      }
      const std::vector<uint8_t> blank(20 * 10 * 4, opaqueBackground ? 255 : 0);
      for (size_t frameIndex = 0; frameIndex < gif->frames.size(); ++frameIndex) {
        const auto& expected = frameIndex == 1 ? blank : blue;
        editor::tests::CompareBitmapToBitmap(
            ToBitmap(gif->frames[frameIndex].rgba, 20, 10), ToBitmap(expected, 20, 10),
            "gif_zero_extent_" + label + "_" + std::to_string(frameIndex),
            editor::tests::PixelmatchIdentityParams());
      }
    }
  }
}

TEST_F(DonnerSvgToolFileTest, GifScaleUsesTheCanvasSizeAtStart) {
  const std::filesystem::path inputPath = WriteSvg("growing.svg", R"(
<svg xmlns="http://www.w3.org/2000/svg" width="20" height="10" viewBox="0 0 20 10">
  <animate attributeName="width" from="30" to="40" dur="1s" fill="freeze"/>
  <rect width="20" height="10" fill="blue"/>
</svg>
)");
  const std::filesystem::path gifPath = tmpDir_ / "scaled.gif";

  std::ostringstream out;
  std::ostringstream err;
  ASSERT_EQ(RunTool({"--gif", gifPath.string(), "--end", "1", "--fps", "4", "--scale", "2",
                     inputPath.string()},
                    &out, &err),
            0)
      << err.str();
  // Twice the 30x10 size at --start, not twice the authored 20x10.
  EXPECT_THAT(out.str(), testing::HasSubstr("Rendered size: 60x20"));
}

TEST_F(DonnerSvgToolFileTest, PngScaleUsesTheAnimatedCanvasSize) {
  const std::filesystem::path inputPath = WriteSvg("growing.svg", R"(
<svg xmlns="http://www.w3.org/2000/svg" width="20" height="10" viewBox="0 0 20 10">
  <animate attributeName="width" from="30" to="40" dur="1s" fill="freeze"/>
  <rect width="20" height="10" fill="blue"/>
</svg>
)");
  const std::filesystem::path outputPath = tmpDir_ / "scaled.png";

  std::ostringstream out;
  std::ostringstream err;
  ASSERT_EQ(
      RunTool({"--scale", "2", "--output", outputPath.string(), inputPath.string()}, &out, &err), 0)
      << err.str();
  // Twice the animated 30x10 size at time zero, not twice the authored 20x10.
  EXPECT_THAT(out.str(), testing::HasSubstr("Rendered size: 60x20"));
}

TEST_F(DonnerSvgToolFileTest, ScaleAppliesToPngAndRejectsOversizedCanvas) {
  const std::filesystem::path inputPath = WriteSvg("input.svg", kSimpleSvg);
  const std::filesystem::path outputPath = tmpDir_ / "scaled.png";

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(
      RunTool({"--scale", "0.5", "--output", outputPath.string(), inputPath.string()}, &out, &err),
      0)
      << err.str();
  EXPECT_THAT(out.str(), testing::HasSubstr("Saved PNG:"));

  err.str("");
  EXPECT_EQ(RunTool({"--scale", "1e300", "--output", outputPath.string(), inputPath.string()}, &out,
                    &err),
            1);
  EXPECT_THAT(err.str(), testing::HasSubstr("makes the canvas larger than 65535 pixels"));
}

TEST_F(DonnerSvgToolFileTest, ResourcesAreSandboxedToTheInputDocumentsDirectory) {
  const std::filesystem::path untrustedDirectory = tmpDir_ / "untrusted";
  const std::filesystem::path privateDirectory = tmpDir_ / "private";
  std::filesystem::create_directories(untrustedDirectory);
  std::filesystem::create_directories(privateDirectory);

  const std::filesystem::path privateSvg = privateDirectory / "secret.svg";
  {
    std::ofstream file(privateSvg, std::ios::binary);
    file << R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
                  <rect width="4" height="4" fill="#ff0000"/>
                </svg>)";
  }

  const std::filesystem::path inputPath = untrustedDirectory / "input.svg";
  {
    std::ofstream file(inputPath, std::ios::binary);
    file << R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
                  <image href="../private/secret.svg" width="4" height="4"/>
                </svg>)";
  }
  const std::filesystem::path blankPath = untrustedDirectory / "blank.svg";
  {
    std::ofstream file(blankPath, std::ios::binary);
    file << R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4"/>)";
  }
  const std::filesystem::path inlinePath = untrustedDirectory / "inline.svg";
  {
    std::ofstream file(inlinePath, std::ios::binary);
    file << R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
                  <rect width="4" height="4" fill="#ff0000"/>
                </svg>)";
  }

  const std::filesystem::path escapedOutput = tmpDir_ / "escaped.png";
  const std::filesystem::path blankOutput = tmpDir_ / "blank.png";
  const std::filesystem::path inlineOutput = tmpDir_ / "inline.png";
  {
    ScopedWorkingDirectory workingDirectory(tmpDir_);
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(RunTool({"--output", escapedOutput.string(), "untrusted/input.svg"}, &out, &err), 0);
    out.str("");
    err.str("");
    EXPECT_EQ(RunTool({"--output", blankOutput.string(), "untrusted/blank.svg"}, &out, &err), 0);
    out.str("");
    err.str("");
    EXPECT_EQ(RunTool({"--output", inlineOutput.string(), "untrusted/inline.svg"}, &out, &err), 0);
  }

  EXPECT_EQ(ReadFile(escapedOutput), ReadFile(blankOutput));
  EXPECT_NE(ReadFile(escapedOutput), ReadFile(inlineOutput));
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

TEST_F(DonnerSvgToolFileTest, ParseWarningsEscapeUnicodeTerminalControls) {
  std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4"><rect x=")";
  source.append("\xc2\x9b", 2);
  source += R"(" width="2" height="2"/></svg>)";
  const std::filesystem::path inputPath = WriteSvg("terminal-control.svg", source);

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(RunTool({"--preview", inputPath.string()}, &out, &err), 0);

  EXPECT_TRUE(err.str().empty());
  EXPECT_THAT(out.str(), testing::HasSubstr(R"(\u009b)"));
  EXPECT_THAT(out.str(), testing::Not(testing::HasSubstr(std::string("\xc2\x9b", 2))));
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

TEST_F(DonnerSvgToolFileTest, InteractiveModeReadsMouseEventsFromTerminalAndQuits) {
  const std::filesystem::path inputPath = WriteSvg("input.svg", kSimpleSvg);
  ScopedPseudoTerminalStdin stdinTty;
  if (!stdinTty.active()) {
    GTEST_SKIP() << "pseudo-terminal setup failed";
  }

  std::thread inputThread([masterFd = stdinTty.masterFd()] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    constexpr std::string_view kClick = "\x1b[<0;1;1M";
    const ssize_t clickBytes = write(masterFd, kClick.data(), kClick.size());
    (void)clickBytes;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    constexpr std::string_view kQuit = "q";
    const ssize_t quitBytes = write(masterFd, kQuit.data(), kQuit.size());
    (void)quitBytes;
  });

  std::ostringstream out;
  std::ostringstream err;
  const int exitCode = RunTool({"--interactive", inputPath.string()}, &out, &err);
  inputThread.join();

  EXPECT_EQ(exitCode, 0);
  EXPECT_TRUE(err.str().empty());
  EXPECT_THAT(out.str(), testing::HasSubstr("Interactive mode: click with the mouse"));
  EXPECT_THAT(out.str(), testing::HasSubstr("\x1b[?1000h\x1b[?1006h"));
  EXPECT_THAT(out.str(), testing::HasSubstr("\x1b[?1000l\x1b[?1006l"));
  EXPECT_THAT(out.str(), testing::Not(testing::HasSubstr("Interactive mode needs a TTY")));
}

}  // namespace
}  // namespace donner::svg
