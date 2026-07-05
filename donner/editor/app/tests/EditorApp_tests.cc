/// @file
///
/// Headless render-session tests covering `RenderSession` state transitions and
/// `RenderSessionRepl` command dispatch. The REPL is driven by an in-memory
/// `std::stringstream` instead of a TTY, and bitmap output is asserted without
/// touching the terminal image viewer path (which is noisy to match against).

#include "donner/editor/app/EditorApp.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#include "donner/editor/app/EditorRepl.h"

namespace donner::editor::app {
namespace {

class RenderSessionTest : public ::testing::Test {
protected:
  void SetUp() override {
    tmpDir_ = std::filesystem::path(::testing::TempDir()) /
              ("editor_app_test_" + std::to_string(::rand()));
    std::filesystem::create_directories(tmpDir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(tmpDir_, ec);
  }

  std::filesystem::path WriteSvg(const std::string& name, std::string_view contents) {
    const auto path = tmpDir_ / name;
    std::ofstream out(path, std::ios::binary);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return path;
  }

  RenderSessionOptions Options() {
    RenderSessionOptions opts;
    opts.defaultWidth = 64;
    opts.defaultHeight = 48;
    opts.sourceOptions.baseDirectory = tmpDir_;
    return opts;
  }

  std::filesystem::path tmpDir_;
};

constexpr std::string_view kSimpleSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="48">
       <rect width="64" height="48" fill="red"/>
     </svg>)";

// -----------------------------------------------------------------------------
// RenderSession core
// -----------------------------------------------------------------------------

TEST_F(RenderSessionTest, EmptyStateBeforeNavigate) {
  RenderSession app(Options());
  EXPECT_EQ(app.current().status, RenderSessionStatus::kEmpty);
  EXPECT_TRUE(app.current().uri.empty());
  EXPECT_TRUE(app.lastGoodBitmap().pixels.empty());
}

TEST_F(RenderSessionTest, NavigateSucceedsOnValidFile) {
  WriteSvg("red.svg", kSimpleSvg);
  RenderSession app(Options());

  const auto& snap = app.navigate("red.svg");
  ASSERT_EQ(snap.status, RenderSessionStatus::kRendered) << snap.message;
  EXPECT_EQ(snap.uri, "red.svg");
  EXPECT_EQ(snap.bitmap.dimensions.x, 64);
  EXPECT_EQ(snap.bitmap.dimensions.y, 48);

  // lastGoodBitmap mirrors the current bitmap on success.
  EXPECT_EQ(app.lastGoodBitmap().pixels, snap.bitmap.pixels);
}

TEST_F(RenderSessionTest, FetchErrorKeepsPreviousBitmap) {
  WriteSvg("red.svg", kSimpleSvg);
  RenderSession app(Options());
  app.navigate("red.svg");
  const auto goodBytes = app.lastGoodBitmap().pixels;
  ASSERT_FALSE(goodBytes.empty());

  // Second navigation fails at the fetch step.
  const auto& bad = app.navigate("does_not_exist.svg");
  EXPECT_EQ(bad.status, RenderSessionStatus::kFetchError);
  EXPECT_TRUE(bad.bitmap.pixels.empty());
  EXPECT_FALSE(bad.message.empty());

  // But lastGoodBitmap still holds the previous successful frame - this
  // is the "keep previous document on screen" contract from the design doc.
  EXPECT_EQ(app.lastGoodBitmap().pixels, goodBytes);
}

TEST_F(RenderSessionTest, NavigateRejectsEmptyUri) {
  RenderSession app(Options());

  const auto& snap = app.navigate("");

  EXPECT_EQ(snap.status, RenderSessionStatus::kFetchError);
  EXPECT_NE(snap.message.find("empty uri"), std::string::npos) << snap.message;
  EXPECT_TRUE(app.lastGoodBitmap().pixels.empty());
}

TEST_F(RenderSessionTest, NavigateRejectsUnsupportedScheme) {
  RenderSession app(Options());

  const auto& snap = app.navigate("https://example.test/image.svg");

  EXPECT_EQ(snap.status, RenderSessionStatus::kFetchError);
  EXPECT_NE(snap.message.find("unsupported scheme"), std::string::npos) << snap.message;
  EXPECT_TRUE(app.lastGoodBitmap().pixels.empty());
}

TEST_F(RenderSessionTest, NavigateLoadsFileSchemeUri) {
  const auto path = WriteSvg("red.svg", kSimpleSvg);
  RenderSession app(Options());

  const auto& snap = app.navigate("file://" + path.string());

  ASSERT_EQ(snap.status, RenderSessionStatus::kRendered) << snap.message;
  EXPECT_EQ(snap.bitmap.dimensions.x, 64);
  EXPECT_EQ(snap.bitmap.dimensions.y, 48);
}

TEST_F(RenderSessionTest, NavigateRejectsDirectoryInput) {
  std::filesystem::create_directories(tmpDir_ / "assets");
  RenderSession app(Options());

  const auto& snap = app.navigate("assets");

  EXPECT_EQ(snap.status, RenderSessionStatus::kFetchError);
  EXPECT_NE(snap.message.find("not a regular file"), std::string::npos) << snap.message;
}

TEST_F(RenderSessionTest, NavigateRejectsFilesOverConfiguredLimit) {
  WriteSvg("large.svg", kSimpleSvg);
  RenderSessionOptions options = Options();
  options.sourceOptions.maxFileBytes = 1;
  RenderSession app(options);

  const auto& snap = app.navigate("large.svg");

  EXPECT_EQ(snap.status, RenderSessionStatus::kFetchError);
  EXPECT_NE(snap.message.find("maxFileBytes"), std::string::npos) << snap.message;
}

TEST_F(RenderSessionTest, ParseErrorSurfacesDistinctStatus) {
  WriteSvg("garbage.svg", "this is not svg at all");
  RenderSession app(Options());
  const auto& snap = app.navigate("garbage.svg");
  EXPECT_EQ(snap.status, RenderSessionStatus::kParseError);
  EXPECT_FALSE(snap.message.empty());
  EXPECT_NE(snap.message.find("parse"), std::string::npos);
}

TEST_F(RenderSessionTest, ReloadWithoutDocumentReportsNoDocument) {
  RenderSession app(Options());

  const auto& snap = app.reload();

  EXPECT_EQ(snap.status, RenderSessionStatus::kEmpty);
  EXPECT_NE(snap.message.find("no document loaded"), std::string::npos) << snap.message;
}

TEST_F(RenderSessionTest, ResizeRejectsInvalidDimensionsAndNoDocument) {
  RenderSession app(Options());

  const auto& invalid = app.resize(0, 48);
  EXPECT_EQ(invalid.status, RenderSessionStatus::kEmpty);
  EXPECT_NE(invalid.message.find("must be positive"), std::string::npos) << invalid.message;

  const auto& noDocument = app.resize(32, 24);
  EXPECT_EQ(noDocument.status, RenderSessionStatus::kEmpty);
  EXPECT_NE(noDocument.message.find("no document loaded"), std::string::npos) << noDocument.message;
}

TEST_F(RenderSessionTest, ResizeReRendersAtNewViewport) {
  WriteSvg("red.svg", kSimpleSvg);
  RenderSession app(Options());
  app.navigate("red.svg");

  // kSimpleSvg is 64x48 (4:3) - request a 128x96 canvas to match the
  // source aspect ratio so Donner's preserveAspectRatio doesn't letterbox
  // the result into an unexpected height.
  const auto& resized = app.resize(128, 96);
  EXPECT_EQ(resized.status, RenderSessionStatus::kRendered) << resized.message;
  EXPECT_EQ(resized.bitmap.dimensions.x, 128);
  EXPECT_EQ(resized.bitmap.dimensions.y, 96);
}

TEST_F(RenderSessionTest, SequentialRendersReplaceFrameOnSameSession) {
  WriteSvg("red.svg", kSimpleSvg);
  WriteSvg("blue.svg",
           R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="48">
       <rect width="64" height="48" fill="blue"/>
     </svg>)");
  RenderSession app(Options());

  const auto& red = app.navigate("red.svg");
  ASSERT_EQ(red.status, RenderSessionStatus::kRendered) << red.message;
  const auto redPixels = red.bitmap.pixels;

  const auto& blue = app.navigate("blue.svg");
  ASSERT_EQ(blue.status, RenderSessionStatus::kRendered) << blue.message;
  EXPECT_EQ(blue.bitmap.dimensions.x, 64);
  EXPECT_EQ(blue.bitmap.dimensions.y, 48);
  EXPECT_NE(blue.bitmap.pixels, redPixels);
  const auto bluePixels = blue.bitmap.pixels;

  const auto& resized = app.resize(32, 24);
  ASSERT_EQ(resized.status, RenderSessionStatus::kRendered) << resized.message;
  EXPECT_EQ(resized.bitmap.dimensions.x, 32);
  EXPECT_EQ(resized.bitmap.dimensions.y, 24);
  EXPECT_NE(resized.bitmap.pixels, bluePixels);
  EXPECT_EQ(app.lastGoodBitmap().pixels, resized.bitmap.pixels);
}

TEST_F(RenderSessionTest, ReloadPicksUpFileChanges) {
  const auto path = WriteSvg("live.svg",
                             R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
         <rect width="32" height="32" fill="red"/>
       </svg>)");
  RenderSession app(Options());
  const auto& first = app.navigate("live.svg");
  ASSERT_EQ(first.status, RenderSessionStatus::kRendered);
  const auto firstPixels = first.bitmap.pixels;

  // Replace the file contents with a differently-colored rect.
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    constexpr std::string_view kUpdated =
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
           <rect width="32" height="32" fill="blue"/>
         </svg>)";
    out.write(kUpdated.data(), static_cast<std::streamsize>(kUpdated.size()));
  }

