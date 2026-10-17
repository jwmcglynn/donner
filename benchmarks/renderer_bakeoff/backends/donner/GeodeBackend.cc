/// @file
/// Donner's Geode GPU renderer as a bakeoff backend, on the platform's native GPU API (Metal on
/// macOS, Vulkan on Linux).

#include <memory>

#include "donner/svg/renderer/RendererGeode.h"
#include "harness/BenchBackend.h"

namespace donner::bakeoff {
namespace {

class GeodeBackend final : public BenchBackend {
public:
  svg::RendererInterface& renderer() override { return renderer_; }

  bool finishGpuWork() override {
    return renderer_.waitForSubmittedWork() == geode::GpuWaitResult::Complete;
  }

private:
  svg::RendererGeode renderer_;
};

const BackendRegistrar kGeodeRegistrar([] {
  BackendInfo info;
  info.name = "geode";
  info.suite = Suite::Gpu;
  info.library = "Donner Geode (Slug coverage)";
#if defined(__APPLE__)
  info.api = "Metal";
#else
  info.api = "Vulkan";
#endif
  info.configuration =
      "Donner RendererGeode on its native GPU backend with a leased headless device; frame time "
      "waits for queue idle and excludes readback";
  info.capabilities = CapabilityTable::All(Support::Native);
  info.donnerBaseline = true;
  info.create = [] { return std::make_unique<GeodeBackend>(); };
  return info;
});

}  // namespace
}  // namespace donner::bakeoff
