#include "donner/svg/components/resources/SubDocumentCache.h"

#include <gtest/gtest.h>

#include <array>
#include <unordered_map>
#include <vector>

#include "donner/base/encoding/Base64.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#define private public
#include "donner/svg/components/resources/ResourceManagerContext.h"
#undef private

namespace donner::svg::components {
namespace {

SVGDocument MakeDocumentWithCanvas(int size) {
  SVGDocument document;
  document.setCanvasSize(size, size);
  return document;
}

SubDocumentCache::ParseCallback MakeDocumentCallback(int size) {
  return [size](const std::vector<uint8_t>& /*content*/,
                ParseWarningSink& /*warningSink*/) -> std::optional<SVGDocumentHandle> {
    return MakeDocumentWithCanvas(size).handle();
  };
}

SubDocumentCache::ParseCallback MakeFailingCallback() {
  return [](const std::vector<uint8_t>& /*content*/,
            ParseWarningSink& warningSink) -> std::optional<SVGDocumentHandle> {
    ParseDiagnostic err;
    err.reason = "Parse failed";
    warningSink.add(std::move(err));
    return std::nullopt;
  };
}

constexpr std::string_view kTinyPngDataUrl =
    "data:image/png;base64,"
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEUlEQVR42mP4z8DwH4QZYAwAR8oH+Rq28akAAAAASUVORK5CYII=";

constexpr std::string_view kValidWoffBase64 =
    "d09GRk9UVE8AAAVAAAkAAAAAB0AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABDRkYgAAADXAAAAdEAAAIu"
    "idw6/09TLzIAAAFAAAAARQAAAGB9nYChY21hcAAAAvwAAABMAAAA5gCvAdxoZWFkAAAA4AAAADYAAAA2"
    "+lXhk2hoZWEAAAEYAAAAIAAAACQL+QhvaG10eAAABTAAAAAQAAAAEBfXAMhtYXhwAAABOAAAAAYAAAAG"
    "AARQAG5hbWUAAAGIAAABcQAAApcq8MrgcG9zdAAAA0gAAAATAAAAIP+4ADIAAQAAAAEAAETkSHhfDzz1"
    "AAMD6AAAAADJHRikAAAAAMkhhVMAZP/2CJwCxgAAAAMAAgAAAAAAAHicY2BkYGB695+NgZOTgSGFIYVj"
    "DgNQBAWwAABbPwN6AABQAAAEAAB4nGNgYf3KOIGBlYGBqYspgoGBwRtCM8YxGDHqMKACRmSOX35eKuMB"
    "BgWGAKZ3/9kYGJhfMAA5EDWMX5jeMYC5ACTpDDIAAAB4nI1SvU7CUBg95d9BF42Dg7mTkzSFhACpG7Q4"
    "QCFA6Ayk/AToJaUw+A4mvoO7b8AT+FKetleCxsF7k+Z85+9+QwFc4QMaoqPhMv5GJ4U8pwSncY8bhTNn"
    "niyeUVY4h2u8KFzALV6Z0jIXZO7wrnCKbx0VTuMJnwpn8KDlFc7iTSsqnDvjC3jUjm7XtsXQ24WiYdt9"
    "b75fj4OSbhiG6UjfMyM5UikWlTrygt1S+iJx9QbCMBK4kOFU+odI0CtVczNeeTKc6evlpEymXKrVK6Jp"
    "jax2t9exnOEf1a43saUf7oQrg9XSn4tWIPdbuOjC5hUYwsMOIVEjZvqc59hjjTEClKDDiK8JBxI+VfOU"
    "/s4myeKv7P9cI04B9WXcLn682MOATITP2QWdIaax/3BK6KigSnXD1hU7I8+M7JrNE/4AiadMdw11egWa"
    "sPi6hTb37KFD5HBXl9kJ94zaQ+4lyEhuuGKPz70FWpwk999+AX0lb34AAAB4nGNgYGBmgGAZBkYGEPAB"
    "8hjBfBYGAyDNAYRMQFqBwY0h4P9/BOv/w/97/m+G6gIBNoZZQJKTwQMuhpDDB4hTRRxgwiLGDABzRQyF"
    "eJxjYGYAg/9bGYwYsAAALMIB6gB4nGNkYGFhYGRkFAn3d3MLSS0ucXZz0w1KTS/NSSwCiSv+4Gf4Ic30"
    "Q4b5hyzL+25ZBgaOOT+bWL9z8E//sU3oO78gAzMjo5iyJki7Aki/AtAABagBKIJ++UW5iTkMQEOBmEGd"
    "wZCBhQnIYGJgZkhinM3Hp/pzudiq79ys3+PZVv8GUl1svzN/BrD+3gOk/waw/ikU+974veN753ez353f"
    "xb5v/m7xZwGbDJvR3wDRH7O+e/xe+2PO70a23w+A/O+NPwOA7D9uvyNEj/+uYP3Nw8an8EPu+zHR77oP"
    "735n/85ueve3rvzvXTXP/hoZ1HzfO0vt+3a2nzy/t4vWfF8xy4P996Sfbqz1bL/z/zxldSuKzQ+Qbqv5"
    "Pn1WIZtHxtpLS+W3fedg/V7DdvN3IeuP1z+9RfMb2lprS+PX2kr/FjBwUtTZ7/1d3U/+u1D4yngbKauQ"
    "SCsVs4vfbarkan5PnMW2sH/GrLk7Vqe9lf4u9vzud4572Wd/O16R/82zN2vbZalL+/bcfXpG/zf3DLlZ"
    "38vY/n36nSz6mzvKR1vO8gfDXbbXe3x+c8t/P3NA9DvD76OsvwvZZFh7fsTT0AV83d1sP5tEgBHPmSYK"
    "ANvV9+MAAAAC7gAAAu4AAAkAAGQI+wBk";

TEST(SubDocumentCacheTest, GetOrParseCachesResult) {
  SubDocumentCache cache;
  const std::vector<uint8_t> content{'h', 'e', 'l', 'l', 'o'};

  int parseCount = 0;
  auto callback = [&parseCount](
                      const std::vector<uint8_t>& /*data*/,
                      ParseWarningSink& /*warningSink*/) -> std::optional<SVGDocumentHandle> {
    ++parseCount;
    return MakeDocumentWithCanvas(101).handle();
  };

  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  auto first = cache.getOrParse("test.svg", content, callback, disabledSink);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(SVGDocument::CreateFromHandle(*first).canvasSize(), Vector2i(101, 101));
  EXPECT_EQ(parseCount, 1);

  auto second = cache.getOrParse("test.svg", content, callback, disabledSink);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*first, *second);
  EXPECT_EQ(parseCount, 1);
  EXPECT_EQ(cache.size(), 1u);
}

TEST(SubDocumentCacheTest, DifferentUrlsCachedSeparately) {
  SubDocumentCache cache;
  const std::vector<uint8_t> contentA{'a'};
  const std::vector<uint8_t> contentB{'b'};

  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  auto a = cache.getOrParse("a.svg", contentA, MakeDocumentCallback(201), disabledSink);
  auto b = cache.getOrParse("b.svg", contentB, MakeDocumentCallback(202), disabledSink);

  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_NE(*a, *b);
  EXPECT_EQ(SVGDocument::CreateFromHandle(*a).canvasSize(), Vector2i(201, 201));
  EXPECT_EQ(SVGDocument::CreateFromHandle(*b).canvasSize(), Vector2i(202, 202));
  EXPECT_EQ(cache.size(), 2u);
}

TEST(SubDocumentCacheTest, GetReturnsNullForMissing) {
  SubDocumentCache cache;
  EXPECT_FALSE(cache.get("nonexistent.svg").has_value());
}

TEST(SubDocumentCacheTest, GetReturnsCachedDocument) {
  SubDocumentCache cache;
  const std::vector<uint8_t> content{'x'};

  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  auto parsed = cache.getOrParse("x.svg", content, MakeDocumentCallback(301), disabledSink);
  ASSERT_TRUE(parsed.has_value());

  auto result = cache.get("x.svg");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(SVGDocument::CreateFromHandle(*result).canvasSize(), Vector2i(301, 301));
}

TEST(SubDocumentCacheTest, ParseFailureReturnsNull) {
  SubDocumentCache cache;
  const std::vector<uint8_t> content{'x'};

  ParseWarningSink warnings;
  auto result = cache.getOrParse("fail.svg", content, MakeFailingCallback(), warnings);

  EXPECT_FALSE(result.has_value());
  EXPECT_TRUE(warnings.hasWarnings());
  EXPECT_EQ(cache.size(), 0u);
}

TEST(SubDocumentCacheTest, RecursionDetection) {
  SubDocumentCache cache;
  const std::vector<uint8_t> content{'r'};

  ParseWarningSink warnings;
  bool recursionDetected = false;

  SubDocumentCache::ParseCallback recursiveCallback =
      [&cache, &content, &recursionDetected](
          const std::vector<uint8_t>& /*data*/,
          ParseWarningSink& warningSink) -> std::optional<SVGDocumentHandle> {
    auto nested = cache.getOrParse("circular.svg", content, MakeDocumentCallback(401), warningSink);
    if (!nested.has_value()) {
      recursionDetected = true;
    }
    return MakeDocumentWithCanvas(402).handle();
  };

  auto result = cache.getOrParse("circular.svg", content, recursiveCallback, warnings);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(SVGDocument::CreateFromHandle(*result).canvasSize(), Vector2i(402, 402));
  EXPECT_TRUE(recursionDetected);

  bool hasCircularWarning = false;
  for (const auto& warning : warnings.warnings()) {
    if (std::string_view(warning.reason).find("Circular") != std::string_view::npos) {
      hasCircularWarning = true;
      break;
    }
  }
  EXPECT_TRUE(hasCircularWarning);
}

TEST(SubDocumentCacheTest, IsLoadingDuringParse) {
  SubDocumentCache cache;
  const std::vector<uint8_t> content{'t'};

  bool wasLoadingDuringParse = false;
  SubDocumentCache::ParseCallback callback =
      [&cache, &wasLoadingDuringParse](
          const std::vector<uint8_t>& /*data*/,
          ParseWarningSink& /*warningSink*/) -> std::optional<SVGDocumentHandle> {
    wasLoadingDuringParse = cache.isLoading("test.svg");
    return MakeDocumentWithCanvas(501).handle();
  };

  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  EXPECT_FALSE(cache.isLoading("test.svg"));
  cache.getOrParse("test.svg", content, callback, disabledSink);
  EXPECT_TRUE(wasLoadingDuringParse);
  EXPECT_FALSE(cache.isLoading("test.svg"));
}

TEST(SubDocumentCacheTest, IndirectRecursionDetection) {
  SubDocumentCache cache;
  const std::vector<uint8_t> contentA{'a'};
  const std::vector<uint8_t> contentB{'b'};

  ParseWarningSink warnings;
  bool bTriedToLoadA = false;

  SubDocumentCache::ParseCallback callbackA;
  SubDocumentCache::ParseCallback callbackB =
      [&cache, &contentA, &bTriedToLoadA, &callbackA](
          const std::vector<uint8_t>& /*data*/,
          ParseWarningSink& warningSink) -> std::optional<SVGDocumentHandle> {
    auto result = cache.getOrParse("a.svg", contentA, callbackA, warningSink);
    bTriedToLoadA = !result.has_value();
    return MakeDocumentWithCanvas(601).handle();
  };

  callbackA = [&cache, &contentB, &callbackB](
                  const std::vector<uint8_t>& /*data*/,
                  ParseWarningSink& warningSink) -> std::optional<SVGDocumentHandle> {
    cache.getOrParse("b.svg", contentB, callbackB, warningSink);
    return MakeDocumentWithCanvas(602).handle();
  };

  auto result = cache.getOrParse("a.svg", contentA, callbackA, warnings);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(SVGDocument::CreateFromHandle(*result).canvasSize(), Vector2i(602, 602));
  EXPECT_TRUE(bTriedToLoadA);
}

class TestResourceLoader : public ResourceLoaderInterface {
public:
  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view url) override {
    auto it = files_.find(std::string(url));
    if (it != files_.end()) {
      return it->second;
    }
    return ResourceLoaderError::NotFound;
  }

