#include "donner/editor/sandbox/bridge/BridgeTexture.h"

#include "donner/base/Vector2.h"

/// @file
///
/// Phase-A CPU-stub implementation. Always reports `ready() == false`
/// so callers fall through to the existing `FramePayload::finalBitmap
/// Pixels` wire path. The point of shipping this is that the protocol
/// opcode + handoff shape + host/backend handshake can be exercised
/// by tests before phase B adds platform-specific texture imports.

namespace donner::editor::sandbox::bridge {

namespace {

class StubHost final : public BridgeTextureHost {
public:
  explicit StubHost(Vector2i dimensions) : dimensions_(dimensions) {}

  BridgeTextureHandle handle() const override {
    BridgeTextureHandle h;
    h.kind = BridgeHandleKind::kCpuStub;
    h.handle = 0;
    h.dimensions = dimensions_;
    h.rowBytes = 0;
    return h;
  }

  uint32_t glTextureId() override { return 0; }

private:
  Vector2i dimensions_;
};

class StubBackend final : public BridgeTextureBackend {
public:
  explicit StubBackend(const BridgeTextureHandle& handle) : dimensions_(handle.dimensions) {}

  bool ready() const override { return false; }
  Vector2i dimensions() const override { return dimensions_; }
  void bindAsRenderTarget() override {}
  void waitRenderComplete() override {}

private:
  Vector2i dimensions_;
};

}  // namespace

std::unique_ptr<BridgeTextureHost> MakeHostStub(Vector2i dimensions) {
  return std::make_unique<StubHost>(dimensions);
}

std::unique_ptr<BridgeTextureBackend> MakeBackendStub(const BridgeTextureHandle& handle) {
  return std::make_unique<StubBackend>(handle);
}

}  // namespace donner::editor::sandbox::bridge
