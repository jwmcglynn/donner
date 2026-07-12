/// @file RenderSnapshotBench.cpp
/// @brief Render snapshot capture and replay benchmarks.
///
/// Usage:
/// ```
/// bazel run -c opt //donner/benchmarks:render_snapshot_bench -- \
///     --benchmark_min_time=0.5s
/// ```

#include <benchmark/benchmark.h>

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace {

using donner::Box2d;
using donner::ParseWarningSink;
using donner::Registry;
using donner::Transform2d;
using donner::svg::ImageParams;
using donner::svg::ImageResource;
using donner::svg::PaintParams;
using donner::svg::PathShape;
using donner::svg::Renderer;
using donner::svg::RendererBitmap;
using donner::svg::RendererDriver;
using donner::svg::RendererInterface;
using donner::svg::RenderSnapshot;
using donner::svg::RenderViewport;
using donner::svg::ResolvedClip;
using donner::svg::StrokeParams;
using donner::svg::SVGDocument;
using donner::svg::TextParams;
using donner::svg::parser::SVGParser;

class NullRenderer final : public RendererInterface {
public:
  void draw(SVGDocument&) override {}
  [[nodiscard]] int width() const override { return 0; }
  [[nodiscard]] int height() const override { return 0; }
  void beginFrame(const RenderViewport&) override {}
  void endFrame() override {}
  void setTransform(const Transform2d&) override {}
  void pushTransform(const Transform2d&) override {}
  void popTransform() override {}
  void pushClip(const ResolvedClip&) override {}
  void popClip() override {}
  void pushIsolatedLayer(double, donner::svg::MixBlendMode) override {}
  void popIsolatedLayer() override {}
  void pushFilterLayer(const donner::svg::components::FilterGraph&,
                       const std::optional<Box2d>&) override {}
  void popFilterLayer() override {}
  void pushMask(const std::optional<Box2d>&) override {}
  void transitionMaskToContent() override {}
  void popMask() override {}
  bool beginPatternTile(const Box2d&, const Transform2d&) override { return true; }
  void endPatternTile(bool) override {}
  void setPaint(const PaintParams&) override {}
  void drawPath(const PathShape&, const StrokeParams&) override {}
  void drawRect(const Box2d&, const StrokeParams&) override {}
  void drawEllipse(const Box2d&, const StrokeParams&) override {}
  void drawImage(const ImageResource&, const ImageParams&) override {}
  void drawText(Registry&, const donner::svg::components::ComputedTextComponent&,
                const TextParams&) override {}
  [[nodiscard]] RendererBitmap takeSnapshot() const override { return RendererBitmap(); }
  [[nodiscard]] std::unique_ptr<RendererInterface> createOffscreenInstance() const override {
    return std::make_unique<NullRenderer>();
  }
};

std::string MakeGridSvg(int count) {
  std::string svg;
  svg.reserve(static_cast<std::size_t>(count) * 82u + 128u);
  svg += R"(<svg xmlns="http://www.w3.org/2000/svg" width="4096" height="4096">)";
  svg += '\n';

  for (int i = 0; i < count; ++i) {
    const int x = (i % 256) * 16;
    const int y = (i / 256) * 16;
    svg += "  <rect x=\"";
    svg += std::to_string(x);
    svg += "\" y=\"";
    svg += std::to_string(y);
    svg += "\" width=\"12\" height=\"12\" fill=\"#";
    svg += (i % 2 == 0) ? "1f77b4" : "ff7f0e";
    svg += "\"/>\n";
  }

  svg += "</svg>";
  return svg;
}

SVGDocument ParseSvgOrAbort(const std::string& svg) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto result = SVGParser::ParseSVG(svg, warnings);
  if (result.hasError()) {
    std::abort();
  }
  return std::move(result.result());
}

void BM_RenderSnapshot_Capture(benchmark::State& state) {
  const int count = static_cast<int>(state.range(0));
  std::string svg = MakeGridSvg(count);
  SVGDocument document = ParseSvgOrAbort(svg);
  NullRenderer renderer;
  RendererDriver driver(renderer);
  std::size_t commandCount = 0;
  std::size_t commandStorageBytes = 0;

  for (auto _ : state) {
    RenderSnapshot snapshot = driver.captureRenderSnapshot(document);
    commandCount = snapshot.commandCount();
    commandStorageBytes = snapshot.estimatedCommandStorageBytes();
    benchmark::DoNotOptimize(commandCount);
  }

  state.SetItemsProcessed(state.iterations() * count);
  state.counters["snapshot_commands"] = static_cast<double>(commandCount);
  state.counters["snapshot_command_storage_bytes"] = static_cast<double>(commandStorageBytes);
}
BENCHMARK(BM_RenderSnapshot_Capture)->Arg(1000)->Arg(10000)->Arg(100000);

void BM_RenderSnapshot_ReplayTinySkiaBackend(benchmark::State& state) {
  const int count = static_cast<int>(state.range(0));
  std::string svg = MakeGridSvg(count);
  SVGDocument document = ParseSvgOrAbort(svg);

  Renderer renderer;
  RendererDriver driver(renderer);
  RenderSnapshot snapshot = driver.captureRenderSnapshot(document);
  const std::size_t commandCount = snapshot.commandCount();
  const std::size_t commandStorageBytes = snapshot.estimatedCommandStorageBytes();

  for (auto _ : state) {
    driver.draw(snapshot);
    benchmark::DoNotOptimize(renderer.width());
  }

  state.SetItemsProcessed(state.iterations() * count);
  state.counters["snapshot_commands"] = static_cast<double>(commandCount);
  state.counters["snapshot_command_storage_bytes"] = static_cast<double>(commandStorageBytes);
}
BENCHMARK(BM_RenderSnapshot_ReplayTinySkiaBackend)->Arg(1000)->Arg(10000)->Arg(100000);

}  // namespace

BENCHMARK_MAIN();