  const auto& reloaded = app.reload();
  ASSERT_EQ(reloaded.status, RenderSessionStatus::kRendered);
  EXPECT_NE(reloaded.bitmap.pixels, firstPixels) << "reload should have picked up the file edit";
}

// -----------------------------------------------------------------------------
// RenderSession watch / pollForChanges
// -----------------------------------------------------------------------------

TEST_F(RenderSessionTest, PollForChangesReturnsFalseWhenWatchDisabled) {
  WriteSvg("red.svg", kSimpleSvg);
  RenderSession app(Options());
  app.navigate("red.svg");
  // Watch is disabled by default.
  EXPECT_FALSE(app.watchEnabled());
  EXPECT_FALSE(app.pollForChanges());
}

TEST_F(RenderSessionTest, PollForChangesReturnsFalseWhenNoFileLoaded) {
  RenderSession app(Options());
  app.setWatchEnabled(true);
  EXPECT_FALSE(app.pollForChanges());
}

TEST_F(RenderSessionTest, PollForChangesDetectsFileModification) {
  const auto path = WriteSvg("watch.svg",
                             R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
         <rect width="32" height="32" fill="red"/>
       </svg>)");
  RenderSession app(Options());
  app.navigate("watch.svg");
  app.setWatchEnabled(true);
  const auto firstPixels = app.lastGoodBitmap().pixels;
  ASSERT_FALSE(firstPixels.empty());

  // No change yet - poll should return false.
  EXPECT_FALSE(app.pollForChanges());

  // Overwrite the file with different content.
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    constexpr std::string_view kUpdated =
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
           <rect width="32" height="32" fill="blue"/>
         </svg>)";
    out.write(kUpdated.data(), static_cast<std::streamsize>(kUpdated.size()));
  }

  // Touch the file to ensure the mtime differs - some filesystems have
  // coarse timestamps (1s resolution).
  std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now());

  EXPECT_TRUE(app.pollForChanges());
  EXPECT_NE(app.lastGoodBitmap().pixels, firstPixels)
      << "pollForChanges should have reloaded with new content";

  // A second poll without further edits should return false.
  EXPECT_FALSE(app.pollForChanges());
}

