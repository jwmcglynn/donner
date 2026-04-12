#include "donner/editor/sandbox/SerializingRenderer.h"

#include <utility>

#include "donner/editor/sandbox/SandboxCodecs.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/resources/ImageResource.h"

namespace donner::editor::sandbox {

SerializingRenderer::SerializingRenderer() = default;
SerializingRenderer::~SerializingRenderer() = default;

void SerializingRenderer::writeStreamHeader() {
  if (headerWritten_) return;
  auto token = writer_.beginMessage(Opcode::kStreamHeader);
  writer_.writeU32(kWireMagic);
  writer_.writeU32(kWireVersion);
  writer_.finishMessage(token);
  headerWritten_ = true;
}

void SerializingRenderer::writeUnsupported(UnsupportedKind kind) {
  writeStreamHeader();
  auto token = writer_.beginMessage(Opcode::kUnsupported);
  writer_.writeU32(static_cast<uint32_t>(kind));
  writer_.finishMessage(token);
  ++unsupportedCount_;
}

void SerializingRenderer::draw(svg::SVGDocument& document) {
  svg::RendererDriver driver(*this, /*verbose=*/false);
  driver.draw(document);
}

void SerializingRenderer::beginFrame(const svg::RenderViewport& viewport) {
  writeStreamHeader();
  width_ = static_cast<int>(viewport.size.x * viewport.devicePixelRatio);
  height_ = static_cast<int>(viewport.size.y * viewport.devicePixelRatio);

  auto token = writer_.beginMessage(Opcode::kBeginFrame);
  EncodeRenderViewport(writer_, viewport);
  writer_.finishMessage(token);
}

void SerializingRenderer::endFrame() {
  auto token = writer_.beginMessage(Opcode::kEndFrame);
  writer_.finishMessage(token);
}

void SerializingRenderer::setTransform(const Transform2d& transform) {
  auto token = writer_.beginMessage(Opcode::kSetTransform);
  EncodeTransform2d(writer_, transform);
  writer_.finishMessage(token);
}

void SerializingRenderer::pushTransform(const Transform2d& transform) {
  auto token = writer_.beginMessage(Opcode::kPushTransform);
  EncodeTransform2d(writer_, transform);
  writer_.finishMessage(token);
}

void SerializingRenderer::popTransform() {
  auto token = writer_.beginMessage(Opcode::kPopTransform);
  writer_.finishMessage(token);
}

void SerializingRenderer::pushClip(const svg::ResolvedClip& clip) {
  if (clip.mask.has_value()) {
    writeUnsupported(UnsupportedKind::kClipMaskChain);
    return;
  }
  auto token = writer_.beginMessage(Opcode::kPushClip);
  EncodeResolvedClip(writer_, clip);
  writer_.finishMessage(token);
}

void SerializingRenderer::popClip() {
  auto token = writer_.beginMessage(Opcode::kPopClip);
  writer_.finishMessage(token);
}

void SerializingRenderer::pushIsolatedLayer(double opacity, svg::MixBlendMode blendMode) {
  auto token = writer_.beginMessage(Opcode::kPushIsolatedLayer);
  writer_.writeF64(opacity);
  EncodeMixBlendMode(writer_, blendMode);
  writer_.finishMessage(token);
}

void SerializingRenderer::popIsolatedLayer() {
  auto token = writer_.beginMessage(Opcode::kPopIsolatedLayer);
  writer_.finishMessage(token);
}

void SerializingRenderer::pushFilterLayer(const svg::components::FilterGraph& filterGraph,
                                          const std::optional<Box2d>& filterRegion) {
  auto token = writer_.beginMessage(Opcode::kPushFilterLayer);
  EncodeFilterGraph(writer_, filterGraph);
  writer_.writeBool(filterRegion.has_value());
  if (filterRegion) {
    EncodeBox2d(writer_, *filterRegion);
  }
  writer_.finishMessage(token);
}

void SerializingRenderer::popFilterLayer() {
  auto token = writer_.beginMessage(Opcode::kPopFilterLayer);
  writer_.finishMessage(token);
}

void SerializingRenderer::pushMask(const std::optional<Box2d>& maskBounds) {
  auto token = writer_.beginMessage(Opcode::kPushMask);
  writer_.writeBool(maskBounds.has_value());
  if (maskBounds) {
    EncodeBox2d(writer_, *maskBounds);
  }
  writer_.finishMessage(token);
}

void SerializingRenderer::transitionMaskToContent() {
  auto token = writer_.beginMessage(Opcode::kTransitionMaskToContent);
  writer_.finishMessage(token);
}

void SerializingRenderer::popMask() {
  auto token = writer_.beginMessage(Opcode::kPopMask);
  writer_.finishMessage(token);
}

void SerializingRenderer::beginPatternTile(const Box2d& tileRect,
                                           const Transform2d& targetFromPattern) {
  auto token = writer_.beginMessage(Opcode::kBeginPatternTile);
  EncodeBox2d(writer_, tileRect);
  EncodeTransform2d(writer_, targetFromPattern);
  writer_.finishMessage(token);
}

void SerializingRenderer::endPatternTile(bool forStroke) {
  auto token = writer_.beginMessage(Opcode::kEndPatternTile);
  writer_.writeBool(forStroke);
  writer_.finishMessage(token);
}

void SerializingRenderer::setPaint(const svg::PaintParams& paint) {
  auto token = writer_.beginMessage(Opcode::kSetPaint);
  EncodePaintParams(writer_, paint);
  writer_.finishMessage(token);
}

void SerializingRenderer::drawPath(const svg::PathShape& path, const svg::StrokeParams& stroke) {
  auto token = writer_.beginMessage(Opcode::kDrawPath);
  EncodePathShape(writer_, path);
  EncodeStrokeParams(writer_, stroke);
  writer_.finishMessage(token);
}

void SerializingRenderer::drawRect(const Box2d& rect, const svg::StrokeParams& stroke) {
  auto token = writer_.beginMessage(Opcode::kDrawRect);
  EncodeBox2d(writer_, rect);
  EncodeStrokeParams(writer_, stroke);
  writer_.finishMessage(token);
}

void SerializingRenderer::drawEllipse(const Box2d& bounds, const svg::StrokeParams& stroke) {
  auto token = writer_.beginMessage(Opcode::kDrawEllipse);
  EncodeBox2d(writer_, bounds);
  EncodeStrokeParams(writer_, stroke);
  writer_.finishMessage(token);
}

void SerializingRenderer::drawImage(const svg::ImageResource& image,
                                     const svg::ImageParams& params) {
  auto token = writer_.beginMessage(Opcode::kDrawImage);
  EncodeImageResource(writer_, image);
  EncodeImageParams(writer_, params);
  writer_.finishMessage(token);
}

void SerializingRenderer::drawText(Registry&,
                                   const svg::components::ComputedTextComponent& text,
                                   const svg::TextParams& params) {
  auto token = writer_.beginMessage(Opcode::kDrawText);
  EncodeComputedTextComponent(writer_, text);
  EncodeTextParams(writer_, params);
  writer_.finishMessage(token);
}

svg::RendererBitmap SerializingRenderer::takeSnapshot() const {
  // A SerializingRenderer has no backing pixels — snapshots are a host
  // concern (performed by the real replay backend). Return an empty bitmap
  // to satisfy the override; callers in the sandbox pipeline should never
  // request a snapshot from here.
  return svg::RendererBitmap{};
}

std::unique_ptr<svg::RendererInterface> SerializingRenderer::createOffscreenInstance() const {
  return nullptr;
}

}  // namespace donner::editor::sandbox