  void addFile(std::string url, std::vector<uint8_t> content) {
    files_[std::move(url)] = std::move(content);
  }

private:
  std::unordered_map<std::string, std::vector<uint8_t>> files_;
};

class ResourceManagerContextTest : public testing::Test {
protected:
  void SetUp() override {
    resourceManager_ = &registry_.ctx().emplace<ResourceManagerContext>(registry_);
  }

  void setResourceLoader(std::unique_ptr<ResourceLoaderInterface> loader) {
    resourceManager_->setResourceLoader(std::move(loader));
  }

  void setSvgParseCallback(SubDocumentCache::ParseCallback callback) {
    resourceManager_->setSvgParseCallback(std::move(callback));
  }

  void setProcessingMode(ProcessingMode mode) { resourceManager_->setProcessingMode(mode); }

  Entity addImage(std::string_view href) {
    Entity entity = registry_.create();
    registry_.emplace<ImageComponent>(entity, ImageComponent{RcString(href)});
    return entity;
  }

  Registry registry_;
  ResourceManagerContext* resourceManager_ = nullptr;
};

TEST_F(ResourceManagerContextTest, LoadExternalSVGNoResourceLoader) {
  setSvgParseCallback([](const std::vector<uint8_t>&,
                         ParseWarningSink&) -> std::optional<SVGDocumentHandle> {
    return MakeDocumentWithCanvas(701).handle();
  });

  ParseWarningSink warnings;
  auto result = resourceManager_->loadExternalSVG("test.svg", warnings);

  EXPECT_FALSE(result.has_value());
  EXPECT_TRUE(warnings.hasWarnings());
}

