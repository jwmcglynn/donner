#pragma once
/**
 * @file
 *
 * A single include that covers all SVG element types, plus \ref donner::svg::AllSVGElements which
 * can be used to perform constexpr lookups across all element types.
 */

#include <entt/entt.hpp>

#include "donner/svg/SVGAnimateElement.h"              // IWYU pragma: export
#include "donner/svg/SVGAnimateMotionElement.h"        // IWYU pragma: export
#include "donner/svg/SVGAnimateTransformElement.h"     // IWYU pragma: export
#include "donner/svg/SVGCircleElement.h"          // IWYU pragma: export
#include "donner/svg/SVGClipPathElement.h"        // IWYU pragma: export
#include "donner/svg/SVGDefsElement.h"            // IWYU pragma: export
#include "donner/svg/SVGEllipseElement.h"         // IWYU pragma: export
#include "donner/svg/SVGFEBlendElement.h"          // IWYU pragma: export
#include "donner/svg/SVGFEColorMatrixElement.h"    // IWYU pragma: export
#include "donner/svg/SVGFEComponentTransferElement.h"  // IWYU pragma: export
#include "donner/svg/SVGFECompositeElement.h"          // IWYU pragma: export
#include "donner/svg/SVGFEConvolveMatrixElement.h"     // IWYU pragma: export
#include "donner/svg/SVGFEDiffuseLightingElement.h"    // IWYU pragma: export
#include "donner/svg/SVGFEDisplacementMapElement.h"    // IWYU pragma: export
#include "donner/svg/SVGFEDistantLightElement.h"       // IWYU pragma: export
#include "donner/svg/SVGFEDropShadowElement.h"         // IWYU pragma: export
#include "donner/svg/SVGFEFloodElement.h"              // IWYU pragma: export
#include "donner/svg/SVGFEFuncAElement.h"              // IWYU pragma: export
#include "donner/svg/SVGFEFuncBElement.h"              // IWYU pragma: export
#include "donner/svg/SVGFEFuncGElement.h"              // IWYU pragma: export
#include "donner/svg/SVGFEFuncRElement.h"              // IWYU pragma: export
#include "donner/svg/SVGFEGaussianBlurElement.h"  // IWYU pragma: export
#include "donner/svg/SVGFEImageElement.h"         // IWYU pragma: export
#include "donner/svg/SVGFEMergeElement.h"         // IWYU pragma: export
#include "donner/svg/SVGFEMergeNodeElement.h"     // IWYU pragma: export
#include "donner/svg/SVGFEMorphologyElement.h"    // IWYU pragma: export
#include "donner/svg/SVGFEOffsetElement.h"        // IWYU pragma: export
#include "donner/svg/SVGFEPointLightElement.h"    // IWYU pragma: export
#include "donner/svg/SVGFESpecularLightingElement.h"  // IWYU pragma: export
#include "donner/svg/SVGFESpotLightElement.h"     // IWYU pragma: export
#include "donner/svg/SVGFETileElement.h"          // IWYU pragma: export
#include "donner/svg/SVGFETurbulenceElement.h"    // IWYU pragma: export
#include "donner/svg/SVGFilterElement.h"          // IWYU pragma: export
#include "donner/svg/SVGGElement.h"               // IWYU pragma: export
#include "donner/svg/SVGImageElement.h"           // IWYU pragma: export
#include "donner/svg/SVGLineElement.h"            // IWYU pragma: export
#include "donner/svg/SVGLinearGradientElement.h"  // IWYU pragma: export
#include "donner/svg/SVGMarkerElement.h"          // IWYU pragma: export
#include "donner/svg/SVGMaskElement.h"            // IWYU pragma: export
#include "donner/svg/SVGMPathElement.h"           // IWYU pragma: export
#include "donner/svg/SVGPathElement.h"            // IWYU pragma: export
#include "donner/svg/SVGPatternElement.h"         // IWYU pragma: export
#include "donner/svg/SVGPolygonElement.h"         // IWYU pragma: export
#include "donner/svg/SVGPolylineElement.h"        // IWYU pragma: export
#include "donner/svg/SVGRadialGradientElement.h"  // IWYU pragma: export
#include "donner/svg/SVGRectElement.h"            // IWYU pragma: export
#include "donner/svg/SVGSVGElement.h"             // IWYU pragma: export
#include "donner/svg/SVGSetElement.h"             // IWYU pragma: export
#include "donner/svg/SVGStopElement.h"            // IWYU pragma: export
#include "donner/svg/SVGStyleElement.h"           // IWYU pragma: export
#include "donner/svg/SVGSymbolElement.h"          // IWYU pragma: export
#include "donner/svg/SVGTSpanElement.h"           // IWYU pragma: export
#include "donner/svg/SVGTextElement.h"            // IWYU pragma: export
#include "donner/svg/SVGTextPathElement.h"        // IWYU pragma: export
#include "donner/svg/SVGUseElement.h"             // IWYU pragma: export

// Types that are not fully-fledged SVG elements by themselves, so they aren't included in \ref
// AllSVGElements.
#include "donner/svg/SVGUnknownElement.h"  // IWYU pragma: keep

namespace donner::svg {

/**
 * A type list of all SVG element types, used by \ref donner::svg::parser::AttributeParser and \ref
 * donner::svg::parser::SVGParser.
 */
using AllSVGElements = entt::type_list<  //
    SVGAnimateElement,                   //
    SVGAnimateMotionElement,             //
    SVGAnimateTransformElement,          //
    SVGCircleElement,                    //
    SVGClipPathElement,                  //
    SVGDefsElement,                      //
    SVGEllipseElement,                   //
    SVGFEBlendElement,                   //
    SVGFEColorMatrixElement,             //
    SVGFEComponentTransferElement,       //
    SVGFECompositeElement,               //
    SVGFEConvolveMatrixElement,          //
    SVGFEDiffuseLightingElement,         //
    SVGFEDisplacementMapElement,         //
    SVGFEDistantLightElement,            //
    SVGFEDropShadowElement,              //
    SVGFEFloodElement,                   //
    SVGFEFuncAElement,                   //
    SVGFEFuncBElement,                   //
    SVGFEFuncGElement,                   //
    SVGFEFuncRElement,                   //
    SVGFEGaussianBlurElement,            //
    SVGFEImageElement,                   //
    SVGFEMergeElement,                   //
    SVGFEMergeNodeElement,               //
    SVGFEMorphologyElement,              //
    SVGFEOffsetElement,                  //
    SVGFEPointLightElement,              //
    SVGFESpecularLightingElement,        //
    SVGFESpotLightElement,               //
    SVGFETileElement,                    //
    SVGFETurbulenceElement,              //
    SVGFilterElement,                    //
    SVGGElement,                         //
    SVGImageElement,                     //
    SVGLineElement,                      //
    SVGLinearGradientElement,            //
    SVGMarkerElement,                    //
    SVGMaskElement,                      //
    SVGMPathElement,                     //
    SVGStopElement,                      //
    SVGRadialGradientElement,            //
    SVGPathElement,                      //
    SVGPatternElement,                   //
    SVGPolygonElement,                   //
    SVGPolylineElement,                  //
    SVGRectElement,                      //
    SVGSetElement,                       //
    SVGStyleElement,                     //
    SVGSVGElement,                       //
    SVGSymbolElement,                    //
    SVGTextElement,                      //
    SVGTextPathElement,                  //
    SVGTSpanElement,                     //
    SVGUseElement>;

}  // namespace donner::svg
