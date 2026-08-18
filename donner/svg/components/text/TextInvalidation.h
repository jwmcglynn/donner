#pragma once
/// @file

#include "donner/base/EcsRegistry.h"

namespace donner::svg::components {

/**
 * Drop the cached text layout of the text root enclosing \p handle.
 *
 * \ref ComputedTextGeometryComponent is a memoized layout derived from the text root's computed
 * style, text content, and positioning attributes. Both renderers draw straight out of it, so any
 * mutation that can change one of those inputs must drop it or the old glyphs keep rendering.
 *
 * Walks up from \p handle until it finds the \ref TextRootComponent, so it is safe to call with a
 * `<tspan>`, `<textPath>`, or the `<text>` root itself. Calling it on an entity outside any text
 * subtree is a no-op.
 *
 * @param handle Element whose enclosing text root should be invalidated.
 * @return The text root entity that was invalidated, or `entt::null` if \p handle is not inside a
 *   text subtree.
 */
Entity InvalidateTextLayout(EntityHandle handle);

}  // namespace donner::svg::components
