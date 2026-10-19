#pragma once
/// @file

#include <span>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseResult.h"
#include "donner/base/xml/XMLQualifiedName.h"

namespace donner::svg::parser {

/**
 * Parse one animation attribute into the element's internal animation state without modifying
 * XML. This is an internal hook for the SVG attribute parser, not a document API.
 *
 * @param handle Animation or mpath element.
 * @param name Attribute name.
 * @param value Attribute value.
 * @return True for a recognized attribute, false for other attributes, or a diagnostic.
 */
ParseResult<bool> ParseAnimationAttribute(EntityHandle handle, const xml::XMLQualifiedNameRef& name,
                                          std::string_view value);

/**
 * Refresh internal animation state after authored attributes change, retaining runtime events.
 * This is an internal hook for the animation timeline, not a document API. Attribute hashes and
 * parsing share a bounded per-call source envelope.
 *
 * @param registry Document storage.
 * @param animations Admitted animation elements.
 * @return False when source admission fails.
 */
bool RefreshAnimationAttributes(Registry& registry, std::span<const Entity> animations);

}  // namespace donner::svg::parser
