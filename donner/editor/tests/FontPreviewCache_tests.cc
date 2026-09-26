#include "donner/editor/FontPreviewCache.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace donner::editor {
namespace {

class FontPreviewCacheTest : public ::testing::Test {
protected:
  void SetUp() override {
    directory_ =
        std::filesystem::temp_directory_path() /
        ("donner-font-preview-test-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::create_directories(directory_);
  }

  void TearDown() override { std::filesystem::remove_all(directory_); }

  static svg::RendererBitmap Bitmap() {
    svg::RendererBitmap bitmap;
    bitmap.dimensions = Vector2i(2, 1);
    bitmap.rowBytes = 8;
    bitmap.alphaType = svg::AlphaType::Unpremultiplied;
    bitmap.pixels = {255, 0, 0, 255, 0, 255, 0, 255};
    return bitmap;
  }

  static constexpr std::string_view kOutlinedSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><path d="M0 0h1v1z"/></svg>)";

  std::filesystem::path directory_;
};

TEST_F(FontPreviewCacheTest, WarmSessionReusesValidatedBitmapAndOutlinedSvg) {
  FontPreviewCache first(directory_, "version-one\ncommit-a");
  first.store("Example", "font-sha:1", kOutlinedSvg, Bitmap());

  FontPreviewCache second(directory_, "version-one\ncommit-a");
  const auto loaded = second.load("Example", "font-sha:1", Vector2i(2, 1));
  ASSERT_TRUE(loaded);
  EXPECT_THAT(loaded->pixels, ::testing::ElementsAreArray(Bitmap().pixels));
  EXPECT_EQ(loaded->rowBytes, 8u);
  EXPECT_EQ(loaded->alphaType, svg::AlphaType::Unpremultiplied);

  std::size_t svgCount = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
    if (entry.path().extension() == ".svg") {
      ++svgCount;
      std::ifstream file(entry.path());
      const std::string svg((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
      EXPECT_THAT(svg, ::testing::HasSubstr("<path"));
      EXPECT_THAT(svg, ::testing::Not(::testing::HasSubstr("<text")));
    }
  }
  EXPECT_EQ(svgCount, 1u);
}

TEST_F(FontPreviewCacheTest, BuildFontAndScaleChangesInvalidateWarmEntry) {
  FontPreviewCache first(directory_, "version-one\ncommit-a");
  first.store("Example", "font-sha:1", kOutlinedSvg, Bitmap());

  EXPECT_FALSE(FontPreviewCache(directory_, "version-one\ncommit-b")
                   .load("Example", "font-sha:1", Vector2i(2, 1)));
  EXPECT_FALSE(first.load("Example", "font-sha:2", Vector2i(2, 1)));
  EXPECT_FALSE(first.load("Other", "font-sha:1", Vector2i(2, 1)));
  EXPECT_FALSE(first.load("Example", "font-sha:1", Vector2i(4, 2)));
  EXPECT_FALSE(first.load("Example", "", Vector2i(2, 1)));
}

TEST_F(FontPreviewCacheTest, RejectsCorruptSidecarAndLiveText) {
  FontPreviewCache cache(directory_, "version-one\ncommit-a");
  cache.store("Live", "font-sha:1", "<svg><text>Live</text></svg>", Bitmap());
  EXPECT_FALSE(cache.load("Live", "font-sha:1", Vector2i(2, 1)));

  cache.store("Example", "font-sha:1", kOutlinedSvg, Bitmap());
  for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
    if (entry.path().extension() == ".rgba") {
      std::ofstream corrupt(entry.path(), std::ios::binary | std::ios::trunc);
      corrupt << "truncated";
    }
  }
  EXPECT_FALSE(cache.load("Example", "font-sha:1", Vector2i(2, 1)));
}

TEST_F(FontPreviewCacheTest, ConcurrentWritersLeaveOneCompletePreview) {
  FontPreviewCache cache(directory_, "version-one\ncommit-a");
  svg::RendererBitmap alternate = Bitmap();
  alternate.pixels[0] = 0;
  std::thread first([&] { cache.store("Example", "font-sha:1", kOutlinedSvg, Bitmap()); });
  std::thread second([&] { cache.store("Example", "font-sha:1", kOutlinedSvg, alternate); });
  first.join();
  second.join();

  const auto loaded = cache.load("Example", "font-sha:1", Vector2i(2, 1));
  ASSERT_TRUE(loaded);
  EXPECT_THAT(loaded->pixels[0], ::testing::AnyOf(0, 255));
  std::size_t svgCount = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
    EXPECT_NE(entry.path().extension(), ".lock");
    if (entry.path().extension() == ".svg") {
      ++svgCount;
    }
  }
  EXPECT_EQ(svgCount, 1u);
}

TEST_F(FontPreviewCacheTest, RejectsMismatchedEmbeddedIdentity) {
  FontPreviewCache cache(directory_, "version-one\ncommit-a");
  cache.store("Example", "font-sha:1", kOutlinedSvg, Bitmap());
  for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
    if (entry.path().extension() != ".rgba") {
      continue;
    }
    std::ifstream input(entry.path(), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const std::size_t identityOffset = bytes.find("font-sha:1");
    ASSERT_NE(identityOffset, std::string::npos);
    bytes[identityOffset] = 'x';
    std::ofstream output(entry.path(), std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  EXPECT_FALSE(cache.load("Example", "font-sha:1", Vector2i(2, 1)));
}

TEST_F(FontPreviewCacheTest, RecoversAnAbandonedLockAfterOneHour) {
  FontPreviewCache cache(directory_, "version-one\ncommit-a");
  cache.store("Example", "font-sha:1", kOutlinedSvg, Bitmap());
  std::filesystem::path base;
  for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
    if (entry.path().extension() == ".svg") {
      base = entry.path();
      base.replace_extension("");
    }
  }
  ASSERT_FALSE(base.empty());
  std::filesystem::remove(base.string() + ".svg");
  std::filesystem::remove(base.string() + ".rgba");
  const auto abandoned = std::filesystem::path(base.string() + ".lock");
  std::filesystem::create_directory(abandoned);
  std::filesystem::last_write_time(
      abandoned, std::filesystem::file_time_type::clock::now() - std::chrono::hours(2));

  cache.store("Example", "font-sha:1", kOutlinedSvg, Bitmap());
  EXPECT_TRUE(cache.load("Example", "font-sha:1", Vector2i(2, 1)).has_value());
  EXPECT_FALSE(std::filesystem::exists(abandoned));
}

}  // namespace
}  // namespace donner::editor