TEST_F(RenderSessionTest, PollForChangesIgnoresDeletedLoadedFile) {
  const auto path = WriteSvg("watch.svg", kSimpleSvg);
  RenderSession app(Options());
  ASSERT_EQ(app.navigate("watch.svg").status, RenderSessionStatus::kRendered);
  app.setWatchEnabled(true);

  std::error_code ec;
  std::filesystem::remove(path, ec);
  ASSERT_FALSE(ec);

  EXPECT_FALSE(app.pollForChanges());
  EXPECT_EQ(app.current().status, RenderSessionStatus::kRendered);
}

// -----------------------------------------------------------------------------
// RenderSessionRepl command dispatch
// -----------------------------------------------------------------------------

class RenderSessionReplTest : public RenderSessionTest {
protected:
  RenderSessionReplOptions ReplOptions() {
    RenderSessionReplOptions opts;
    opts.printBanner = false;
    opts.prompt = "";
    opts.showEnabled = false;  // don't touch terminal image viewer in tests
    return opts;
  }
};

TEST_F(RenderSessionReplTest, HelpListsAllCommands) {
  RenderSession app(Options());
  std::stringstream in("help\nquit\n");
  std::stringstream out;
  RenderSessionRepl repl(app, in, out, ReplOptions());
  repl.run();

  const auto text = out.str();
  for (const auto* cmd : {"load ", "reload", "resize", "show", "save ", "watch", "quit"}) {
    EXPECT_NE(text.find(cmd), std::string::npos) << "help missing: " << cmd;
  }
}

