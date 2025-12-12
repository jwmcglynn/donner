#include "donner/svg/components/resources/ResourceManagerContext.h"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <memory>
#include <variant>

#include "donner/base/ParseError.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/css/FontFace.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg::components {

namespace {

class FakeResourceLoader : public ResourceLoaderInterface {
public:
  explicit FakeResourceLoader(std::vector<uint8_t> data) : data_(std::move(data)) {}

  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view /*url*/) override {
    return data_;
  }

private:
  std::vector<uint8_t> data_;
};

class CountingResourceLoader : public ResourceLoaderInterface {
public:
  explicit CountingResourceLoader(
      std::variant<std::vector<uint8_t>, ResourceLoaderError> response)
      : response_(std::move(response)) {}

  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view /*url*/) override {
    ++callCount_;
    return response_;
  }

  int callCount() const { return callCount_; }

private:
  std::variant<std::vector<uint8_t>, ResourceLoaderError> response_;
  int callCount_ = 0;
};

std::vector<uint8_t> LoadFile(const std::string& location) {
  std::ifstream file(location, std::ios::binary);
  EXPECT_TRUE(file.is_open()) << "Failed to open file: " << location;
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

}  // namespace

TEST(ResourceManagerContext, AppliesCssFamilyNameToLoadedFonts) {
  Registry registry;
  ResourceManagerContext resourceManager(registry);

  const std::string location =
      ::donner::Runfiles::instance().Rlocation("donner/base/fonts/testdata/valid-001.woff");
  std::vector<uint8_t> fontData = LoadFile(location);
  ASSERT_FALSE(fontData.empty());

  auto loader = std::make_unique<FakeResourceLoader>(fontData);
  resourceManager.setExternalFontLoadingEnabled(true);
  resourceManager.setResourceLoader(std::move(loader));

  css::FontFace fontFace;
  fontFace.familyName = RcString("CssFamily");
  css::FontFaceSource source;
  source.kind = css::FontFaceSource::Kind::Url;
  source.payload = RcString("test://font.woff");
  fontFace.sources.push_back(std::move(source));

  resourceManager.addFontFaces(std::span<const css::FontFace>(&fontFace, 1));
  resourceManager.loadResources(nullptr);

  ASSERT_EQ(resourceManager.loadedFonts().size(), 1u);
  const components::FontResource& resource = resourceManager.loadedFonts().front();
  ASSERT_TRUE(resource.font.familyName.has_value());
  EXPECT_EQ(resource.font.familyName.value(), "CssFamily");
  EXPECT_EQ(resourceManager.fontLoadTelemetry().scheduledLoads, 1u);
  EXPECT_EQ(resourceManager.fontLoadTelemetry().loadedFonts, 1u);
}

TEST(ResourceManagerContext, ExternalFontLoadingIsOptIn) {
  Registry registry;
  ResourceManagerContext resourceManager(registry);

  const std::string location =
      ::donner::Runfiles::instance().Rlocation("donner/base/fonts/testdata/valid-001.woff");
  std::vector<uint8_t> fontData = LoadFile(location);
  ASSERT_FALSE(fontData.empty());

  auto loader = std::make_unique<FakeResourceLoader>(fontData);
  resourceManager.setResourceLoader(std::move(loader));

  css::FontFace fontFace;
  fontFace.familyName = RcString("BlockedFont");
  css::FontFaceSource source;
  source.kind = css::FontFaceSource::Kind::Url;
  source.payload = RcString("test://font.woff");
  fontFace.sources.push_back(std::move(source));

  resourceManager.addFontFaces(std::span<const css::FontFace>(&fontFace, 1));
  std::vector<ParseError> warnings;
  resourceManager.loadResources(&warnings);

  EXPECT_TRUE(resourceManager.loadedFonts().empty());
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_EQ(warnings.front().reason, "External font loading is disabled");
  EXPECT_EQ(resourceManager.fontLoadTelemetry().blockedByDisabledExternalFonts, 1u);

  // Enable external font loading and retry loading the deferred font.
  resourceManager.setExternalFontLoadingEnabled(true);
  warnings.clear();
  resourceManager.loadResources(&warnings);

  ASSERT_EQ(resourceManager.loadedFonts().size(), 1u);
  EXPECT_TRUE(warnings.empty());
  EXPECT_EQ(resourceManager.loadedFonts().front().font.familyName, "BlockedFont");
  EXPECT_EQ(resourceManager.fontLoadTelemetry().loadedFonts, 1u);
  EXPECT_EQ(resourceManager.fontLoadTelemetry().blockedByDisabledExternalFonts, 1u);
}

TEST(ResourceManagerContext, ContinuousRenderingDefersRemoteFontLoads) {
  Registry registry;
  ResourceManagerContext resourceManager(registry);

  const std::string location =
      ::donner::Runfiles::instance().Rlocation("donner/base/fonts/testdata/valid-001.woff");
  std::vector<uint8_t> fontData = LoadFile(location);
  ASSERT_FALSE(fontData.empty());

  auto loader = std::make_unique<CountingResourceLoader>(fontData);
  CountingResourceLoader* loaderPtr = loader.get();
  resourceManager.setResourceLoader(std::move(loader));
  resourceManager.setExternalFontLoadingEnabled(true);
  resourceManager.setFontRenderMode(FontRenderMode::kContinuous);

  css::FontFace fontFace;
  fontFace.familyName = RcString("DeferredFont");
  css::FontFaceSource source;
  source.kind = css::FontFaceSource::Kind::Url;
  source.payload = RcString("test://font.woff");
  fontFace.sources.push_back(std::move(source));

  resourceManager.addFontFaces(std::span<const css::FontFace>(&fontFace, 1));
  std::vector<ParseError> warnings;
  resourceManager.loadResources(&warnings);

  EXPECT_TRUE(resourceManager.loadedFonts().empty());
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_EQ(warnings.front().reason, "Deferred font load: continuous rendering mode");
  EXPECT_EQ(loaderPtr->callCount(), 0);
  EXPECT_EQ(resourceManager.fontLoadTelemetry().deferredForContinuousRendering, 1u);
  EXPECT_EQ(resourceManager.fontLoadTelemetry().scheduledLoads, 0u);

  // Switch to one-shot mode and ensure the deferred font loads.
  resourceManager.setFontRenderMode(FontRenderMode::kOneShot);
  warnings.clear();
  resourceManager.loadResources(&warnings);

  EXPECT_TRUE(warnings.empty());
  ASSERT_EQ(resourceManager.loadedFonts().size(), 1u);
  EXPECT_EQ(resourceManager.loadedFonts().front().font.familyName, "DeferredFont");
  EXPECT_EQ(loaderPtr->callCount(), 1);
  EXPECT_EQ(resourceManager.fontLoadTelemetry().loadedFonts, 1u);
}

}  // namespace donner::svg::components
