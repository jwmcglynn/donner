#include "donner/editor/AddressBarDispatcher.h"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

namespace donner::editor {
namespace {

/// In-memory \ref SvgFetcher for exercising \ref AddressBarDispatcher. Tests
/// drive the outcome of each `fetch()` call directly so the dispatcher's
/// success/error branches are covered without network I/O.
class FakeFetcher : public SvgFetcher {
public:
  FetchHandle fetch(std::string_view uri, FetchCallback cb,
                    FetchProgressCallback progressCb = {}) override {
    ++fetchCalls;
    lastUri.assign(uri);
    const FetchHandle handle = nextHandle_++;
    if (nextBytes.has_value()) {
      FetchBytes payload = std::move(*nextBytes);
      nextBytes.reset();
      // Emit a simulated "fully downloaded" progress tick right before
      // completion so tests of the progress pathway have something to
      // observe.
      if (progressCb && nextProgressTotal > 0) {
        progressCb(nextProgressTotal, nextProgressTotal);
      }
      cb(std::move(payload), std::nullopt);
    } else if (nextError.has_value()) {
      FetchError err = std::move(*nextError);
      nextError.reset();
      cb(std::nullopt, std::move(err));
    } else {
      // Leave the handle live so cancellation paths can be tested.
      pendingCallbacks[handle] = std::move(cb);
      pendingProgressCallbacks[handle] = std::move(progressCb);
    }
    return handle;
  }

  void cancel(FetchHandle h) override {
    ++cancelCalls;
    lastCancelledHandle = h;
    pendingCallbacks.erase(h);
    pendingProgressCallbacks.erase(h);
  }