TEST_F(RenderSessionReplTest, BannerPromptAndBlankLinesDoNotCountAsCommands) {
  RenderSessionReplOptions options = ReplOptions();
  options.printBanner = true;
  options.prompt = "donner-test> ";

  RenderSession app(Options());
  std::stringstream in("\n?\nexit\n");
  std::stringstream out;
  RenderSessionRepl repl(app, in, out, options);

  EXPECT_EQ(repl.run(), 2);

  const std::string text = out.str();
  EXPECT_NE(text.find("Donner Editor MVP"), std::string::npos) << text;
  EXPECT_NE(text.find("donner-test> "), std::string::npos) << text;
  EXPECT_NE(text.find("Commands:"), std::string::npos) << text;
}

TEST_F(RenderSessionReplTest, LoadStatusSaveCycle) {
  WriteSvg("red.svg", kSimpleSvg);
  const auto pngPath = tmpDir_ / "out.png";

  std::stringstream in;
  in << "load red.svg\n"
     << "status\n"
     << "save " << pngPath.string() << "\n"
     << "quit\n";
  std::stringstream out;

  RenderSession app(Options());
  RenderSessionRepl repl(app, in, out, ReplOptions());
  const int dispatched = repl.run();
  EXPECT_EQ(dispatched, 4);

  const auto text = out.str();
  EXPECT_NE(text.find("rendered 64x48"), std::string::npos) << text;
  EXPECT_NE(text.find("uri=red.svg"), std::string::npos);
  EXPECT_NE(text.find("save: wrote"), std::string::npos);

  // PNG file exists and starts with the PNG magic.
  ASSERT_TRUE(std::filesystem::exists(pngPath));
  std::ifstream png(pngPath, std::ios::binary);
  char magic[8] = {};
  png.read(magic, 8);
  const unsigned char expected[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(static_cast<unsigned char>(magic[i]), expected[i]) << "byte " << i;
  }
}

TEST_F(RenderSessionReplTest, UnknownCommandDoesNotCrash) {
  RenderSession app(Options());
  std::stringstream in("frobnicate\nquit\n");
  std::stringstream out;
  RenderSessionRepl repl(app, in, out, ReplOptions());
  repl.run();
  EXPECT_NE(out.str().find("unknown command"), std::string::npos);
}

TEST_F(RenderSessionReplTest, InvalidCommandArityAndDimensionsReturnFalse) {
  RenderSession app(Options());
  std::stringstream in;
  std::stringstream out;
  RenderSessionRepl repl(app, in, out, ReplOptions());

  EXPECT_FALSE(repl.dispatch(""));
  EXPECT_FALSE(repl.dispatch("load"));
  EXPECT_FALSE(repl.dispatch("resize 10"));
  EXPECT_FALSE(repl.dispatch("resize wide 20"));
  EXPECT_FALSE(repl.dispatch("resize 20 0"));
  EXPECT_FALSE(repl.dispatch("save"));
  EXPECT_FALSE(repl.dispatch("watch"));
  EXPECT_FALSE(repl.dispatch("bogus"));

  const std::string text = out.str();
  EXPECT_NE(text.find("usage: load <uri>"), std::string::npos) << text;
  EXPECT_NE(text.find("usage: resize <width> <height>"), std::string::npos) << text;
  EXPECT_NE(text.find("resize: invalid dimension 'wide'"), std::string::npos) << text;
  EXPECT_NE(text.find("resize: invalid dimension '0'"), std::string::npos) << text;
  EXPECT_NE(text.find("usage: save <out.png>"), std::string::npos) << text;
  EXPECT_NE(text.find("usage: watch on|off"), std::string::npos) << text;
  EXPECT_NE(text.find("unknown command 'bogus'"), std::string::npos) << text;
}

TEST_F(RenderSessionReplTest, ResizeCommandParsesDimensions) {
  WriteSvg("red.svg", kSimpleSvg);
  // Match the SVG's 4:3 aspect so preserveAspectRatio doesn't letterbox.
  std::stringstream in("load red.svg\nresize 128 96\nstatus\nquit\n");
  std::stringstream out;

  RenderSession app(Options());
  RenderSessionRepl repl(app, in, out, ReplOptions());
  repl.run();

  const auto text = out.str();
  EXPECT_NE(text.find("rendered 128x96"), std::string::npos) << text;
}

TEST_F(RenderSessionReplTest, ReloadCommandPrintsUpdatedStatus) {
  WriteSvg("red.svg", kSimpleSvg);
  std::stringstream in("load red.svg\nreload\nquit\n");
  std::stringstream out;

  RenderSession app(Options());
  RenderSessionRepl repl(app, in, out, ReplOptions());
  EXPECT_EQ(repl.run(), 3);

  const std::string text = out.str();
  EXPECT_NE(text.find("rendered 64x48"), std::string::npos) << text;
  EXPECT_NE(text.find("uri=red.svg"), std::string::npos) << text;
}

