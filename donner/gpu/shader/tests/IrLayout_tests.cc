/// @file
/// WGSL layout-rule tests, including the byte-layout anchors that the GPU runtime's C++ mirror
/// structs (Geode's Uniforms / Band / InstanceTransform) assert.

#include "donner/gpu/shader/IrLayout.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <vector>

#include "donner/gpu/shader/IrType.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::ElementsAre;
using testing::Eq;
using testing::Field;
using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

/// Matches a StructMemberLayout by offset.
auto OffsetIs(uint32_t offset) {
  return Field("offsetBytes", &StructMemberLayout::offsetBytes, Eq(offset));
}

TypeLayout LayoutOrFail(const IrType& type, AddressSpace addressSpace) {
  return GetShaderResultOrFail(ComputeTypeLayout(type, addressSpace));
}

StructLayout StructLayoutOrFail(const IrType& type, AddressSpace addressSpace) {
  ShaderResult<StructLayout> layout = ComputeStructLayout(type, addressSpace);
  if (layout.hasError()) {
    ADD_FAILURE() << "Expected a struct layout, got error: " << layout.error();
    return StructLayout{};
  }
  return std::move(layout).result();
}

TEST(IrLayoutTests, ScalarAndVectorLayouts) {
  EXPECT_THAT(LayoutOrFail(IrType::F32(), AddressSpace::Storage), Eq(TypeLayout{4, 4}));
  EXPECT_THAT(LayoutOrFail(IrType::I32(), AddressSpace::Storage), Eq(TypeLayout{4, 4}));
  EXPECT_THAT(LayoutOrFail(IrType::U32(), AddressSpace::Uniform), Eq(TypeLayout{4, 4}));
  EXPECT_THAT(LayoutOrFail(IrType::Vec2f(), AddressSpace::Storage), Eq(TypeLayout{8, 8}));
  EXPECT_THAT(LayoutOrFail(IrType::Vec3f(), AddressSpace::Storage), Eq(TypeLayout{16, 12}));
  EXPECT_THAT(LayoutOrFail(IrType::Vec4f(), AddressSpace::Storage), Eq(TypeLayout{16, 16}));
  EXPECT_THAT(LayoutOrFail(IrType::Mat4x4f(), AddressSpace::Storage), Eq(TypeLayout{16, 64}));
}

TEST(IrLayoutTests, NonHostShareableTypesHaveNoLayout) {
  EXPECT_THAT(ComputeTypeLayout(IrType::Bool(), AddressSpace::Storage),
              IsShaderError(HasSubstr("bool is not host-shareable")));
  EXPECT_THAT(ComputeTypeLayout(IrType::Vec2(ScalarKind::Bool), AddressSpace::Storage),
              IsShaderError(HasSubstr("vectors of bool")));
  EXPECT_THAT(ComputeTypeLayout(IrType::Texture2dF32(), AddressSpace::Storage),
              IsShaderError(HasSubstr("resource type")));
  EXPECT_THAT(ComputeTypeLayout(IrType::SamplerType(), AddressSpace::Storage),
              IsShaderError(HasSubstr("resource type")));
}

TEST(IrLayoutTests, Vec3PaddingInsideStruct) {
  const IrType structType = GetShaderResultOrFail(
      IrType::Struct("Padded", {{"a", IrType::Vec3f()}, {"b", IrType::F32()}}), IrType::F32());

  const StructLayout layout = StructLayoutOrFail(structType, AddressSpace::Storage);
  EXPECT_THAT(layout.alignBytes, Eq(16u));
  // The f32 packs into the vec3's tail padding at offset 12; the struct rounds to 16.
  EXPECT_THAT(layout.sizeBytes, Eq(16u));
  EXPECT_THAT(layout.members, ElementsAre(OffsetIs(0), OffsetIs(12)));
}

