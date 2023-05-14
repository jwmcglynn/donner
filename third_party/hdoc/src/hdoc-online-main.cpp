// Copyright 2019-2023 hdoc
// SPDX-License-Identifier: AGPL-3.0-only

#include "llvm/Support/Signals.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/Threading.h"

#include "frontend/Frontend.hpp"
#include "indexer/Indexer.hpp"
#include "serde/SerdeUtils.hpp"
#include "serde/Serialization.hpp"

#include <httplib.h>

namespace {

#ifdef HDOC_RELEASE_BUILD
constexpr char hdocURL[] = "https://app.hdoc.io";
#else
constexpr char hdocURL[] = "https://staging.hdoc.io";
#endif

/// @brief Verify that the user's API key is valid prior to uploading documentation
bool verify() {
  const char* val     = std::getenv("HDOC_PROJECT_API_KEY");
  std::string api_key = val == NULL ? std::string("") : std::string(val);
  if (api_key == "") {
    spdlog::error("No API key was found in the HDOC_PROJECT_API_KEY environment variable. Unable to proceed.");
    return false;
  }

  httplib::Client  cli(hdocURL);
  httplib::Headers headers{
      {"Authorization", "Api-Key " + api_key},
  };

  const auto res = cli.Get("/api/verify/", headers);
  if (res == nullptr) {
    spdlog::error("Connection failed, unable to proceed. Check that you're connected to the internet.");
    return false;
  }

  if (res->status != 200) {
    spdlog::error("Verification failed, ensure your API key is correct and you are subscribed (status={}): {}",
                  res->status,
                  res->reason);
    return false;
  }
  return true;
}

/// @brief Upload the serialized Index to hdoc.io for hosting
void uploadDocs(const std::string_view data) {
  spdlog::info("Uploading documentation for hosting.");
  const char* val     = std::getenv("HDOC_PROJECT_API_KEY");
  std::string api_key = val == NULL ? std::string("") : std::string(val);
  if (api_key == "") {
    spdlog::error("No API key was found in the HDOC_PROJECT_API_KEY environment variable. Unable to proceed.");
    return;
  }

  httplib::Client cli(hdocURL);
  cli.set_compress(true);
  httplib::Headers headers{
      {"Authorization", "Api-Key " + api_key},
      {"Content-Disposition", "inline;filename=hdoc-payload.json"},
      {"X-Schema-Version", "v5"},
  };

  const auto res = cli.Put("/api/upload/", headers, data.data(), data.size(), "application/json");
  if (res == nullptr) {
    spdlog::error("Upload failed, unable to proceed. Check that you're connected to the internet.");
    return;
  }

  if (res->status != 200) {
    spdlog::error("Documentation upload failed (status={}): {}", res->status, res->reason);
  } else {
    // Temporarily set the log level to the info level so that the URL to the documentation is
    // printed to the terminal.
    spdlog::set_level(spdlog::level::info);
    spdlog::info("{}", res->body);
    spdlog::set_level(spdlog::level::warn);
  }
}

} // namespace

int main(int argc, char** argv) {
  // Print stack trace on failure
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);

  hdoc::types::Config cfg;
  cfg.binaryType = hdoc::types::BinaryType::Online;
  hdoc::frontend::Frontend frontend(argc, argv, &cfg);

  // Check if user is verified prior to indexing everything
  if (hdoc::serde::verify() == false) {
    return EXIT_FAILURE;
  }

  // Ensure that cfg was properly initialized
  if (!cfg.initialized) {
    return EXIT_FAILURE;
  }

  llvm::ThreadPool       pool(llvm::hardware_concurrency(cfg.numThreads));
  hdoc::indexer::Indexer indexer(&cfg, pool);
  indexer.run();
  indexer.pruneMethods();
  indexer.pruneTypeRefs();
  indexer.resolveNamespaces();
  indexer.updateRecordNames();
  indexer.printStats();
  const hdoc::types::Index* index = indexer.dump();

  const std::string data = hdoc::serde::serializeToJSON(*index, cfg);
  hdoc::serde::uploadDocs(data);

  // Ensure that cfg was properly initialized
  bool res = dumpJSONPayload(cfg.outputFilename, data);
  if (res == false) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