TEST_F(ResourceManagerContextTest, LoadExternalSVGNoParseCallback) {
  auto loader = std::make_unique<TestResourceLoader>();
  loader->addFile("test.svg", {'<', 's', 'v', 'g', '>'});
  setResourceLoader(std::move(loader));

  ParseWarningSink warnings;
  auto result = resourceManager_->loadExternalSVG("test.svg", warnings);

  EXPECT_FALSE(result.has_value());
  EXPECT_TRUE(warnings.hasWarnings());
}

TEST_F(ResourceManagerContextTest, LoadExternalSVGSecureModeBlocked) {
  auto loader = std::make_unique<TestResourceLoader>();
  loader->addFile("test.svg", {'<', 's', 'v', 'g', '>'});
  setResourceLoader(std::move(loader));
  setSvgParseCallback([](const std::vector<uint8_t>&,
                         ParseWarningSink&) -> std::optional<SVGDocumentHandle> {
    return MakeDocumentWithCanvas(702).handle();
  });
  setProcessingMode(ProcessingMode::SecureStatic);

  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  auto result = resourceManager_->loadExternalSVG("test.svg", disabledSink);
  EXPECT_FALSE(result.has_value());
}

TEST_F(ResourceManagerContextTest, LoadExternalSVGFileNotFound) {
  auto loader = std::make_unique<TestResourceLoader>();
  setResourceLoader(std::move(loader));
  setSvgParseCallback([](const std::vector<uint8_t>&,
                         ParseWarningSink&) -> std::optional<SVGDocumentHandle> {
    return MakeDocumentWithCanvas(703).handle();
  });

  ParseWarningSink warnings;
  auto result = resourceManager_->loadExternalSVG("missing.svg", warnings);

  EXPECT_FALSE(result.has_value());
  EXPECT_TRUE(warnings.hasWarnings());
}

