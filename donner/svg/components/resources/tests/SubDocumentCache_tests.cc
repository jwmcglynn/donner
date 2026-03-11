#include "donner/svg/components/resources/SubDocumentCache.h"

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>

#include "donner/svg/components/resources/ResourceManagerContext.h"

namespace donner::svg::components {
namespace {

// A simple parse callback that wraps the content as a string in std::any.
SubDocumentCache::ParseCallback makeStringCallback() {
  return [](const std::vector<uint8_t>& content,
            std::vector<ParseError>* /*outWarnings*/) -> std::optional<std::any> {
    return std::any(std::string(content.begin(), content.end()));
  };
}

// A parse callback that always fails.
SubDocumentCache::ParseCallback makeFailingCallback() {
  return [](const std::vector<uint8_t>& /*content*/,
            std::vector<ParseError>* outWarnings) -> std::optional<std::any> {
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
  auto callback = [&parseCount](const std::vector<uint8_t>& data,
                                std::vector<ParseError>*) -> std::optional<std::any> {
    ++parseCount;
    return std::any(std::string(data.begin(), data.end()));
  };

  std::any* first = cache.getOrParse("test.svg", content, callback, nullptr);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(std::any_cast<std::string>(*first), "hello");
  EXPECT_EQ(parseCount, 1);

  // Second call should return cached result without re-parsing.
  std::any* second = cache.getOrParse("test.svg", content, callback, nullptr);
  EXPECT_EQ(first, second);
  EXPECT_EQ(parseCount, 1);
  EXPECT_EQ(cache.size(), 1u);
}

TEST(SubDocumentCacheTest, DifferentUrlsCachedSeparately) {
  SubDocumentCache cache;
  const std::vector<uint8_t> contentA{'a'};
  const std::vector<uint8_t> contentB{'b'};

  std::any* a = cache.getOrParse("a.svg", contentA, makeStringCallback(), nullptr);
  std::any* b = cache.getOrParse("b.svg", contentB, makeStringCallback(), nullptr);

  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_NE(a, b);
  EXPECT_EQ(std::any_cast<std::string>(*a), "a");
  EXPECT_EQ(std::any_cast<std::string>(*b), "b");
  EXPECT_EQ(cache.size(), 2u);
}

TEST(SubDocumentCacheTest, GetReturnsNullForMissing) {
  SubDocumentCache cache;
  EXPECT_EQ(cache.get("nonexistent.svg"), nullptr);
}

TEST(SubDocumentCacheTest, GetReturnsCachedDocument) {
  SubDocumentCache cache;
  const std::vector<uint8_t> content{'x'};

  cache.getOrParse("x.svg", content, makeStringCallback(), nullptr);

  std::any* result = cache.get("x.svg");
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(std::any_cast<std::string>(*result), "x");
}

TEST(SubDocumentCacheTest, ParseFailureReturnsNull) {
  SubDocumentCache cache;
  const std::vector<uint8_t> content{'x'};

  std::vector<ParseError> warnings;
  std::any* result = cache.getOrParse("fail.svg", content, makeFailingCallback(), &warnings);

  EXPECT_EQ(result, nullptr);
  EXPECT_FALSE(warnings.empty());
  EXPECT_EQ(cache.size(), 0u);
}

TEST(SubDocumentCacheTest, RecursionDetection) {
  SubDocumentCache cache;
  const std::vector<uint8_t> content{'r'};

  std::vector<ParseError> warnings;
  bool recursionDetected = false;

  // Create a callback that tries to parse the same URL (simulating circular reference).
  SubDocumentCache::ParseCallback recursiveCallback =
      [&cache, &content, &recursionDetected](
          const std::vector<uint8_t>& /*data*/,
          std::vector<ParseError>* outWarnings) -> std::optional<std::any> {
    // During parsing of "circular.svg", try to load "circular.svg" again.
    std::any* nested = cache.getOrParse("circular.svg", content, makeStringCallback(), outWarnings);
    if (nested == nullptr) {
      recursionDetected = true;
    }
    return std::any(std::string("outer"));
  };

  std::any* result = cache.getOrParse("circular.svg", content, recursiveCallback, &warnings);

  // The outer parse should succeed.
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(std::any_cast<std::string>(*result), "outer");

  // The recursive inner call should have been blocked.
  EXPECT_TRUE(recursionDetected);

  // A warning about circular reference should have been emitted.
  bool hasCircularWarning = false;
  for (const auto& w : warnings) {
    if (std::string_view(w.reason).find("Circular") != std::string_view::npos) {
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
      [&cache, &wasLoadingDuringParse](const std::vector<uint8_t>& data,
                                       std::vector<ParseError>*) -> std::optional<std::any> {
    wasLoadingDuringParse = cache.isLoading("test.svg");
    return std::any(std::string(data.begin(), data.end()));
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

  // A.svg's callback tries to load B.svg, and B.svg's callback tries to load A.svg.
  SubDocumentCache::ParseCallback callbackA;
  SubDocumentCache::ParseCallback callbackB =
      [&cache, &contentA, &bTriedToLoadA, &callbackA](
          const std::vector<uint8_t>& /*data*/,
          std::vector<ParseError>* outWarnings) -> std::optional<std::any> {
    // B tries to load A — should detect recursion since A is still loading.
    std::any* result = cache.getOrParse("a.svg", contentA, callbackA, outWarnings);
    bTriedToLoadA = (result == nullptr);
    return std::any(std::string("B"));
  };

  callbackA = [&cache, &contentB, &callbackB](
                  const std::vector<uint8_t>& /*data*/,
                  std::vector<ParseError>* outWarnings) -> std::optional<std::any> {
    // A tries to load B.
    cache.getOrParse("b.svg", contentB, callbackB, outWarnings);
    return std::any(std::string("A"));
  };

  std::any* result = cache.getOrParse("a.svg", contentA, callbackA, &warnings);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(std::any_cast<std::string>(*result), "A");

  // B should have detected that A is still loading.
  EXPECT_TRUE(bTriedToLoadA);
}

// --- ResourceManagerContext::loadExternalSVG tests ---

/// A simple in-process resource loader for testing.
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
  void SetUp() override { resourceManager_ = &registry_.ctx().emplace<ResourceManagerContext>(registry_); }

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
  // No resource loader set — should fail gracefully.
  setSvgParseCallback(
      [](const std::vector<uint8_t>&, std::vector<ParseError>*) -> std::optional<std::any> {
        return std::any(std::string("doc"));
      });

  std::vector<ParseError> warnings;
  std::any* result = resourceManager_->loadExternalSVG("test.svg", &warnings);

  EXPECT_EQ(result, nullptr);
  EXPECT_FALSE(warnings.empty());
}

TEST_F(ResourceManagerContextTest, LoadExternalSVGNoParseCallback) {
  auto loader = std::make_unique<TestResourceLoader>();
  loader->addFile("test.svg", {'<', 's', 'v', 'g', '>'});
  setResourceLoader(std::move(loader));
  // No parse callback set.

  std::vector<ParseError> warnings;
  std::any* result = resourceManager_->loadExternalSVG("test.svg", &warnings);

  EXPECT_EQ(result, nullptr);
  EXPECT_FALSE(warnings.empty());
}

TEST_F(ResourceManagerContextTest, LoadExternalSVGSecureModeBlocked) {
  auto loader = std::make_unique<TestResourceLoader>();
  loader->addFile("test.svg", {'<', 's', 'v', 'g', '>'});
  setResourceLoader(std::move(loader));
  setSvgParseCallback(
      [](const std::vector<uint8_t>&, std::vector<ParseError>*) -> std::optional<std::any> {
        return std::any(std::string("doc"));
      });
  setProcessingMode(ProcessingMode::SecureStatic);

  std::any* result = resourceManager_->loadExternalSVG("test.svg", nullptr);
  EXPECT_EQ(result, nullptr);
}

TEST_F(ResourceManagerContextTest, LoadExternalSVGFileNotFound) {
  auto loader = std::make_unique<TestResourceLoader>();
  // Don't add any files — loader will return NotFound.
  setResourceLoader(std::move(loader));
  setSvgParseCallback(
      [](const std::vector<uint8_t>&, std::vector<ParseError>*) -> std::optional<std::any> {
        return std::any(std::string("doc"));
      });

  std::vector<ParseError> warnings;
  std::any* result = resourceManager_->loadExternalSVG("missing.svg", &warnings);

  EXPECT_EQ(result, nullptr);
  EXPECT_FALSE(warnings.empty());
}

TEST_F(ResourceManagerContextTest, LoadExternalSVGSuccess) {
  auto loader = std::make_unique<TestResourceLoader>();
  const std::vector<uint8_t> svgContent{'<', 's', 'v', 'g', '>'};
  loader->addFile("test.svg", svgContent);
  setResourceLoader(std::move(loader));
  setSvgParseCallback(
      [](const std::vector<uint8_t>& data, std::vector<ParseError>*) -> std::optional<std::any> {
        return std::any(std::string(data.begin(), data.end()));
      });

  std::vector<ParseError> warnings;
  std::any* result = resourceManager_->loadExternalSVG("test.svg", &warnings);

  ASSERT_NE(result, nullptr);
  EXPECT_EQ(std::any_cast<std::string>(*result), "<svg>");
  EXPECT_TRUE(warnings.empty());
}

TEST_F(ResourceManagerContextTest, LoadExternalSVGCachesResult) {
  int parseCount = 0;
  auto loader = std::make_unique<TestResourceLoader>();
  loader->addFile("test.svg", {'x'});
  setResourceLoader(std::move(loader));
  setSvgParseCallback(
      [&parseCount](const std::vector<uint8_t>& data,
                    std::vector<ParseError>*) -> std::optional<std::any> {
        ++parseCount;
        return std::any(std::string(data.begin(), data.end()));
      });

  std::any* first = resourceManager_->loadExternalSVG("test.svg", nullptr);
  std::any* second = resourceManager_->loadExternalSVG("test.svg", nullptr);

  EXPECT_EQ(first, second);
  EXPECT_EQ(parseCount, 1);
}

}  // namespace
}  // namespace donner::svg::components
