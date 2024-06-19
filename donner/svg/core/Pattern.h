#pragma once
/// @file

#include "donner/css/Color.h"

namespace donner::svg {

/**
 * PatternUnits controls the coordinate system of the x/y/width/height attributes:
 * - For \ref PatternUnits::ObjectBoundingBox (**default**), coordinates are percentages of the
 * object bounding box, where 0.0 is the left and 1.0 is the right, and similarly for the y axis.
 * - For \ref PatternUnits::UserSpaceOnUse, `patternUnits="objectBoundingBox"`, coordinates are
 * specified in the current user space where the pattern is referenced.
 *
 * See also: \ref PatternContentUnits
 *
 * For example, both of these patterns appear the same when applied to a 50x50 shape, but the
 * `objectBoundingBox` one changes based on the shape the pattern is applied to:
 * ```
 * <pattern id="bbox" x="0.1" y="0.2" width="0.5" height="0.5">
 *   <rect x="0" y="0" width="12" height="12" fill="lime" />
 *   <rect x="10" y="10" width="12" height="12" fill="gray" />
 * </pattern>
 *
 * <pattern id="userspace" patternUnits="userSpaceOnUse" x="5" y="10" width="25" height="25">
 *   <rect x="0" y="0" width="12" height="12" fill="lime" />
 *   <rect x="10" y="10" width="12" height="12" fill="gray" />
 * </pattern>
 * ```
 *
 * See also: \ref PatternUnits
 *
 * \htmlonly
 * <svg xmlns="http://www.w3.org/2000/svg" id="patternUnits"
 *    width="400" height="150" viewBox="0 0 200 75">
 *   <text x="100" y="7" class="bold">PatternUnits</text>
 *   <style>
 *     #patternUnits text { fill: gray; font-size: 6px; text-anchor: middle }
 *     #patternUnits text.bold { font-weight: bold }
 *   </style>
 *   <pattern id="bbox" y="0.2" width="0.5" height="0.5">
 *     <rect x="0" y="0" width="12" height="12" fill="lime" />
 *     <rect x="10" y="10" width="12" height="12" fill="gray" />
 *   </pattern>
 *   <pattern id="userspace" patternUnits="userSpaceOnUse" y="10" width="25" height="25">
 *     <rect x="0" y="0" width="12" height="12" fill="lime" />
 *     <rect x="10" y="10" width="12" height="12" fill="gray" />
 *   </pattern>
 *
 *   <g transform="translate(0 10)">
 *     <rect width="100" height="50" fill="lightgray" />
 *     <rect width="50" height="50" fill="url(#bbox)" stroke="red">
 *       <animate
 *         attributeName="width"
 *         dur="10s"
 *         values="25;100;25"
 *         keyTimes="0;0.5;1"
 *         repeatCount="indefinite" />
 *     </rect>
 *     <text x="50" y="57">objectBoundingBox (default)</text>
 *
 *     <rect x="100" width="100" height="50" fill="lightblue" />
 *     <rect x="100" width="50" height="50" fill="url(#userspace)" stroke="red">
 *       <animate
 *         attributeName="width"
 *         dur="10s"
 *         values="25;100;25"
 *         keyTimes="0;0.5;1"
 *         repeatCount="indefinite" />
 *       </rect>
 *     <text x="150" y="57">userSpaceOnUse</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 */
enum class PatternUnits {
  /// The pattern's x/y/width/height attributes are specified in the current user space where the
  /// pattern is referenced.
  UserSpaceOnUse,
  /// The pattern's x/y/width/height attributes are specified as percentages of the object bounding
  ObjectBoundingBox,
  /// The default value, \ref PatternUnits::ObjectBoundingBox.
  Default = ObjectBoundingBox,
};

/**
 * PatternContentUnits controls the coordinate system of the children of the pattern:
 * - For \ref PatternContentUnits::UserSpaceOnUse (**default**),
 * coordinates are specified in the current user space where the pattern is referenced.
 * - For \ref PatternContentUnits::ObjectBoundingBox, `patternContentUnits="objectBoundingBox"`,
 * coordinates are percentages of the object bounding box, where 0.0 is the left and 1.0 is the
 * right, and similarly for the y axis.
 *
 * With userSpaceOnUse, as the shape size changes, padding will be added between tiles. With
 * objectBoundingBox, the tiles will be stretched to fill the shape.
 *
 * ```
 * <pattern id="contentUserspace" patternContentUnits="userSpaceOnUse"
 *     x="0" y="0.2" width="0.5" height="0.5">
 *   <rect x="0" y="0" width="12" height="12" fill="lime" />
 *   <rect x="10" y="10" width="12" height="12" fill="gray" />
 * </pattern>
 *
 * <pattern id="contentBbox" patternContentUnits="objectBoundingBox"
 *     x="0" y="0.2" width="0.5" height="0.5">
 *   <rect x="0" y="0" width="0.24" height="0.24" fill="lime" />
 *   <rect x="0.2" y="0.2" width="0.24" height="0.24" fill="gray" />
 * </pattern>
 * ```
 * \htmlonly
 * <svg xmlns="http://www.w3.org/2000/svg" id="patternContentUnits"
 *     width="400" height="150" viewBox="0 0 200 75">
 *   <text x="100" y="7" class="bold">
 *     PatternContentUnits
 *   </text>
 *   <style>
 *     #patternContentUnits text { fill: gray; font-size: 6px; text-anchor: middle }
 *     #patternContentUnits text.bold { font-weight: bold }
 *   </style>
 *   <pattern id="contentUserspace" patternContentUnits="userSpaceOnUse"
 *       x="0" y="0.2" width="0.5" height="0.5">
 *     <rect x="0" y="0" width="12" height="12" fill="lime" />
 *     <rect x="10" y="10" width="12" height="12" fill="gray" />
 *   </pattern>
 *   <pattern id="contentBbox" patternContentUnits="objectBoundingBox"
 *       x="0" y="0.2" width="0.5" height="0.5">
 *     <rect x="0" y="0" width="0.24" height="0.24" fill="lime" />
 *     <rect x="0.2" y="0.2" width="0.24" height="0.24" fill="gray" />
 *   </pattern>
 *
 *   <g transform="translate(0 10)">
 *     <rect width="100" height="50" fill="lightgray" />
 *     <rect width="50" height="50" fill="url(#contentUserspace)" stroke="red">
 *       <animate
 *         attributeName="width"
 *         dur="10s"
 *         values="25;100;25"
 *         keyTimes="0;0.5;1"
 *         repeatCount="indefinite" />
 *     </rect>
 *     <text x="50" y="57">userSpaceOnUse (default)</text>
 *
 *     <rect x="100" width="100" height="50" fill="lightblue" />
 *     <rect x="100" width="50" height="50" fill="url(#contentBbox)" stroke="red">
 *       <animate
 *         attributeName="width"
 *         dur="10s"
 *         values="25;100;25"
 *         keyTimes="0;0.5;1"
 *         repeatCount="indefinite" />
 *       </rect>
 *     <text x="150" y="57">objectBoundingBox</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 */
enum class PatternContentUnits {
  /// The children of the pattern are specified in the current user space where the pattern is
  /// referenced.
  UserSpaceOnUse,
  /// The children of the pattern are specified as percentages of the object bounding box, where 0.0
  /// is the left and 1.0 is the right, and similarly for the y axis.
  ObjectBoundingBox,
  /// The default value, \ref PatternContentUnits::UserSpaceOnUse.
  Default = UserSpaceOnUse,
};

}  // namespace donner::svg
