/**
 * @example custom_css_parser.cc Custom CSS Parser
 *
 * Demonstrates how to use the Donner CSS library in a third-party library to implement CSS3 parsing
 * and selector matching. This example loads a stylesheet, creates a fake document tree, and the
 * matches the rules against the tree.
 *
 * ```sh
 * bazel run //examples:custom_css_parser
 * ```
 */

#include <iostream>

#include "donner/base/element/tests/FakeElement.h"
#include "donner/css/CSS.h"
#include "donner/css/Specificity.h"

using namespace donner::css;
using donner::FakeElement;

int main(int argc, char* argv[]) {
  //! [parse_stylesheet]
  Stylesheet stylesheet = CSS::ParseStylesheet(R"(
    g {
      fill: black;
    }

    path {
      fill: blue;
    }

    path.withColor {
      fill: red !important;
      stroke: blue;
    }

    g > :nth-child(2n of path) {
      fill: green;
    }
  )");

  std::cout << "Parsed stylesheet:\n" << stylesheet << "\n";
  //! [parse_stylesheet]

  // Build a document tree and query against it.
  FakeElement group = FakeElement("g");
  FakeElement path1 = FakeElement("path");
  path1.setId("path1");
  path1.setAttribute("d", "M 1 1 L 4 5");
  group.appendChild(path1);

  FakeElement path2 = FakeElement("path");
  path2.setId("path2");
  path2.setClassName("withColor");
  path2.setAttribute("d", "M 5 1 L 9 5");
  group.appendChild(path2);

  std::cout << "Using document tree:\n";
  std::cout << group.printAsTree() << "\n";
  // Outputs:
  //! [document_tree]
  // FakeElement: g, numChildren=2
  // - FakeElement: path#path1[d=M 1 1 L 4 5], numChildren=0
  // - FakeElement: path#path2.withColor[d=M 5 1 L 9 5], numChildren=0
  //! [document_tree]

  //! [match_rules]
  for (const auto& rule : stylesheet.rules()) {
    bool foundMatch = false;
    std::cout << "Matching " << rule.selector << ":\n";
    for (const auto& element : {group, path1, path2}) {
      if (SelectorMatchResult match = rule.selector.matches(element)) {
        foundMatch = true;
        std::cout << " - Matched " << element << " - " << match.specificity << "\n";
      }
    }

    if (foundMatch) {
      std::cout << "\n";
    } else {
      std::cout << " - No match\n\n";
    }
  }
  //! [match_rules]

  //! [parse_selector]
  // CSS Selectors can also be parsed directly from a string, for implementing querySelector.
  if (std::optional<Selector> selector = CSS::ParseSelector("g > #path1")) {
    std::cout << "Parsed selector: " << *selector << "\n";
    if (SelectorMatchResult match = selector->matches(path1)) {
      std::cout << "Matched " << path1 << " - " << match.specificity << "\n";
    } else {
      std::cout << "No match\n";
    }
  } else {
    std::cerr << "Failed to parse selector\n";
    std::abort();
  }
  //! [parse_selector]

  //! [parse_style_attribute]
  // Style attribute values, which are a list of `key: value;` pairs (css declarations).
  std::vector<Declaration> declarations = CSS::ParseStyleAttribute("fill: red; stroke: blue;");
  std::cout << "Parsed style attribute:\n";
  for (const auto& declaration : declarations) {
    std::cout << declaration << "\n";
  }
  //! [parse_style_attribute]

  return 0;
}
