#include "donner/editor/TransformSyntaxPreserving.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
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
  const Transform2d parentFromElement = Transform2d::Rotate(60.0 * kDeg);
  // No author source: the canonical function, with the angle at edit precision.
  EXPECT_EQ(serializeTransformForWriteback(std::nullopt, parentFromElement).str(), "rotate(60)");
}

TEST(TransformSyntaxPreserving, WrapperWritesCanonicalTranslateAtEditPrecision) {
  // A drag inside a rotated group, on an element with no transform of its own, writes back a
  // canonical translate. It reads at the same six-decimal precision as an author-form update.
  const Transform2d parentFromElement =
      Transform2d::Translate(Vector2d(25.09652419453986, 6.99835504621448));
  EXPECT_EQ(serializeTransformForWriteback(std::nullopt, parentFromElement).str(),
            "translate(25.096524, 6.998355)");
}

TEST(TransformSyntaxPreserving, WrapperClearsAttributeWhenRoundedCanonicalIsIdentity) {
  // Dragging back to the start leaves floating-point residue below edit precision: the attribute
  // is removed, as for an exact identity, rather than written as translate(0).
  const Transform2d parentFromElement = Transform2d::Translate(Vector2d(3.0e-7, -2.0e-7));
  ASSERT_FALSE(parentFromElement.isIdentity());
  EXPECT_THAT(serializeTransformForWriteback(std::nullopt, parentFromElement).str(),
              testing::IsEmpty());
}

TEST(TransformSyntaxPreserving, WrapperDropsSubPrecisionResidueFromTheCanonicalFunction) {
  // Residue below edit precision does not select a longer function form.
  EXPECT_EQ(
      serializeTransformForWriteback(std::nullopt, Transform2d::Translate(Vector2d(5.0, 1.0e-9)))
          .str(),
      "translate(5)");
  const Transform2d parentFromRotated =
      Transform2d::Rotate(30.0 * kDeg) * Transform2d::Translate(Vector2d(1.0e-9, -1.0e-9));
  EXPECT_EQ(serializeTransformForWriteback(std::nullopt, parentFromRotated).str(), "rotate(30)");
}

TEST(TransformSyntaxPreserving, WrapperKeepsTinyScaleExact) {
  // Six decimals would turn this scale into 0.000123 (a 0.4% error); a linear entry is rounded only
  // when that changes it by at most one part in a million.
  const Transform2d parentFromElement = Transform2d::Scale(1.23456e-4);
  EXPECT_EQ(serializeTransformForWriteback(std::nullopt, parentFromElement).str(),
            "scale(0.000123456)");
}

TEST(TransformSyntaxPreserving, WrapperRoundsSmallMatrixEntriesToEditPrecision) {
  // Entries below one round to the same six decimals: sin(10 degrees) is 0.173648, not 17 digits.
  const Transform2d parentFromElement =
      Transform2d::Rotate(10.0 * kDeg) *
      Transform2d::Translate(Vector2d(100.0 / 11.0, 120.0 / 11.0));
  const std::string written = serializeTransformForWriteback(std::nullopt, parentFromElement).str();
  EXPECT_EQ(written, "matrix(0.984808, 0.173648, -0.173648, 0.984808, 9.090909, 10.909091)");
  ExpectRoundTrips(written, parentFromElement);
}

TEST(TransformSyntaxPreserving, WrapperDropsLinearResidueFromTheCanonicalFunction) {
  // Off-diagonal residue far below edit precision does not force a matrix.
  Transform2d parentFromElement;
  parentFromElement.data[1] = 1.0e-13;
  parentFromElement.data[2] = -1.0e-13;
  parentFromElement.data[4] = 5.0;
  parentFromElement.data[5] = 7.0;
  EXPECT_EQ(serializeTransformForWriteback(std::nullopt, parentFromElement).str(),
            "translate(5, 7)");

  // Dragging back to the start with residue in both the linear part and the translation clears the
  // attribute.
  Transform2d parentFromReturned;
  parentFromReturned.data[0] = 1.0 + 1.0e-13;
  parentFromReturned.data[1] = 1.0e-13;
  parentFromReturned.data[2] = -1.0e-13;
  parentFromReturned.data[3] = 1.0 - 1.0e-13;
  parentFromReturned.data[4] = 2.0e-7;
  parentFromReturned.data[5] = -1.0e-7;
  ASSERT_FALSE(parentFromReturned.isIdentity());
  EXPECT_THAT(serializeTransformForWriteback(std::nullopt, parentFromReturned).str(),
              testing::IsEmpty());
}

TEST(TransformSyntaxPreserving, WrapperKeepsTheSmallAxisOfAnAnisotropicScale) {
  // Each axis is rounded at its own precision, so a tiny axis next to a unit one is kept rather
  // than rounded to a singular scale(1, 0).
  EXPECT_EQ(
      serializeTransformForWriteback(std::nullopt, Transform2d::Scale(Vector2d(1.0, 1.0e-7))).str(),
      "scale(1, 1e-07)");
  EXPECT_EQ(
      serializeTransformForWriteback(std::nullopt, Transform2d::Scale(Vector2d(1000.0, 1.23456e-4)))
          .str(),
      "scale(1000, 0.000123456)");
}

