#pragma once
/// @file
/// Test adapter that records the hooks AdapterBase invokes.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"
#include "harness/AdapterBase.h"

namespace donner::bakeoff::tests {

/// One recorded fill or stroke.
struct RecordedDraw {
  bool stroke = false;                    //!< True for a stroke, false for a fill.
  FillRule fillRule = FillRule::NonZero;  //!< Fill rule of a fill.
  PaintDesc paint;                        //!< Resolved paint.
  Box2d deviceBounds;                     //!< Device-space bounds of the geometry.
};

/// Records hooks as strings and draws as \ref RecordedDraw.
class RecordingAdapter final : public AdapterBase {
public:
  /// @param capabilities Capabilities to advertise.
  explicit RecordingAdapter(CapabilityTable capabilities) : AdapterBase(capabilities) {}

  std::vector<std::string> events;  //!< Hook names in call order.
  std::vector<RecordedDraw> draws;  //!< Fills and strokes in call order.
  std::vector<ClipDesc> clips;      //!< Clips in push order.
  bool acceptFilters = true;        //!< Result returned from the filter hook.
  bool acceptPatterns = false;      //!< Result returned from the pattern tile hook.

protected:
  void onBeginFrame(int, int) override { events.push_back("begin"); }
  void onEndFrame() override { events.push_back("end"); }
  void onFillPath(const Path& path, FillRule fillRule, const PaintDesc& paint,
                  const Transform2d& deviceFromLocal) override {
    draws.push_back({false, fillRule, paint, deviceFromLocal.transformBox(path.bounds())});
  }
  void onStrokePath(const Path& path, const svg::StrokeParams&, const PaintDesc& paint,
                    const Transform2d& deviceFromLocal) override {
    draws.push_back({true, FillRule::NonZero, paint, deviceFromLocal.transformBox(path.bounds())});
  }
  svg::RendererBitmap onSnapshot() const override { return {}; }
  bool onPushClip(const ClipDesc& clip) override {
    events.push_back("pushClip");
    clips.push_back(clip);
    return true;
  }
  bool onPushFilter(const svg::components::FilterGraph&, const Transform2d&) override {
    events.push_back("pushFilter");
    return acceptFilters;
  }
  void onPopFilter() override { events.push_back("popFilter"); }
  bool onBeginPatternTile(const Box2d&, const Transform2d&) override {
    events.push_back("beginPatternTile");
    return acceptPatterns;
  }
  void onEndPatternTile(bool forStroke) override {
    events.push_back(forStroke ? "endPatternTile(stroke)" : "endPatternTile(fill)");
  }
  void onPopClip() override { events.push_back("popClip"); }
  bool onPushLayer(double, svg::MixBlendMode) override {
    events.push_back("pushLayer");
    return true;
  }
  void onPopLayer() override { events.push_back("popLayer"); }
  bool onPushMask(svg::MaskType, const Transform2d&, const std::optional<Box2d>&) override {
    events.push_back("pushMask");
    return true;
  }
  void onMaskContent() override { events.push_back("maskContent"); }
  void onPopMask() override { events.push_back("popMask"); }
};

/// Matches a \ref FeatureUsage with no dropped feature, listing the dropped ones on failure.
MATCHER(RendersFully, "drops no feature") {
  bool full = true;
  for (std::size_t i = 0; i < kFeatureCount; ++i) {
    const auto feature = static_cast<Feature>(i);
    if (arg.dropped(feature) > 0) {
      *result_listener << (full ? "dropped " : ", ") << FeatureName(feature) << " x"
                       << arg.dropped(feature);
      full = false;
    }
  }
  return full;
}

/// Parses \p source at a 100x100 canvas; aborts the test on parse failure.
inline svg::SVGDocument ParseForTest(std::string_view source) {
  ParseWarningSink warnings;
  auto parsed = svg::parser::SVGParser::ParseSVG(source, warnings);
  if (parsed.hasError()) {
    ADD_FAILURE() << parsed.error();
    return svg::SVGDocument();
  }
  svg::SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(100, 100);
  return document;
}

}  // namespace donner::bakeoff::tests
