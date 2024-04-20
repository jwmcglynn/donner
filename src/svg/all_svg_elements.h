#pragma once
/**
 * @file
 *
 * A single include that covers all SVG element types, plus \ref AllSVGElements which can be used to
 * perform constexpr lookups across all element types.
 */

#include <entt/entt.hpp>

#include "src/svg/svg_circle_element.h"
#include "src/svg/svg_defs_element.h"
#include "src/svg/svg_ellipse_element.h"
#include "src/svg/svg_g_element.h"
#include "src/svg/svg_line_element.h"
#include "src/svg/svg_linear_gradient_element.h"
#include "src/svg/svg_path_element.h"
#include "src/svg/svg_pattern_element.h"
#include "src/svg/svg_polygon_element.h"
#include "src/svg/svg_polyline_element.h"
#include "src/svg/svg_radial_gradient_element.h"
#include "src/svg/svg_rect_element.h"
#include "src/svg/svg_stop_element.h"
#include "src/svg/svg_style_element.h"
#include "src/svg/svg_svg_element.h"
#include "src/svg/svg_use_element.h"

// Types that are not fully-fledged SVG elements by themselves, so they aren't included in \ref
// AllSVGElements.
#include "src/svg/svg_unknown_element.h"  // NOLINT(unused-includes)

namespace donner::svg {

/**
 * A type list of all SVG element types, used by \ref AttributeParser and \ref XMLParser.
 */
using AllSVGElements = entt::type_list<  //
    SVGCircleElement,                    //
    SVGDefsElement,                      //
    SVGEllipseElement,                   //
    SVGGElement,                         //
    SVGLineElement,                      //
    SVGLinearGradientElement,            //
    SVGStopElement,                      //
    SVGRadialGradientElement,            //
    SVGPathElement,                      //
    SVGPatternElement,                   //
    SVGPolygonElement,                   //
    SVGPolylineElement,                  //
    SVGRectElement,                      //
    SVGStyleElement,                     //
    SVGSVGElement,                       //
    SVGUseElement>;

}  // namespace donner::svg
