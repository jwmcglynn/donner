#pragma once
/// @file
/// A Geode context over fake browser bridges, for counting the synchronous browser calls that
/// Geode work makes from a client thread.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/gpu/browser/tests/FakeBrowserBridge.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"

namespace donner::geode::tests {

/**
 * Synchronous browser calls, by the operation kind the browser GPU owner counts them under.
 *
 * In the browser, every bridge call from a thread other than the canvas owner is one blocking
 * round trip to the owner, except two. Command recording: all commands between
 * `beginCommandBuffer` and `endCommandBuffer` travel as one batch, split every 1024 commands.
 * Deferred calls (buffer writes, texture view, bind group and layout construction, object
 * destruction): they
 * travel with the thread's next round trip, or on their own as a command batch once 256 calls or
 * 256 KiB of payload are waiting. Pipeline descriptors are counted once each here although the
 * browser sends them in several calls.
 */
struct BrowserOwnerRoundTrips {
  uint64_t ownershipQuery = 0;         //!< Combined ownership/loss queries.
  uint64_t deviceLossQuery = 0;        //!< Loss queries.
  uint64_t completionQuery = 0;        //!< Completed-serial queries.
  uint64_t resourceCreation = 0;       //!< Buffer, texture, view, sampler and shader creation.
  uint64_t resourceDestruction = 0;    //!< Object destruction.
  uint64_t bufferWrite = 0;            //!< Queue buffer writes.
  uint64_t textureWrite = 0;           //!< Queue texture writes.
  uint64_t resourceSharing = 0;        //!< Texture shares and their registrations.
  uint64_t commandBatch = 0;           //!< Recorded command batches.
  uint64_t queueSubmit = 0;            //!< Queue submissions.
  uint64_t bindGroupConstruction = 0;  //!< Bind group and bind group layout descriptors.
  uint64_t pipelineConstruction = 0;   //!< Pipeline and pipeline layout descriptors.
  uint64_t other = 0;                  //!< Every other call, such as buffer mapping.

  /// Every round trip of every kind.
  [[nodiscard]] uint64_t total() const;

  /// Equality operator. @param other Counts to compare against.
  bool operator==(const BrowserOwnerRoundTrips& other) const = default;
};

/// How this count models one bridge operation outside command recording, for checking the model
/// against the browser bridge's own dispatch.
struct BrowserOperationModel {
  std::string_view operation;  //!< Bridge method, and leading word of its recorded line.
  std::string_view kind;       //!< `BrowserGpuOperationKind` enumerator its round trip counts as.
  bool deferred = false;       //!< Whether it is in the bridge's own deferred-operation list.
};

/// Every operation the count models; any other operation outside recording counts as Other.
std::vector<BrowserOperationModel> BrowserOperationModels();

/// Prints every kind by name. @param value Counts. @param os Output stream.
void PrintTo(const BrowserOwnerRoundTrips& value, std::ostream* os);

/**
 * A Geode context whose runtime devices are browser devices over fake browser bridges.
 *
 * Every runtime device opened over the context's root, including sibling contexts, runs on one
 * fake browser device, records into one call log and counts into one set of queries. Submissions
 * complete as soon as they are accepted. The fake renders nothing, so this measures the calls
 * Geode work makes, not its pixels.
 */
class FakeBrowserGeodeContext {
public:
  /// Opens the context. @return The context; its \ref device is null if none could be opened.
  static FakeBrowserGeodeContext Open();

  /// The Geode context, or null when it could not be opened.
  std::shared_ptr<GeodeDevice> device;

  /// The round trips made since \ref Open or the previous call, as one frame that ends with the
  /// worker's flush of the calls it still holds, and starts a new span.
  BrowserOwnerRoundTrips takeRoundTrips();

  /// Deferred calls the last span still held after its last round trip, which the frame-end flush
  /// sent. @return The count.
  [[nodiscard]] std::size_t trailingDeferredCalls() const { return trailingDeferredCalls_; }

private:
  std::shared_ptr<std::vector<std::string>> calls_ = std::make_shared<std::vector<std::string>>();
  std::shared_ptr<gpu::browser::FakeBrowserBridge::QueryCounts> queries_ =
      std::make_shared<gpu::browser::FakeBrowserBridge::QueryCounts>();
  std::size_t consumedCalls_ = 0;
  std::size_t trailingDeferredCalls_ = 0;
  gpu::browser::FakeBrowserBridge::QueryCounts consumedQueries_;
};

}  // namespace donner::geode::tests