TEST(IrLayoutTests, ArrayStrideStorageVsUniform) {
  const IrType arrayF32 =
      GetShaderResultOrFail(IrType::SizedArray(IrType::F32(), 4), IrType::F32());
  const IrType arrayVec2 =
      GetShaderResultOrFail(IrType::SizedArray(IrType::Vec2f(), 2), IrType::F32());

  // Storage: stride = roundUp(align, size) of the element.
  EXPECT_THAT(ComputeArrayStride(arrayF32, AddressSpace::Storage), HasShaderResult());
  EXPECT_THAT(GetShaderResultOrFail(ComputeArrayStride(arrayF32, AddressSpace::Storage)), Eq(4u));
  EXPECT_THAT(GetShaderResultOrFail(ComputeArrayStride(arrayVec2, AddressSpace::Storage)), Eq(8u));
  EXPECT_THAT(LayoutOrFail(arrayF32, AddressSpace::Storage), Eq(TypeLayout{4, 16}));

  // Uniform: element stride rounds up to a multiple of 16. This is a deliberate policy: WGSL
  // validation would reject the natural stride (there is no stride attribute), so the layout
  // engine defines the rounded stride and emitters must materialize padded element wrappers
  // when ArrayStrideInfo::paddedFromNatural is set.
  EXPECT_THAT(GetShaderResultOrFail(ComputeArrayStride(arrayF32, AddressSpace::Uniform)), Eq(16u));
  EXPECT_THAT(GetShaderResultOrFail(ComputeArrayStride(arrayVec2, AddressSpace::Uniform)), Eq(16u));
  EXPECT_THAT(LayoutOrFail(arrayF32, AddressSpace::Uniform), Eq(TypeLayout{16, 64}));
}

TEST(IrLayoutTests, ArrayStrideInfoReportsUniformPadding) {
  const IrType arrayF32 =
      GetShaderResultOrFail(IrType::SizedArray(IrType::F32(), 4), IrType::F32());
  const IrType arrayVec4 =
      GetShaderResultOrFail(IrType::SizedArray(IrType::Vec4f(), 4), IrType::F32());

  // array<f32, 4> in uniform is padded (natural stride 4 -> 16); the emitter obligation flag is
  // set so packet 5 wraps the element type.
  EXPECT_THAT(GetShaderResultOrFail(ComputeArrayStrideInfo(arrayF32, AddressSpace::Uniform)),
              Eq(ArrayStrideInfo{16, true}));
  EXPECT_THAT(GetShaderResultOrFail(ComputeArrayStrideInfo(arrayF32, AddressSpace::Storage)),
              Eq(ArrayStrideInfo{4, false}));
  // slug_fill's clipPolygonPlanes array<vec4f, 4> has a natural stride of 16: no padding needed.
  EXPECT_THAT(GetShaderResultOrFail(ComputeArrayStrideInfo(arrayVec4, AddressSpace::Uniform)),
              Eq(ArrayStrideInfo{16, false}));
}

TEST(IrLayoutTests, RuntimeArrayHasStrideButNoSize) {
  const IrType runtimeArray =
      GetShaderResultOrFail(IrType::RuntimeArray(IrType::F32()), IrType::F32());

  EXPECT_THAT(GetShaderResultOrFail(ComputeArrayStride(runtimeArray, AddressSpace::Storage)),
              Eq(4u));
  EXPECT_THAT(ComputeTypeLayout(runtimeArray, AddressSpace::Storage),
              IsShaderError(HasSubstr("no fixed size")));
}

