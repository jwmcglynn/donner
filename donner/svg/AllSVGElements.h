#pragma once
/**
 * @file
 *
 * A single include that covers all SVG element types, plus \ref donner::svg::AllSVGElements which
 * can be used to perform constexpr lookups across all element types.
 */

#include <entt/entt.hpp>

#include "donner/svg/SVGCircleElement.h"          // IWYU pragma: export
#include "donner/svg/SVGDefsElement.h"            // IWYU pragma: export
#include "donner/svg/SVGEllipseElement.h"         // IWYU pragma: export
#include "donner/svg/SVGFEGaussianBlurElement.h"  // IWYU pragma: export
#include "donner/svg/SVGFilterElement.h"          // IWYU pragma: export
#include "donner/svg/SVGGElement.h"               // IWYU pragma: export
#include "donner/svg/SVGLineElement.h"            // IWYU pragma: export
#include "donner/svg/SVGLinearGradientElement.h"  // IWYU pragma: export
#include "donner/svg/SVGPathElement.h"            // IWYU pragma: export
#include "donner/svg/SVGPatternElement.h"         // IWYU pragma: export
#include "donner/svg/SVGPolygonElement.h"         // IWYU pragma: export
#include "donner/svg/SVGPolylineElement.h"        // IWYU pragma: export
#include "donner/svg/SVGRadialGradientElement.h"  // IWYU pragma: export
#include "donner/svg/SVGRectElement.h"            // IWYU pragma: export
#include "donner/svg/SVGSVGElement.h"             // IWYU pragma: export
#include "donner/svg/SVGStopElement.h"            // IWYU pragma: export
#include "donner/svg/SVGStyleElement.h"           // IWYU pragma: export
#include "donner/svg/SVGUseElement.h"             // IWYU pragma: export

// Types that are not fully-fledged SVG elements by themselves, so they aren't included in \ref
// AllSVGElements.
#include "donner/svg/SVGUnknownElement.h"  // IWYU pragma: keep

namespace donner::svg {

/**
 * A type list of all SVG element types, used by \ref AttributeParser and \ref XMLParser.
 */
using AllSVGElements = entt::type_list<  //
    SVGCircleElement,                    //
    SVGDefsElement,                      //
    SVGEllipseElement,                   //
    SVGFEGaussianBlurElement,            //
    SVGFilterElement,                    //
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