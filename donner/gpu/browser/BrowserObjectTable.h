#pragma once
/// @file
/// \c donner::gpu::browser::BrowserObjectTable - checked identifiers for browser-owned objects.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string_view>
#include <utility>
#include <vector>

namespace donner::gpu::browser {

/// Kind of browser object a \ref donner::gpu::browser::BrowserObjectId "BrowserObjectId" names.
///
/// The kind travels with every identifier so the browser side can refuse an identifier that names
/// a live object of the wrong kind, rather than calling a texture method on a buffer and
/// surfacing whatever the browser makes of that.
enum class BrowserObjectKind : uint8_t {
  Buffer,           //!< A buffer.
  Texture,          //!< A texture.
  TextureView,      //!< A view of a texture.
  Sampler,          //!< A sampler.
  BindGroupLayout,  //!< A bind group layout.
  BindGroup,        //!< A bind group.
  PipelineLayout,   //!< A pipeline layout.
  ShaderModule,     //!< A shader module.
  RenderPipeline,   //!< A render pipeline.
  ComputePipeline,  //!< A compute pipeline.
  Surface,          //!< A presentation surface and the canvas context behind it.
  BufferMapping,    //!< A host mapping of a buffer range.
  /// Number of kinds. Not a kind; it exists so per-kind storage and the protocol table below are
  /// sized by the enumeration itself rather than by a number kept in step with it by hand.
  kCount,
};

/// Number of \ref BrowserObjectKind enumerators, for per-kind table sizing.
inline constexpr size_t kBrowserObjectKindCount = static_cast<size_t>(BrowserObjectKind::kCount);

/// Returns the name of \p kind, e.g. `"buffer"`. Used in diagnostics.
/// @param kind Kind to name.
std::string_view BrowserObjectKindName(BrowserObjectKind kind);

/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, BrowserObjectKind value);

/**
 * Identifier of one object the browser side owns.
 *
 * Identifiers are minted in increasing order and never reused, so an identifier retained past its
 * object's destruction names nothing rather than naming whatever was created next. That is the
 * property slot indices alone cannot provide: the runtime recycles slots, so a stale slot index
 * would eventually address a live object of the same kind.
 */
using BrowserObjectId = uint32_t;

/// Identifier value that names no browser object. Every operation refuses it.
inline constexpr BrowserObjectId kNoBrowserObject = 0;

/// What \ref BrowserObjectTable::insert did: the identifier it minted, and any identifier the
/// slot was still holding.
struct BrowserObjectInsertion {
  /// Newly minted identifier, or \ref kNoBrowserObject when none could be minted.
  BrowserObjectId id = kNoBrowserObject;
  /// Identifier the slot held before this insertion, or \ref kNoBrowserObject when it was free.
  /// The caller releases it on the browser side; it is never handed out again.
  BrowserObjectId displaced = kNoBrowserObject;
};

/**
 * Maps the runtime's per-kind resource slots to the identifiers the browser side knows them by.
 *
 * The runtime addresses a resource by (kind, slot index) and reuses a slot once its occupant is
 * gone; the browser side addresses the same object by an identifier that is never reused. This
 * table is the single place the two namings meet, so a stale identifier cannot re-enter
 * circulation through slot reuse.
 *
 * Identifiers are drawn from one counter shared by every kind, which keeps an identifier
 * meaningful on its own: a value that names a buffer never also names a texture, so the kind
 * check the browser side performs is a genuine second check rather than a restatement of the
 * first.
 */
class BrowserObjectTable {
public:
  /// Largest slot index this table will store.
  ///
  /// Per-kind storage is a dense vector indexed by slot, so a slot index decides an allocation
  /// size. The runtime allocates slots densely from zero and its resource limits are far below
  /// this, so a slot index anywhere near it did not come from the runtime; refusing it keeps a
  /// wrong index from becoming a large allocation.
  static constexpr uint32_t kMaxSlotIndex = (1u << 20) - 1;

  /**
   * Mints an identifier for \p slotIndex of \p kind and records the mapping.
   *
   * A slot that still holds an identifier has its old one displaced rather than refused. Most
   * resource kinds are released through a destruction hook before their slot is reused, so
   * nothing is ever displaced for them; a surface has no such hook, because the platform object
   * it presents to outlives the runtime's handle, so its slot can be reused while this table
   * still names the browser object it used to hold. Reporting the displaced identifier lets the
   * caller release it on the browser side in that case, and makes the common case where nothing
   * was displaced visible rather than assumed.
   *
   * Returns a null \ref BrowserObjectInsertion::id if the identifier space is exhausted or
   * \p slotIndex is beyond \ref kMaxSlotIndex; callers fail closed on that rather than proceeding
   * with an identifier the browser side would refuse.
   *
   * @param kind Kind of object occupying the slot.
   * @param slotIndex Runtime slot index of the resource.
   */
  BrowserObjectInsertion insert(BrowserObjectKind kind, uint32_t slotIndex);

  /**
   * Returns the identifier recorded for (\p kind, \p slotIndex), or nullopt when the slot holds
   * none.
   *
   * @param kind Kind of object to look up.
   * @param slotIndex Runtime slot index of the resource.
   */
  std::optional<BrowserObjectId> find(BrowserObjectKind kind, uint32_t slotIndex) const;

  /**
   * Removes and returns the identifier recorded for (\p kind, \p slotIndex), or nullopt when the
   * slot holds none. The identifier is not returned to circulation.
   *
   * @param kind Kind of object to remove.
   * @param slotIndex Runtime slot index of the resource.
   */
  std::optional<BrowserObjectId> remove(BrowserObjectKind kind, uint32_t slotIndex);

  /// Number of slots currently holding an identifier, across every kind.
  size_t liveCount() const;

  /**
   * Empties the table and returns everything it held, so teardown can release each browser object
   * exactly once.
   *
   * Entries come back grouped by kind in enumerator order and by ascending slot within a kind,
   * which makes teardown order deterministic and therefore reproducible in a test.
   */
  std::vector<std::pair<BrowserObjectKind, BrowserObjectId>> takeAll();

  /// Identifier the next \ref insert will mint, for tests that pin the never-reused property.
  BrowserObjectId nextIdForTest() const { return nextId_; }

private:
  /// Per-kind slot storage; each vector is indexed by runtime slot index and holds
  /// \ref kNoBrowserObject for a slot with no live object.
  std::array<std::vector<BrowserObjectId>, kBrowserObjectKindCount> slotsByKind_;

  /// Next identifier to mint. Monotonic: a released identifier is never handed out again.
  BrowserObjectId nextId_ = 1;
};

}  // namespace donner::gpu::browser
