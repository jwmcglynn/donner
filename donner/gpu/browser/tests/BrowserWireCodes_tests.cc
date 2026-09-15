#include "donner/gpu/browser/BrowserWireCodes.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "donner/base/tests/Runfiles.h"

namespace donner::gpu::browser {

using testing::ElementsAre;
using testing::ElementsAreArray;
using testing::Eq;
using testing::Optional;

// These expectations are the protocol, not a restatement of the implementation:
// `library_donner_gpu.js` holds the same numbers, so changing one of them here without changing it
// there breaks the browser side silently. Pinning each value makes that a test failure instead.

namespace {

/// Reads the `protocolCodes` array out of `library_donner_gpu.js`.
///
/// The library is JavaScript, so there is nothing to link against and nothing a C++ compiler can
/// check; reading its text is what lets this test compare the two tables. The array is a flat list
/// of integers, so the parse is deliberately blunt: find the array, drop line comments, and take
/// every run of digits. Anything it cannot find comes back empty and the test says so rather than
/// comparing against nothing.
std::vector<uint32_t> ReadLibraryProtocolCodes() {
  const std::string path =
      Runfiles::instance().Rlocation("donner/gpu/browser/library_donner_gpu.js");
  std::ifstream input(path);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  const std::string source = buffer.str();

  const size_t start = source.find("protocolCodes: [");
  if (start == std::string::npos) {
    return {};
  }
  const size_t open = source.find('[', start);
  const size_t close = source.find(']', open);
  if (open == std::string::npos || close == std::string::npos) {
    return {};
  }

  std::vector<uint32_t> codes;
  std::istringstream lines(source.substr(open + 1, close - open - 1));
  std::string line;
  while (std::getline(lines, line)) {
    const size_t comment = line.find("//");
    if (comment != std::string::npos) {
      line.erase(comment);
    }
    size_t index = 0;
    while (index < line.size()) {
      if (std::isdigit(static_cast<unsigned char>(line[index])) == 0) {
        ++index;
        continue;
      }
      size_t end = index;
      while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end])) != 0) {
        ++end;
      }
      codes.push_back(static_cast<uint32_t>(std::stoul(line.substr(index, end - index))));
      index = end;
    }
  }
  return codes;
}

}  // namespace

TEST(BrowserWireCodes, EncodesEveryTextureFormat) {
  EXPECT_THAT(WireTextureFormat(TextureFormat::RGBA8Unorm), Optional(Eq(1u)));
  EXPECT_THAT(WireTextureFormat(TextureFormat::BGRA8Unorm), Optional(Eq(2u)));
  EXPECT_THAT(WireTextureFormat(TextureFormat::R8Unorm), Optional(Eq(3u)));
  EXPECT_THAT(WireTextureFormat(TextureFormat::RGBA32Float), Optional(Eq(4u)));
}

TEST(BrowserWireCodes, RefusesAnEnumeratorThisProtocolHasNoNameFor) {
  EXPECT_THAT(WireTextureFormat(static_cast<TextureFormat>(99)), Eq(std::nullopt));
  EXPECT_THAT(WireBindingType(static_cast<BindingType>(99)), Eq(std::nullopt));
  EXPECT_THAT(WirePresentMode(static_cast<PresentMode>(99)), Eq(std::nullopt));
  EXPECT_THAT(WireSurfaceAlphaMode(static_cast<SurfaceAlphaMode>(99)), Eq(std::nullopt));
}

TEST(BrowserWireCodes, EncodesAnEmptyMaskAsNoFlags) {
  EXPECT_THAT(WireTextureUsage(TextureUsage::None), Optional(Eq(0u)));
  EXPECT_THAT(WireBufferUsage(BufferUsage::None), Optional(Eq(0u)));
  EXPECT_THAT(WireShaderStage(ShaderStage::None), Optional(Eq(0u)));
  EXPECT_THAT(WireColorWriteMask(ColorWriteMask::None), Optional(Eq(0u)));
}

TEST(BrowserWireCodes, EncodesCombinedFlags) {
  EXPECT_THAT(WireTextureUsage(TextureUsage::RenderAttachment | TextureUsage::CopySrc),
              Optional(Eq(0b00101u)));
  EXPECT_THAT(WireBufferUsage(BufferUsage::Index | BufferUsage::MapRead), Optional(Eq(0b1000010u)));
  EXPECT_THAT(WireShaderStage(ShaderStage::Vertex | ShaderStage::Fragment), Optional(Eq(0b11u)));
  EXPECT_THAT(WireColorWriteMask(ColorWriteMask::All), Optional(Eq(0b1111u)));
}

