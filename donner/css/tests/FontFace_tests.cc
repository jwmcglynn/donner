/// @file
/// Tests for the `@font-face` identity key.
///
/// Callers deduplicate registration on this key: two declarations that produce the same key are
/// treated as one and the later is dropped in favour of what the earlier already resolved to. Two
/// properties therefore matter and are what this file pins. The key must be STABLE, or an
/// unchanged declaration re-announced every style recompute keeps minting new identities and
/// invalidating everything cached against them. And it must be INJECTIVE over the fields it
/// covers, or two declarations that should stay separate collapse and one silently serves the
/// other's bytes. The field values come from the document, so the second property has to hold for
/// hostile values too, not just tidy ones.

#include "donner/css/FontFace.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/RcString.h"

namespace donner::css {
namespace {

std::shared_ptr<const std::vector<uint8_t>> MakeBytes(size_t size) {
  return std::make_shared<const std::vector<uint8_t>>(size, uint8_t{0x42});
}

/// A face with one inline-data source, the common shape.
FontFace MakeDataFace(std::string_view family,
                      const std::shared_ptr<const std::vector<uint8_t>>& payload) {
  FontFace face;
  face.familyName = RcString(family);
  FontFaceSource source;
  source.kind = FontFaceSource::Kind::Data;
  source.payload = payload;
  face.sources.push_back(std::move(source));
  return face;
}

/// A face with one URL source.
FontFace MakeUrlFace(std::string_view family, std::string_view url) {
  FontFace face;
  face.familyName = RcString(family);
  FontFaceSource source;
  source.kind = FontFaceSource::Kind::Url;
  source.payload = RcString(url);
  face.sources.push_back(std::move(source));
  return face;
}

TEST(FontFaceIdentityKeyTest, EqualDeclarationsShareAKey) {
  const auto payload = MakeBytes(64);
  EXPECT_EQ(FontFaceIdentityKey(MakeDataFace("TestFont", payload)),
            FontFaceIdentityKey(MakeDataFace("TestFont", payload)));
}

TEST(FontFaceIdentityKeyTest, FamilyNameIsCaseInsensitive) {
  const auto payload = MakeBytes(64);
  // Face selection compares family names case-insensitively, so the key has to agree or one family
  // would hold two identities that always resolve to each other.
  EXPECT_EQ(FontFaceIdentityKey(MakeDataFace("TestFont", payload)),
            FontFaceIdentityKey(MakeDataFace("tESTfONT", payload)));
  EXPECT_NE(FontFaceIdentityKey(MakeDataFace("TestFont", payload)),
            FontFaceIdentityKey(MakeDataFace("OtherFont", payload)));
}

TEST(FontFaceIdentityKeyTest, EachMatchingDescriptorSeparatesDeclarations) {
  const auto payload = MakeBytes(64);
  const FontFace base = MakeDataFace("TestFont", payload);
  const std::string baseKey = FontFaceIdentityKey(base);

  FontFace heavier = base;
  heavier.fontWeight = 700;
  EXPECT_NE(FontFaceIdentityKey(heavier), baseKey);

  FontFace italic = base;
  italic.fontStyle = 1;
  EXPECT_NE(FontFaceIdentityKey(italic), baseKey);

  FontFace condensed = base;
  condensed.fontStretch = 3;
  EXPECT_NE(FontFaceIdentityKey(condensed), baseKey);
}

TEST(FontFaceIdentityKeyTest, EachSourceFieldSeparatesDeclarations) {
  const auto payload = MakeBytes(64);
  const FontFace base = MakeDataFace("TestFont", payload);
  const std::string baseKey = FontFaceIdentityKey(base);

  FontFace otherKind = base;
  otherKind.sources[0].kind = FontFaceSource::Kind::Local;
  EXPECT_NE(FontFaceIdentityKey(otherKind), baseKey);

  // Trust decides whether the bytes may reach a length-unaware parser, so it is not cosmetic.
  FontFace trusted = base;
  trusted.sources[0].trusted = true;
  EXPECT_NE(FontFaceIdentityKey(trusted), baseKey);

  FontFace withFormat = base;
  withFormat.sources[0].formatHint = RcString("woff2");
  EXPECT_NE(FontFaceIdentityKey(withFormat), baseKey);

  FontFace withTech = base;
  withTech.sources[0].techHints.push_back(RcString("variations"));
  EXPECT_NE(FontFaceIdentityKey(withTech), baseKey);

  FontFace otherBytes = MakeDataFace("TestFont", MakeBytes(64));
  EXPECT_NE(FontFaceIdentityKey(otherBytes), baseKey)
      << "Two separately allocated buffers are two payloads even when their contents match.";
}

TEST(FontFaceIdentityKeyTest, SourceCountAndOrderSeparateDeclarations) {
  const auto first = MakeBytes(64);
  const auto second = MakeBytes(128);

  FontFace one = MakeDataFace("TestFont", first);
  FontFace two = one;
  two.sources.push_back(one.sources[0]);
  two.sources[1].payload = second;
  EXPECT_NE(FontFaceIdentityKey(one), FontFaceIdentityKey(two));

  FontFace reversed = two;
  std::swap(reversed.sources[0], reversed.sources[1]);
  EXPECT_NE(FontFaceIdentityKey(two), FontFaceIdentityKey(reversed))
      << "Sources are an ordered fallback list, so their order is part of the declaration.";
}

TEST(FontFaceIdentityKeyTest, PayloadAlternativeIsTaggedApartFromTheKind) {
  // A malformed source can pair any kind with either payload alternative, so the alternative is
  // recorded separately from the kind rather than inferred from it.
  FontFace urlPayload = MakeUrlFace("TestFont", "payload-bytes");
  FontFace dataPayload = MakeDataFace("TestFont", MakeBytes(64));
  dataPayload.sources[0].kind = FontFaceSource::Kind::Url;
  EXPECT_NE(FontFaceIdentityKey(urlPayload), FontFaceIdentityKey(dataPayload));
}

/// Fields are length-prefixed rather than delimited. With a delimiter, a value containing it could
/// shift where the following fields appear to start and let two different declarations produce one
/// key, which would silently drop the second in favour of the first's bytes. Family names, hints,
/// and URLs all come from the document, so these are values an author can actually write.
TEST(FontFaceIdentityKeyTest, SeparatorsEmbeddedInFieldValuesDoNotCollide) {
  const auto payload = MakeBytes(64);

  // Try to make the family name swallow the descriptors that follow it, for every ASCII character
  // that could plausibly be serving as a separator.
  for (const std::string_view separator :
       {std::string_view(":"), std::string_view(";"), std::string_view(","), std::string_view("|"),
        std::string_view("\x1f"), std::string_view("\x1e"), std::string_view("\0", 1),
        std::string_view("\n")}) {
    const std::string spillover = std::string("Test") + std::string(separator) + "400" +
                                  std::string(separator) + "0" + std::string(separator) + "5";
    EXPECT_NE(FontFaceIdentityKey(MakeDataFace(spillover, payload)),
              FontFaceIdentityKey(MakeDataFace("Test", payload)))
        << "A family name embedding a separator collided with a shorter one.";
  }

  // The same attempt through a URL.
  EXPECT_NE(FontFaceIdentityKey(MakeUrlFace("TestFont", "a:b")),
            FontFaceIdentityKey(MakeUrlFace("TestFont", "a")));

  FontFace hintSpillover = MakeDataFace("TestFont", payload);
  hintSpillover.sources[0].formatHint = RcString("woff2:1:variations");
  FontFace hintSplit = MakeDataFace("TestFont", payload);
  hintSplit.sources[0].formatHint = RcString("woff2");
  hintSplit.sources[0].techHints.push_back(RcString("variations"));
  EXPECT_NE(FontFaceIdentityKey(hintSpillover), FontFaceIdentityKey(hintSplit));
}

/// The case that decides the encoding, rather than merely surviving it. Two tech hints are two
/// adjacent fields the document controls with no fixed-width field between them, so moving the
/// boundary between their values leaves the concatenation identical: `{"a:b", "c"}` and
/// `{"a", "b:c"}` are the same character sequence once joined by any delimiter, whatever the
/// delimiter is. Only writing each field's length can keep them apart, and they must stay apart,
/// because two `@font-face` rules with different technology requirements are different rules and
/// deduplication would otherwise drop one of them.
TEST(FontFaceIdentityKeyTest, MovingTheBoundaryBetweenAdjacentFieldsDoesNotCollide) {
  const auto payload = MakeBytes(64);

  FontFace joined = MakeDataFace("TestFont", payload);
  joined.sources[0].techHints.push_back(RcString("a:b"));
  joined.sources[0].techHints.push_back(RcString("c"));

  FontFace split = MakeDataFace("TestFont", payload);
  split.sources[0].techHints.push_back(RcString("a"));
  split.sources[0].techHints.push_back(RcString("b:c"));

  EXPECT_NE(FontFaceIdentityKey(joined), FontFaceIdentityKey(split))
      << "Adjacent field values were run together, so where one ends and the next begins is not "
         "recoverable from the key.";
}

/// A declaration with no sources at all is still a declaration, and two of them for different
/// families must not collapse.
TEST(FontFaceIdentityKeyTest, SourcelessDeclarationsStaySeparateByFamily) {
  FontFace one;
  one.familyName = RcString("TestFont");
  FontFace two;
  two.familyName = RcString("OtherFont");
  EXPECT_NE(FontFaceIdentityKey(one), FontFaceIdentityKey(two));
  EXPECT_EQ(FontFaceIdentityKey(one), FontFaceIdentityKey(one));
}

}  // namespace
}  // namespace donner::css
