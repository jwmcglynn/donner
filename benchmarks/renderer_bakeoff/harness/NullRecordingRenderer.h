#pragma once
/// @file
/// A renderer that rasterizes nothing and only counts the operations Donner's traversal emits.
/// Its frame time is Donner's own traversal and preparation cost, and its \ref FeatureUsage is
/// the feature census of a scene.

#include "harness/AdapterBase.h"

namespace donner::bakeoff {

/// Records every operation as natively supported and draws nothing.
class NullRecordingRenderer final : public AdapterBase {
public:
  NullRecordingRenderer();

protected:
  void onBeginFrame(int /*pixelWidth*/, int /*pixelHeight*/) override {}
  void onEndFrame() override {}
  void onFillPath(const Path& /*path*/, FillRule /*fillRule*/, const PaintDesc& /*paint*/,
                  const Transform2d& /*deviceFromLocal*/) override {}
  void onStrokePath(const Path& /*path*/, const svg::StrokeParams& /*stroke*/,
                    const PaintDesc& /*paint*/, const Transform2d& /*deviceFromLocal*/) override {}
  svg::RendererBitmap onSnapshot() const override { return {}; }
  bool onPushClip(const ClipDesc& /*clip*/) override { return true; }
  bool onPushLayer(double /*opacity*/, svg::MixBlendMode /*blendMode*/) override { return true; }
  bool onPushMask(svg::MaskType /*maskType*/, const Transform2d& /*deviceFromLocal*/,
                  const std::optional<Box2d>& /*bounds*/) override {
    return true;
  }
  bool onPushFilter(const svg::components::FilterGraph& /*graph*/,
                    const Transform2d& /*deviceFromLocal*/) override {
    return true;
  }
  bool onBeginPatternTile(const Box2d& /*tileRect*/,
                          const Transform2d& /*targetFromPattern*/) override {
    return true;
  }
  bool onDrawImage(const svg::ImageResource& /*image*/, const Box2d& /*targetRect*/,
                   const Transform2d& /*deviceFromLocal*/, bool /*pixelated*/) override {
    return true;
  }
  bool onDrawText(Registry& /*registry*/, const svg::components::ComputedTextComponent& /*text*/,
                  const svg::TextParams& /*params*/) override {
    return true;
  }
};

}  // namespace donner::bakeoff
