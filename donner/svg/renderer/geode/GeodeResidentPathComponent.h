#pragma once
/// @file
/// Per-entity GPU residence for Geode's cached encoded-path geometry
/// (design doc 0030 wave 2: "GPU residence").
///
/// The M2 `GeodePathCacheComponent` keeps the CPU-side `EncodedPath`
/// across frames, but wave 1 still re-uploaded that geometry to a fresh
/// per-frame bump arena on every draw - the measured headline cost was a
/// static Ghostscript Tiger writing ~1.44 MB per frame across 2,432
/// `writeBuffer` calls even though the CPU encode cache hit, plus one
/// `createBindGroup` per draw (304/frame).
///
/// This component gives each cached path a persistent GPU buffer that
/// survives frames, so an unchanged document's steady-state frame writes
/// ~zero geometry bytes and re-uses a cached bind group. It lives BESIDE
/// `GeodePathCacheComponent` on the same entity and is removed by the
/// same `ComputedPathComponent` on_update / on_destroy listener
/// (`RendererGeode::Impl::onComputedPathChanged`), so the GPU residence
/// invalidates exactly when the geometry changes. Registry teardown
/// (document close) destroys the component and RAII-frees the buffer -
/// the eviction story for "many distinct documents".
///
/// Lives in `donner::geode` (not `donner::svg::geode`) to match the other
/// Geode types referenced unqualified via `geode::` inside `donner::svg`.

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "donner/gpu/Handles.h"

namespace donner::geode {

/// GPU-resident geometry for one cached `EncodedPath` (a fill slot or a
/// stroke slot). Owns a single combined-usage GPU buffer that holds the
/// path's vertex quad, the six analytic dual-ray SSBO regions, and the
/// per-draw uniform block, plus the cached fill bind group. The buffer
/// and bind group are built once by `GeoEncoder` on first residence and
/// reused every subsequent unchanged frame.
///
/// Move-only (owns donner::gpu handles). Default-constructed slots are empty;
/// `GeoEncoder::fillPathResident` populates them lazily.
struct GeodeResidentSlot {
  /// Combined Vertex|Storage|Uniform|CopyDst buffer. Layout (each region
  /// offset satisfies the binding's alignment requirement):
  ///   [ vertex quad | bands | curves | vBands | vCurves | hGrid | vGrid | uniform ]
  gpu::Buffer buffer;

  /// Cached fill bind group. All twelve bindings reference stable
  /// objects (this slot's `buffer` sub-ranges + device-owned dummy
  /// texture/sampler/identity-instance handles), so it survives frames
  /// and encoders. Rebuilt only when the geometry buffer is
  /// re-allocated.
  gpu::BindGroup bindGroup;

  /// A byte sub-range of `buffer`. `size == 0` is never bound directly -
  /// empty SSBO regions reserve a 4-byte zero-filled slot so the shader's
  /// band-count gate keeps them un-dereferenced (matching the wave-1
  /// arena `allocStorageOrDummy` dummy).
  struct Region {
    uint64_t offset = 0;
    uint64_t size = 0;
  };

  Region vertex;   ///< Vertex quad range (4-byte aligned offset).
  Region bands;    ///< Horizontal band SSBO (binding 1).
  Region curves;   ///< Horizontal curve SSBO (binding 2).
  Region vBands;   ///< Vertical band SSBO (binding 8).
  Region vCurves;  ///< Vertical curve SSBO (binding 9).
  Region hGrid;    ///< Horizontal band grid (binding 10).
  Region vGrid;    ///< Vertical band grid (binding 11).
  Region uniform;  ///< Per-draw uniform block (binding 0, 256-aligned).

  uint32_t vertexCount = 0;  ///< Number of quad vertices (draw count).

  /// Frame index in which this slot was last drawn via the resident path.
  /// A slot's single uniform buffer + cached bind group can only serve ONE
  /// draw per frame (all draws recorded against a frame's command buffer
  /// read the buffer's final contents at submit time). When the same slot
  /// is drawn again in the same frame at a different transform/color
  /// (markers, non-adjacent repeated `<use>`), the second and later draws
  /// fall back to the wave-1 arena path so each gets its own uniform. The
  /// steady-state win is unaffected for the common case of a path drawn
  /// once per frame (Tiger / Lion). Sentinel `~0` means "never drawn".
  uint64_t lastResidentFrame = ~uint64_t{0};

