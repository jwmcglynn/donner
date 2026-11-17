#pragma once
/// @file

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/Path.h"
#include "donner/base/Transform.h"

namespace donner::svg::components {

class StyleResourceBudget;

/**
 * One piece of an element's geometry and the transform that places it in a measurement space. A
 * path is bounded after a later transform is composed with \ref spaceFromPiece, so its bounds stay
 * tight however the transforms combine; a box, such as an image's or a text's, is mapped as a box.
 */
struct BoundsGeometryPiece {
  Transform2d spaceFromPiece;  //!< Transform from the piece into the measurement space.
  /// The piece's path, shared with other pieces that hold the same path, or its box.
  std::variant<std::shared_ptr<const Path>, Box2d> geometry;
};

/**
 * Bounds of @p pieces after @p targetFromSpace maps their measurement space into a target space, or
 * nothing when there are no pieces or a bound is not finite.
 *
 * @param pieces Geometry collected in one measurement space.
 * @param targetFromSpace Transform from the measurement space to the target space.
 */
std::optional<Box2d> BoundGeometryPieces(std::span<const BoundsGeometryPiece> pieces,
                                         const Transform2d& targetFromSpace);

/// Whether @p first and @p second hold exactly the same commands and points.
bool HasIdenticalPathData(const Path& first, const Path& second);

/**
 * Path copies that collected geometry holds, shared between pieces that hold the same path, such as
 * the pieces of every external `<use>` of one fragment. Each distinct copy is charged to a byte
 * allowance. Copies are found by a hash of their contents, so sharing a path costs one full
 * comparison at most however many different copies its source element has.
 */
class BoundsGeometryPaths {
public:
  /**
   * A shared copy of @p path, which @p source in @p registry holds, reusing an identical copy made
   * earlier.
   *
   * @param registry Registry holding @p source.
   * @param source Entity whose geometry @p path is.
   * @param path Path to share.
   * @param budget Allowance a new copy's bytes are charged to.
   * @return The shared copy, or nullptr when no copy is shared: once a new copy has not fit the
   *   allowance, or when a different copy of @p source has the same hash.
   */
  std::shared_ptr<const Path> share(const Registry& registry, Entity source, const Path& path,
                                    StyleResourceBudget& budget);

  /// Forget every copy.
  void clear() { paths_.clear(); }

private:
  /// One source element's copies by the hash of their contents, one copy per hash.
  using CopiesByHash = std::unordered_map<std::size_t, std::shared_ptr<const Path>>;

  std::map<std::pair<const Registry*, Entity>, CopiesByHash> paths_;
};

/// Where collected geometry goes: its pieces, the shared path copies they hold, and the allowance
/// those copies are charged to.
struct BoundsGeometryCollector {
  std::vector<BoundsGeometryPiece>& pieces;  //!< Receives the pieces.
  BoundsGeometryPaths& paths;                //!< Shares the pieces' path copies.
  StyleResourceBudget& budget;               //!< Charged for new path copies.
};

}  // namespace donner::svg::components
