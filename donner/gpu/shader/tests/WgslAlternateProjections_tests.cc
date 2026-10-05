/// @file
/// The test-only WGSL alternates cover every production family a native build links.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cctype>
#include <cstdlib>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/LinkedProjection.h"
#include "donner/gpu/shader/ProductionShaderFamilies.h"
#include "donner/gpu/shader/tests/CompiledShaderViewPrinter.h"

namespace donner::gpu::shader {
namespace {

using testing::AllOf;
using testing::Eq;
using testing::Field;
using testing::IsEmpty;
using testing::Not;
using testing::NotNull;
using testing::Ref;
using testing::UnorderedElementsAreArray;

/// One production family's linked native artifact and its authored WGSL.
struct FamilyCase {
  std::string_view name;                    //!< Program name.
  const CompiledShaderView& (*native)();    //!< Linked native artifact.
  const CompiledShaderView& (*authored)();  //!< Authored WGSL artifact.
};

/// Prints the program name. @param os Output stream. @param family Case to print.
std::ostream& operator<<(std::ostream& os, const FamilyCase& family) {
  return os << family.name;
}

#define DONNER_FAMILY_CASE(family) \
  FamilyCase{#family, &programs::family##NativeShader, &programs::family##Shader},
const FamilyCase kFamilies[] = {DONNER_FOR_EACH_PRODUCTION_SHADER_FAMILY(DONNER_FAMILY_CASE)};
#undef DONNER_FAMILY_CASE

class WgslAlternateProjectionsTest : public testing::TestWithParam<FamilyCase> {};

INSTANTIATE_TEST_SUITE_P(Families, WgslAlternateProjectionsTest, testing::ValuesIn(kFamilies),
                         [](const testing::TestParamInfo<FamilyCase>& info) {
                           return std::string(info.param.name);
                         });

TEST_P(WgslAlternateProjectionsTest, WgslDeviceReceivesTheAuthoredWgsl) {
  const CompiledShaderView& selected =
      SelectLinkedProjection(ShaderSourceKind::Wgsl, GetParam().native());

  EXPECT_THAT(selected, Ref(GetParam().authored()));
  EXPECT_THAT(selected, Field("wgsl", &CompiledShaderView::wgsl,
                              AllOf(Not(IsEmpty()), Eq(GetParam().authored().wgsl))));
}

TEST_P(WgslAlternateProjectionsTest, NativeDeviceReceivesTheLinkedArtifact) {
  EXPECT_THAT(SelectLinkedProjection(kLinkedShaderSourceKind, GetParam().native()),
              Ref(GetParam().native()));
}

/// Spells a program name the way the build names its family, as in `slug_fill` for `SlugFill`.
/// @param program Program name.
std::string FamilyName(std::string_view program) {
  std::string family;
  for (const char c : program) {
    if (std::isupper(static_cast<unsigned char>(c)) && !family.empty()) {
      family.push_back('_');
    }
    family.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return family;
}

/// Splits the comma-separated family list the build passes in. @param list List to split.
std::vector<std::string> SplitFamilies(std::string_view list) {
  std::vector<std::string> families;
  while (!list.empty()) {
    const size_t comma = list.find(',');
    families.emplace_back(list.substr(0, comma));
    list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
  }
  return families;
}

TEST(WgslAlternateProjectionsCensusTest, CoversExactlyTheFamiliesProductionLibrariesLink) {
  const char* expected = std::getenv("DONNER_PRODUCTION_SHADER_FAMILIES");
  ASSERT_THAT(expected, NotNull()) << "the BUILD rule passes PRODUCTION_SHADER_FAMILIES";

  std::vector<std::string> covered;
  for (const FamilyCase& family : kFamilies) {
    covered.push_back(FamilyName(family.name));
  }
  EXPECT_THAT(covered, UnorderedElementsAreArray(SplitFamilies(expected)));
}

}  // namespace
}  // namespace donner::gpu::shader
