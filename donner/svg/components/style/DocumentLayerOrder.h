#pragma once

#include <memory>
#include <span>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/css/Stylesheet.h"

namespace donner::svg::components {

/// Frozen author layer ranks for one cascade pass.
class DocumentLayerOrder {
public:
  /// Creates a pass-owned order. Returns null when no CSS author sheet carries layer syntax.
  static std::unique_ptr<DocumentLayerOrder> Build(const Registry& registry);
  ~DocumentLayerOrder();

  /// Borrowed rank lookup for one sheet occurrence. Zero denotes a limit refusal.
  struct SheetRanks {
    std::uint16_t unlayered;
    std::span<const std::uint16_t> local;

    /// Looks up a rule's rank in normal precedence order.
    std::uint16_t forLayer(css::LayerId layer) const {
      if (layer == css::kUnlayered) {
        return unlayered;
      }
      return layer < local.size() ? local[layer] : 0;
    }
  };

  /// False when the root stylesheet has invalid flattened import provenance.
  bool allowsImportsOf(const css::Stylesheet& sheet) const;

  /// Whether at least one attached author stylesheet enables ranked cascade.
  bool hasRanks() const;

  /// Returns ranks for a single author stylesheet occurrence.
  SheetRanks ranksFor(const css::Stylesheet& sheet) const UTILS_LIFETIME_BOUND;

  /// Author and UA sheets in live tree order when layering is active.
  std::span<const Entity> stylesheets() const UTILS_LIFETIME_BOUND;

  /// Reports a bounded diagnostic when a layer or import exceeded a document limit.
  void reportRefusal(ParseWarningSink& warnings) const;

private:
  struct Data;
  DocumentLayerOrder();
  std::unique_ptr<Data> data_;
};

}  // namespace donner::svg::components
