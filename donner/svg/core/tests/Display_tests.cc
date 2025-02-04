/**
 * @file Tests for \ref donner::svg::Display enum and its ostream output operator.
 */

#include "donner/svg/core/Display.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref Display values.
TEST(DisplayTest, OstreamOutput) {
  EXPECT_THAT(Display::Inline, ToStringIs("inline"));
  EXPECT_THAT(Display::Block, ToStringIs("block"));
  EXPECT_THAT(Display::ListItem, ToStringIs("list-item"));
  EXPECT_THAT(Display::InlineBlock, ToStringIs("inline-block"));
  EXPECT_THAT(Display::Table, ToStringIs("table"));
  EXPECT_THAT(Display::InlineTable, ToStringIs("inline-table"));
  EXPECT_THAT(Display::TableRowGroup, ToStringIs("table-row-group"));
  EXPECT_THAT(Display::TableHeaderGroup, ToStringIs("table-header-group"));
  EXPECT_THAT(Display::TableFooterGroup, ToStringIs("table-footer-group"));
  EXPECT_THAT(Display::TableRow, ToStringIs("table-row"));
  EXPECT_THAT(Display::TableColumnGroup, ToStringIs("table-column-group"));
  EXPECT_THAT(Display::TableColumn, ToStringIs("table-column"));
  EXPECT_THAT(Display::TableCell, ToStringIs("table-cell"));
  EXPECT_THAT(Display::TableCaption, ToStringIs("table-caption"));
  EXPECT_THAT(Display::None, ToStringIs("none"));
}

}  // namespace donner::svg
