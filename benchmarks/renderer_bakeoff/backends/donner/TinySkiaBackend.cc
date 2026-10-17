/// @file
/// Donner's tiny-skia software renderer as a bakeoff backend.

#include <memory>

#include "donner/svg/renderer/RendererTinySkia.h"
#include "harness/BenchBackend.h"

namespace donner::bakeoff {
namespace {

class TinySkiaBackend final : public BenchBackend {
public:
  svg::RendererInterface& renderer() override { return renderer_; }

private:
  svg::RendererTinySkia renderer_;
};

const BackendRegistrar kTinySkiaRegistrar([] {
  BackendInfo info;
  info.name = "tiny_skia";
  info.suite = Suite::Software;
  info.library = "tiny-skia-cpp (Donner RendererTinySkia)";
  info.api = "CPU";
  info.configuration = "Donner default software backend as shipped, single-threaded";
  info.capabilities = CapabilityTable::All(Support::Native);
  info.donnerBaseline = true;
  info.create = [] { return std::make_unique<TinySkiaBackend>(); };
  return info;
});

}  // namespace
}  // namespace donner::bakeoff
