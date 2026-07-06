#include "donner/editor/TransformSyntaxPreserving.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "donner/base/MathUtils.h"
#include "donner/base/Transform.h"
#include "donner/svg/parser/TransformParser.h"

namespace donner::editor {
namespace {

using ::testing::Optional;

constexpr double kDeg = MathConstants<double>::kDegToRad;

/// Parse a transform string into its matrix (helper for building targets that
/// are, by construction, expressible in a given author form).
Transform2d ParseMatrix(std::string_view s) {
  auto result = svg::parser::TransformParser::Parse(s);
  EXPECT_FALSE(result.hasError()) << "failed to parse target: " << s;
  return result.result();
}

/// Assert @p candidate re-parses to @p target within tolerance.
void ExpectRoundTrips(std::string_view candidate, const Transform2d& target) {
  const Transform2d parsed = ParseMatrix(candidate);
  for (int k = 0; k < 6; ++k) {
    EXPECT_NEAR(parsed.data[k], target.data[k], 1e-3)
        << "component " << k << " of '" << candidate << "'";
  }
}

std::string Reserialize(std::string_view author, const Transform2d& target) {
  auto result = reserializeTransformPreservingSyntax(author, target);
  EXPECT_TRUE(result.has_value()) << "expected preservation for author '" << author << "'";
  return result.has_value() ? std::string(std::string_view(*result)) : std::string();
}

// --- rotate-only ----------------------------------------------------------

TEST(TransformSyntaxPreserving, RotateOnlyKeepsRotateAtOrigin) {
  const Transform2d target = Transform2d::Rotate(60.0 * kDeg);
  EXPECT_EQ(Reserialize("rotate(45)", target), "rotate(60)");
}

TEST(TransformSyntaxPreserving, RotateOnlySnapsAwayFloatingPointNoise) {
  // A target composed from two rotations carries fp noise; the emitted angle
  // must still be the clean "60", not "60.00000000001".
  const Transform2d target = Transform2d::Rotate(45.0 * kDeg) * Transform2d::Rotate(15.0 * kDeg);
  EXPECT_EQ(Reserialize("rotate(45)", target), "rotate(60)");
}

TEST(TransformSyntaxPreserving, RotateOnlyAboutCenterGainsCenterArgs) {
  const Transform2d target = ParseMatrix("rotate(60, 10, 10)");
  const std::string out = Reserialize("rotate(45)", target);
  EXPECT_EQ(out, "rotate(60, 10, 10)");
  ExpectRoundTrips(out, target);
}

TEST(TransformSyntaxPreserving, RotateWithCenterArgsPreservesCenter) {
  // Editing the angle of an authored rotate(a, cx, cy) about the same center
  // keeps the center arguments and only bumps the angle.
  const Transform2d target = ParseMatrix("rotate(50, 30, 40)");
  const std::string out = Reserialize("rotate(20, 30, 40)", target);
  EXPECT_THAT(out, ::testing::StartsWith("rotate("));
  EXPECT_THAT(out, ::testing::HasSubstr("30"));
  EXPECT_THAT(out, ::testing::HasSubstr("40"));
  EXPECT_THAT(out, ::testing::Not(::testing::HasSubstr("matrix")));
  ExpectRoundTrips(out, target);
}

// --- translate-only -------------------------------------------------------

TEST(TransformSyntaxPreserving, TranslateTwoArgUpdatesNumbers) {
  const Transform2d target = Transform2d::Translate(Vector2d(15.0, 27.0));
  EXPECT_EQ(Reserialize("translate(10, 20)", target), "translate(15, 27)");
}

TEST(TransformSyntaxPreserving, TranslateOneArgStaysOneArgWhenYZero) {
  const Transform2d target = Transform2d::Translate(Vector2d(25.0, 0.0));
  EXPECT_EQ(Reserialize("translate(10)", target), "translate(25)");
}

TEST(TransformSyntaxPreserving, TranslateOneArgExpandsWhenYChanges) {
  const Transform2d target = Transform2d::Translate(Vector2d(25.0, 4.0));
  const std::string out = Reserialize("translate(10)", target);
  EXPECT_THAT(out, ::testing::StartsWith("translate("));
  EXPECT_THAT(out, ::testing::Not(::testing::HasSubstr("matrix")));
  ExpectRoundTrips(out, target);
}

TEST(TransformSyntaxPreserving, TranslateKeepsUnchangedTokenVerbatim) {
  // Only x moved; the y token is re-emitted exactly as authored.
  const Transform2d target = Transform2d::Translate(Vector2d(12.5, 20.0));
  EXPECT_EQ(Reserialize("translate(10, 20)", target), "translate(12.5, 20)");
}

// --- scale ----------------------------------------------------------------

TEST(TransformSyntaxPreserving, ScaleUniformStaysUniform) {
  const Transform2d target = Transform2d::Scale(Vector2d(3.0, 3.0));
  EXPECT_EQ(Reserialize("scale(2)", target), "scale(3)");
}

TEST(TransformSyntaxPreserving, ScaleUniformExpandsToTwoArgs) {
  const Transform2d target = Transform2d::Scale(Vector2d(3.0, 2.0));
  const std::string out = Reserialize("scale(2)", target);
  EXPECT_THAT(out, ::testing::StartsWith("scale("));
  EXPECT_THAT(out, ::testing::Not(::testing::HasSubstr("matrix")));
  ExpectRoundTrips(out, target);
}

TEST(TransformSyntaxPreserving, ScaleTwoArgUpdatesBoth) {
  const Transform2d target = Transform2d::Scale(Vector2d(4.0, 3.0));
  EXPECT_EQ(Reserialize("scale(2, 3)", target), "scale(4, 3)");
}

// --- translate + rotate lists --------------------------------------------

TEST(TransformSyntaxPreserving, TranslateRotateUpdatesRotationComponent) {
  const Transform2d target = ParseMatrix("translate(10, 20) rotate(45)");
  EXPECT_EQ(Reserialize("translate(10, 20) rotate(30)", target), "translate(10, 20) rotate(45)");
}

TEST(TransformSyntaxPreserving, TranslateRotateUpdatesTranslateComponent) {
  const Transform2d target = ParseMatrix("translate(18, 27) rotate(30)");
  EXPECT_EQ(Reserialize("translate(10, 20) rotate(30)", target), "translate(18, 27) rotate(30)");
}

TEST(TransformSyntaxPreserving, RotateTranslateListPreservesOrderAndForm) {
  const Transform2d target = ParseMatrix("rotate(45) translate(5, 6)");
  const std::string out = Reserialize("rotate(30) translate(5, 6)", target);
  EXPECT_THAT(out, ::testing::StartsWith("rotate("));
  EXPECT_THAT(out, ::testing::HasSubstr("translate("));
  EXPECT_THAT(out, ::testing::Not(::testing::HasSubstr("matrix")));
  ExpectRoundTrips(out, target);
}

// --- matrix stays matrix --------------------------------------------------

TEST(TransformSyntaxPreserving, MatrixInputStaysMatrix) {
  const Transform2d target = Transform2d::Translate(Vector2d(9.0, 6.0));
  const std::string out = Reserialize("matrix(1, 0, 0, 1, 5, 6)", target);
  EXPECT_THAT(out, ::testing::StartsWith("matrix("));
  EXPECT_EQ(out, "matrix(1, 0, 0, 1, 9, 6)");
}

// --- fallbacks ------------------------------------------------------------

TEST(TransformSyntaxPreserving, RotationChangeAgainstSkewFallsBack) {
  // The design's canonical fallback: a decomposed rotation change against a
  // skew matrix cannot be represented in the author's rotate() structure.
  const Transform2d skewTarget = ParseMatrix("matrix(1, 0.5, 0.5, 1, 0, 0)");
  EXPECT_FALSE(reserializeTransformPreservingSyntax("rotate(45)", skewTarget).has_value());
}

TEST(TransformSyntaxPreserving, SkewFunctionInAuthorListFallsBack) {
  const Transform2d target = ParseMatrix("rotate(60) skewX(10)");
  EXPECT_FALSE(reserializeTransformPreservingSyntax("rotate(45) skewX(10)", target).has_value());
}

TEST(TransformSyntaxPreserving, RotationIntroducedOnTranslateOnlyFallsBack) {
  // translate(x, y) cannot absorb a new rotation component.
  const Transform2d target = ParseMatrix("translate(10, 20) rotate(30)");
  EXPECT_FALSE(reserializeTransformPreservingSyntax("translate(10, 20)", target).has_value());
}

TEST(TransformSyntaxPreserving, MalformedAuthorFallsBack) {
  const Transform2d target = Transform2d::Translate(Vector2d(5.0, 0.0));
  EXPECT_FALSE(reserializeTransformPreservingSyntax("not a transform", target).has_value());
  EXPECT_FALSE(reserializeTransformPreservingSyntax("translate(", target).has_value());
}

// --- serializeTransformForWriteback wrapper -------------------------------

TEST(TransformSyntaxPreserving, WrapperPrefersAuthorForm) {
  const Transform2d target = Transform2d::Rotate(60.0 * kDeg);
  EXPECT_EQ(std::string_view(serializeTransformForWriteback(RcString("rotate(45)"), target)),
            "rotate(60)");
}

TEST(TransformSyntaxPreserving, WrapperFallsBackToCanonicalWithoutAuthorSource) {
  const Transform2d target = Transform2d::Rotate(60.0 * kDeg);
  // No author source: canonical serializer output.
  EXPECT_EQ(std::string_view(serializeTransformForWriteback(std::nullopt, target)),
            std::string_view(toSVGTransformString(target)));
}

TEST(TransformSyntaxPreserving, WrapperFallsBackToCanonicalOnSkew) {
  const Transform2d skewTarget = ParseMatrix("matrix(1, 0.5, 0.5, 1, 0, 0)");
  EXPECT_EQ(std::string_view(serializeTransformForWriteback(RcString("rotate(45)"), skewTarget)),
            std::string_view(toSVGTransformString(skewTarget)));
}

TEST(TransformSyntaxPreserving, WrapperClearsAttributeOnIdentity) {
  const Transform2d identity;
  EXPECT_TRUE(
      std::string_view(serializeTransformForWriteback(RcString("rotate(45)"), identity)).empty());
}

}  // namespace
}  // namespace donner::editor