TEST(IrLayoutTests, NestedStructUniformGets16ByteAlignmentAndFollowerRounding) {
  const IrType inner =
      GetShaderResultOrFail(IrType::Struct("Inner", {{"x", IrType::F32()}}), IrType::F32());
  const IrType outer = GetShaderResultOrFail(
      IrType::Struct("Outer", {{"a", IrType::F32()}, {"b", inner}, {"c", IrType::F32()}}),
      IrType::F32());

  // Storage: Inner is align 4 size 4; members pack tightly.
  const StructLayout storageLayout = StructLayoutOrFail(outer, AddressSpace::Storage);
  EXPECT_THAT(storageLayout.members, ElementsAre(OffsetIs(0), OffsetIs(4), OffsetIs(8)));
  EXPECT_THAT(storageLayout.sizeBytes, Eq(12u));

  // Uniform: the nested struct member aligns to 16 AND the member following a struct-typed
  // member must start at least roundUp(16, SizeOf(Inner)) bytes after its start (WGSL uniform
  // layout constraint), so c lands at 32, not 20.
  const StructLayout uniformLayout = StructLayoutOrFail(outer, AddressSpace::Uniform);
  EXPECT_THAT(uniformLayout.members, ElementsAre(OffsetIs(0), OffsetIs(16), OffsetIs(32)));
  EXPECT_THAT(uniformLayout.alignBytes, Eq(16u));
  EXPECT_THAT(uniformLayout.sizeBytes, Eq(48u));
}

TEST(IrLayoutTests, UniformFollowerOfLargerNestedStructRoundsToNext16) {
  // Inner3 is 12 bytes (three f32, align 4): its size is not a multiple of 16, so in uniform
  // space the follower advances by roundUp(16, 12) = 16 even though the follower itself only
  // needs 4-byte alignment.
  const IrType inner = GetShaderResultOrFail(
      IrType::Struct("Inner3", {{"x", IrType::F32()}, {"y", IrType::F32()}, {"z", IrType::F32()}}),
      IrType::F32());
  const IrType outer = GetShaderResultOrFail(
      IrType::Struct("Outer3", {{"a", inner}, {"b", IrType::F32()}}), IrType::F32());

  const StructLayout storageLayout = StructLayoutOrFail(outer, AddressSpace::Storage);
  EXPECT_THAT(storageLayout.members, ElementsAre(OffsetIs(0), OffsetIs(12)));
  EXPECT_THAT(storageLayout.sizeBytes, Eq(16u));

  const StructLayout uniformLayout = StructLayoutOrFail(outer, AddressSpace::Uniform);
  EXPECT_THAT(uniformLayout.members, ElementsAre(OffsetIs(0), OffsetIs(16)));
  EXPECT_THAT(uniformLayout.alignBytes, Eq(16u));
  EXPECT_THAT(uniformLayout.sizeBytes, Eq(32u));
}

// == Anchor structs ===========================================================================
// These three struct layouts must equal the byte layouts the Geode encoder's C++ mirror structs
// assert (GeoEncoder.cc): Uniforms == 288 bytes with the commented field offsets, Band == 32
// bytes, InstanceTransform == 32 bytes. The types are constructed here through the IR type API
// and laid out by the WGSL rules.

/// Builds the slug_fill `Uniforms` struct via the IR type API.
IrType MakeSlugFillUniforms() {
  const IrType planesArray =
      GetShaderResultOrFail(IrType::SizedArray(IrType::Vec4f(), 4), IrType::F32());
  return GetShaderResultOrFail(
      IrType::Struct("Uniforms",
                     {
                         {"mvp", IrType::Mat4x4f()},        {"patternFromPath", IrType::Mat4x4f()},
                         {"viewport", IrType::Vec2f()},     {"tileSize", IrType::Vec2f()},
                         {"color", IrType::Vec4f()},        {"fillRule", IrType::U32()},
                         {"paintMode", IrType::U32()},      {"patternOpacity", IrType::F32()},
                         {"hasClipPolygon", IrType::U32()}, {"hasClipMask", IrType::U32()},
                         {"_pad0", IrType::U32()},          {"_pad1", IrType::U32()},
                         {"_pad2", IrType::U32()},          {"yBase", IrType::F32()},
                         {"hStride", IrType::F32()},        {"hBandCount", IrType::U32()},
                         {"xBase", IrType::F32()},          {"vStride", IrType::F32()},
                         {"vBandCount", IrType::U32()},     {"_gridPad0", IrType::U32()},
                         {"_gridPad1", IrType::U32()},      {"clipPolygonPlanes", planesArray},
                     }),
      IrType::F32());
}