  /// Process-unique id (see `gpu::Device::deviceId()`, i.e. the identity of
  /// the GeodeDevice's adapter device) of the device that created `buffer` /
  /// `bindGroup`. A document's ECS residence components can outlive the
  /// device that filled them and later be rendered by a second
  /// `RendererGeode` / `GeodeDevice`; the donner::gpu runtime fails closed
  /// (DeviceMismatch) on a bind group or buffer from device A inside device
  /// B's render pass. When this id does not match the current device at draw
  /// time the slot is treated as non-resident and re-uploaded onto the
  /// current device (the stale handles are released without touching the old
  /// device). `0` means "no device owns this slot yet".
  uint64_t owningDeviceId = 0;

  /// True once `buffer` + `bindGroup` hold the current encode. Cleared
  /// when the slot is reset or when a re-upload is required.
  bool resident = false;

  /// Identity guard defending against any missed invalidation: the
  /// address of the `EncodedPath` last uploaded plus a cheap size
  /// fingerprint. If either differs at draw time the geometry is
  /// re-uploaded. Component removal is the primary invalidation; this is
  /// belt-and-suspenders for the in-place stroke-slot rebuild path.
  const void* encodedKey = nullptr;
  uint64_t encodedFingerprint = 0;

  /// Bytes last written to the uniform region. A draw whose recomputed
  /// uniform matches this skips the `writeBuffer` entirely (steady-state
  /// static frame => zero buffer writes); a camera/color change rewrites
  /// only this 288-byte region and keeps the cached bind group.
  std::vector<uint8_t> lastUniform;

  /// Live-resident-bytes gauge, co-owned with `GeodeDevice` so the
  /// accounting is lifetime-safe even if the owning document outlives the
  /// device. Incremented on residence, decremented on reset/destroy.
  std::shared_ptr<std::atomic<int64_t>> liveBytesGauge;
  int64_t accountedBytes = 0;

  GeodeResidentSlot() = default;
  ~GeodeResidentSlot() { reset(); }

  GeodeResidentSlot(const GeodeResidentSlot&) = delete;
  GeodeResidentSlot& operator=(const GeodeResidentSlot&) = delete;
  GeodeResidentSlot(GeodeResidentSlot&&) noexcept = default;
  GeodeResidentSlot& operator=(GeodeResidentSlot&& other) noexcept {
    if (this != &other) {
      reset();
      buffer = std::move(other.buffer);
      bindGroup = std::move(other.bindGroup);
      vertex = other.vertex;
      bands = other.bands;
      curves = other.curves;
      vBands = other.vBands;
      vCurves = other.vCurves;
      hGrid = other.hGrid;
      vGrid = other.vGrid;
      uniform = other.uniform;
      vertexCount = other.vertexCount;
      lastResidentFrame = other.lastResidentFrame;
      resident = other.resident;
      encodedKey = other.encodedKey;
      encodedFingerprint = other.encodedFingerprint;
      owningDeviceId = other.owningDeviceId;
      lastUniform = std::move(other.lastUniform);
      liveBytesGauge = std::move(other.liveBytesGauge);
      accountedBytes = other.accountedBytes;
      other.accountedBytes = 0;
      other.resident = false;
      other.encodedKey = nullptr;
      other.owningDeviceId = 0;
    }
    return *this;
  }

  /// Release the GPU buffer + bind group handles and settle the live-bytes
  /// gauge. Safe to call on an empty slot.
  ///
  /// Lifetime contract under the donner::gpu runtime: destroying a handle
  /// that an already-SUBMITTED frame references is safe - the backend
  /// object's destruction is deferred by submission serial. But a handle
  /// referenced by RECORDED-BUT-UNSUBMITTED commands must NOT be dropped
  /// before submit: `gpu::Device::submit` re-validates every recorded
  /// identity and fails closed on a stale one. The existing flow guarantees
  /// this: invalidation (the ComputedPathComponent listener and the stroke
  /// slot rebuild) runs pre-recording, and same-frame repeat draws fall back
  /// to the arena path before any re-upload would touch this slot.
  void reset() {
    if (accountedBytes != 0 && liveBytesGauge) {
      liveBytesGauge->fetch_sub(accountedBytes, std::memory_order_relaxed);
    }
    accountedBytes = 0;
    bindGroup = gpu::BindGroup();
    buffer = gpu::Buffer();
    resident = false;
    encodedKey = nullptr;
    encodedFingerprint = 0;
    owningDeviceId = 0;
    lastUniform.clear();
  }
};

/// Sibling of `GeodePathCacheComponent`: holds the GPU residence for an
/// entity's fill and stroke encodes. Installed lazily by `RendererGeode`
/// at the solid-fill draw sites; removed by the same entt listener that
/// clears `GeodePathCacheComponent` when geometry changes.
struct GeodeResidentPathComponent {
  GeodeResidentSlot fillSlot;
  GeodeResidentSlot strokeSlot;
};

}  // namespace donner::geode
