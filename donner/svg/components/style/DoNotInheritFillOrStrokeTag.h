#pragma once
/// @file

namespace donner::svg::components {

/**
 * This component is added to entities to indicate that 'fill' and 'stroke' attributes should not be
 * inherited, which is used for \ref xml_pattern because it establishes a shadow tree, and we do not
 * want to recursively inherit 'fill' or 'stroke' values into the children.
 */
struct DoNotInheritFillOrStrokeTag {};

}  // namespace donner::svg::components
