/// @file

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "donner/base/PackagedResource.h"
#include "donner/editor/DisclosureChevron.h"
#include "donner/editor/EmbeddedSvgIcon.h"
#include "donner/editor/LayersPanel.h"
#include "donner/editor/SidebarPresenter.h"
#include "donner/editor/ToolbarIconSet.h"
#include "donner/svg/renderer/RendererTinySkia.h"

namespace donner::editor {
namespace {

constexpr std::string_view kIcon =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">)"
    R"(<path d="M8 2 L14 14 L2 14 Z" fill="#000"/></svg>)";

std::span<const uint8_t> BytesOf(std::string_view source) {
  return {reinterpret_cast<const uint8_t*>(source.data()), source.size()};
}

std::vector<EmbeddedSvgIconRequest> AllPrewarmRequests() {
  std::vector<EmbeddedSvgIconRequest> requests;
  for (const auto& group : {ToolbarIconPrewarmRequests(), DisclosureChevronPrewarmRequests(),
                            LayersPanelIconPrewarmRequests(), SidebarIconPrewarmRequests()}) {
    requests.insert(requests.end(), group.begin(), group.end());
  }
  return requests;
}

struct ScopedResourceSession {
  std::shared_ptr<PackagedResourceSession> previous = GetPackagedResourceSession();
  ~ScopedResourceSession() { EXPECT_EQ(SetPackagedResourceSession(previous), true); }

