// Copyright 2019-2023 hdoc
// SPDX-License-Identifier: AGPL-3.0-only

#include "serde/Serialization.hpp"
#include "serde/JSONDeserializer.hpp"
#include "serde/JSONSerializer.hpp"
#include "serde/SerdeUtils.hpp"
#include "types/SerializedMarkdownFile.hpp"
#include "types/Symbols.hpp"

#include "rapidjson/prettywriter.h"
#include "spdlog/spdlog.h"

#include <fstream>
#include <string>

namespace hdoc::serde {

std::string serializeToJSON(const hdoc::types::Index& index, const hdoc::types::Config& cfg) {
  hdoc::serde::JSONSerializer jsonSerializer(&index, &cfg);
  std::string                 payload = jsonSerializer.getJSONPayload();
  return payload;
}

bool deserializeFromJSON(hdoc::types::Index& index, hdoc::types::Config& cfg) {
  hdoc::serde::JSONDeserializer      jsonDeserializer;
  std::optional<rapidjson::Document> doc = jsonDeserializer.parseJSONToDocument();
  if (doc.has_value() == false) {
    spdlog::error("Unable to parse JSON document, it is likely missing or not valid JSON. Aborting.");
    return false;
  }

  const bool passedSchemaValidation = jsonDeserializer.validateJSON(*doc);
  if (passedSchemaValidation == false) {
    spdlog::error("JSON schema validation of the input JSON file failed. Aborting.");
    return false;
  }

  std::vector<hdoc::types::SerializedMarkdownFile> serializedFiles;
  jsonDeserializer.deserializeJSONPayload(*doc, index, cfg, serializedFiles);

  if (serializedFiles.size() > 0) {
    std::filesystem::path markdownFilesDir = std::filesystem::path("hdoc-markdown-dump");
    std::filesystem::create_directories(markdownFilesDir);

    // Serialized Markdown files are "recreated" (dumped) to a temporary directory
    // The Config object used by the server is then recreated as a copy of the client's
    // but with the paths readjusted
    for (const auto& f : serializedFiles) {
      std::ofstream(markdownFilesDir / f.filename) << f.contents;
      // The homepage isn't added to mdPaths, we don't want it to appear in the sidebar
      if (f.isHomepage == true) {
        cfg.homepage = std::filesystem::path(markdownFilesDir / f.filename);
        continue;
      }
      cfg.mdPaths.emplace_back(markdownFilesDir / f.filename);
    }
  }
  return true;
}

bool deserializeFromJSONFragment(hdoc::types::Index& index, hdoc::types::Config& cfg, const std::string& jsonFile) {
  hdoc::serde::JSONDeserializer      jsonDeserializer;
  std::optional<rapidjson::Document> doc = jsonDeserializer.parseJSONToDocument(jsonFile);
  if (doc.has_value() == false) {
    spdlog::error("Unable to parse JSON document, it is likely missing or not valid JSON. Aborting.");
    return false;
  }

  /*const bool passedSchemaValidation = jsonDeserializer.validateJSON(*doc);
  if (passedSchemaValidation == false) {
    spdlog::error("JSON schema validation of the input JSON file failed. Aborting.");
    return false;
  }*/

  std::vector<hdoc::types::SerializedMarkdownFile> serializedFiles;
  jsonDeserializer.deserializeJSONPayload(*doc, index, cfg, serializedFiles);

#if 0
  if (serializedFiles.size() > 0) {
    std::filesystem::path markdownFilesDir = std::filesystem::path("hdoc-markdown-dump");
    std::filesystem::create_directories(markdownFilesDir);

    // Serialized Markdown files are "recreated" (dumped) to a temporary directory
    // The Config object used by the server is then recreated as a copy of the client's
    // but with the paths readjusted
    for (const auto& f : serializedFiles) {
      std::ofstream(markdownFilesDir / f.filename) << f.contents;
      // The homepage isn't added to mdPaths, we don't want it to appear in the sidebar
      if (f.isHomepage == true) {
        cfg.homepage = std::filesystem::path(markdownFilesDir / f.filename);
        continue;
      }
      cfg.mdPaths.emplace_back(markdownFilesDir / f.filename);
    }
  }
#endif
  return true;
}

} // namespace hdoc::serde
