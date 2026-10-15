/// @file
/// The null traversal recorder as a bakeoff backend.

#include <memory>

#include "harness/BenchBackend.h"
#include "harness/NullRecordingRenderer.h"

namespace donner::bakeoff {
namespace {

class NullBackend final : public BenchBackend {
public:
  svg::RendererInterface& renderer() override { return renderer_; }
  [[nodiscard]] const FeatureUsage* featureUsage() const override { return &renderer_.usage(); }
  void resetFeatureUsage() override { renderer_.resetUsage(); }

private:
  NullRecordingRenderer renderer_;
};

const BackendRegistrar kNullRegistrar([] {
  BackendInfo info;
  info.name = "null";
  info.suite = Suite::Reference;
  info.library = "Donner traversal only";
  info.api = "none";
  info.configuration =
      "Donner parse, style, layout and RendererDriver traversal; every draw is discarded";
  info.capabilities = CapabilityTable::All(Support::Native);
  info.create = [] { return std::make_unique<NullBackend>(); };
  return info;
});

}  // namespace
}  // namespace donner::bakeoff
