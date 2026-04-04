#pragma once
/// @file

#include <gmock/gmock.h>

#include "donner/svg/text/TextBackend.h"

namespace donner::svg {

class MockTextBackend : public TextBackend {
public:
  MOCK_METHOD(FontVMetrics, fontVMetrics, (FontHandle), (const, override));
  MOCK_METHOD(float, scaleForPixelHeight, (FontHandle, float), (const, override));
  MOCK_METHOD(float, scaleForEmToPixels, (FontHandle, float), (const, override));
  MOCK_METHOD(std::optional<UnderlineMetrics>, underlineMetrics, (FontHandle), (const, override));
  MOCK_METHOD(std::optional<SubSuperMetrics>, subSuperMetrics, (FontHandle), (const, override));
  MOCK_METHOD(PathSpline, glyphOutline, (FontHandle, int, float), (const, override));
  MOCK_METHOD(bool, isBitmapOnly, (FontHandle), (const, override));
  MOCK_METHOD(bool, isCursive, (uint32_t), (const, override));
  MOCK_METHOD(bool, hasSmallCapsFeature, (FontHandle), (const, override));
  MOCK_METHOD(std::optional<BitmapGlyph>, bitmapGlyph, (FontHandle, int, float),
              (const, override));
  MOCK_METHOD(ShapedRun, shapeRun,
              (FontHandle, float, std::string_view, size_t, size_t, bool, FontVariant, bool),
              (const, override));
  MOCK_METHOD(double, crossSpanKern,
              (FontHandle, float, FontHandle, float, uint32_t, uint32_t, bool), (const, override));
};

}  // namespace donner::svg
