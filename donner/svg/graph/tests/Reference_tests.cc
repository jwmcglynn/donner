#include "donner/svg/graph/Reference.h"

#include <gtest/gtest.h>

namespace donner::svg {
namespace {

TEST(ReferenceTest, SameDocumentFragment) {
  Reference ref("#myId");
  EXPECT_FALSE(ref.isExternal());
  EXPECT_EQ(ref.documentUrl(), "");
  EXPECT_EQ(ref.fragment(), "myId");
}

TEST(ReferenceTest, ExternalWithFragment) {
  Reference ref("file.svg#elementId");
  EXPECT_TRUE(ref.isExternal());
  EXPECT_EQ(ref.documentUrl(), "file.svg");
  EXPECT_EQ(ref.fragment(), "elementId");
}

TEST(ReferenceTest, ExternalWithoutFragment) {
  Reference ref("file.svg");
  EXPECT_TRUE(ref.isExternal());
  EXPECT_EQ(ref.documentUrl(), "file.svg");
  EXPECT_EQ(ref.fragment(), "");
}

TEST(ReferenceTest, ExternalWithPath) {
  Reference ref("path/to/file.svg#rect1");
  EXPECT_TRUE(ref.isExternal());
  EXPECT_EQ(ref.documentUrl(), "path/to/file.svg");
  EXPECT_EQ(ref.fragment(), "rect1");
}

TEST(ReferenceTest, EmptyHref) {
  Reference ref("");
  EXPECT_FALSE(ref.isExternal());
  EXPECT_EQ(ref.documentUrl(), "");
  EXPECT_EQ(ref.fragment(), "");
}

TEST(ReferenceTest, FragmentOnly) {
  Reference ref("#");
  EXPECT_FALSE(ref.isExternal());
  EXPECT_EQ(ref.documentUrl(), "");
  EXPECT_EQ(ref.fragment(), "");
}

TEST(ReferenceTest, DataUrlNotExternal) {
  Reference ref("data:image/svg+xml;base64,PHN2Zz4=");
  EXPECT_FALSE(ref.isExternal());
  EXPECT_EQ(ref.documentUrl(), "");
  EXPECT_EQ(ref.fragment(), "");
}

TEST(ReferenceTest, ExternalWithTrailingHash) {
  Reference ref("file.svg#");
  EXPECT_TRUE(ref.isExternal());
  EXPECT_EQ(ref.documentUrl(), "file.svg");
  EXPECT_EQ(ref.fragment(), "");
}

}  // namespace
}  // namespace donner::svg
