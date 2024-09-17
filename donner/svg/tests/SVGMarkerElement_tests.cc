#include "donner/svg/SVGMarkerElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGPolygonElement.h"
#include "donner/svg/SVGPolylineElement.h"
#include "donner/svg/renderer/RendererSkia.h"
#include "gtest/gtest.h"

namespace donner::svg::tests {

class SVGMarkerElementTests : public ::testing::Test {
protected:
  SVGMarkerElementTests() : document_(SVGDocument()), marker_(SVGMarkerElement::Create(document_)) {
    marker_.setMarkerWidth(10);
    marker_.setMarkerHeight(10);
    marker_.setRefX(5);
    marker_.setRefY(5);
    marker_.setOrient("auto");

    document_.svgElement().appendChild(marker_);
  }

  SVGDocument document_;
  SVGMarkerElement marker_;
};

TEST_F(SVGMarkerElementTests, MarkerAttributes) {
  EXPECT_EQ(marker_.markerWidth(), 10);
  EXPECT_EQ(marker_.markerHeight(), 10);
  EXPECT_EQ(marker_.refX(), 5);
  EXPECT_EQ(marker_.refY(), 5);
  // EXPECT_EQ(marker_.orient(), "auto");
}

TEST_F(SVGMarkerElementTests, MarkerStartProperty) {
  auto polygon = SVGPolygonElement::Create(document_);
  polygon.setPoints({{50, 50}, {250, 50}, {150, 150}, {250, 250}, {50, 250}});
  polygon.setAttribute("marker-start", "url(#marker)");

  auto renderer = RendererSkia();
  renderer.draw(document_);

  // Add assertions to verify the rendering of the marker at the start of the path
}

TEST_F(SVGMarkerElementTests, MarkerMidProperty) {
  auto polyline = SVGPolylineElement::Create(document_);
  polyline.setPoints({{50, 50}, {250, 50}, {150, 150}, {250, 250}, {50, 250}});
  polyline.setAttribute("marker-mid", "url(#marker)");

  auto renderer = RendererSkia();
  renderer.draw(document_);

  // Add assertions to verify the rendering of the marker at the midpoints of the path
}

TEST_F(SVGMarkerElementTests, MarkerEndProperty) {
  auto polygon = SVGPolygonElement::Create(document_);
  polygon.setPoints({{50, 50}, {250, 50}, {150, 150}, {250, 250}, {50, 250}});
  polygon.setAttribute("marker-end", "url(#marker)");

  auto renderer = RendererSkia();
  renderer.draw(document_);

  // Add assertions to verify the rendering of the marker at the end of the path
}

}  // namespace donner::svg::tests
