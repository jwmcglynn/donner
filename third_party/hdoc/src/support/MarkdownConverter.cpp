// Copyright 2019-2023 hdoc
// SPDX-License-Identifier: AGPL-3.0-only

#include "support/MarkdownConverter.hpp"

#include <fstream>

#include "cmark-gfm-core-extensions.h"

namespace {

class OneTimeInit {
public:
  OneTimeInit() {
    // Ensure that GFM's table extension is loaded.
    cmark_gfm_core_extensions_ensure_registered();
  }
};

} // namespace

hdoc::utils::MarkdownConverter hdoc::utils::MarkdownConverter::fromString(const std::string& mdString) {
  return MarkdownConverter(mdString);
}

hdoc::utils::MarkdownConverter hdoc::utils::MarkdownConverter::fromFile(const std::filesystem::path& mdPath) {
  // Slurp Markdown file into a string for later conversion.
  std::ifstream     ifs(mdPath);
  const std::string content((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));

  return MarkdownConverter(content, mdPath.string());
}

hdoc::utils::MarkdownConverter::MarkdownConverter(const std::string& content, std::optional<std::string> mdPath) {
  static OneTimeInit oneTimeInit;

  this->markdownParser                   = cmark_parser_new(CMARK_OPT_DEFAULT | CMARK_OPT_UNSAFE);
  cmark_syntax_extension* tableExtension = cmark_find_syntax_extension("table");
  if (tableExtension) {
    cmark_parser_attach_syntax_extension(this->markdownParser, tableExtension);
  } else {
    spdlog::warn("Unable to locate Markdown table extension.");
    return;
  }

  // Parse the raw Markdown into nodes, and then render it into HTML.
  cmark_parser_feed(this->markdownParser, content.c_str(), content.size());
  this->markdownDoc = cmark_parser_finish(this->markdownParser);
  if (!this->markdownDoc) {
    if (mdPath) {
      spdlog::warn("Parsing of Markdown file {} failed. Skipping this file.", *mdPath);
    } else {
      spdlog::warn("Parsing of Markdown string failed.");
    }
    return;
  }

  // Convert the Markdown nodes into HTML.
  this->htmlBuf = cmark_render_html(this->markdownDoc, CMARK_OPT_DEFAULT | CMARK_OPT_UNSAFE, NULL);
  if (!this->htmlBuf) {
    if (mdPath) {
      spdlog::warn("Conversion of Markdown file {} to HTML failed. Skipping this file.", *mdPath);
    } else {
      spdlog::warn("Conversion of Markdown string to HTML failed.");
    }
    return;
  }

  // Convert HTML into a string for later retrieval.
  this->html        = std::string(this->htmlBuf);
  this->initialized = true;
}

hdoc::utils::MarkdownConverter::~MarkdownConverter() {
  cmark_parser_free(this->markdownParser);
  cmark_node_free(this->markdownDoc);
  free(this->htmlBuf);
}

CTML::Node hdoc::utils::MarkdownConverter::getHTMLNode() const {
  if (this->initialized == false) {
    return CTML::Node();
  }

  CTML::Node main("main");
  main.AppendRawHTML(this->html);
  return main;
}
