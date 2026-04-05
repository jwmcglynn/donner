#include "donner/svg/components/resources/SubDocumentCache.h"

#include <gtest/gtest.h>

#include <unordered_map>
#include <vector>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"

namespace donner::svg::components {
namespace {

SVGDocument MakeDocumentWithCanvas(int size) {
  SVGDocument document;
  document.setCanvasSize(size, size);
  return document;
}

SubDocumentCache::ParseCallback MakeDocumentCallback(int size) {
  return [size](const std::vector<uint8_t>& /*content*/,
                std::vector<ParseError>* /*outWarnings*/) -> std::optional<SVGDocumentHandle> {
    return MakeDocumentWithCanvas(size).handle();
  };
}

SubDocumentCache::ParseCallback MakeFailingCallback() {
  return [](const std::vector<uint8_t>& /*content*/,
            std::vector<ParseError>* outWarnings) -> std::optional<SVGDocumentHandle> {
    if (outWarnings) {
      ParseError err;
      err.reason = "Parse failed";
      outWarnings->emplace_back(err);
    }
    return std::nullopt;
  };
}

TEST(SubDocumentCacheTest, GetOrParseCachesResult) {
  SubDocumentCache cache;
  const std::vector<uint8_t> content{'h', 'e', 'l', 'l', 'o'};

  int parseCount = 0;
  auto callback = [&parseCount](
                      const std::vector<uint8_t>& /*data*/,
                      std::vector<ParseError>* /*warnings*/) -> std::optional<SVGDocumentHandle> {
    ++parseCount;
    return MakeDocumentWithCanvas(101).handle();
  };

  auto first = cache.getOrParse("test.svg", content, callback, nullptr);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(SVGDocument::CreateFromHandle(*first).canvasSize(), Vector2i(101, 101));
  EXPECT_EQ(parseCount, 1);

  auto second = cache.getOrParse("test.svg", content, callback, nullptr);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*first, *second);
  EXPECT_EQ(parseCount, 1);
  EXPECT_EQ(cache.size(), 1u);
}

TEST(SubDocumentCacheTest, DifferentUrlsCachedSeparately) {
  SubDocumentCache cache;
  const std::vector<uint8_t> contentA{'a'};
  const std::vector<uint8_t> contentB{'b'};

  auto a = cache.getOrParse("a.svg", contentA, MakeDocumentCallback(201), nullptr);
  auto b = cache.getOrParse("b.svg", contentB, MakeDocumentCallback(202), nullptr);

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

  auto parsed = cache.getOrParse("x.svg", content, MakeDocumentCallback(301), nullptr);
  ASSERT_TRUE(parsed.has_value());

  auto result = cache.get("x.svg");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(SVGDocument::CreateFromHandle(*result).canvasSize(), Vector2i(301, 301));
}

TEST(SubDocumentCacheTest, ParseFailureReturnsNull) {
  SubDocumentCache cache;
  const std::vector<uint8_t> content{'x'};

  std::vector<ParseError> warnings;
  auto result = cache.getOrParse("fail.svg", content, MakeFailingCallback(), &warnings);

  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(warnings.empty());
  EXPECT_EQ(cache.size(), 0u);
}

TEST(SubDocumentCacheTest, RecursionDetection) {
  SubDocumentCache cache;
  const std::vector<uint8_t> content{'r'};

  std::vector<ParseError> warnings;
  bool recursionDetected = false;

  SubDocumentCache::ParseCallback recursiveCallback =
      [&cache, &content, &recursionDetected](
          const std::vector<uint8_t>& /*data*/,
          std::vector<ParseError>* outWarnings) -> std::optional<SVGDocumentHandle> {
    auto nested = cache.getOrParse("circular.svg", content, MakeDocumentCallback(401), outWarnings);
    if (!nested.has_value()) {
      recursionDetected = true;
    }
    return MakeDocumentWithCanvas(402).handle();
  };

  auto result = cache.getOrParse("circular.svg", content, recursiveCallback, &warnings);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(SVGDocument::CreateFromHandle(*result).canvasSize(), Vector2i(402, 402));
  EXPECT_TRUE(recursionDetected);

  bool hasCircularWarning = false;
  for (const auto& warning : warnings) {
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
          std::vector<ParseError>* /*warnings*/) -> std::optional<SVGDocumentHandle> {
    wasLoadingDuringParse = cache.isLoading("test.svg");
    return MakeDocumentWithCanvas(501).handle();
  };

  EXPECT_FALSE(cache.isLoading("test.svg"));
  cache.getOrParse("test.svg", content, callback, nullptr);
  EXPECT_TRUE(wasLoadingDuringParse);
  EXPECT_FALSE(cache.isLoading("test.svg"));
}

TEST(SubDocumentCacheTest, IndirectRecursionDetection) {
  SubDocumentCache cache;
  const std::vector<uint8_t> contentA{'a'};
  const std::vector<uint8_t> contentB{'b'};

  std::vector<ParseError> warnings;
  bool bTriedToLoadA = false;

  SubDocumentCache::ParseCallback callbackA;
  SubDocumentCache::ParseCallback callbackB =
      [&cache, &contentA, &bTriedToLoadA, &callbackA](
          const std::vector<uint8_t>& /*data*/,
          std::vector<ParseError>* outWarnings) -> std::optional<SVGDocumentHandle> {
    auto result = cache.getOrParse("a.svg", contentA, callbackA, outWarnings);
    bTriedToLoadA = !result.has_value();
    return MakeDocumentWithCanvas(601).handle();
  };

  callbackA = [&cache, &contentB, &callbackB](
                  const std::vector<uint8_t>& /*data*/,
                  std::vector<ParseError>* outWarnings) -> std::optional<SVGDocumentHandle> {
    cache.getOrParse("b.svg", contentB, callbackB, outWarnings);
    return MakeDocumentWithCanvas(602).handle();
  };

  auto result = cache.getOrParse("a.svg", contentA, callbackA, &warnings);
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

  Registry registry_;
  ResourceManagerContext* resourceManager_ = nullptr;
};

TEST_F(ResourceManagerContextTest, LoadExternalSVGNoResourceLoader) {
  setSvgParseCallback([](const std::vector<uint8_t>&,
                         std::vector<ParseError>*) -> std::optional<SVGDocumentHandle> {
    return MakeDocumentWithCanvas(701).handle();
  });

  std::vector<ParseError> warnings;
  auto result = resourceManager_->loadExternalSVG("test.svg", &warnings);

  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(warnings.empty());
}

TEST_F(ResourceManagerContextTest, LoadExternalSVGNoParseCallback) {
  auto loader = std::make_unique<TestResourceLoader>();
  loader->addFile("test.svg", {'<', 's', 'v', 'g', '>'});
  setResourceLoader(std::move(loader));

  std::vector<ParseError> warnings;
  auto result = resourceManager_->loadExternalSVG("test.svg", &warnings);

  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(warnings.empty());
}

TEST_F(ResourceManagerContextTest, LoadExternalSVGSecureModeBlocked) {
  auto loader = std::make_unique<TestResourceLoader>();
  loader->addFile("test.svg", {'<', 's', 'v', 'g', '>'});
  setResourceLoader(std::move(loader));
  setSvgParseCallback([](const std::vector<uint8_t>&,
                         std::vector<ParseError>*) -> std::optional<SVGDocumentHandle> {
    return MakeDocumentWithCanvas(702).handle();
  });
  setProcessingMode(ProcessingMode::SecureStatic);

  auto result = resourceManager_->loadExternalSVG("test.svg", nullptr);
  EXPECT_FALSE(result.has_value());
}

TEST_F(ResourceManagerContextTest, LoadExternalSVGFileNotFound) {
  auto loader = std::make_unique<TestResourceLoader>();
  setResourceLoader(std::move(loader));
  setSvgParseCallback([](const std::vector<uint8_t>&,
                         std::vector<ParseError>*) -> std::optional<SVGDocumentHandle> {
    return MakeDocumentWithCanvas(703).handle();
  });

  std::vector<ParseError> warnings;
  auto result = resourceManager_->loadExternalSVG("missing.svg", &warnings);

  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(warnings.empty());
}

TEST_F(ResourceManagerContextTest, LoadExternalSVGSuccess) {
  auto loader = std::make_unique<TestResourceLoader>();
  const std::vector<uint8_t> svgContent{'<', 's', 'v', 'g', '>'};
  loader->addFile("test.svg", svgContent);
  setResourceLoader(std::move(loader));
  setSvgParseCallback([](const std::vector<uint8_t>& data,
                         std::vector<ParseError>*) -> std::optional<SVGDocumentHandle> {
    return MakeDocumentWithCanvas(static_cast<int>(data.size()) + 700).handle();
  });

  std::vector<ParseError> warnings;
  auto result = resourceManager_->loadExternalSVG("test.svg", &warnings);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(SVGDocument::CreateFromHandle(*result).canvasSize(), Vector2i(705, 705));
  EXPECT_TRUE(warnings.empty());
}

TEST_F(ResourceManagerContextTest, LoadExternalSVGCachesResult) {
  int parseCount = 0;
  auto loader = std::make_unique<TestResourceLoader>();
  loader->addFile("test.svg", {'x'});
  setResourceLoader(std::move(loader));
  setSvgParseCallback([&parseCount](const std::vector<uint8_t>& /*data*/,
                                    std::vector<ParseError>*) -> std::optional<SVGDocumentHandle> {
    ++parseCount;
    return MakeDocumentWithCanvas(801).handle();
  });

  auto first = resourceManager_->loadExternalSVG("test.svg", nullptr);
  auto second = resourceManager_->loadExternalSVG("test.svg", nullptr);

  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*first, *second);
  EXPECT_EQ(parseCount, 1);
}

}  // namespace
}  // namespace donner::svg::components
