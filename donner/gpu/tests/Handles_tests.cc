/// @file
/// Tests for \ref donner::gpu::Handle and \ref donner::gpu::HandleRef.

#include "donner/gpu/Handles.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <utility>

using testing::Eq;
using testing::HasSubstr;

namespace donner::gpu {

TEST(HandlesTests, DefaultConstructedIsNull) {
  const Buffer buffer;
  EXPECT_FALSE(buffer.isValid());
  EXPECT_EQ(buffer, nullptr);
  EXPECT_THAT(buffer.generation(), Eq(0u));
  EXPECT_THAT(buffer.deviceId(), Eq(0u));
}

TEST(HandlesTests, CreateForBackendIsValid) {
  const Buffer buffer = Buffer::CreateForBackend(3, 2, 77);
  EXPECT_TRUE(buffer.isValid());
  EXPECT_NE(buffer, nullptr);
  EXPECT_THAT(buffer.slotIndex(), Eq(3u));
  EXPECT_THAT(buffer.generation(), Eq(2u));
  EXPECT_THAT(buffer.deviceId(), Eq(77u));
}

TEST(HandlesTests, MoveConstructionTransfersAndNullsSource) {
  Buffer original = Buffer::CreateForBackend(1, 5, 9);
  const Buffer moved(std::move(original));

  EXPECT_THAT(moved.slotIndex(), Eq(1u));
  EXPECT_THAT(moved.generation(), Eq(5u));
  EXPECT_THAT(moved.deviceId(), Eq(9u));
  EXPECT_EQ(original, nullptr);  // NOLINT(bugprone-use-after-move): moved-from state is the API.
}

TEST(HandlesTests, MoveAssignmentTransfersAndNullsSource) {
  Buffer original = Buffer::CreateForBackend(4, 6, 2);
  Buffer target;
  target = std::move(original);

  EXPECT_THAT(target.slotIndex(), Eq(4u));
  EXPECT_THAT(target.generation(), Eq(6u));
  EXPECT_EQ(original, nullptr);  // NOLINT(bugprone-use-after-move): moved-from state is the API.
}

TEST(HandlesTests, HandleRefCapturesIdentity) {
  const Texture texture = Texture::CreateForBackend(2, 3, 4);
  const TextureRef ref = texture;

  EXPECT_TRUE(ref.isValid());
  EXPECT_THAT(ref.slotIndex(), Eq(texture.slotIndex()));
  EXPECT_THAT(ref.generation(), Eq(texture.generation()));
  EXPECT_THAT(ref.deviceId(), Eq(texture.deviceId()));

  const TextureRef nullRef;
  EXPECT_EQ(nullRef, nullptr);
}

TEST(HandlesTests, PrintToNamesResource) {
  EXPECT_THAT(testing::PrintToString(Buffer::CreateForBackend(0, 1, 1)), HasSubstr("buffer#0@1"));
  EXPECT_THAT(testing::PrintToString(Buffer()), HasSubstr("buffer(null)"));
  EXPECT_THAT(testing::PrintToString(TextureViewRef()), HasSubstr("textureView(null)"));
}

}  // namespace donner::gpu
