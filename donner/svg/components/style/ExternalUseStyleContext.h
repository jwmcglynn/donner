#pragma once
/// @file
/// Temporary computed-parent state for one external-use occurrence.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"

namespace donner::svg::components {

class StyleResourceBudget;

/// Installed only while the child document's write access is held.
struct ExternalUseStyleContext {
  /// Share the host's computed style, whose record owns its lifetime charge.
  ExternalUseStyleContext(Entity root, std::shared_ptr<const ComputedStyle> hostStyle,
                          const ResolvedPaintServer& fill, const ResolvedPaintServer& stroke,
                          std::shared_ptr<StyleResourceBudget> work)
      : root(root),
        parent{.style = std::move(hostStyle)},
        fill(fill),
        stroke(stroke),
        work(std::move(work)) {}

  ExternalUseStyleContext(const ExternalUseStyleContext&) = delete;
  ExternalUseStyleContext& operator=(const ExternalUseStyleContext&) = delete;

  bool unsupportedResource = false;  ///< An inherited resource was refused in this occurrence.
  /// \ref FontManager::hostFontKey of the fonts the host lends this occurrence; zero for none.
  /// Text prepared under one set of lent fonts is not reused under another.
  std::uint64_t hostFontKey = 0;
  Entity root;                    ///< Referenced instance root.
  ComputedStyleComponent parent;  ///< Host's shared computed style and canonical resource values.
  ResolvedPaintServer fill;       ///< Host-registry-qualified fill.
  ResolvedPaintServer stroke;     ///< Host-registry-qualified stroke.
  std::shared_ptr<StyleResourceBudget>
      work;  ///< Frame-wide work allowance, never reset by child passes.
};

}  // namespace donner::svg::components