  std::shared_ptr<PackagedResourceSession> fresh() const {
    return std::make_shared<PackagedResourceSession>(
        [source = previous](const PackagedResource& resource) { return source->load(resource); });
  }
};

std::vector<std::weak_ptr<const std::vector<uint8_t>>> WeakRequestOwners(
    const std::vector<EmbeddedSvgIconRequest>& requests, PackagedResourceSession& session) {
  const auto resources = PackagedResources();
  std::vector<std::weak_ptr<const std::vector<uint8_t>>> owners;
  for (const auto& request : requests) {
    const std::string digest = ResourceSha256(request.svgBytes.bytes());
    const auto found = std::find_if(resources.begin(), resources.end(), [&](const auto* resource) {
      return resource->sha256 == digest;
    });
    EXPECT_NE(found, resources.end());
    if (found != resources.end()) {
      const ResourceBytes owner = session.load(**found);
      EXPECT_NE(owner, nullptr);
      if (owner) {
        EXPECT_EQ(owner->data(), request.svgBytes.data());
        owners.push_back(owner);
      }
    }
  }
  return owners;
}

void ExpectOwnersExpired(const std::vector<std::weak_ptr<const std::vector<uint8_t>>>& owners,
                         bool expected) {
  for (std::size_t i = 0; i < owners.size(); ++i) {
    EXPECT_EQ(owners[i].expired(), expected) << "request " << i;
  }
}

TEST(IconPrewarmOwnership, ProducersRetireEncodedBytesWithTheirRequestsAndSession) {
  const ScopedResourceSession restore;
  auto first = restore.fresh();
  ASSERT_EQ(SetPackagedResourceSession(first), true);
  auto firstRequests = AllPrewarmRequests();
  ASSERT_EQ(firstRequests.size(), 22u);
  const auto firstOwners = WeakRequestOwners(firstRequests, *first);
  ASSERT_EQ(firstOwners.size(), firstRequests.size());

  auto second = restore.fresh();
  ASSERT_EQ(SetPackagedResourceSession(second), true);
  first.reset();
  ExpectOwnersExpired(firstOwners, false);
  auto secondRequests = AllPrewarmRequests();
  const auto secondOwners = WeakRequestOwners(secondRequests, *second);
  ASSERT_EQ(secondOwners.size(), secondRequests.size());

  firstRequests.clear();
  ExpectOwnersExpired(firstOwners, true);
  ASSERT_EQ(SetPackagedResourceSession(restore.previous), true);
  second.reset();
  ExpectOwnersExpired(secondOwners, false);
  secondRequests.clear();
  ExpectOwnersExpired(secondOwners, true);
}

class CountingIconRenderer : public svg::RendererTinySkia {
public:
  void draw(svg::SVGDocument& document) override {
    ++drawCalls;
    RendererTinySkia::draw(document);
  }
  void beginFrame(const svg::RenderViewport& viewport) override {
    ++frameCalls;
    RendererTinySkia::beginFrame(viewport);
  }
  int drawCalls = 0;
  int frameCalls = 0;
};

struct ScopedIconRenderer {
  CountingIconRenderer renderer;
  ScopedIconRenderer() { ConfigureEmbeddedSvgIconRenderer(renderer); }
  ~ScopedIconRenderer() { ResetEmbeddedSvgIconRenderer(renderer); }
};

TEST(IconPrewarmOwnership, BitmapCacheRetainsNoEncodedOwnerAfterRequestRetirement) {
  ScopedIconRenderer configured;
  ResourceBytes owner =
      std::make_shared<const std::vector<uint8_t>>(BytesOf(kIcon).begin(), BytesOf(kIcon).end());
  const std::weak_ptr<const std::vector<uint8_t>> weak = owner;
  std::vector<EmbeddedSvgIconRequest> requests = {{ResourceLease(owner), 20, true}};
  PrewarmEmbeddedSvgIcons(requests);
  ASSERT_EQ(configured.renderer.frameCalls, 1);
  owner.reset();
  requests.clear();
  EXPECT_EQ(weak.expired(), true);
  EXPECT_THAT(RenderEmbeddedSvgIcon(BytesOf(kIcon), 20), testing::Optional(testing::_));
  EXPECT_EQ(configured.renderer.drawCalls, 0);
}

TEST(IconPrewarmOwnership, CacheUsesContentAndEveryRasterIdentityField) {
  ScopedIconRenderer configured;
  const std::string first(kIcon);
  const std::string second(kIcon);
  ASSERT_NE(first.data(), second.data());
  const std::array<EmbeddedSvgIconRequest, 1> requests = {{{BytesOf(first), 20, true}}};
  PrewarmEmbeddedSvgIcons(requests);
  ASSERT_EQ(configured.renderer.frameCalls, 1);
  EXPECT_THAT(RenderEmbeddedSvgIcon(BytesOf(second), 20), testing::Optional(testing::_));
  EXPECT_EQ(configured.renderer.drawCalls, 0);
  EXPECT_EQ(configured.renderer.frameCalls, 1);

  std::string changed(first);
  changed.replace(changed.find("#000"), 4, "#fff");
  EXPECT_THAT(RenderEmbeddedSvgIcon(BytesOf(changed), 20), testing::Optional(testing::_));
  EXPECT_EQ(configured.renderer.drawCalls, 1);
  EXPECT_THAT(RenderEmbeddedSvgIcon(BytesOf(second + " "), 20), testing::Optional(testing::_));
  EXPECT_EQ(configured.renderer.drawCalls, 2);
  EXPECT_THAT(RenderEmbeddedSvgIcon(BytesOf(second), 21), testing::Optional(testing::_));
  EXPECT_EQ(configured.renderer.drawCalls, 3);
  EXPECT_THAT(RenderEmbeddedSvgArtwork(BytesOf(second), 20), testing::Optional(testing::_));
  EXPECT_EQ(configured.renderer.drawCalls, 4);

  constexpr int kRepeats = 100;
  const auto cachedStart = std::chrono::steady_clock::now();
  for (int i = 0; i < kRepeats; ++i) {
    EXPECT_THAT(RenderEmbeddedSvgIcon(BytesOf(second), 20), testing::Optional(testing::_));
  }
  const auto cachedElapsed = std::chrono::steady_clock::now() - cachedStart;
  EXPECT_EQ(configured.renderer.drawCalls, 4);
  const auto rasterStart = std::chrono::steady_clock::now();
  for (int i = 0; i < kRepeats; ++i) {
    EXPECT_THAT(RenderEmbeddedSvgArtwork(BytesOf(second), 20), testing::Optional(testing::_));
  }
  const auto rasterElapsed = std::chrono::steady_clock::now() - rasterStart;
  EXPECT_EQ(configured.renderer.drawCalls, 4 + kRepeats);
  RecordProperty(
      "contentCacheNanosecondsPerCall",
      std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(cachedElapsed).count() /
                     kRepeats));
  RecordProperty(
      "uncachedRasterNanosecondsPerCall",
      std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(rasterElapsed).count() /
                     kRepeats));
}

}  // namespace
}  // namespace donner::editor