TEST(BrowserWireCodes, RefusesAMaskCarryingABitThisProtocolHasNoNameFor) {
  EXPECT_THAT(WireTextureUsage(static_cast<TextureUsage>(1u << 20)), Eq(std::nullopt));
  EXPECT_THAT(WireBufferUsage(static_cast<BufferUsage>(1u << 20)), Eq(std::nullopt));
  EXPECT_THAT(WireShaderStage(static_cast<ShaderStage>(1u << 20)), Eq(std::nullopt));
  EXPECT_THAT(WireColorWriteMask(static_cast<ColorWriteMask>(1u << 20)), Eq(std::nullopt));

  // A known flag alongside an unknown one is still refused: encoding the recognized half would
  // hand the browser a mask that means something narrower than the caller asked for.
  EXPECT_THAT(WireTextureUsage(TextureUsage::Sampled | static_cast<TextureUsage>(1u << 20)),
              Eq(std::nullopt));
}

TEST(BrowserWireCodes, EncodesTheRemainingDescriptorEnumerations) {
  EXPECT_THAT(WireFilterMode(FilterMode::Nearest), Optional(Eq(1u)));
  EXPECT_THAT(WireFilterMode(FilterMode::Linear), Optional(Eq(2u)));
  EXPECT_THAT(WireAddressMode(AddressMode::ClampToEdge), Optional(Eq(1u)));
  EXPECT_THAT(WireAddressMode(AddressMode::Repeat), Optional(Eq(2u)));
  EXPECT_THAT(WireVertexFormat(VertexFormat::Float32x2), Optional(Eq(1u)));
  EXPECT_THAT(WireVertexFormat(VertexFormat::Float32x4), Optional(Eq(2u)));
  EXPECT_THAT(WireVertexFormat(VertexFormat::Uint32), Optional(Eq(3u)));
  EXPECT_THAT(WireVertexStepMode(VertexStepMode::Vertex), Optional(Eq(1u)));
  EXPECT_THAT(WireVertexStepMode(VertexStepMode::Instance), Optional(Eq(2u)));
  EXPECT_THAT(WireIndexFormat(IndexFormat::Uint16), Optional(Eq(1u)));
  EXPECT_THAT(WireIndexFormat(IndexFormat::Uint32), Optional(Eq(2u)));
  EXPECT_THAT(WirePrimitiveTopology(PrimitiveTopology::TriangleList), Optional(Eq(1u)));
  EXPECT_THAT(WirePrimitiveTopology(PrimitiveTopology::TriangleStrip), Optional(Eq(2u)));
  EXPECT_THAT(WireCullMode(CullMode::None), Optional(Eq(1u)));
  EXPECT_THAT(WireCullMode(CullMode::Back), Optional(Eq(2u)));
  EXPECT_THAT(WireBlendFactor(BlendFactor::Zero), Optional(Eq(1u)));
  EXPECT_THAT(WireBlendFactor(BlendFactor::One), Optional(Eq(2u)));
  EXPECT_THAT(WireBlendFactor(BlendFactor::SrcAlpha), Optional(Eq(3u)));
  EXPECT_THAT(WireBlendFactor(BlendFactor::OneMinusSrcAlpha), Optional(Eq(4u)));
  EXPECT_THAT(WireBlendFactor(BlendFactor::OneMinusDstAlpha), Optional(Eq(5u)));
  EXPECT_THAT(WireBlendOperation(BlendOperation::Add), Optional(Eq(1u)));
  EXPECT_THAT(WireBlendOperation(BlendOperation::Max), Optional(Eq(2u)));
  EXPECT_THAT(WireBindingType(BindingType::UniformBuffer), Optional(Eq(1u)));
  EXPECT_THAT(WireBindingType(BindingType::ReadOnlyStorageBuffer), Optional(Eq(2u)));
  EXPECT_THAT(WireBindingType(BindingType::SampledTexture2dFloat), Optional(Eq(3u)));
  EXPECT_THAT(WireBindingType(BindingType::FilteringSampler), Optional(Eq(4u)));
  EXPECT_THAT(WireBindingType(BindingType::WriteOnlyStorageTexture2d), Optional(Eq(5u)));
  EXPECT_THAT(WireBindingType(BindingType::SampledTexture2dUnfilterableFloat), Optional(Eq(6u)));
  EXPECT_THAT(WireLoadOp(LoadOp::Clear), Optional(Eq(1u)));
  EXPECT_THAT(WireLoadOp(LoadOp::Load), Optional(Eq(2u)));
  EXPECT_THAT(WireStoreOp(StoreOp::Store), Optional(Eq(1u)));
  EXPECT_THAT(WireStoreOp(StoreOp::Discard), Optional(Eq(2u)));
  EXPECT_THAT(WirePresentMode(PresentMode::Fifo), Optional(Eq(1u)));
  EXPECT_THAT(WirePresentMode(PresentMode::Immediate), Optional(Eq(2u)));
  EXPECT_THAT(WirePresentMode(PresentMode::Mailbox), Optional(Eq(3u)));
  EXPECT_THAT(WireSurfaceAlphaMode(SurfaceAlphaMode::Opaque), Optional(Eq(1u)));
  EXPECT_THAT(WireSurfaceAlphaMode(SurfaceAlphaMode::Premultiplied), Optional(Eq(2u)));
  EXPECT_THAT(WireSurfaceAlphaMode(SurfaceAlphaMode::Inherit), Optional(Eq(3u)));
}

