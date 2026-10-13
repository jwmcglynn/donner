#pragma once
/// @file
/// Temporary, owned computed-parent state for one external-use occurrence.

#include <array>
#include <memory>
#include <optional>
#include <utility>

#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::svg::components {

class StyleResourceBudget;

/// Installed only while the child document's write access is held.
struct ExternalUseStyleContext {
  /// Create an owned parent snapshot after its caller has reserved its dynamic bytes.
  ExternalUseStyleContext(Entity root, const PropertyRegistry& properties,
                          const ResolvedPaintServer& fill, const ResolvedPaintServer& stroke,
                          std::shared_ptr<DocumentResourceFamilyBudget> budget,
                          std::size_t chargedBytes, std::shared_ptr<StyleResourceBudget> work)
      : root(root),
        parent(properties),
        fill(fill),
        stroke(stroke),
        family(std::move(budget)),
        chargedBytes(chargedBytes),
        work(std::move(work)) {}

  ~ExternalUseStyleContext() {
    parent.reset();
    if (family) {
      family->release(DocumentResourceFamilyBudget::Kind::ComputedStyle, chargedBytes);
    }
  }

  ExternalUseStyleContext(const ExternalUseStyleContext&) = delete;
  ExternalUseStyleContext& operator=(const ExternalUseStyleContext&) = delete;

  bool unsupportedResource = false;  ///< An inherited resource was refused in this occurrence.
  Entity root;                       ///< Referenced instance root.
  std::optional<PropertyRegistry> parent;  ///< Host's owned computed parent properties.
  ResolvedPaintServer fill;                ///< Host-registry-qualified fill.
  ResolvedPaintServer stroke;              ///< Host-registry-qualified stroke.
  std::shared_ptr<DocumentResourceFamilyBudget> family;  ///< Shared retained-byte envelope.
  std::size_t chargedBytes;                              ///< Live charge for the parent snapshot.
  std::shared_ptr<StyleResourceBudget>
      work;  ///< Frame-wide work allowance, never reset by child passes.
};

}  // namespace donner::svg::components
