#pragma once
/// @file
///
/// Cross-process GPU-texture bridging — phase A: interface only.
///
/// The long-term goal (see `docs/design_docs/0023-editor_sandbox.md`
/// §"Cross-process texture bridging") is to let the sandbox backend
/// render directly into a GPU texture the host can sample, replacing
/// the current GPU → CPU → wire → CPU → GPU round-trip with a single
/// shared allocation. Phase A establishes the interface, the protocol
/// opcode, and a pass-through stub so the plumbing can be tested end-
/// to-end before the platform-specific hal import lands in phase B.
///
/// Ownership model:
///   * Host allocates the shared texture (host has GL context and
///     owns the display-side lifetime).
///   * Host passes a `BridgeTextureHandle` to the backend once, at
///     session start, via `kAttachSharedTexture`.
///   * Backend imports the handle into its `svg::Renderer` via
///     `svg::Renderer::attachSharedTexture(...)` (not yet plumbed)
///     and draws into it each frame.
///   * Subsequent `FramePayload`s reference the bound texture
///     implicitly — the `finalBitmapPixels` field stays empty when
///     `hasSharedTextureBinding == true`.
///
/// The handle is a platform-typed value (mach_port / FD / HANDLE).
/// Phase A stores it as an opaque 64-bit integer so the abstract
/// interface stays portable; platform-specific code in phase B+
/// interprets it.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "donner/base/Vector2.h"

namespace donner::editor::sandbox::bridge {

/// Platform-specific shared surface type. Lets the platform-specific
/// import code (phase B+) switch on which kind of handle it's
/// receiving without redefining the opaque-integer interface.
enum class BridgeHandleKind : uint8_t {
  /// Phase-A stub — no real platform surface; treat as a signal to
  /// fall through to the `finalBitmapPixels` wire path. Removed when
  /// phase B lands on any platform.
  kCpuStub = 0,
  /// macOS IOSurface; handle is a `mach_port_t` (cast to uint64_t).
  /// Phase B.
  kIOSurfaceMacOS = 1,
  /// Linux dmabuf; handle is a file descriptor (cast to uint64_t).
  /// Phase C.
  kDmabufLinux = 2,
  /// Windows D3D11/12 shared handle; handle is a `HANDLE` (cast to
  /// uint64_t). Phase D.
  kSharedHandleWindows = 3,
};

/// Platform-opaque handle to a host-allocated shared GPU texture.
/// Phase A: all values default to `kCpuStub` + `handle=0` so the
/// session protocol can carry a well-formed descriptor even before
/// platform bridges are implemented.
struct BridgeTextureHandle {
  BridgeHandleKind kind = BridgeHandleKind::kCpuStub;
  uint64_t handle = 0;
  Vector2i dimensions = Vector2i::Zero();
  /// Bytes per row. 0 means "platform decides" (tight packing).
  uint32_t rowBytes = 0;
};

/// Abstract consumer-side bridge: implemented on the **backend**
/// (the process that draws into the shared texture). Construction
/// is deferred to platform-specific factories; phase A ships only
/// the stub factory + the plumbing hooks that call it.
class BridgeTextureBackend {
 public:
  virtual ~BridgeTextureBackend() = default;

  /// True once the backend has successfully imported the shared
  /// texture and is ready to render into it. False while phase A's
  /// CPU-stub path is active — the caller should fall back to
  /// `finalBitmapPixels`.
  [[nodiscard]] virtual bool ready() const = 0;

  /// Dimensions of the shared texture. Zero when `ready() == false`.
  [[nodiscard]] virtual Vector2i dimensions() const = 0;

  /// Signal to the backend renderer that the next `renderFrame`
  /// should target the shared texture. Phase A no-ops; phase B
  /// calls `wgpuDeviceCreateTextureFromHal` and binds the result
  /// to the render pass.
  virtual void bindAsRenderTarget() = 0;

  /// Block until the GPU has finished writing to the shared texture.
  /// Phase A no-ops (CPU path is synchronous anyway); phase B
  /// submits a fence and waits on it to guarantee the host reads
  /// only fully-composed frames.
  virtual void waitRenderComplete() = 0;
};

/// Abstract producer-side bridge: implemented on the **host** (the
/// process that allocates the shared texture and owns the display).
/// Host constructs this once at session start, hands a
/// `BridgeTextureHandle` to the backend, and each frame calls
/// `consumeFrame()` to pick up the latest backend-written contents
/// for upload/blit.
class BridgeTextureHost {
 public:
  virtual ~BridgeTextureHost() = default;

  /// Descriptor the host sends to the backend via
  /// `kAttachSharedTexture`. Stable across the session.
  [[nodiscard]] virtual BridgeTextureHandle handle() const = 0;

  /// Called once per frame after the backend has published a new
  /// frame. Returns the platform GL texture id the ImGui shader
  /// should sample (or 0 if no frame is available yet / phase-A
  /// stub).
  [[nodiscard]] virtual uint32_t glTextureId() = 0;
};

/// Phase-A factories. Return a stub that signals "no real bridge
/// available" (`ready() == false`); real platform factories follow
/// in phase B+.
[[nodiscard]] std::unique_ptr<BridgeTextureHost> MakeHostStub(Vector2i dimensions);
[[nodiscard]] std::unique_ptr<BridgeTextureBackend> MakeBackendStub(
    const BridgeTextureHandle& handle);

#if defined(__APPLE__)
/// Phase-B macOS factories. Allocate / consume a real `IOSurface`;
/// the `IOSurfaceID` is serialized in `BridgeTextureHandle.handle`.
/// `ready()` still returns `false` until the wgpu-native → Metal
/// import and `CGLTexImageIOSurface2D` pieces land (phase-B-rest),
/// at which point the GPU path engages and the CPU `finalBitmap
/// Pixels` field becomes a fallback.
[[nodiscard]] std::unique_ptr<BridgeTextureHost> MakeHost_macOS(Vector2i dimensions);
[[nodiscard]] std::unique_ptr<BridgeTextureBackend> MakeBackend_macOS(
    const BridgeTextureHandle& handle);
#endif

}  // namespace donner::editor::sandbox::bridge
