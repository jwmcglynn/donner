#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/css/parser/stylesheet_parser.h"
#include "src/css/parser/tests/selector_test_utils.h"
#include "src/css/parser/tests/token_test_utils.h"

using testing::ElementsAre;

namespace donner::css::parser {

MATCHER_P2(SelectorRuleIs, selector, declarations, "") {
  return testing::ExplainMatchResult(selector, arg.selector, result_listener) &&
         testing::ExplainMatchResult(declarations, arg.declarations, result_listener);
}

TEST(StylesheetParser, Empty) {
  EXPECT_THAT(StylesheetParser::Parse("").rules(), ElementsAre());
}

TEST(StylesheetParser, WithRules) {
  EXPECT_THAT(StylesheetParser::Parse(R"(
    test, .class {
      name: value;
    }
  )")
                  .rules(),
              ElementsAre(SelectorRuleIs(
                  SelectorsAre(ComplexSelectorIs(EntryIs(TypeSelectorIs("test"))),
                               ComplexSelectorIs(EntryIs(ClassSelectorIs("class")))),
                  ElementsAre(DeclarationIs("name", ElementsAre(TokenIsIdent("value")))))));
}

}  // namespace donner::css::parser