TEST(BrowserWireCodes, EncodesEveryBrowserObjectKind) {
  EXPECT_THAT(WireBrowserObjectKind(BrowserObjectKind::Buffer), Optional(Eq(1u)));
  EXPECT_THAT(WireBrowserObjectKind(BrowserObjectKind::Texture), Optional(Eq(2u)));
  EXPECT_THAT(WireBrowserObjectKind(BrowserObjectKind::TextureView), Optional(Eq(3u)));
  EXPECT_THAT(WireBrowserObjectKind(BrowserObjectKind::Sampler), Optional(Eq(4u)));
  EXPECT_THAT(WireBrowserObjectKind(BrowserObjectKind::BindGroupLayout), Optional(Eq(5u)));
  EXPECT_THAT(WireBrowserObjectKind(BrowserObjectKind::BindGroup), Optional(Eq(6u)));
  EXPECT_THAT(WireBrowserObjectKind(BrowserObjectKind::PipelineLayout), Optional(Eq(7u)));
  EXPECT_THAT(WireBrowserObjectKind(BrowserObjectKind::ShaderModule), Optional(Eq(8u)));
  EXPECT_THAT(WireBrowserObjectKind(BrowserObjectKind::RenderPipeline), Optional(Eq(9u)));
  EXPECT_THAT(WireBrowserObjectKind(BrowserObjectKind::ComputePipeline), Optional(Eq(10u)));
  EXPECT_THAT(WireBrowserObjectKind(BrowserObjectKind::Surface), Optional(Eq(11u)));
  EXPECT_THAT(WireBrowserObjectKind(BrowserObjectKind::BufferMapping), Optional(Eq(12u)));

  // The sentinel is a count, not a kind, and encoding it would name a thirteenth object kind.
  EXPECT_THAT(WireBrowserObjectKind(BrowserObjectKind::kCount), Eq(std::nullopt));
}

TEST(BrowserWireCodes, DecodesTheStatusesAndOutcomesTheBrowserReports) {
  EXPECT_THAT(BridgeStatusFromWire(1), Optional(Eq(BridgeStatus::Success)));
  EXPECT_THAT(BridgeStatusFromWire(6), Optional(Eq(BridgeStatus::Failed)));
  EXPECT_THAT(RequestStateFromWire(2), Optional(Eq(BrowserDeviceRequestState::Ready)));
  EXPECT_THAT(MapSliceStateFromWire(3), Optional(Eq(MapSliceState::DeviceLost)));
  EXPECT_THAT(SurfaceStatusFromWire(5), Optional(Eq(SurfaceStatus::Timeout)));
}