TEST(TransformSyntaxPreserving, WrapperKeepsFullPrecisionForANearlySingularMatrix) {
  // Nearly parallel columns: rounding each entry to six decimals would move the point
  // (-1e8, 1e8) from about (49, 51) to (0, 100), so the writeback keeps full precision.
  Transform2d parentFromElement;
  parentFromElement.data[0] = 1.0;
  parentFromElement.data[1] = 1.0;
  parentFromElement.data[2] = 1.00000049;
  parentFromElement.data[3] = 1.00000051;
  EXPECT_EQ(serializeTransformForWriteback(std::nullopt, parentFromElement).str(),
            toSVGTransformString(parentFromElement).str());
}

TEST(TransformSyntaxPreserving, WrapperWritesCanonicalMatrixAtEditPrecision) {
  // A drag at a non-integer zoom produces repeating decimals in every matrix entry.
  const Transform2d parentFromElement =
      Transform2d::Rotate(15.0 * kDeg) *
      Transform2d::Translate(Vector2d(100.0 / 11.0, 120.0 / 11.0));
  const std::string written = serializeTransformForWriteback(std::nullopt, parentFromElement).str();
  EXPECT_EQ(written, "matrix(0.965926, 0.258819, -0.258819, 0.965926, 9.090909, 10.909091)");
  ExpectRoundTrips(written, parentFromElement);
}

TEST(TransformSyntaxPreserving, WrapperRewriteOfWrittenValueIsStable) {
  // Writing back an unchanged, already-rounded transform reproduces the same text, so a drag that
  // starts from the written value adds no error of its own.
  const Transform2d parentFromElement =
      Transform2d::Rotate(15.0 * kDeg) *
      Transform2d::Translate(Vector2d(100.0 / 11.0, 120.0 / 11.0));
  const std::string first = serializeTransformForWriteback(std::nullopt, parentFromElement).str();
  EXPECT_EQ(serializeTransformForWriteback(std::nullopt, ParseMatrix(first)).str(), first);
  EXPECT_EQ(serializeTransformForWriteback(RcString(first), ParseMatrix(first)).str(), first);
}

