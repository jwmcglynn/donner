/// @file
///
/// Phase-B macOS implementation of the host-side texture bridge:
/// allocates an `IOSurface` the host owns and exposes its `mach_port`
/// so the backend can import it as a Metal/wgpu render target.
///
/// Scope of *this* file:
///   * Host-side allocation (`MakeHost_macOS`).
///   * Mach-port serialization (the `BridgeTextureHandle::handle`
///     carries an `IOSurfaceID`, convertible to `mach_port_t` via
///     `IOSurfaceCreateMachPort` on demand).
///   * Host-side Mach-port receive (`MakeBackend_macOS`), which
///     round-trips through `IOSurfaceLookupFromMachPort` so the
///     backend holds an `IOSurfaceRef` it can hand to Metal later.
///
/// Deliberately **out of scope** for this commit:
///   * wgpu-native MTLTexture import via the hal surface — needs
///     wgpu-rs's `wgpuTextureCreateFromMetalTexture`-style hook,
///     which requires a live `wgpu::Device` from Geode. Lands
///     alongside Geode integration tests that can observe the
///     resulting texture rendering.
///   * Host-side GL import via `CGLTexImageIOSurface2D` — needs
///     a live GL context; lands when the editor shell wires the
///     bridge handle into its texture-upload path.
///
/// Until those two pieces land, `ready() == false` and the caller
/// falls through to the existing `finalBitmapPixels` wire path —
/// same fallback behaviour as the CPU stub, but with real
/// cross-process plumbing exercised underneath.

#include "donner/editor/sandbox/bridge/BridgeTexture.h"

#include <IOSurface/IOSurfaceRef.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cstring>

namespace donner::editor::sandbox::bridge {

namespace {

/// `kIOSurfacePixelFormat` expects a FourCC code; "BGRA" matches the
/// layout we use for the rest of the editor pipeline (RGBA8 with
/// alpha last, little-endian memory order). Hard-coding here means
/// the host + backend don't have to negotiate pixel format at
/// session start.
constexpr uint32_t kPixelFormatBGRA8 = 'BGRA';
constexpr int kBytesPerElement = 4;

/// Tiny RAII wrapper so `IOSurfaceRef` gets released along the
/// `StubHost` / `StubBackend` object lifetimes. `IOSurface*` are
/// CFType-ish — `CFRetain` / `CFRelease` work on them.
class IOSurfaceRetainer {
 public:
  IOSurfaceRetainer() = default;
  explicit IOSurfaceRetainer(IOSurfaceRef surface) : surface_(surface) {
    if (surface_ != nullptr) {
      CFRetain(surface_);
    }
  }
  ~IOSurfaceRetainer() {
    if (surface_ != nullptr) {
      CFRelease(surface_);
    }
  }
  IOSurfaceRetainer(const IOSurfaceRetainer&) = delete;
  IOSurfaceRetainer& operator=(const IOSurfaceRetainer&) = delete;
  IOSurfaceRetainer(IOSurfaceRetainer&& other) noexcept : surface_(other.surface_) {
    other.surface_ = nullptr;
  }
  IOSurfaceRetainer& operator=(IOSurfaceRetainer&& other) noexcept {
    if (this != &other) {
      if (surface_ != nullptr) CFRelease(surface_);
      surface_ = other.surface_;
      other.surface_ = nullptr;
    }
    return *this;
  }

  [[nodiscard]] IOSurfaceRef get() const { return surface_; }
  [[nodiscard]] bool valid() const { return surface_ != nullptr; }

 private:
  IOSurfaceRef surface_ = nullptr;
};

/// Build the `CFDictionary` argument for `IOSurfaceCreate`. Uses
/// `CFDictionary` / `CFNumber` directly rather than the Foundation
/// `NSDictionary` bridge so this file stays buildable as plain C++
/// (no Objective-C runtime required).
CFDictionaryRef MakeSurfaceProperties(int width, int height) {
  CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 4, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);

  const auto setInt = [&](CFStringRef key, int value) {
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
    CFDictionarySetValue(dict, key, n);
    CFRelease(n);
  };
  const auto setU32 = [&](CFStringRef key, uint32_t value) {
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(dict, key, n);
    CFRelease(n);
  };

  setInt(kIOSurfaceWidth, width);
  setInt(kIOSurfaceHeight, height);
  setInt(kIOSurfaceBytesPerElement, kBytesPerElement);
  setU32(kIOSurfacePixelFormat, kPixelFormatBGRA8);
  return dict;
}

/// Host-side producer. Owns an `IOSurfaceRef` for the session's
/// lifetime. Phase B ships `handle()` returning a descriptor with
/// the surface's `IOSurfaceID` in the `handle` field (easy to
/// serialize over the session wire); the translation to
/// `mach_port_t` at attach time is done by the backend-side code
/// once we have a `mach_msg`-carrying transport in place.
class MacOSHost final : public BridgeTextureHost {
 public:
  MacOSHost(IOSurfaceRetainer surface, Vector2i dimensions)
      : surface_(std::move(surface)), dimensions_(dimensions) {}

