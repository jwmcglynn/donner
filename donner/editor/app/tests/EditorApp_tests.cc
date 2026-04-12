/// @file
///
/// MVP editor tests covering `EditorApp` state transitions and `EditorRepl`
/// command dispatch. These are fully headless — the REPL is driven by an
/// in-memory `std::stringstream` instead of a TTY, and the status chips
/// and `.rnr` files are asserted without touching the terminal image
/// viewer path (which is noisy to match against).

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "donner/editor/app/EditorApp.h"
#include "donner/editor/app/EditorRepl.h"
#include "donner/editor/sandbox/RnrFile.h"

namespace donner::editor::app {
namespace {

class EditorAppTest : public ::testing::Test {
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

  EditorAppOptions Options() {
    EditorAppOptions opts;
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
// EditorApp core
// -----------------------------------------------------------------------------

TEST_F(EditorAppTest, EmptyStateBeforeNavigate) {
  EditorApp app(Options());
  EXPECT_EQ(app.current().status, EditorStatus::kEmpty);
  EXPECT_TRUE(app.current().uri.empty());
  EXPECT_TRUE(app.lastGoodBitmap().pixels.empty());
}

TEST_F(EditorAppTest, NavigateSucceedsOnValidFile) {
  WriteSvg("red.svg", kSimpleSvg);
  EditorApp app(Options());

  const auto& snap = app.navigate("red.svg");
  ASSERT_EQ(snap.status, EditorStatus::kRendered) << snap.message;
  EXPECT_EQ(snap.uri, "red.svg");
  EXPECT_EQ(snap.bitmap.dimensions.x, 64);
  EXPECT_EQ(snap.bitmap.dimensions.y, 48);
  EXPECT_FALSE(snap.wire.empty());
  EXPECT_EQ(snap.unsupportedCount, 0u);

  // lastGoodBitmap mirrors the current bitmap on success.
  EXPECT_EQ(app.lastGoodBitmap().pixels, snap.bitmap.pixels);
}

TEST_F(EditorAppTest, FetchErrorKeepsPreviousBitmap) {
  WriteSvg("red.svg", kSimpleSvg);
  EditorApp app(Options());
  app.navigate("red.svg");
  const auto goodBytes = app.lastGoodBitmap().pixels;
  ASSERT_FALSE(goodBytes.empty());

  // Second navigation fails at the fetch step.
  const auto& bad = app.navigate("does_not_exist.svg");
  EXPECT_EQ(bad.status, EditorStatus::kFetchError);
  EXPECT_TRUE(bad.bitmap.pixels.empty());
  EXPECT_FALSE(bad.message.empty());

  // But lastGoodBitmap still holds the previous successful frame — this
  // is the "keep previous document on screen" contract from the design doc.
  EXPECT_EQ(app.lastGoodBitmap().pixels, goodBytes);
}

TEST_F(EditorAppTest, ParseErrorSurfacesDistinctStatus) {
  WriteSvg("garbage.svg", "this is not svg at all");
  EditorApp app(Options());
  const auto& snap = app.navigate("garbage.svg");
  EXPECT_EQ(snap.status, EditorStatus::kParseError);
  EXPECT_FALSE(snap.message.empty());
  EXPECT_NE(snap.message.find("parse"), std::string::npos);
}

TEST_F(EditorAppTest, ResizeReRendersAtNewViewport) {
  WriteSvg("red.svg", kSimpleSvg);
  EditorApp app(Options());
  app.navigate("red.svg");

  // kSimpleSvg is 64x48 (4:3) — request a 128x96 canvas to match the
  // source aspect ratio so Donner's preserveAspectRatio doesn't letterbox
  // the result into an unexpected height.
  const auto& resized = app.resize(128, 96);
  EXPECT_EQ(resized.status, EditorStatus::kRendered) << resized.message;
  EXPECT_EQ(resized.bitmap.dimensions.x, 128);
  EXPECT_EQ(resized.bitmap.dimensions.y, 96);
}

TEST_F(EditorAppTest, ReloadPicksUpFileChanges) {
  const auto path = WriteSvg("live.svg",
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
         <rect width="32" height="32" fill="red"/>
       </svg>)");
  EditorApp app(Options());
  const auto& first = app.navigate("live.svg");
  ASSERT_EQ(first.status, EditorStatus::kRendered);
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
  ASSERT_EQ(reloaded.status, EditorStatus::kRendered);
  EXPECT_NE(reloaded.bitmap.pixels, firstPixels)
      << "reload should have picked up the file edit";
}

// -----------------------------------------------------------------------------
// EditorApp watch / pollForChanges
// -----------------------------------------------------------------------------

TEST_F(EditorAppTest, PollForChangesReturnsFalseWhenWatchDisabled) {
  WriteSvg("red.svg", kSimpleSvg);
  EditorApp app(Options());
  app.navigate("red.svg");
  // Watch is disabled by default.
  EXPECT_FALSE(app.watchEnabled());
  EXPECT_FALSE(app.pollForChanges());
}

TEST_F(EditorAppTest, PollForChangesReturnsFalseWhenNoFileLoaded) {
  EditorApp app(Options());
  app.setWatchEnabled(true);
  EXPECT_FALSE(app.pollForChanges());
}

TEST_F(EditorAppTest, PollForChangesDetectsFileModification) {
  const auto path = WriteSvg("watch.svg",
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
         <rect width="32" height="32" fill="red"/>
       </svg>)");
  EditorApp app(Options());
  app.navigate("watch.svg");
  app.setWatchEnabled(true);
  const auto firstPixels = app.lastGoodBitmap().pixels;
  ASSERT_FALSE(firstPixels.empty());

  // No change yet — poll should return false.
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

  // Touch the file to ensure the mtime differs — some filesystems have
  // coarse timestamps (1s resolution).
  std::filesystem::last_write_time(
      path, std::filesystem::file_time_type::clock::now());

  EXPECT_TRUE(app.pollForChanges());
  EXPECT_NE(app.lastGoodBitmap().pixels, firstPixels)
      << "pollForChanges should have reloaded with new content";

  // A second poll without further edits should return false.
  EXPECT_FALSE(app.pollForChanges());
}

// -----------------------------------------------------------------------------
// EditorRepl command dispatch
// -----------------------------------------------------------------------------

class EditorReplTest : public EditorAppTest {
protected:
  EditorReplOptions ReplOptions() {
    EditorReplOptions opts;
    opts.printBanner = false;
    opts.prompt = "";
    opts.showEnabled = false;  // don't touch terminal image viewer in tests
    return opts;
  }
};

TEST_F(EditorReplTest, HelpListsAllCommands) {
  EditorApp app(Options());
  std::stringstream in("help\nquit\n");
  std::stringstream out;
  EditorRepl repl(app, in, out, ReplOptions());
  repl.run();

  const auto text = out.str();
  for (const auto* cmd : {"load ", "reload", "resize", "show", "save ", "inspect",
                          "record", "quit"}) {
    EXPECT_NE(text.find(cmd), std::string::npos) << "help missing: " << cmd;
  }
}

TEST_F(EditorReplTest, LoadStatusSaveCycle) {
  WriteSvg("red.svg", kSimpleSvg);
  const auto pngPath = tmpDir_ / "out.png";

  std::stringstream in;
  in << "load red.svg\n"
     << "status\n"
     << "save " << pngPath.string() << "\n"
     << "quit\n";
  std::stringstream out;

  EditorApp app(Options());
  EditorRepl repl(app, in, out, ReplOptions());
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

TEST_F(EditorReplTest, RecordWritesValidRnrFile) {
  WriteSvg("red.svg", kSimpleSvg);
  const auto rnrPath = tmpDir_ / "demo.rnr";

  std::stringstream in;
  in << "load red.svg\n"
     << "record " << rnrPath.string() << "\n"
     << "quit\n";
  std::stringstream out;

  EditorApp app(Options());
  EditorRepl repl(app, in, out, ReplOptions());
  repl.run();

  EXPECT_NE(out.str().find("record: wrote"), std::string::npos);
  ASSERT_TRUE(std::filesystem::exists(rnrPath));

  // Load the .rnr back and verify its contents look reasonable.
  sandbox::RnrHeader header;
  std::vector<uint8_t> wire;
  ASSERT_EQ(sandbox::LoadRnrFile(rnrPath, header, wire), sandbox::RnrIoStatus::kOk);
  EXPECT_EQ(header.width, 64u);
  EXPECT_EQ(header.height, 48u);
  EXPECT_FALSE(wire.empty());
}

TEST_F(EditorReplTest, InspectProducesCommandDump) {
  WriteSvg("red.svg", kSimpleSvg);

  std::stringstream in("load red.svg\ninspect\nquit\n");
  std::stringstream out;

  EditorApp app(Options());
  EditorRepl repl(app, in, out, ReplOptions());
  repl.run();

  const auto text = out.str();
  EXPECT_NE(text.find("beginFrame"), std::string::npos);
  EXPECT_NE(text.find("endFrame"), std::string::npos);
  EXPECT_NE(text.find("drawPath"), std::string::npos);
}

TEST_F(EditorReplTest, UnknownCommandDoesNotCrash) {
  EditorApp app(Options());
  std::stringstream in("frobnicate\nquit\n");
  std::stringstream out;
  EditorRepl repl(app, in, out, ReplOptions());
  repl.run();
  EXPECT_NE(out.str().find("unknown command"), std::string::npos);
}

TEST_F(EditorReplTest, InspectWithNoFrameReportsError) {
  EditorApp app(Options());
  std::stringstream in("inspect\nquit\n");
  std::stringstream out;
  EditorRepl repl(app, in, out, ReplOptions());
  repl.run();
  EXPECT_NE(out.str().find("no frame available"), std::string::npos);
}

TEST_F(EditorReplTest, ResizeCommandParsesDimensions) {
  WriteSvg("red.svg", kSimpleSvg);
  // Match the SVG's 4:3 aspect so preserveAspectRatio doesn't letterbox.
  std::stringstream in("load red.svg\nresize 128 96\nstatus\nquit\n");
  std::stringstream out;

  EditorApp app(Options());
  EditorRepl repl(app, in, out, ReplOptions());
  repl.run();

  const auto text = out.str();
  EXPECT_NE(text.find("rendered 128x96"), std::string::npos) << text;
}

TEST_F(EditorReplTest, WatchOnOffCommand) {
  EditorApp app(Options());
  std::stringstream in("watch on\nwatch off\nquit\n");
  std::stringstream out;
  EditorRepl repl(app, in, out, ReplOptions());
  repl.run();

  const auto text = out.str();
  EXPECT_NE(text.find("watch: enabled"), std::string::npos) << text;
  EXPECT_NE(text.find("watch: disabled"), std::string::npos) << text;
  // After "watch off", the app's watch should be disabled.
  EXPECT_FALSE(app.watchEnabled());
}

TEST_F(EditorReplTest, WatchInvalidArgPrintsUsage) {
  EditorApp app(Options());
  std::stringstream in("watch maybe\nquit\n");
  std::stringstream out;
  EditorRepl repl(app, in, out, ReplOptions());
  repl.run();

  const auto text = out.str();
  EXPECT_NE(text.find("usage: watch on|off"), std::string::npos) << text;
}

TEST_F(EditorReplTest, HelpListsWatchCommand) {
  EditorApp app(Options());
  std::stringstream in("help\nquit\n");
  std::stringstream out;
  EditorRepl repl(app, in, out, ReplOptions());
  repl.run();

  const auto text = out.str();
  EXPECT_NE(text.find("watch"), std::string::npos) << text;
}

}  // namespace
}  // namespace donner::editor::app