TEST_F(ResourceManagerContextTest, LoadExternalSVGSuccess) {
  auto loader = std::make_unique<TestResourceLoader>();
  const std::vector<uint8_t> svgContent{'<', 's', 'v', 'g', '>'};
  loader->addFile("test.svg", svgContent);
  setResourceLoader(std::move(loader));
  setSvgParseCallback([](const std::vector<uint8_t>& data,
                         ParseWarningSink&) -> std::optional<SVGDocumentHandle> {
    return MakeDocumentWithCanvas(static_cast<int>(data.size()) + 700).handle();
  });

  ParseWarningSink warnings;
  auto result = resourceManager_->loadExternalSVG("test.svg", warnings);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(SVGDocument::CreateFromHandle(*result).canvasSize(), Vector2i(705, 705));
  EXPECT_FALSE(warnings.hasWarnings());
}

TEST_F(ResourceManagerContextTest, LoadExternalSVGCachesResult) {
  int parseCount = 0;
  auto loader = std::make_unique<TestResourceLoader>();
  loader->addFile("test.svg", {'x'});
  setResourceLoader(std::move(loader));
  setSvgParseCallback([&parseCount](const std::vector<uint8_t>& /*data*/,
                                    ParseWarningSink&) -> std::optional<SVGDocumentHandle> {
    ++parseCount;
    return MakeDocumentWithCanvas(801).handle();
  });

  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  auto first = resourceManager_->loadExternalSVG("test.svg", disabledSink);
  auto second = resourceManager_->loadExternalSVG("test.svg", disabledSink);

  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*first, *second);
  EXPECT_EQ(parseCount, 1);
}