TEST(TransformSyntaxPreserving, WrapperLandsEachRepeatedDragWithinHalfAnEditUnit) {
  // Each drag composes a delta onto the transform parsed from the previous writeback, which is
  // what the user sees. Every writeback, canonical or author-form, lands within half an
  // edit-precision unit of that drag's target.
  constexpr double kHalfEditUnit = 0.5e-6 + 1e-12;
  const Vector2d step(100.0 / 11.0, -2.0 / 7.0);
  std::optional<RcString> written;
  Transform2d parentFromWritten;
  for (int drag = 1; drag <= 50; ++drag) {
    const Transform2d parentFromDragged = parentFromWritten * Transform2d::Translate(step);
    written = serializeTransformForWriteback(written, parentFromDragged);
    parentFromWritten = ParseMatrix(*written);
    EXPECT_THAT(std::string_view(*written), testing::StartsWith("translate("));
    EXPECT_NEAR(parentFromWritten.data[4], parentFromDragged.data[4], kHalfEditUnit)
        << "drag " << drag << ": " << *written;
    EXPECT_NEAR(parentFromWritten.data[5], parentFromDragged.data[5], kHalfEditUnit)
        << "drag " << drag << ": " << *written;
  }
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

// --- error-path and edge-branch coverage ----------------------------------

TEST(TransformSyntaxPreserving, PreservesVerbatimExponentToken) {
  // The author wrote x in exponent form and only y changes; the untouched x
  // token must round-trip byte-for-byte, exercising the number scanner's
  // exponent branch.
  const Transform2d target = Transform2d::Translate(Vector2d(10.0, 27.0));
  EXPECT_EQ(Reserialize("translate(1e1, 20)", target), "translate(1e1, 27)");
}

TEST(TransformSyntaxPreserving, PreservesVerbatimSignedToken) {
  // Explicit-sign token on the unchanged x component is re-emitted as authored.
  const Transform2d target = Transform2d::Translate(Vector2d(-10.0, 25.0));
  EXPECT_EQ(Reserialize("translate(-10, 20)", target), "translate(-10, 25)");
}

TEST(TransformSyntaxPreserving, DoubleCommaAuthorFallsBack) {
  const Transform2d target = Transform2d::Translate(Vector2d(5.0, 6.0));
  EXPECT_FALSE(reserializeTransformPreservingSyntax("translate(10,,20)", target).has_value());
}

TEST(TransformSyntaxPreserving, NonAlphaLeadingAuthorFallsBack) {
  const Transform2d target = Transform2d::Translate(Vector2d(5.0, 6.0));
  EXPECT_FALSE(reserializeTransformPreservingSyntax("9translate(10)", target).has_value());
}

TEST(TransformSyntaxPreserving, NonNumericArgumentFallsBack) {
  const Transform2d target = Transform2d::Translate(Vector2d(5.0, 0.0));
  EXPECT_FALSE(reserializeTransformPreservingSyntax("translate(x)", target).has_value());
}

TEST(TransformSyntaxPreserving, BareDotArgumentFallsBack) {
  const Transform2d target = Transform2d::Translate(Vector2d(5.0, 0.0));
  EXPECT_FALSE(reserializeTransformPreservingSyntax("translate(.)", target).has_value());
}

TEST(TransformSyntaxPreserving, WhitespaceOnlyAuthorFallsBack) {
  const Transform2d target = Transform2d::Translate(Vector2d(5.0, 0.0));
  EXPECT_FALSE(reserializeTransformPreservingSyntax("   ", target).has_value());
}

TEST(TransformSyntaxPreserving, SingularTargetFallsBack) {
  // A degenerate (zero-scale) target cannot be decomposed, so the structured
  // update bails and the caller must fall back to canonical serialization.
  const Transform2d singular = ParseMatrix("matrix(0, 0, 0, 0, 5, 5)");
  EXPECT_FALSE(reserializeTransformPreservingSyntax("translate(10, 20)", singular).has_value());
}

TEST(TransformSyntaxPreserving, DuplicateFunctionOfSameKindFallsBack) {
  // Two translate() functions in one list are not a shape the structured path
  // solves, so it declines rather than guessing.
  const Transform2d target = ParseMatrix("translate(1, 2) translate(3, 4)");
  EXPECT_FALSE(
      reserializeTransformPreservingSyntax("translate(1, 2) translate(3, 4)", target).has_value());
}

TEST(TransformSyntaxPreserving, ScaleChangeOnRotateOnlyFallsBack) {
  // rotate() has no scale parameter to absorb the target's scale, and the
  // rotate-only fallback also rejects the non-unit scale.
  const Transform2d target = Transform2d::Scale(Vector2d(2.0, 2.0));
  EXPECT_FALSE(reserializeTransformPreservingSyntax("rotate(45)", target).has_value());
}

TEST(TransformSyntaxPreserving, PureTranslationOnRotateOnlyFallsBack) {
  // A pure translation has zero rotation, so no rotation-about-a-center can
  // reproduce it; the rotate-only fallback declines.
  const Transform2d target = Transform2d::Translate(Vector2d(5.0, 5.0));
  EXPECT_FALSE(reserializeTransformPreservingSyntax("rotate(45)", target).has_value());
}

TEST(TransformSyntaxPreserving, RotateOnlyAboutCenterKeepsAuthoredAngle) {
  // Same angle, but the element is now rotated about a non-origin center: the
  // angle token stays verbatim and explicit center arguments are added.
  const Transform2d target = ParseMatrix("rotate(45, 10, 10)");
  const std::string out = Reserialize("rotate(45)", target);
  EXPECT_THAT(out, ::testing::StartsWith("rotate(45"));
  EXPECT_THAT(out, ::testing::Not(::testing::HasSubstr("matrix")));
  ExpectRoundTrips(out, target);
}

TEST(TransformSyntaxPreserving, RotationDeltaWrapsThroughShortestArc) {
  // A rotation edit whose raw delta exceeds half a turn must normalize to the
  // shortest signed arc before it is applied to the author's angle.
  const Transform2d target = ParseMatrix("rotate(170)");
  const std::string out = Reserialize("rotate(-80)", target);
  EXPECT_THAT(out, ::testing::StartsWith("rotate("));
  EXPECT_THAT(out, ::testing::Not(::testing::HasSubstr("matrix")));
  ExpectRoundTrips(out, target);
}

TEST(TransformSyntaxPreserving, PreservesVerbatimSignedExponentToken) {
  // Exponent with an explicit sign (1e+1) on the unchanged component is
  // re-emitted byte-for-byte, exercising the scanner branch that consumes the
  // exponent sign.
  const Transform2d target = Transform2d::Translate(Vector2d(10.0, 27.0));
  EXPECT_EQ(Reserialize("translate(1e+1, 20)", target), "translate(1e+1, 27)");
}

TEST(TransformSyntaxPreserving, RotationDeltaWrapsThroughNegativeArc) {
  // A raw delta at or below -180 degrees normalizes upward: editing
  // rotate(170) to a -170 degree pose applies +20, emitting rotate(190),
  // which is the same rotation.
  const Transform2d target = ParseMatrix("rotate(-170)");
  EXPECT_EQ(Reserialize("rotate(170)", target), "rotate(190)");
  ExpectRoundTrips("rotate(190)", target);
}

TEST(TransformSyntaxPreserving, UnknownFunctionNameFallsBack) {
  // foo(1) survives the lexical function-list scan but is not a valid SVG
  // transform, so the author-source parse fails and the caller falls back.
  const Transform2d target = Transform2d::Translate(Vector2d(5.0, 0.0));
  EXPECT_FALSE(reserializeTransformPreservingSyntax("foo(1)", target).has_value());
}

}  // namespace
}  // namespace donner::editor
