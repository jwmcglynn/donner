#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RenderingContext.h"
#include "donner/svg/renderer/tests/ComputedStyleSnapshot.h"
#include "donner/svg/renderer/tests/OrderedSnapshotMatcher.h"
#include "donner/svg/renderer/tests/SvgCorpus.h"

namespace donner::svg::components {
namespace {
using tests::CorpusDocument;
using tests::SnapshotEq;

/// Two draws on independent documents. The reference rebuilds its styles and render tree even
/// when the tested mutation would normally take the retained path.
void CompareAfterCorpusEdit(SVGDocument& actual, SVGDocument& reference, Renderer& renderer,
                            Renderer& referenceRenderer, std::string_view artifactName) {
  auto& state = reference.registry().ctx().get<RenderTreeState>();
  state.retainedUpdatesEnabled = false;
  state.needsFullStyleRecompute = true;
  state.needsFullRebuild = true;
  renderer.draw(actual);
  referenceRenderer.draw(reference);
  const auto actualStyles = PositionedStyles(actual);
  const auto expectedStyles = PositionedStyles(reference);
  ASSERT_THAT(actualStyles, ::testing::SizeIs(expectedStyles.size()));
  // The full sprite has thousands of records. Construct a diagnostic matcher only for a mismatch.
  const auto mismatch =
      std::mismatch(actualStyles.begin(), actualStyles.end(), expectedStyles.begin());
  if (mismatch.first != actualStyles.end()) {
    EXPECT_THAT(*mismatch.first, ::testing::Eq(*mismatch.second)) << mismatch.second->position;
  }
  EXPECT_THAT(PositionedRenderInstances(actual), SnapshotEq(PositionedRenderInstances(reference)));
  const RendererBitmap expected = referenceRenderer.takeSnapshot();
  const RendererBitmap observed = renderer.takeSnapshot();
  if (expected.empty()) {
    // Documents rejected by the render size limit still have comparable styles and instances.
    EXPECT_THAT(observed.dimensions, ::testing::Eq(expected.dimensions));
    EXPECT_THAT(observed.pixels, SnapshotEq(expected.pixels));
  } else {
    editor::tests::CompareBitmapToBitmap(observed, expected, artifactName,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// Authored elements in breadth-first order, excluding shadow clones. Two parses of one document
/// produce the same targets, even when their render trees use different entity identifiers.
std::vector<SVGElement> AuthoredElements(SVGDocument& document) {
  std::vector<SVGElement> result{document.svgElement()};
  for (std::size_t i = 0; i < result.size(); ++i) {
    for (auto child = result[i].firstChild(); child; child = child->nextSibling()) {
      result.push_back(*child);
    }
  }
  return result;
}

struct CorpusEditTargets {
  std::size_t leaf = 0;
  std::optional<std::size_t> withClass;
  std::map<std::string, std::size_t> presentation;
};

/// Start at a stable offset for each document (32-bit FNV-1a). Sample one authored leaf, one
/// element with an existing class when possible, and one occurrence per presentation attribute.
/// The targeted fixtures separately exercise particular selector directions and fallback paths.
CorpusEditTargets SelectCorpusEditTargets(const std::vector<SVGElement>& elements,
                                          std::string_view name) {
  std::uint32_t hash = 2166136261u;
  for (const unsigned char c : name) {
    hash = (hash ^ c) * 16777619u;
  }
  const std::size_t start = hash % elements.size();
  CorpusEditTargets targets;
  std::optional<std::size_t> firstLeaf;
  for (std::size_t offset = 0; offset < elements.size(); ++offset) {
    const std::size_t index = (start + offset) % elements.size();
    const auto& element = elements[index];
    if (!firstLeaf && !element.firstChild()) {
      firstLeaf = index;
    }
    if (!targets.withClass && !element.className().empty()) {
      targets.withClass = index;
    }
    for (const auto& attribute : element.attributes()) {
      if (attribute.namespacePrefix.empty() &&
          PropertyRegistry::isPresentationAttributeName(attribute.name)) {
        targets.presentation.try_emplace(std::string(std::string_view(attribute.name)), index);
      }
    }
  }
  targets.leaf = firstLeaf.value_or(start);
  return targets;
}

class StyleEditCorpusTest : public ::testing::TestWithParam<CorpusDocument> {
protected:
  void SetUp() override {
    actual_ = tests::ParseCorpusDocument(GetParam());
    reference_ = tests::ParseCorpusDocument(GetParam());
    if (!actual_ || !reference_) {
      GTEST_SKIP() << "Does not parse: " << GetParam();
    }
    for (SVGDocument* document : {&*actual_, &*reference_}) {
      document->setCanvasSize(48, 48);
    }
    renderer_.draw(*actual_);
    referenceRenderer_.draw(*reference_);
    elements_ = AuthoredElements(*actual_);
    mirrors_ = AuthoredElements(*reference_);
    ASSERT_THAT(elements_, ::testing::Not(::testing::IsEmpty()));
    ASSERT_THAT(mirrors_, ::testing::SizeIs(elements_.size()));
    targets_ = SelectCorpusEditTargets(elements_, GetParam().name);
  }

  template <typename Edit>
  void compare(std::string_view operation, std::size_t index, Edit edit) {
    SCOPED_TRACE(::testing::Message()
                 << GetParam().name << ", " << operation << ", target " << index);
    edit(elements_[index]);
    edit(mirrors_[index]);
    std::string artifactName = GetParam().name + "_" + std::string(operation) + "_" +
                               std::to_string(index) + "_" + std::to_string(editOrdinal_++);
    for (char& ch : artifactName) {
      constexpr std::string_view kSafe =
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";
      if (kSafe.find(ch) == std::string_view::npos) {
        ch = '_';
      }
    }
    CompareAfterCorpusEdit(*actual_, *reference_, renderer_, referenceRenderer_, artifactName);
  }

  CorpusEditTargets targets_;

private:
  std::optional<SVGDocument> actual_, reference_;
  std::vector<SVGElement> elements_, mirrors_;
  Renderer renderer_, referenceRenderer_;
  std::size_t editOrdinal_ = 0;
};

TEST_P(StyleEditCorpusTest, ClassAndIdEditsMatchFullRebuild) {
  const std::size_t withClass = targets_.withClass.value_or(targets_.leaf);
  compare("add class", targets_.leaf, [](auto& e) {
    e.setClassName(std::string(std::string_view(e.className())) + " corpus-edited");
  });
  compare("replace class", withClass, [](auto& e) { e.setClassName("corpus-replaced"); });
  compare("remove class", withClass, [](auto& e) { e.removeAttribute("class"); });
  compare("set id", targets_.leaf, [](auto& e) { e.setId("corpus-edited-id"); });
}

TEST_P(StyleEditCorpusTest, InlineAndFillEditsMatchFullRebuild) {
  compare("inline style", targets_.leaf,
          [](auto& e) { e.setStyle("fill:#196ab2;stroke:#9d412b"); });
  compare("update inline style", targets_.leaf, [](auto& e) { e.updateStyle("fill:#37c29a"); });
  compare("set fill", targets_.leaf, [](auto& e) { e.setAttribute("fill", "#357fb9"); });
}

TEST_P(StyleEditCorpusTest, RemovingAuthoredPresentationMatchesFullRebuild) {
  for (const auto& [attribute, index] : targets_.presentation) {
    compare(attribute, index,
            [&](auto& e) { e.removeAttribute(xml::XMLQualifiedNameRef(attribute)); });
  }
}

// Separate the two large artwork fixtures so a single full-sprite edit has the entire
// ordinary test deadline. The two targets partition the same manifest and use identical tests.
std::vector<CorpusDocument> EditCorpusDocuments() {
  auto documents = tests::CorpusDocuments();
  constexpr bool large = DONNER_LARGE_EDIT_CORPUS != 0;
  std::erase_if(documents, [](const CorpusDocument& doc) {
    const bool largeArtwork = doc.name == "bootstrap_icons" || doc.name == "donner_splash";
    return largeArtwork != large;
  });
  return documents;
}

INSTANTIATE_TEST_SUITE_P(Corpus, StyleEditCorpusTest, ::testing::ValuesIn(EditCorpusDocuments()),
                         [](const ::testing::TestParamInfo<CorpusDocument>& info) {
                           return info.param.name;
                         });

}  // namespace
}  // namespace donner::svg::components