TEST_F(ResourceManagerContextTest, LoadResourcesSecureModeSkipsImages) {
  setProcessingMode(ProcessingMode::SecureStatic);

  const Entity imageEntity = addImage(kTinyPngDataUrl);

  ParseWarningSink warnings;
  resourceManager_->loadResources(warnings);

  EXPECT_FALSE(registry_.all_of<LoadedImageComponent>(imageEntity));
  EXPECT_FALSE(warnings.hasWarnings());
}

TEST_F(ResourceManagerContextTest, LoadResourcesWithoutLoaderWarnsForExternalImage) {
  const Entity imageEntity = addImage("missing.png");

  ParseWarningSink warnings;
  resourceManager_->loadResources(warnings);

  EXPECT_TRUE(registry_.all_of<LoadedImageComponent>(imageEntity));
  EXPECT_TRUE(warnings.hasWarnings());
}

TEST_F(ResourceManagerContextTest, LoadResourcesSkipsAlreadyLoadedImages) {
  const Entity imageEntity = addImage("missing.png");
  registry_.emplace<LoadedImageComponent>(imageEntity);

  ParseWarningSink warnings;
  resourceManager_->loadResources(warnings);

  EXPECT_TRUE(registry_.all_of<LoadedImageComponent>(imageEntity));
  EXPECT_TRUE(warnings.hasWarnings());
}

TEST_F(ResourceManagerContextTest, LoadResourcesLoadsRasterImage) {
  setResourceLoader(std::make_unique<TestResourceLoader>());

  const Entity imageEntity = addImage(kTinyPngDataUrl);

  ParseWarningSink warnings;
  resourceManager_->loadResources(warnings);

  ASSERT_TRUE(registry_.all_of<LoadedImageComponent>(imageEntity));
  auto size = resourceManager_->getImageSize(imageEntity);
  ASSERT_TRUE(size.has_value());
  EXPECT_GT(size->x, 0);
  EXPECT_GT(size->y, 0);
  EXPECT_FALSE(warnings.hasWarnings());
}

