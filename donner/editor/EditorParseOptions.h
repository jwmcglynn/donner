#pragma once
/// @file

#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {

/// Parse options for editor documents. The editor round-trips user content, so
/// the parser must PRESERVE user / `data-*` attributes in the DOM rather than
/// dropping them - editor features read them back via `getAttribute` (e.g.
/// `IsLocked` reads `data-donner-locked`), and a save must not silently lose the
/// user's own attributes. The library default `disableUserAttributes = true` is
/// correct for a strict renderer, not for an authoring tool. Every editor-side
/// parse (document load, structural replace, clipboard validation) uses this.
inline svg::parser::SVGParser::Options EditorParseOptions() {
  svg::parser::SVGParser::Options options;
  options.disableUserAttributes = false;
  return options;
}

}  // namespace donner::editor