TEST(BrowserWireCodes, RefusesAStatusCodeThisProtocolAssignsNoMeaning) {
  // Zero is never assigned, so a value that arrives unset decodes to nothing rather than to the
  // first enumerator.
  EXPECT_THAT(BridgeStatusFromWire(0), Eq(std::nullopt));
  EXPECT_THAT(RequestStateFromWire(0), Eq(std::nullopt));
  EXPECT_THAT(MapSliceStateFromWire(0), Eq(std::nullopt));
  EXPECT_THAT(SurfaceStatusFromWire(0), Eq(std::nullopt));

  EXPECT_THAT(BridgeStatusFromWire(7), Eq(std::nullopt));
  EXPECT_THAT(RequestStateFromWire(5), Eq(std::nullopt));
  EXPECT_THAT(MapSliceStateFromWire(5), Eq(std::nullopt));
  EXPECT_THAT(SurfaceStatusFromWire(6), Eq(std::nullopt));
}

TEST(BrowserWireCodes, DecodesEveryCodeToTheValueTheProtocolAssignsIt) {
  // All nineteen decoded mappings, written out rather than walked, so transposing two cases in one
  // of the translations fails here. A transposition is the failure worth catching: every code still
  // decodes, so a check that only asked whether it decodes would pass while a caller took, say, the
  // new-surface recovery for a lost device.
  EXPECT_THAT(BridgeStatusFromWire(1), Optional(Eq(BridgeStatus::Success)));
  EXPECT_THAT(BridgeStatusFromWire(2), Optional(Eq(BridgeStatus::UnknownObject)));
  EXPECT_THAT(BridgeStatusFromWire(3), Optional(Eq(BridgeStatus::WrongObjectKind)));
  EXPECT_THAT(BridgeStatusFromWire(4), Optional(Eq(BridgeStatus::NotOwner)));
  EXPECT_THAT(BridgeStatusFromWire(5), Optional(Eq(BridgeStatus::DeviceLost)));
  EXPECT_THAT(BridgeStatusFromWire(6), Optional(Eq(BridgeStatus::Failed)));

  EXPECT_THAT(RequestStateFromWire(1), Optional(Eq(BrowserDeviceRequestState::Pending)));
  EXPECT_THAT(RequestStateFromWire(2), Optional(Eq(BrowserDeviceRequestState::Ready)));
  EXPECT_THAT(RequestStateFromWire(3), Optional(Eq(BrowserDeviceRequestState::Unavailable)));
  EXPECT_THAT(RequestStateFromWire(4), Optional(Eq(BrowserDeviceRequestState::Failed)));

  EXPECT_THAT(MapSliceStateFromWire(1), Optional(Eq(MapSliceState::Pending)));
  EXPECT_THAT(MapSliceStateFromWire(2), Optional(Eq(MapSliceState::Ready)));
  EXPECT_THAT(MapSliceStateFromWire(3), Optional(Eq(MapSliceState::DeviceLost)));
  EXPECT_THAT(MapSliceStateFromWire(4), Optional(Eq(MapSliceState::Failed)));

  EXPECT_THAT(SurfaceStatusFromWire(1), Optional(Eq(SurfaceStatus::Success)));
  EXPECT_THAT(SurfaceStatusFromWire(2), Optional(Eq(SurfaceStatus::Outdated)));
  EXPECT_THAT(SurfaceStatusFromWire(3), Optional(Eq(SurfaceStatus::Lost)));
  EXPECT_THAT(SurfaceStatusFromWire(4), Optional(Eq(SurfaceStatus::DeviceLost)));
  EXPECT_THAT(SurfaceStatusFromWire(5), Optional(Eq(SurfaceStatus::Timeout)));
}

TEST(BrowserWireCodes, ProtocolTableMatchesTheOneTheJavaScriptLibraryHolds) {
  const std::span<const uint32_t> table = ProtocolCodeTable();

  // No entry is zero: zero is what an unassigned value contributes, so one here would mean
  // something lost its code and the table no longer describes what is sent.
  EXPECT_THAT(std::count(table.begin(), table.end(), 0u), 0);

  // The comparison the bridge performs at runtime, performed here against the library's own text,
  // so the two tables are held together at build time rather than first at a browser.
  const std::vector<uint32_t> library = ReadLibraryProtocolCodes();
  ASSERT_THAT(library, testing::Not(testing::IsEmpty()))
      << "could not read protocolCodes out of library_donner_gpu.js";
  EXPECT_THAT(library, ElementsAreArray(table));
}

}  // namespace donner::gpu::browser