TEST_F(ResourceManagerContextTest, GetImageSizeReturnsNulloptWithoutLoadedImage) {
  Entity entity = registry_.create();
  EXPECT_EQ(resourceManager_->getImageSize(entity), std::nullopt);
}

TEST_F(ResourceManagerContextTest, LoadResourcesLoadsSvgSubDocument) {
  auto loader = std::make_unique<TestResourceLoader>();
  loader->addFile("image.svg", {'<', 's', 'v', 'g', '/', '>'});
  setResourceLoader(std::move(loader));
  setSvgParseCallback([](const std::vector<uint8_t>&,
                         ParseWarningSink&) -> std::optional<SVGDocumentHandle> {
    return MakeDocumentWithCanvas(901).handle();
  });

  const Entity imageEntity = addImage("image.svg");

  ParseWarningSink warnings;
  resourceManager_->loadResources(warnings);

  EXPECT_TRUE(registry_.all_of<LoadedSVGImageComponent>(imageEntity));
  EXPECT_FALSE(registry_.all_of<LoadedImageComponent>(imageEntity));
  EXPECT_FALSE(warnings.hasWarnings());
}

TEST_F(ResourceManagerContextTest, LoadResourcesSvgWithoutParseCallbackWarns) {
  auto loader = std::make_unique<TestResourceLoader>();
  loader->addFile("image.svg", {'<', 's', 'v', 'g', '/', '>'});
  setResourceLoader(std::move(loader));

  const Entity imageEntity = addImage("image.svg");

  ParseWarningSink warnings;
  resourceManager_->loadResources(warnings);

  EXPECT_TRUE(registry_.all_of<LoadedImageComponent>(imageEntity));
  EXPECT_FALSE(registry_.all_of<LoadedSVGImageComponent>(imageEntity));
  EXPECT_TRUE(warnings.hasWarnings());
}

TEST_F(ResourceManagerContextTest, LoadResourcesSvgParseFailureCreatesEmptyLoadedImage) {
  auto loader = std::make_unique<TestResourceLoader>();
  loader->addFile("image.svg", {'<', 's', 'v', 'g', '/', '>'});
  setResourceLoader(std::move(loader));
  setSvgParseCallback([](const std::vector<uint8_t>&,
                         ParseWarningSink& warningSink) -> std::optional<SVGDocumentHandle> {
    ParseDiagnostic err;
    err.reason = "Parse failed";
    warningSink.add(std::move(err));
    return std::nullopt;
  });

  const Entity imageEntity = addImage("image.svg");

  ParseWarningSink warnings;
  resourceManager_->loadResources(warnings);

  EXPECT_TRUE(registry_.all_of<LoadedImageComponent>(imageEntity));
  EXPECT_FALSE(registry_.all_of<LoadedSVGImageComponent>(imageEntity));
  EXPECT_TRUE(warnings.hasWarnings());
}

TEST_F(ResourceManagerContextTest, AddFontFacesLoadsUrlAndDataFonts) {
  setResourceLoader(std::make_unique<TestResourceLoader>());
  auto maybeFontBytes = donner::DecodeBase64Data(kValidWoffBase64);
  ASSERT_TRUE(maybeFontBytes.hasResult());
  const auto& fontBytes = maybeFontBytes.result();

  css::FontFace urlFace;
  urlFace.familyName = "UrlFont";
  css::FontFaceSource urlSource;
  urlSource.kind = css::FontFaceSource::Kind::Url;
  urlSource.payload = RcString(std::string("data:application/x-font-woff;base64,") +
                               std::string(kValidWoffBase64));
  urlFace.sources.push_back(urlSource);

  css::FontFace dataFace;
  dataFace.familyName = "DataFont";
  css::FontFaceSource dataSource;
  dataSource.kind = css::FontFaceSource::Kind::Data;
  dataSource.payload = std::make_shared<const std::vector<uint8_t>>(fontBytes);
  dataFace.sources.push_back(dataSource);

  const std::array<css::FontFace, 2> fontFaces{urlFace, dataFace};
  resourceManager_->addFontFaces(fontFaces);

  ParseWarningSink warnings;
  resourceManager_->loadResources(warnings);

  EXPECT_EQ(resourceManager_->fontFaces().size(), 2u);
  EXPECT_EQ(resourceManager_->loadedFonts().size(), 2u);
  EXPECT_FALSE(warnings.hasWarnings());
}

