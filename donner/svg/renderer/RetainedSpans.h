#pragma once
/// @file
/// Per-shape retained rasterization for the tiny-skia backend.
///
/// Scan conversion is the expensive half of a shape draw, and its whole pixel effect is the
/// sequence of blit calls it makes. Recording that sequence once and replaying it on later
/// frames turns an unchanged shape into a blit-only draw. This file owns the storage for those
/// recordings, the key that decides whether a recording still describes the draw the renderer
/// is about to make, and the memory bound on how much a document may keep.
///
/// The correctness rule is one-directional: a recording may be discarded for any reason, but it
/// may only be replayed when every input the recorded coverage depended on is unchanged. Keys
/// therefore compare conservatively (bitwise float equality, patterns never equal), and every
/// input that is re-supplied at replay rather than baked into the coverage (the paint the draw
/// built its blitter from, the clip mask) is either part of the key or reproduced exactly.

#include <cstdint>
#include <optional>

#include "donner/base/EcsRegistry.h"
#include "donner/base/FillRule.h"
#include "donner/base/Transform.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Paint.h"
#include "tiny_skia/SpanCapture.h"
#include "tiny_skia/Stroke.h"

namespace donner::svg {

/// Everything a captured blit sequence depends on, besides the shape's geometry.
///
/// Geometry is deliberately absent: it is tracked by removing a shape's whole retained entry
/// when its resolved path changes, which costs nothing per frame, rather than by re-comparing
/// point arrays on every draw. Everything else is small enough to compare directly, so the
/// renderer never has to reason about which mutation hooks mark which property.
struct RetainedSpanKey {
  /// Local-to-device transform the coverage was scan converted with.
  Transform2d deviceFromLocal;

  /// Paint the draw was handed. The coverage itself carries no color, but the paint decides
  /// antialiasing, and a draw may derive its blitter's paint from this one (a hairline stroke
  /// folds its coverage into the shader opacity, and a transformed draw transforms the
  /// shader), so a replay is only sound while this input is unchanged.
  tiny_skia::Paint paint;

  /// Stroke configuration, unused by a fill pass.
  tiny_skia::Stroke stroke;

  /// Identity of the clip mask in effect, or zero for no clip. @see RendererTinySkia's clip
  /// epoch assignment: equal epochs mean byte-equal masks.
  std::uint64_t clipEpoch = 0;

  /// Surface the coverage was recorded against. Recorded runs are device-space rectangles that
  /// nothing re-clips, so a differently sized surface cannot receive them.
  tiny_skia::IntSize surfaceSize;

  /// Fill rule, unused by a stroke pass.
  FillRule fillRule = FillRule::NonZero;

  /// Whether the stroke pass replaced closed contours with explicit closing lines, which the
  /// zero-length-gap dash case does and which changes the outline the stroker expands.
  bool openDashSeam = false;

  friend bool operator==(const RetainedSpanKey& lhs, const RetainedSpanKey& rhs);
};

/// One retained fill or stroke pass of one shape.
struct RetainedSpanSlot {
  /// Recorded coverage, plus the paint the recorded draw built its blitter from. Kept across
  /// invalidations so a shape that is rebuilt repeatedly settles into allocation-free captures.
  tiny_skia::SpanCapture capture;

  /// Inputs `capture` was recorded under. Only meaningful while `valid` is true.
  RetainedSpanKey key;

  /// Whether `capture` holds a replayable recording.
  bool valid = false;

  /// Bytes this slot is currently charged against the document budget.
  std::size_t chargedBytes = 0;

  /// Drops the recording, keeping the allocation for the next capture.
  void invalidate() { valid = false; }
};

/**
 * Retained rasterization output for one shape, attached to the entity its geometry came from.
 *
 * Removal is the invalidation mechanism for geometry: `RendererTinySkia` listens on the
 * resolved-path update and destroy signals and drops this component, so a shape whose outline
 * changed cannot replay stale coverage. Everything else is decided by comparing
 * \ref RetainedSpanKey at draw time.
 *
 * @ingroup ecs_components
 */
struct RetainedSpansComponent {
  RetainedSpanSlot fill;    ///< Retained fill pass.
  RetainedSpanSlot stroke;  ///< Retained stroke pass.

  /// Frame index of the most recent draw that read or wrote this entry, used to evict the
  /// coldest entries first when a document exceeds its budget.
  std::uint64_t lastUsedFrame = 0;