  int fetchCalls = 0;
  int cancelCalls = 0;
  std::string lastUri;
  FetchHandle lastCancelledHandle = 0;
  std::optional<FetchBytes> nextBytes;
  std::optional<FetchError> nextError;
  uint64_t nextProgressTotal = 0;
  std::unordered_map<FetchHandle, FetchCallback> pendingCallbacks;
  std::unordered_map<FetchHandle, FetchProgressCallback> pendingProgressCallbacks;

private:
  FetchHandle nextHandle_ = 1;
};

struct CapturedLoad {
  std::string originUri;
  std::vector<uint8_t> bytes;
  std::optional<std::string> resolvedPath;
};

class AddressBarDispatcherTest : public ::testing::Test {
protected:
  AddressBar bar_;
  FakeFetcher fetcher_;
  std::vector<CapturedLoad> loads_;
  AddressBarDispatcher dispatcher_{bar_, fetcher_, [this](const AddressBarLoadRequest& r) {
                                     loads_.push_back(
                                         {r.originUri, r.bytes, r.resolvedPath});
                                   }};
};

TEST_F(AddressBarDispatcherTest, DropPayloadSkipsFetcherAndInvokesLoad) {
  const std::vector<uint8_t> bytes = {0x3C, 0x73, 0x76, 0x67};  // "<svg"
  bar_.notifyDropPayload("dropped.svg", bytes);

  dispatcher_.pump();

  EXPECT_EQ(fetcher_.fetchCalls, 0);
  ASSERT_EQ(loads_.size(), 1u);
  EXPECT_EQ(loads_[0].originUri, "dropped.svg");
  EXPECT_EQ(loads_[0].bytes, bytes);
  EXPECT_FALSE(loads_[0].resolvedPath.has_value());

  ASSERT_EQ(bar_.history().size(), 1u);
  EXPECT_EQ(bar_.history()[0], "dropped.svg");
}

TEST_F(AddressBarDispatcherTest, SuccessfulFetchRoutesBytesAndLogsHistory) {
  fetcher_.nextBytes = FetchBytes{{0x3C, 0x73, 0x76, 0x67}, "/resolved/file.svg",
                                  "file:///resolved/file.svg"};
  bar_.notifyDropPayload("file:///resolved/file.svg", {});  // uri-only navigation

  dispatcher_.pump();

  EXPECT_EQ(fetcher_.fetchCalls, 1);
  EXPECT_EQ(fetcher_.lastUri, "file:///resolved/file.svg");
  ASSERT_EQ(loads_.size(), 1u);
  EXPECT_EQ(loads_[0].originUri, "file:///resolved/file.svg");
  ASSERT_TRUE(loads_[0].resolvedPath.has_value());
  EXPECT_EQ(*loads_[0].resolvedPath, "/resolved/file.svg");

  ASSERT_EQ(bar_.history().size(), 1u);
  EXPECT_EQ(bar_.history()[0], "file:///resolved/file.svg");
}

TEST_F(AddressBarDispatcherTest, FetchErrorDoesNotInvokeLoadOrPushHistory) {
  fetcher_.nextError =
      FetchError{FetchError::Kind::kNetworkError, "dns lookup failed for example.com", {}};
  bar_.notifyDropPayload("https://example.com/foo.svg", {});

  dispatcher_.pump();

  EXPECT_EQ(fetcher_.fetchCalls, 1);
  EXPECT_TRUE(loads_.empty());
  EXPECT_TRUE(bar_.history().empty());
}

TEST_F(AddressBarDispatcherTest, SecondNavigationCancelsInFlightFetch) {
  // First navigation: fetcher leaves the callback pending so we can verify
  // cancellation.
  bar_.notifyDropPayload("https://slow.example/first.svg", {});
  dispatcher_.pump();
  ASSERT_EQ(fetcher_.fetchCalls, 1);
  EXPECT_EQ(fetcher_.cancelCalls, 0);
  EXPECT_EQ(fetcher_.pendingCallbacks.size(), 1u);

  // Second navigation: should cancel the first before issuing the new fetch.
  bar_.notifyDropPayload("https://fast.example/second.svg", {});
  dispatcher_.pump();

  EXPECT_EQ(fetcher_.fetchCalls, 2);
  EXPECT_EQ(fetcher_.cancelCalls, 1);
  EXPECT_EQ(fetcher_.lastUri, "https://fast.example/second.svg");
}

TEST_F(AddressBarDispatcherTest, DestructorCancelsPendingFetch) {
  AddressBar localBar;
  FakeFetcher localFetcher;
  {
    AddressBarDispatcher local(localBar, localFetcher, [](const AddressBarLoadRequest&) {});
    localBar.notifyDropPayload("https://example.com/foo.svg", {});
    local.pump();
    EXPECT_EQ(localFetcher.cancelCalls, 0);
    EXPECT_EQ(localFetcher.pendingCallbacks.size(), 1u);
  }
  // Dispatcher went out of scope with a fetch still pending — it should have
  // cancelled it.
  EXPECT_EQ(localFetcher.cancelCalls, 1);
}

TEST_F(AddressBarDispatcherTest, PumpIsNoOpWhenNothingPending) {
  dispatcher_.pump();
  EXPECT_EQ(fetcher_.fetchCalls, 0);
  EXPECT_TRUE(loads_.empty());
}

// Regression gate for the on-demand render loop: the editor's main loop
// reads `isFetchInFlight()` to decide whether to keep polling while a
// URI fetch is in progress. If this returned false during a pending
// fetch the loop would go idle, the completion callback would land on
// a tick no one was listening to, and the loading chip would stall.
TEST_F(AddressBarDispatcherTest, IsFetchInFlightReflectsActiveHandle) {
  EXPECT_FALSE(dispatcher_.isFetchInFlight());

  // Kick off a fetch that the fake fetcher leaves pending.
  bar_.notifyDropPayload("https://slow.example/foo.svg", {});
  dispatcher_.pump();
  EXPECT_TRUE(dispatcher_.isFetchInFlight());

  // Now resolve it by firing the stored callback with some bytes.
  ASSERT_EQ(fetcher_.pendingCallbacks.size(), 1u);
  auto cb = std::move(fetcher_.pendingCallbacks.begin()->second);
  fetcher_.pendingCallbacks.clear();
  cb(FetchBytes{{0x3C, 0x73, 0x76, 0x67}, {}, "https://slow.example/foo.svg"}, std::nullopt);
  EXPECT_FALSE(dispatcher_.isFetchInFlight());
}

TEST_F(AddressBarDispatcherTest, IsFetchInFlightClearsAfterAsyncSuccess) {
  // Async success path: fetch leaves callback pending, then we resolve it
  // later — mirrors the real WASM / HTTP fetcher where bytes arrive on a
  // later tick.
  bar_.notifyDropPayload("https://slow.example/ok.svg", {});
  dispatcher_.pump();
  EXPECT_TRUE(dispatcher_.isFetchInFlight());

  ASSERT_EQ(fetcher_.pendingCallbacks.size(), 1u);
  auto cb = std::move(fetcher_.pendingCallbacks.begin()->second);
  fetcher_.pendingCallbacks.clear();
  cb(FetchBytes{{0x3C, 0x73, 0x76, 0x67}, {}, "https://slow.example/ok.svg"}, std::nullopt);
  EXPECT_FALSE(dispatcher_.isFetchInFlight());
}

TEST_F(AddressBarDispatcherTest, IsFetchInFlightClearsAfterAsyncError) {
  // Async error path — same reasoning as the success case.
  bar_.notifyDropPayload("https://slow.example/err.svg", {});
  dispatcher_.pump();
  EXPECT_TRUE(dispatcher_.isFetchInFlight());

  ASSERT_EQ(fetcher_.pendingCallbacks.size(), 1u);
  auto cb = std::move(fetcher_.pendingCallbacks.begin()->second);
  fetcher_.pendingCallbacks.clear();
  cb(std::nullopt, FetchError{FetchError::Kind::kNetworkError, "timed out", {}});
  EXPECT_FALSE(dispatcher_.isFetchInFlight());
}

}  // namespace
}  // namespace donner::editor