TEST_F(ResourceManagerContextTest, LoadResourcesWarnsForInvalidAndUnsupportedFonts) {
  auto loader = std::make_unique<TestResourceLoader>();
  setResourceLoader(std::move(loader));

  css::FontFace invalidDataFace;
  invalidDataFace.familyName = "InvalidData";
  css::FontFaceSource invalidDataSource;
  invalidDataSource.kind = css::FontFaceSource::Kind::Data;
  invalidDataSource.payload =
      std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0x00, 0x01, 0x02});
  invalidDataFace.sources.push_back(invalidDataSource);

  css::FontFace unsupportedFace;
  unsupportedFace.familyName = "Unsupported";
  css::FontFaceSource localSource;
  localSource.kind = css::FontFaceSource::Kind::Local;
  localSource.payload = RcString("Arial");
  unsupportedFace.sources.push_back(localSource);

  css::FontFace missingUrlFace;
  missingUrlFace.familyName = "MissingUrl";
  css::FontFaceSource missingUrlSource;
  missingUrlSource.kind = css::FontFaceSource::Kind::Url;
  missingUrlSource.payload = RcString("missing.woff");
  missingUrlFace.sources.push_back(missingUrlSource);

  const std::array<css::FontFace, 3> fontFaces{invalidDataFace, unsupportedFace, missingUrlFace};
  resourceManager_->addFontFaces(fontFaces);

  ParseWarningSink warnings;
  resourceManager_->loadResources(warnings);

  EXPECT_TRUE(warnings.hasWarnings());
  EXPECT_TRUE(resourceManager_->loadedFonts().empty());
}

TEST_F(ResourceManagerContextTest, GetLoadedImageComponentReturnsExistingLoadedImage) {
  const Entity imageEntity = addImage("ignored");
  auto& loaded = registry_.emplace<LoadedImageComponent>(imageEntity);
  loaded.image = ImageResource{{0xFF, 0x00, 0x00, 0xFF}, 1, 1};

  const LoadedImageComponent* result = resourceManager_->getLoadedImageComponent(imageEntity);

  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(result->image.has_value());
  EXPECT_EQ(result->image->width, 1);
  EXPECT_EQ(result->image->height, 1);
}

TEST_F(ResourceManagerContextTest, GetLoadedImageComponentReturnsNullWithoutImageComponent) {
  Entity entity = registry_.create();
  EXPECT_EQ(resourceManager_->getLoadedImageComponent(entity), nullptr);
}

TEST_F(ResourceManagerContextTest, GetLoadedImageComponentLoadsDataUriWithoutResourceLoader) {
  const Entity imageEntity = addImage(kTinyPngDataUrl);

  const LoadedImageComponent* result = resourceManager_->getLoadedImageComponent(imageEntity);

  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(result->image.has_value());
  EXPECT_GT(result->image->width, 0);
  EXPECT_GT(result->image->height, 0);
}

TEST_F(ResourceManagerContextTest, GetLoadedImageComponentReturnsNullForSvgDataUri) {
  const Entity imageEntity =
      addImage("data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciLz4=");

  EXPECT_EQ(resourceManager_->getLoadedImageComponent(imageEntity), nullptr);
}

}  // namespace
}  // namespace donner::svg::components