  /// Frame index this entry last counted a draw in, paired with \ref drawsThisFrame.
  std::uint64_t drawFrame = 0;

  /// Draws this entry has seen within `drawFrame`.
  std::uint32_t drawsThisFrame = 0;

  /// Set when one entity is drawn more than once in a single frame, which happens when a
  /// shape is instanced through a shadow tree or split into separate fill and stroke passes by
  /// `paint-order`. One entry cannot describe several draws at once, and alternating between
  /// them would re-capture on every draw, so the entry stops retaining instead.
  bool ambiguous = false;

  /// Bytes both slots are charged against the document budget.
  [[nodiscard]] std::size_t chargedBytes() const { return fill.chargedBytes + stroke.chargedBytes; }
};

/**
 * Document-wide state for retained coverage, stored in the registry context.
 *
 * Retained coverage is attacker-influenced: a document chooses how many shapes exist and how
 * much coverage each produces. The budget here is what keeps that bounded. Exceeding it evicts
 * the coldest entries, and a working set that does not fit at all turns retention off for the
 * document rather than growing without limit.
 *
 * It also issues frame identities, because retained entries belong to the document rather than
 * to any one renderer: several renderers can draw one document, and each of their frames needs
 * an identity the others cannot collide with.
 */
struct RetainedSpanDocumentState {
  /// Default budget. Sized so a full-viewport document of a few hundred shapes fits while a
  /// pathological one cannot grow unbounded.
  static constexpr std::size_t kDefaultBudgetBytes = 32u * 1024u * 1024u;

  /// Bytes currently held by every retained entry in this document.
  std::size_t liveBytes = 0;

  /// Ceiling on `liveBytes`.
  std::size_t budgetBytes = kDefaultBudgetBytes;

  /// Set when the working set did not fit the budget, which turns retention off for this
  /// document until it is reset. Falling back wholesale is cheaper and more predictable than
  /// re-capturing the same shapes every frame.
  bool disabled = false;

  /// Entries evicted to stay under budget, across the document's lifetime.
  std::uint64_t evictions = 0;

  /// Issues frame identities. Every frame of every renderer drawing this document takes the
  /// next value, so "already drawn in this frame" cannot be confused with "drawn by another
  /// renderer".
  std::uint64_t frameCounter = 0;
};

/// Counters describing what a renderer's retained coverage did, for tests and benchmarks.
///
/// Counts are per fill or stroke pass of a shape draw, and cover the frame most recently drawn.
/// Draw kinds retention does not reach at all (text, images, layer composites) are not counted
/// here at all, not even as bypassed.
struct RetainedSpanStats {
  std::uint64_t replayedDraws = 0;     ///< Passes served from retained coverage.
  std::uint64_t capturedDraws = 0;     ///< Passes that rasterized and recorded their coverage.
  std::uint64_t invalidatedDraws = 0;  ///< Captures that replaced a recording whose key changed.
  std::uint64_t bypassedDraws = 0;     ///< Passes retention did not apply to.
  /// Draws whose recording was refused at replay, which then rasterized instead. A refusal is
  /// the surface-size guard doing its job, never a dropped shape.
  std::uint64_t refusedReplays = 0;
  /// Draws that could not be recorded (a tiled surface, or a blit the packed record cannot
  /// hold). The pixels are painted regardless.
  std::uint64_t unrecordableDraws = 0;
  std::size_t liveBytes = 0;      ///< Bytes retained by the document at the end of the frame.
  std::uint64_t evictions = 0;    ///< Entries evicted to stay under budget.
  bool documentDisabled = false;  ///< Whether the document fell back to immediate mode.
};

/// Connects the resolved-path invalidation listener to `registry`, once per registry.
void EnsureRetainedSpanInvalidationWired(Registry& registry);

/// Returns the document's retained-coverage state, creating it if needed.
RetainedSpanDocumentState& RetainedSpanStateFor(Registry& registry, std::size_t budgetBytes);

/// Drops every retained entry in `registry` and resets its accounting.
void ClearRetainedSpans(Registry& registry);

/// Evicts the coldest entries until the document is under budget.
///
/// Entries last used before `currentFrame` go first. When dropping all of those still leaves
/// the document over budget, its working set does not fit, so retention is disabled for the
/// document and everything is dropped.
void EvictRetainedSpansToBudget(Registry& registry, std::uint64_t currentFrame);

}  // namespace donner::svg
