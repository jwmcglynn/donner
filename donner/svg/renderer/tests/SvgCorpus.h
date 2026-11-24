#pragma once
/// @file

#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "donner/svg/SVGDocument.h"

namespace donner::svg::tests {

/// A document of the repository's SVG corpora: its path and a test name derived from where it
/// sits in the corpus.
struct CorpusDocument {
  std::filesystem::path path;  //!< Absolute path of the document.
  std::string name;            //!< Unique, alphanumeric-and-underscore test name.

  /// Print the path, for test failure output.
  friend std::ostream& operator<<(std::ostream& os, const CorpusDocument& document) {
    return os << document.path.string();
  }
};

/// Every SVG of the resvg suite and the renderer test data, plus the splash and the icon sprite,
/// sorted by name. Reads the runfiles named in the test's `data`.
std::vector<CorpusDocument> CorpusDocuments();

/**
 * Read and parse \p document with default options and warnings disabled.
 *
 * @param document Corpus document.
 * @return The parsed document, or nullopt when it does not parse.
 */
std::optional<SVGDocument> ParseCorpusDocument(const CorpusDocument& document);

}  // namespace donner::svg::tests