  BridgeTextureHandle handle() const override {
    BridgeTextureHandle h;
    h.kind = BridgeHandleKind::kIOSurfaceMacOS;
    h.dimensions = dimensions_;
    h.rowBytes = surface_.valid()
                     ? static_cast<uint32_t>(IOSurfaceGetBytesPerRow(surface_.get()))
                     : 0;
    // `IOSurfaceID` fits in 32 bits; zero-extend into the handle
    // field. Peer side calls `IOSurfaceLookupFromMachPort` or
    // `IOSurfaceLookup` depending on transport.
    h.handle = surface_.valid() ? IOSurfaceGetID(surface_.get()) : 0;
    return h;
  }

  uint32_t glTextureId() override {
    // Lazy GL import (`CGLTexImageIOSurface2D`) goes here in a
    // follow-up — see file docstring. Phase-B-partial returns 0 so
    // the caller falls through to the CPU bitmap wire field.
    return 0;
  }

 private:
  IOSurfaceRetainer surface_;
  Vector2i dimensions_;
};

/// Backend-side consumer. Looks up the `IOSurfaceRef` from the
/// incoming `IOSurfaceID`; holds a retain so the surface outlives
/// the session's render frames.
class MacOSBackend final : public BridgeTextureBackend {
 public:
  explicit MacOSBackend(const BridgeTextureHandle& handle) : dimensions_(handle.dimensions) {
    if (handle.kind == BridgeHandleKind::kIOSurfaceMacOS && handle.handle != 0) {
      // `IOSurfaceLookup` only works when host + backend share the
      // same user session. Cross-session shares go through
      // `IOSurfaceLookupFromMachPort` — which needs a mach port we
      // don't yet carry over the wire. In-process (our current
      // `EditorBackendClient::InProcess` mode) uses the same
      // session, so `IOSurfaceLookup` is correct there.
      surface_ = IOSurfaceRetainer(IOSurfaceLookup(static_cast<IOSurfaceID>(handle.handle)));
    }
  }

  bool ready() const override {
    // Keep `false` until the wgpu-native → Metal import lands. The
    // surface is populated but we can't render into it yet without
    // the hal surface.
    return false;
  }

  Vector2i dimensions() const override { return dimensions_; }

  void bindAsRenderTarget() override {
    // TODO(bridge-phase-B-rest): wgpu-native hal surface →
    // `wgpuTextureCreateExternalFromIOSurface` or equivalent. Until
    // then this is a no-op and `ready()` stays false, forcing the
    // CPU fallback.
  }

  void waitRenderComplete() override {
    // TODO(bridge-phase-B-rest): submit fence + wait. No-op while
    // the GPU path isn't wired.
  }

  /// Phase-B-partial: expose the underlying surface so tests can
  /// verify the round-trip from `IOSurfaceID` → backend. Not in the
  /// public interface — internal helper only.
  [[nodiscard]] bool hasSurface() const { return surface_.valid(); }

 private:
  IOSurfaceRetainer surface_;
  Vector2i dimensions_;
};

}  // namespace

/// macOS host factory. Allocates a fresh RGBA8 `IOSurface` sized to
/// the requested dimensions. Returns the CPU-stub host as a safety
/// fallback if allocation fails (e.g. user session is
/// resource-starved) — the caller then falls through to the
/// `finalBitmapPixels` wire path.
std::unique_ptr<BridgeTextureHost> MakeHost_macOS(Vector2i dimensions) {
  if (dimensions.x <= 0 || dimensions.y <= 0) {
    return MakeHostStub(dimensions);
  }
  CFDictionaryRef props = MakeSurfaceProperties(dimensions.x, dimensions.y);
  IOSurfaceRef surface = IOSurfaceCreate(props);
  CFRelease(props);
  if (surface == nullptr) {
    return MakeHostStub(dimensions);
  }
  IOSurfaceRetainer retainer(surface);
  CFRelease(surface);  // retainer took its own ref
  return std::make_unique<MacOSHost>(std::move(retainer), dimensions);
}

/// macOS backend factory. Performs the `IOSurfaceLookup` round-trip
/// so the backend holds a retain on the same surface the host
/// allocated.
std::unique_ptr<BridgeTextureBackend> MakeBackend_macOS(const BridgeTextureHandle& handle) {
  return std::make_unique<MacOSBackend>(handle);
}

}  // namespace donner::editor::sandbox::bridge
