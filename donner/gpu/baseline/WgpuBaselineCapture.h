#pragma once
/// @file
/// Renders corpus scenes through the current production renderer as a black box.
///
/// This is the pre-cutover oracle: the wgpu-backed Geode path, driven exactly the way production
/// rendering drives it, with only its public outputs read back. Both the capture binary that
/// writes the committed PNGs and the check-mode test that re-renders and diffs against them go
/// through here, so the frozen bytes and the bytes under test can never come from two different
/// code paths.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "donner/gpu/baseline/BaselineCorpus.h"

namespace donner::geode {
class GeodeDevice;
}  // namespace donner::geode

namespace donner::gpu::baseline {

/// The adapter a capture ran on. Frozen pixels are only meaningful against the environment that
/// produced them, so every capture records this and the check mode refuses to compare across a
/// mismatch instead of reporting a difference it cannot attribute.
struct CaptureEnvironment {
  std::string adapterName;     //!< Vendor and device string reported by the adapter.
  std::string adapterBackend;  //!< "Metal", "Vulkan", "D3D12", "OpenGL", ...
  std::string adapterType;     //!< "DiscreteGPU", "IntegratedGPU", "CPU", or "Unknown".
};

/// Holds one production device for the whole corpus, because device creation compiles every
/// shared pipeline and is far more expensive than any single scene.
class WgpuBaselineCapturer {
public:
  /// Creates a capturer, or returns null when no GPU adapter is available on this host.
  static std::unique_ptr<WgpuBaselineCapturer> Create();

  ~WgpuBaselineCapturer();

  WgpuBaselineCapturer(const WgpuBaselineCapturer&) = delete;
  WgpuBaselineCapturer& operator=(const WgpuBaselineCapturer&) = delete;

  /// The adapter this capturer is bound to.
  const CaptureEnvironment& environment() const { return environment_; }

  /**
   * Renders one scene and reads its pixels back.
   *
   * @param scene Scene to render.
   * @param pixelsOut Receives tightly packed RGBA8 premultiplied pixels, row-major,
   *   `kCorpusSize * kCorpusSize * 4` bytes.
   * @return An empty string on success, or a human-readable reason the capture failed.
   */
  std::string capture(const CorpusScene& scene, std::vector<uint8_t>& pixelsOut);

private:
  explicit WgpuBaselineCapturer(std::unique_ptr<geode::GeodeDevice> device);

  std::unique_ptr<geode::GeodeDevice> device_;
  CaptureEnvironment environment_;
};

/// Renders the corpus scene named `sceneName` on `capturer`. Fails when the corpus has no such
/// scene, which keeps a stale golden filename from silently comparing against nothing.
std::string CaptureNamedScene(WgpuBaselineCapturer& capturer, std::string_view sceneName,
                              std::vector<uint8_t>& pixelsOut);

}  // namespace donner::gpu::baseline
