#pragma once
/**
 * @file Display.h
 *
 * Defines the \ref donner::svg::Display enum, which is used to determine how an element is
 * rendered.
 */

#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * The parsed result of the CSS 'display' property, see
 * https://www.w3.org/TR/CSS2/visuren.html#propdef-display.
 *
 * Note that in SVG2, there are only two distinct behaviors, 'none', and everything else rendered as
 * normal, see https://www.w3.org/TR/SVG2/render.html#VisibilityControl
 *
 * > Elements that have any other display value than none are rendered as normal.
 *
 */
enum class Display {
  Inline,       ///< [DEFAULT] "inline": Causes an element to generate one or more inline boxes.
  Block,        ///< "block": Causes an element to generate a block box.
  ListItem,     ///< "list-item": Causes an element to act as a list item.
  InlineBlock,  ///< "inline-block": Causes an element to generate an inline-level block container.
  Table,        ///< "table": Specifies that an element defines a block-level table, see
                ///< https://www.w3.org/TR/CSS2/tables.html#table-display.
  InlineTable,  ///< "inline-table": Specifies that an element defines a inline-level table, see
                ///< https://www.w3.org/TR/CSS2/tables.html#table-display.
  TableRowGroup,     ///< "table-row-group": Specifies that an element groups one or more rows.
  TableHeaderGroup,  ///< "table-header-group": Like 'table-row-group', but for visual formatting,
                     ///< the row group is always displayed before all other rows and row groups and
                     ///< after any top captions.
  TableFooterGroup,  ///< "table-footer-group": Like 'table-row-group', but for visual formatting,
                     ///< the row group is always displayed after all other rows and row groups and
                     ///< before any bottom captions.
  TableRow,          ///< "table-row": Specifies that an element is a row of cells.
  TableColumnGroup,  ///< "table-column-group": Specifies that an element groups one or more
                     ///< columns.
  TableColumn,       ///< "table-column": Specifies that an element is a column of cells.
  TableCell,         ///< "table-cell": Specifies that an element represents a table cell.
  TableCaption,      ///< "table-caption": Specifies a caption for the table.
  None,              ///< "none": The element is not rendered.
};

/**
 * Ostream output operator for \ref Display enum, outputs the CSS value.
 */
inline std::ostream& operator<<(std::ostream& os, Display value) {
  switch (value) {
    case Display::Inline: return os << "inline";
    case Display::Block: return os << "block";
    case Display::ListItem: return os << "list-item";
    case Display::InlineBlock: return os << "inline-block";
    case Display::Table: return os << "table";
    case Display::InlineTable: return os << "inline-table";
    case Display::TableRowGroup: return os << "table-row-group";
    case Display::TableHeaderGroup: return os << "table-header-group";
    case Display::TableFooterGroup: return os << "table-footer-group";
    case Display::TableRow: return os << "table-row";
    case Display::TableColumnGroup: return os << "table-column-group";
    case Display::TableColumn: return os << "table-column";
    case Display::TableCell: return os << "table-cell";
    case Display::TableCaption: return os << "table-caption";
    case Display::None: return os << "none";
  }

  UTILS_UNREACHABLE();
}

}  // namespace donner::svg