TEST_F(RenderSessionReplTest, ShowAndSaveReportMissingFrameBeforeLoad) {
  RenderSession app(Options());
  std::stringstream in;
  std::stringstream out;
  RenderSessionRepl repl(app, in, out, ReplOptions());

  EXPECT_TRUE(repl.dispatch("show"));
  EXPECT_TRUE(repl.dispatch("save out.png"));

  const std::string text = out.str();
  EXPECT_NE(text.find("show: no frame available"), std::string::npos) << text;
  EXPECT_NE(text.find("save: no frame available"), std::string::npos) << text;
}

TEST_F(RenderSessionReplTest, ShowDisabledAndSaveOpenFailureAreReported) {
  WriteSvg("red.svg", kSimpleSvg);
  const auto outputDirectory = tmpDir_ / "output-directory";
  std::filesystem::create_directories(outputDirectory);

  RenderSession app(Options());
  ASSERT_EQ(app.navigate("red.svg").status, RenderSessionStatus::kRendered);

  std::stringstream in;
  std::stringstream out;
  RenderSessionRepl repl(app, in, out, ReplOptions());

  EXPECT_TRUE(repl.dispatch("show"));
  EXPECT_TRUE(repl.dispatch("save " + outputDirectory.string()));

  const std::string text = out.str();
  EXPECT_NE(text.find("show: disabled in this repl session"), std::string::npos) << text;
  EXPECT_NE(text.find("save: cannot open"), std::string::npos) << text;
}

TEST_F(RenderSessionReplTest, ShowEnabledRendersTerminalPreview) {
  WriteSvg("red.svg", kSimpleSvg);
  RenderSession app(Options());
  ASSERT_EQ(app.navigate("red.svg").status, RenderSessionStatus::kRendered);

  RenderSessionReplOptions options = ReplOptions();
  options.showEnabled = true;
  std::stringstream in;
  std::stringstream out;
  RenderSessionRepl repl(app, in, out, options);

  EXPECT_TRUE(repl.dispatch("show"));

  EXPECT_EQ(out.str().find("show: disabled"), std::string::npos) << out.str();
  EXPECT_FALSE(out.str().empty());
}

TEST_F(RenderSessionReplTest, WatchOnOffCommand) {
  RenderSession app(Options());
  std::stringstream in("watch on\nwatch off\nquit\n");
  std::stringstream out;
  RenderSessionRepl repl(app, in, out, ReplOptions());
  repl.run();

  const auto text = out.str();
  EXPECT_NE(text.find("watch: enabled"), std::string::npos) << text;
  EXPECT_NE(text.find("watch: disabled"), std::string::npos) << text;
  // After "watch off", the app's watch should be disabled.
  EXPECT_FALSE(app.watchEnabled());
}

TEST_F(RenderSessionReplTest, RunReportsAutoReloadWhenWatchPollDetectsChange) {
  const auto path = WriteSvg("watch.svg", kSimpleSvg);
  RenderSession app(Options());
  ASSERT_EQ(app.navigate("watch.svg").status, RenderSessionStatus::kRendered);
  app.setWatchEnabled(true);

  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    constexpr std::string_view kUpdated =
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="48">
           <rect width="64" height="48" fill="blue"/>
         </svg>)";
    out.write(kUpdated.data(), static_cast<std::streamsize>(kUpdated.size()));
  }
  std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now());

  std::stringstream in("status\nquit\n");
  std::stringstream out;
  RenderSessionRepl repl(app, in, out, ReplOptions());

  EXPECT_EQ(repl.run(), 2);

  const std::string text = out.str();
  EXPECT_NE(text.find("[auto-reloaded]"), std::string::npos) << text;
  EXPECT_NE(text.find("rendered 64x48"), std::string::npos) << text;
}

TEST_F(RenderSessionReplTest, WatchInvalidArgPrintsUsage) {
  RenderSession app(Options());
  std::stringstream in("watch maybe\nquit\n");
  std::stringstream out;
  RenderSessionRepl repl(app, in, out, ReplOptions());
  repl.run();

  const auto text = out.str();
  EXPECT_NE(text.find("usage: watch on|off"), std::string::npos) << text;
}

TEST_F(RenderSessionReplTest, HelpListsWatchCommand) {
  RenderSession app(Options());
  std::stringstream in("help\nquit\n");
  std::stringstream out;
  RenderSessionRepl repl(app, in, out, ReplOptions());
  repl.run();

  const auto text = out.str();
  EXPECT_NE(text.find("watch"), std::string::npos) << text;
}

}  // namespace
}  // namespace donner::editor::app