/// Builds the slug_fill `Band` struct via the IR type API.
IrType MakeSlugFillBand() {
  return GetShaderResultOrFail(IrType::Struct("Band",
                                              {
                                                  {"curveStart", IrType::U32()},
                                                  {"curveCount", IrType::U32()},
                                                  {"yMin", IrType::F32()},
                                                  {"yMax", IrType::F32()},
                                                  {"xMin", IrType::F32()},
                                                  {"xMax", IrType::F32()},
                                                  {"_pad0", IrType::F32()},
                                                  {"_pad1", IrType::F32()},
                                              }),
                               IrType::F32());
}

/// Builds the slug_fill `InstanceTransform` struct via the IR type API.
IrType MakeInstanceTransform() {
  return GetShaderResultOrFail(
      IrType::Struct("InstanceTransform", {{"row0", IrType::Vec4f()}, {"row1", IrType::Vec4f()}}),
      IrType::F32());
}

TEST(IrLayoutAnchorTests, SlugFillUniformsIs288BytesWithEncoderOffsets) {
  const StructLayout layout = StructLayoutOrFail(MakeSlugFillUniforms(), AddressSpace::Uniform);

  EXPECT_THAT(layout.sizeBytes, Eq(288u));
  EXPECT_THAT(layout.alignBytes, Eq(16u));
  // Offsets from the C++ mirror struct comments in GeoEncoder.cc.
  EXPECT_THAT(layout.members,
              ElementsAre(OffsetIs(0),      // mvp
                          OffsetIs(64),     // patternFromPath
                          OffsetIs(128),    // viewport
                          OffsetIs(136),    // tileSize
                          OffsetIs(144),    // color
                          OffsetIs(160),    // fillRule
                          OffsetIs(164),    // paintMode
                          OffsetIs(168),    // patternOpacity
                          OffsetIs(172),    // hasClipPolygon
                          OffsetIs(176),    // hasClipMask
                          OffsetIs(180),    // _pad0
                          OffsetIs(184),    // _pad1
                          OffsetIs(188),    // _pad2
                          OffsetIs(192),    // yBase
                          OffsetIs(196),    // hStride
                          OffsetIs(200),    // hBandCount
                          OffsetIs(204),    // xBase
                          OffsetIs(208),    // vStride
                          OffsetIs(212),    // vBandCount
                          OffsetIs(216),    // _gridPad0
                          OffsetIs(220),    // _gridPad1
                          OffsetIs(224)));  // clipPolygonPlanes (224 .. 288)
}

TEST(IrLayoutAnchorTests, SlugFillBandIs32Bytes) {
  const StructLayout layout = StructLayoutOrFail(MakeSlugFillBand(), AddressSpace::Storage);

  EXPECT_THAT(layout.sizeBytes, Eq(32u));
  EXPECT_THAT(layout.alignBytes, Eq(4u));
  EXPECT_THAT(layout.members, ElementsAre(OffsetIs(0), OffsetIs(4), OffsetIs(8), OffsetIs(12),
                                          OffsetIs(16), OffsetIs(20), OffsetIs(24), OffsetIs(28)));

  // As a storage runtime array element, the stride equals the struct size.
  const IrType bandArray =
      GetShaderResultOrFail(IrType::RuntimeArray(MakeSlugFillBand()), IrType::F32());
  EXPECT_THAT(GetShaderResultOrFail(ComputeArrayStride(bandArray, AddressSpace::Storage)), Eq(32u));
}

TEST(IrLayoutAnchorTests, InstanceTransformIs32Bytes) {
  const StructLayout layout = StructLayoutOrFail(MakeInstanceTransform(), AddressSpace::Storage);

  EXPECT_THAT(layout.sizeBytes, Eq(32u));
  EXPECT_THAT(layout.alignBytes, Eq(16u));
  EXPECT_THAT(layout.members, ElementsAre(OffsetIs(0), OffsetIs(16)));
}

}  // namespace
}  // namespace donner::gpu::shader
