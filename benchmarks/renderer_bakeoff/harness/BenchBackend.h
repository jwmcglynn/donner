#pragma once
/// @file
/// Registry of benchmarkable backends. Each backend library registers a \ref BackendInfo at
/// static-initialization time; the benchmark binary links the backend libraries it wants.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "donner/svg/renderer/RendererInterface.h"
#include "harness/FeatureSupport.h"

namespace donner::bakeoff {

/// One live backend instance owned by the benchmark for a single run.
class BenchBackend {
public:
  virtual ~BenchBackend() = default;

  /// The renderer that receives the document traversal.
  virtual svg::RendererInterface& renderer() = 0;

  /// Blocks until all GPU work submitted so far has completed, without reading pixels back.
  /// Software backends finish their work inside the draw and return immediately.
  /// @return False when the backend could not confirm completion (device lost or timed out).
  virtual bool finishGpuWork() { return true; }

  /// Feature uses and drops recorded by an adapter, or null for a backend that renders every
  /// feature through Donner's own implementation.
  [[nodiscard]] virtual const FeatureUsage* featureUsage() const { return nullptr; }

  /// Clears \ref featureUsage, if any.
  virtual void resetFeatureUsage() {}

  /// GPU-resident bytes the backend reports, or -1 when the backend exposes no such counter.
  [[nodiscard]] virtual std::int64_t gpuAllocatedBytes() const { return -1; }
};

/// Which comparison a backend belongs to.
enum class Suite : uint8_t {
  Gpu,        //!< GPU suite.
  Software,   //!< Software suite.
  Reference,  //!< Measurement aid, such as the null traversal recorder.
};

/// Stable lower-case name of \p suite: `gpu`, `software` or `reference`.
/// @param suite Suite to name.
std::string_view SuiteName(Suite suite);

/// Static description and factory of one backend configuration.
struct BackendInfo {
  std::string name;               //!< Unique identifier, e.g. `thorvg_sw`.
  Suite suite = Suite::Software;  //!< Comparison suite.
  std::string library;            //!< Library name and version, e.g. `ThorVG 1.1.2`.
  std::string api;                //!< Raster API, e.g. `CPU`, `Metal`, `wgpu-native (Metal)`.
  int threads = 1;                //!< Worker threads the library may use; 0 means library default.
  std::string configuration;      //!< Free-form build and runtime configuration notes.
  CapabilityTable capabilities;   //!< Feature support levels.
  /// True for backends whose capability table is defined by Donner's own renderer, which every
  /// other backend is compared against.
  bool donnerBaseline = false;
  /// Creates a fresh instance, or null when the backend cannot run on this host.
  std::function<std::unique_ptr<BenchBackend>()> create;
};

/// Adds a backend to the global registry. Names must be unique.
/// @param info Backend description.
void RegisterBackend(BackendInfo info);

/// All registered backends, in registration order.
std::span<const BackendInfo> RegisteredBackends();

/// Looks up a registered backend by name.
/// @param name Backend name.
/// @return The backend, or null when no backend has that name.
const BackendInfo* FindBackend(std::string_view name);

/// Registers a backend at static-initialization time; use once per backend translation unit.
struct BackendRegistrar {
  /// @param makeInfo Builds the backend description.
  explicit BackendRegistrar(const std::function<BackendInfo()>& makeInfo) {
    RegisterBackend(makeInfo());
  }
};

}  // namespace donner::bakeoff
