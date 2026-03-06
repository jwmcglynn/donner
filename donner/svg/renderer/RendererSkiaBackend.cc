#include <memory>
#include <span>

#include "donner/svg/renderer/RendererInternal.h"
#include "donner/svg/renderer/RendererSkia.h"

namespace donner::svg {
namespace {

class RendererSkiaImplementation final : public RendererImplementation {
public:
  explicit RendererSkiaImplementation(bool verbose) : renderer_(verbose) {}

  void draw(SVGDocument& document) override { renderer_.draw(document); }

  void beginFrame(const RenderViewport& viewport) override { renderer_.beginFrame(viewport); }

  void endFrame() override { renderer_.endFrame(); }

  void setTransform(const Transformd& transform) override { renderer_.setTransform(transform); }

  void pushTransform(const Transformd& transform) override { renderer_.pushTransform(transform); }

  void popTransform() override { renderer_.popTransform(); }

  void pushClip(const ResolvedClip& clip) override { renderer_.pushClip(clip); }

  void popClip() override { renderer_.popClip(); }

  void pushIsolatedLayer(double opacity) override { renderer_.pushIsolatedLayer(opacity); }

  void popIsolatedLayer() override { renderer_.popIsolatedLayer(); }

  void pushFilterLayer(std::span<const FilterEffect> effects) override {
    renderer_.pushFilterLayer(effects);
  }

  void popFilterLayer() override { renderer_.popFilterLayer(); }

  void pushMask(const std::optional<Boxd>& maskBounds) override { renderer_.pushMask(maskBounds); }

  void transitionMaskToContent() override { renderer_.transitionMaskToContent(); }

  void popMask() override { renderer_.popMask(); }

  void beginPatternTile(const Boxd& tileRect, const Transformd& patternToTarget) override {
    renderer_.beginPatternTile(tileRect, patternToTarget);
  }

  void endPatternTile(bool forStroke) override { renderer_.endPatternTile(forStroke); }

  void setPaint(const PaintParams& paint) override { renderer_.setPaint(paint); }

  void drawPath(const PathShape& path, const StrokeParams& stroke) override {
    renderer_.drawPath(path, stroke);
  }

  void drawRect(const Boxd& rect, const StrokeParams& stroke) override {
    renderer_.drawRect(rect, stroke);
  }

  void drawEllipse(const Boxd& bounds, const StrokeParams& stroke) override {
    renderer_.drawEllipse(bounds, stroke);
  }

  void drawImage(const ImageResource& image, const ImageParams& params) override {
    renderer_.drawImage(image, params);
  }

  void drawText(const components::ComputedTextComponent& text, const TextParams& params) override {
    renderer_.drawText(text, params);
  }

  RendererBitmap takeSnapshot() const override { return renderer_.takeSnapshot(); }

  int width() const override { return renderer_.width(); }

  int height() const override { return renderer_.height(); }

private:
  RendererSkia renderer_;
};

}  // namespace

std::unique_ptr<RendererImplementation> CreateRendererImplementation(bool verbose) {
  return std::make_unique<RendererSkiaImplementation>(verbose);
}

}  // namespace donner::svg
