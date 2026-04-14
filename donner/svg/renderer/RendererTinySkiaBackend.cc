#include <memory>
#include <span>

#include "donner/svg/renderer/RendererInternal.h"
#include "donner/svg/renderer/RendererTinySkia.h"

namespace donner::svg {
namespace {

class RendererTinySkiaImplementation final : public RendererImplementation {
public:
  explicit RendererTinySkiaImplementation(bool verbose) : renderer_(verbose) {}

  void draw(SVGDocument& document) override { renderer_.draw(document); }

  void beginFrame(const RenderViewport& viewport) override { renderer_.beginFrame(viewport); }

  void endFrame() override { renderer_.endFrame(); }

  void setTransform(const Transform2d& transform) override { renderer_.setTransform(transform); }

  void pushTransform(const Transform2d& transform) override { renderer_.pushTransform(transform); }

  void popTransform() override { renderer_.popTransform(); }

  void pushClip(const ResolvedClip& clip) override { renderer_.pushClip(clip); }

  void popClip() override { renderer_.popClip(); }

  void pushIsolatedLayer(double opacity, MixBlendMode blendMode) override {
    renderer_.pushIsolatedLayer(opacity, blendMode);
  }

  void popIsolatedLayer() override { renderer_.popIsolatedLayer(); }

  void pushFilterLayer(const components::FilterGraph& filterGraph,
                       const std::optional<Box2d>& filterRegion) override {
    renderer_.pushFilterLayer(filterGraph, filterRegion);
  }

  void popFilterLayer() override { renderer_.popFilterLayer(); }

  void pushMask(const std::optional<Box2d>& maskBounds) override { renderer_.pushMask(maskBounds); }

  void transitionMaskToContent() override { renderer_.transitionMaskToContent(); }

  void popMask() override { renderer_.popMask(); }

  void beginPatternTile(const Box2d& tileRect, const Transform2d& targetFromPattern) override {
    renderer_.beginPatternTile(tileRect, targetFromPattern);
  }

  void endPatternTile(bool forStroke) override { renderer_.endPatternTile(forStroke); }

  void setPaint(const PaintParams& paint) override { renderer_.setPaint(paint); }

  void drawPath(const PathShape& path, const StrokeParams& stroke) override {
    renderer_.drawPath(path, stroke);
  }

  void drawRect(const Box2d& rect, const StrokeParams& stroke) override {
    renderer_.drawRect(rect, stroke);
  }

  void drawEllipse(const Box2d& bounds, const StrokeParams& stroke) override {
    renderer_.drawEllipse(bounds, stroke);
  }

  void drawImage(const ImageResource& image, const ImageParams& params) override {
    renderer_.drawImage(image, params);
  }

  void drawText(Registry& registry, const components::ComputedTextComponent& text,
                const TextParams& params) override {
    renderer_.drawText(registry, text, params);
  }

  RendererBitmap takeSnapshot() const override { return renderer_.takeSnapshot(); }

  std::unique_ptr<RendererInterface> createOffscreenInstance() const override {
    return renderer_.createOffscreenInstance();
  }

  int width() const override { return renderer_.width(); }

  int height() const override { return renderer_.height(); }

private:
  RendererTinySkia renderer_;
};

}  // namespace

std::unique_ptr<RendererImplementation> CreateRendererImplementation(bool verbose) {
  return std::make_unique<RendererTinySkiaImplementation>(verbose);
}

}  // namespace donner::svg
