#include "donner/base/PackagedResource.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "donner/base/tests/Runfiles.h"
#include "donner/base/tests/TestTempDir.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner {
namespace {
using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::NotNull;

const PackagedResource kFixture{
    "kFixture",
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    3,
    3,
    "image/svg+xml",
    "sample",
    "assets/ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad.svg",
    ""};

template <typename T>
concept CanBorrowData = requires(T&& value) { std::forward<T>(value).data(); };
template <typename T>
concept CanBorrowText = requires(T&& value) { std::forward<T>(value).text(); };
template <typename T>
concept CanBorrowBytes = requires(T&& value) { std::forward<T>(value).bytes(); };
static_assert(CanBorrowData<ResourceLease&> && CanBorrowText<ResourceLease&> &&
              CanBorrowBytes<ResourceLease&>);
static_assert(!CanBorrowData<ResourceLease> && !CanBorrowText<ResourceLease> &&
              !CanBorrowBytes<ResourceLease>);
static_assert(std::is_convertible_v<const ResourceLease&, std::span<const uint8_t>>);
static_assert(!std::is_convertible_v<ResourceLease&&, std::span<const uint8_t>>);
static_assert(!std::is_constructible_v<std::span<const uint8_t>, ResourceLease&&>);

TEST(PackagedResource, DeferredRequestsCoalesceUntilSuccessOrExplicitRetry) {
  PackagedResourceSession session;
  size_t dispatches = 0;
  for (size_t frame = 0; frame < 1000; ++frame) {
    session.request(kFixture, [&] { ++dispatches; });
  }
  EXPECT_EQ(dispatches, 1u);
  EXPECT_EQ(session.ready(kFixture), false);
  EXPECT_EQ(session.retry(kFixture), false);
  session.fail(kFixture);
  for (size_t frame = 0; frame < 1000; ++frame) {
    session.request(kFixture, [&] { ++dispatches; });
  }
  EXPECT_EQ(dispatches, 1u);
  EXPECT_EQ(session.failed(kFixture), true);
  EXPECT_EQ(session.retry(kFixture), true);
  session.request(kFixture, [&] {
    ++dispatches;
    EXPECT_EQ(session.install(kFixture, std::vector<uint8_t>{'a', 'b', 'c'}), true);
  });
  for (size_t frame = 0; frame < 1000; ++frame) {
    session.request(kFixture, [&] { ++dispatches; });
  }
  EXPECT_EQ(dispatches, 2u);
  EXPECT_EQ(session.ready(kFixture), true);
  EXPECT_EQ(session.failed(kFixture), false);
  EXPECT_EQ(session.retry(kFixture), false);
  EXPECT_EQ(session.retainedBytes(), 3u);
}

TEST(PackagedResource, InvalidDeferredCompletionEndsPendingAndAllowsExplicitRetry) {
  PackagedResourceSession session;
  size_t dispatches = 0;
  EXPECT_EQ(session.request(kFixture, [&] { ++dispatches; }), true);
  EXPECT_EQ(session.install(kFixture, std::vector<uint8_t>{'x', 'b', 'c'}), false);
  EXPECT_EQ(session.failed(kFixture), true);
  EXPECT_EQ(session.retry(kFixture), true);
  EXPECT_EQ(session.request(kFixture, [&] { ++dispatches; }), true);
  EXPECT_EQ(dispatches, 2u);
  session.fail(kFixture);
}

TEST(PackagedResource, CanonicalSerializationMatchesGeneratorFixture) {
  std::ifstream stream(
      Runfiles::instance().Rlocation("build_defs/testdata/packaged-resource-manifest.json"));
  ASSERT_EQ(stream.is_open(), true);
  const std::string expected{std::istreambuf_iterator<char>(stream),
                             std::istreambuf_iterator<char>()};
  EXPECT_EQ(DonnerPackagedResourceManifest(), expected);
}

TEST(PackagedResource, HashKnownVectorsAndBlockBoundaries) {
  EXPECT_EQ(ResourceSha256({}), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(ResourceSha256(std::vector<uint8_t>{'a', 'b', 'c'}), kFixture.sha256);
  const std::string value = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  EXPECT_EQ(ResourceSha256({reinterpret_cast<const uint8_t*>(value.data()), value.size()}),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  const std::vector<uint8_t> million(1000000, 'a');
  EXPECT_EQ(ResourceSha256(million),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(PackagedResource, OwnsInstalledBytesAndLeaseSurvivesSessionTeardown) {
  auto session = std::make_shared<PackagedResourceSession>();
  std::vector<uint8_t> input{'a', 'b', 'c'};
  ASSERT_EQ(session->install(kFixture, input), true);
  input[0] = 'z';
  ResourceBytes lease = session->load(kFixture);
  ASSERT_THAT(lease, NotNull());
  EXPECT_THAT(*lease, ElementsAre('a', 'b', 'c'));
  EXPECT_EQ(session->retainedBytes(), 3u);
  EXPECT_EQ(session->install(kFixture, std::vector<uint8_t>{'a', 'b', 'c'}), true);
  EXPECT_EQ(session->retainedBytes(), 3u);
  session.reset();
  EXPECT_THAT(*lease, ElementsAre('a', 'b', 'c'));
}

TEST(PackagedResource, OwningLeaseAllowsSessionSwitchWithoutDangling) {
  const auto previous = GetPackagedResourceSession();
  auto first = std::make_shared<PackagedResourceSession>();
  ASSERT_EQ(first->install(kFixture, std::vector<uint8_t>{'a', 'b', 'c'}), true);
  ASSERT_EQ(SetPackagedResourceSession(first), true);
  ResourceLease lease(kFixture);
  auto second = std::make_shared<PackagedResourceSession>();
  EXPECT_EQ(SetPackagedResourceSession(second), true);
  first.reset();
  EXPECT_THAT(lease.bytes(), ElementsAre('a', 'b', 'c'));
  EXPECT_EQ(SetPackagedResourceSession(previous), true);
}

TEST(PackagedResource, LegacyBorrowedSpanPreventsSessionReplacement) {
  EXPECT_EXIT(
      {
        auto session = std::make_shared<PackagedResourceSession>();
        if (!session->install(kFixture, std::vector<uint8_t>{'a', 'b', 'c'}) ||
            !SetPackagedResourceSession(session)) {
          std::_Exit(1);
        }
        const auto borrowed = kFixture.view();
        if (SetPackagedResourceSession(std::make_shared<PackagedResourceSession>()) ||
            GetPackagedResourceSession() != session || borrowed.size() != 3 || borrowed[0] != 'a') {
          std::_Exit(2);
        }
        std::_Exit(0);
      },
      testing::ExitedWithCode(0), "");
}

TEST(PackagedResource, RejectsTruncatedCorruptOversizedAndUnknownBytes) {
  PackagedResourceSession session;
  EXPECT_EQ(session.install(kFixture, std::vector<uint8_t>{'a', 'b'}), false);
  EXPECT_EQ(session.install(kFixture, std::vector<uint8_t>{'a', 'b', 'x'}), false);
  EXPECT_EQ(session.install(kFixture, std::vector<uint8_t>{'a', 'b', 'c', 'd'}), false);
  EXPECT_EQ(session.ready(kFixture), false);
  EXPECT_EQ(session.retainedBytes(), 0u);
  EXPECT_THAT(session.error(), HasSubstr("Invalid packaged resource"));
  EXPECT_EQ(DonnerInstallPackagedResource(std::string(64, '0').c_str(),
                                          reinterpret_cast<const uint8_t*>("abc"), 3),
            0);
}

TEST(PackagedResource, RejectsTraversalEvenWithMatchingPayload) {
  const PackagedResource traversal{"kTraversal", kFixture.sha256,         3, 3, "image/svg+xml",
                                   "sample",     "assets/../outside.svg", ""};
  PackagedResourceSession session;
  EXPECT_EQ(session.install(traversal, std::vector<uint8_t>{'a', 'b', 'c'}), false);
}

TEST(PackagedResource, ProviderPayloadRemainsUntrusted) {
  PackagedResourceSession session([](const PackagedResource&) -> ResourceBytes {
    return std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{'a', 'b', 'x'});
  });
  EXPECT_EQ(session.load(kFixture), nullptr);
  EXPECT_EQ(session.failed(kFixture), true);
  EXPECT_EQ(session.retainedBytes(), 0u);
}

TEST(PackagedResource, DirectoryProviderRejectsSymlinksAndRelocatesCompleteSet) {
  const auto root = TestTempDir() / "packaged-resource-relocation" / "donner-assets";
  std::filesystem::create_directories(root / "assets");
  {
    std::ofstream stream(root / "manifest.json");
    stream << DonnerPackagedResourceManifest();
  }
  const auto asset = root / kFixture.path;
  {
    std::ofstream stream(asset, std::ios::binary);
    stream << "abc";
  }
  PackagedResourceSession installed(PackagedResourceDirectoryProvider(root.string()));
  auto bytes = installed.load(kFixture);
  ASSERT_THAT(bytes, NotNull());
  EXPECT_THAT(*bytes, ElementsAre('a', 'b', 'c'));
  std::filesystem::remove(asset);
  std::filesystem::create_symlink(root / "outside.svg", asset);
  {
    std::ofstream stream(root / "outside.svg");
    stream << "abc";
  }
  PackagedResourceSession escaped(PackagedResourceDirectoryProvider(root.string()));
  EXPECT_EQ(escaped.load(kFixture), nullptr);
  EXPECT_THAT(escaped.error(), HasSubstr("Missing packaged resource"));
}

TEST(PackagedResource, InstalledManifestAndExactInventoryAreRequired) {
  const std::array<std::string, 7> mutations = {"missing", "corrupt", "schema", "mixed",
                                                "extra",   "asset",   "link"};
  for (const auto& mutation : mutations) {
    SCOPED_TRACE(mutation);
    const auto root = TestTempDir() / ("manifest-" + mutation);
    std::filesystem::create_directories(root / "assets");
    {
      std::ofstream stream(root / "manifest.json");
      stream << DonnerPackagedResourceManifest();
    }
    {
      std::ofstream stream(root / kFixture.path);
      stream << "abc";
    }
    if (mutation == "missing") {
      std::filesystem::remove(root / "manifest.json");
    } else if (mutation == "corrupt") {
      std::ofstream(root / "manifest.json") << "invalid";
    } else if (mutation == "schema" || mutation == "mixed") {
      std::string manifest = DonnerPackagedResourceManifest();
      if (mutation == "schema") {
        manifest[manifest.find("schemaVersion") + 15] = '2';
      } else {
        manifest[manifest.find(kFixture.sha256)] = '0';
      }
      std::ofstream(root / "manifest.json") << manifest;
    } else if (mutation == "extra") {
      std::ofstream(root / "assets/extra.svg") << "extra";
    } else if (mutation == "asset") {
      std::filesystem::remove(root / kFixture.path);
    } else if (mutation == "link") {
      std::filesystem::rename(root / "manifest.json", root / "outside.json");
      std::filesystem::create_symlink(root / "outside.json", root / "manifest.json");
    }
    PackagedResourceSession session(PackagedResourceDirectoryProvider(root.string()));
    EXPECT_EQ(session.load(kFixture), nullptr);
    EXPECT_EQ(session.retainedBytes(), 0u);
  }
}

TEST(PackagedResource, ManifestEnumerationDoesNotLoadBytes) {
  size_t requests = 0;
  auto session =
      std::make_shared<PackagedResourceSession>([&](const PackagedResource&) -> ResourceBytes {
        ++requests;
        return {};
      });
  const auto previous = GetPackagedResourceSession();
  SetPackagedResourceSession(session);
  const std::string manifest = DonnerPackagedResourceManifest();
  SetPackagedResourceSession(previous);
  EXPECT_THAT(manifest, HasSubstr("\"schemaVersion\":1"));
  EXPECT_THAT(manifest, HasSubstr(kFixture.sha256));
  EXPECT_EQ(requests, 0u);
  EXPECT_EQ(session->retainedBytes(), 0u);
}
}  // namespace
}  // namespace donner
