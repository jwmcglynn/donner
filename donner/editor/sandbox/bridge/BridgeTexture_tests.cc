/// @file
///
/// Phase-A tests for the cross-process texture bridge. The stub is
/// deliberately pass-through, so these tests are shape-gates rather
/// than behavior-gates — they lock in the handoff semantics so phase
/// B+ implementations can be added without breaking the interface.

#include "donner/editor/sandbox/bridge/BridgeTexture.h"

#include <gtest/gtest.h>

namespace donner::editor::sandbox::bridge {
namespace {

TEST(BridgeTextureStubTest, HostDescriptorCarriesDimensionsAndCpuStubKind) {
  auto host = MakeHostStub(Vector2i(640, 480));
  ASSERT_NE(host, nullptr);

  const BridgeTextureHandle handle = host->handle();
  EXPECT_EQ(handle.kind, BridgeHandleKind::kCpuStub)
      << "phase-A host must report `kCpuStub` so the session routes "
         "to the `finalBitmapPixels` wire fallback; a real platform "
         "kind here means phase B shipped without updating this "
         "test.";
  EXPECT_EQ(handle.handle, 0u);
  EXPECT_EQ(handle.dimensions.x, 640);
  EXPECT_EQ(handle.dimensions.y, 480);
}

TEST(BridgeTextureStubTest, BackendStubReportsNotReady) {
  auto host = MakeHostStub(Vector2i(100, 100));
  auto backend = MakeBackendStub(host->handle());
  ASSERT_NE(backend, nullptr);

  EXPECT_FALSE(backend->ready())
      << "phase-A backend stub must report `ready() == false` so the "
         "caller falls through to the CPU bitmap path. Returning true "
         "here would make the caller skip `finalBitmapPixels` and the "
         "user would see a black canvas.";
  EXPECT_EQ(backend->dimensions().x, 100);
  EXPECT_EQ(backend->dimensions().y, 100);

  // No-op methods must not crash.
  backend->bindAsRenderTarget();
  backend->waitRenderComplete();
}

TEST(BridgeTextureStubTest, HostGlTextureIdIsZeroWhileStub) {
  auto host = MakeHostStub(Vector2i(10, 10));
  EXPECT_EQ(host->glTextureId(), 0u)
      << "phase-A host stub must return `0` for the GL texture id so "
         "the consumer knows to use the `finalBitmapPixels`-uploaded "
         "texture instead.";
}

#if defined(__APPLE__)
// ---------------------------------------------------------------------------
// Phase-B macOS: IOSurface host + mach-port-less in-process round-trip.
// ---------------------------------------------------------------------------

TEST(BridgeTextureMacOSTest, HostAllocatesRealIOSurface) {
  auto host = MakeHost_macOS(Vector2i(256, 256));
  ASSERT_NE(host, nullptr);

  const BridgeTextureHandle h = host->handle();
  EXPECT_EQ(h.kind, BridgeHandleKind::kIOSurfaceMacOS)
      << "macOS factory should advertise `kIOSurfaceMacOS` — the CPU "
         "stub kind here means `IOSurfaceCreate` failed and the "
         "factory fell back silently.";
  EXPECT_NE(h.handle, 0u) << "real IOSurface must have a non-zero `IOSurfaceID`";
  EXPECT_EQ(h.dimensions, Vector2i(256, 256));
  EXPECT_GE(h.rowBytes, 256u * 4u) << "rowBytes < tight packing — IOSurface layout wrong";
}

TEST(BridgeTextureMacOSTest, BackendLooksUpHostAllocatedSurface) {
  auto host = MakeHost_macOS(Vector2i(64, 48));
  ASSERT_NE(host, nullptr);
  const BridgeTextureHandle h = host->handle();
  ASSERT_EQ(h.kind, BridgeHandleKind::kIOSurfaceMacOS);
  ASSERT_NE(h.handle, 0u);

  // Same-process lookup — uses `IOSurfaceLookup(IOSurfaceID)`. Cross-
  // process would use `IOSurfaceLookupFromMachPort` (phase-B-rest
  // wires the mach-port transport).
  auto backend = MakeBackend_macOS(h);
  ASSERT_NE(backend, nullptr);
  EXPECT_EQ(backend->dimensions(), Vector2i(64, 48));
  EXPECT_FALSE(backend->ready())
      << "backend must still report `!ready()` until wgpu-native "
         "Metal import + CGL texture bridge land; premature true "
         "here masks the CPU fallback path.";
}

TEST(BridgeTextureMacOSTest, BackendWithoutHandleIsInertButSafe) {
  BridgeTextureHandle h;
  h.kind = BridgeHandleKind::kIOSurfaceMacOS;
  h.handle = 0;  // no IOSurfaceID
  h.dimensions = Vector2i(16, 16);

  auto backend = MakeBackend_macOS(h);
  ASSERT_NE(backend, nullptr);
  EXPECT_FALSE(backend->ready());
  backend->bindAsRenderTarget();  // must not crash
  backend->waitRenderComplete();  // must not crash
}

TEST(BridgeTextureMacOSTest, ZeroDimensionsFallsBackToStub) {
  auto host = MakeHost_macOS(Vector2i::Zero());
  ASSERT_NE(host, nullptr);
  EXPECT_EQ(host->handle().kind, BridgeHandleKind::kCpuStub)
      << "zero-sized request must degrade to the CPU stub — trying "
         "to `IOSurfaceCreate` a 0×0 surface returns null, and a "
         "null `IOSurfaceRef` in the host would silently misbehave "
         "later.";
}
#endif

}  // namespace
}  // namespace donner::editor::sandbox::bridge
