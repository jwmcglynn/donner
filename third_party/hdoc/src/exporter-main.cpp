// Copyright 2019-2023 hdoc
// SPDX-License-Identifier: AGPL-3.0-only

#include "llvm/Support/Signals.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/Threading.h"

#include "indexer/Indexer.hpp"
#include "serde/HTMLWriter.hpp"
#include "serde/SerdeUtils.hpp"
#include "serde/Serialization.hpp"

#include "argparse/argparse.hpp"
#include "spdlog/spdlog.h"
#include "toml++/toml.h"
#include "version.hpp"

#include <iostream>

int main(int argc, char** argv) {
  // Print stack trace on failure
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);

  hdoc::types::Config cfg;
  cfg.hdocVersion = HDOC_VERSION;
  cfg.binaryType  = hdoc::types::BinaryType::Full;

  argparse::ArgumentParser program("hdoc", cfg.hdocVersion);
  program.add_argument("--verbose").help("Whether to use verbose output").default_value(false).implicit_value(true);
  program.add_argument("--config").help("Path to .hdoc.toml file").required();
  program.add_argument("--input").help("Path to input json files").nargs(argparse::nargs_pattern::at_least_one);
  program.add_argument("--output").help("Path to output html files").required();

  // Parse command line arguments
  try {
    program.parse_args(argc, argv);
  } catch (const std::runtime_error& err) {
    spdlog::error("Error found while parsing command line arguments: {}", err.what());
    return EXIT_FAILURE;
  }

  // Toggle verbosity depending on state of command line switch
  if (program.get<bool>("--verbose") == true) {
    spdlog::set_level(spdlog::level::info);
  } else {
    spdlog::set_level(spdlog::level::warn);
  }

  // Check that the current directory contains a .hdoc.toml file
  cfg.rootDir = std::filesystem::current_path();

  std::filesystem::path configFile;
  if (auto config = program.present<std::string>("--config")) {
    configFile = std::filesystem::path(*config);
    if (!std::filesystem::is_regular_file(configFile)) {
      spdlog::error("Specified config file {} does not exist.", *config);
      return EXIT_FAILURE;
    }
  } else {
    configFile = cfg.rootDir / ".hdoc.toml";
    if (!std::filesystem::is_regular_file(configFile)) {
      spdlog::error("Current directory doesn't contain an .hdoc.toml file.", *config);
      return EXIT_FAILURE;
    }
  }

  const std::vector<std::string> inputFiles = program.get<std::vector<std::string>>("--input");
  if (inputFiles.empty()) {
    spdlog::error("No input files specified.");
    return EXIT_FAILURE;
  }

  // Check to see if all the files exist.
  for (const auto& file : inputFiles) {
    if (!std::filesystem::is_regular_file(file)) {
      spdlog::error("Input file {} does not exist.", file);
      return EXIT_FAILURE;
    }
  }

  const std::string outputDir = program.get<std::string>("--output");
  cfg.outputDir               = outputDir;

  // Parse configuration file
  toml::table toml;
  try {
    toml = toml::parse_file((configFile).string());
  } catch (const toml::parse_error& err) {
    spdlog::error("Error in configuration file: {} ({}:{}:{})",
                  err.description(),
                  *err.source().path,
                  err.source().begin.line,
                  err.source().begin.column);
    return EXIT_FAILURE;
  }

  // Check if the output directory is specified. Print a warning if it's specified for online versions of hdoc,
  // and throw an error if it's specified for full versions of hdoc because we need to know where to save the docs.
  std::optional<std::string_view> output_dir = toml["paths"]["output_dir"].value<std::string_view>();
  if (!output_dir) {
    spdlog::error(
        "No 'output_dir' specified in .hdoc.toml. It is required so that documentation can be saved locally.");
  }

  // Get other arguments from the .hdoc.toml file.
  cfg.projectName      = toml["project"]["name"].value_or("");
  cfg.projectVersion   = toml["project"]["version"].value_or("");
  cfg.gitRepoURL       = toml["project"]["git_repo_url"].value_or("");
  cfg.gitDefaultBranch = toml["project"]["git_default_branch"].value_or("");
  if (cfg.projectName == "") {
    spdlog::error("Project name in .hdoc.toml is empty, not a string, or invalid.");
    return EXIT_FAILURE;
  }
  if (cfg.gitRepoURL != "" && cfg.gitRepoURL.back() != '/') {
    spdlog::error("Git repo URL is missing the mandatory trailing slash: {}", cfg.gitRepoURL);
    return EXIT_FAILURE;
  }

  // If numThreads is not an integer, return an error
  if (toml["project"]["num_threads"].type() != toml::node_type::integer &&
      toml["project"]["num_threads"].type() != toml::node_type::none) {
    spdlog::error("Number of threads in .hdoc.toml is not an integer.");
    return EXIT_FAILURE;
  }
  // If numThreads wasn't defined, use the default value of 0 (index with all available threads)
  if (toml["project"]["num_threads"].type() == toml::node_type::none) {
    cfg.numThreads = 0;
  }
  // Otherwise it must be defined and be an integer, so check if it's valid and set if so
  else {
    int64_t rawNumThreads = toml["project"]["num_threads"].as_integer()->get();
    if (rawNumThreads < 0) {
      spdlog::error("Number of threads must be a positive integer greater than or equal to 0.");
      return EXIT_FAILURE;
    }
    cfg.numThreads = rawNumThreads;
  }

  if (const toml::value<bool>* ignorePrivateMembers = toml["ignore"]["ignore_private_members"].as_boolean()) {
    cfg.ignorePrivateMembers = ignorePrivateMembers->get();
  }

  // Collect paths to markdown files
  cfg.homepage = std::filesystem::path(toml["pages"]["homepage"].value_or(""));
  if (const auto& mdPaths = toml["pages"]["paths"].as_array()) {
    for (const auto& md : *mdPaths) {
      std::string s = md.value_or(std::string(""));
      if (s == "") {
        spdlog::warn("A path to a markdown file in .hdoc.toml was malformed, ignoring it.");
        continue;
      }
      std::filesystem::path mdPath(s);
      if (std::filesystem::exists(mdPath) == false || std::filesystem::is_regular_file(mdPath) == false) {
        spdlog::warn("A path to a markdown file in .hdoc.toml either doesn't exist or isn't a file, ignoring it.");
        continue;
      }
      cfg.mdPaths.emplace_back(mdPath);
    }
  }

  // Get the current timestamp
  const auto        time_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::stringstream ss;
  ss << std::put_time(std::gmtime(&time_t), "%FT%T UTC");
  cfg.timestamp = ss.str();

  cfg.initialized = true;

  // Dump state of the Config object
  spdlog::info("hdoc version: {}", cfg.hdocVersion);
  spdlog::info("Timestamp: {}", cfg.timestamp);
  spdlog::info("Root directory: {}", cfg.rootDir.string());
  spdlog::info("Output directory: {}", cfg.outputDir.string());
  spdlog::info("Project name: {}", cfg.projectName);
  spdlog::info("Project version: {}", cfg.projectVersion);

  // Ensure that cfg was properly initialized
  if (!cfg.initialized) {
    return EXIT_FAILURE;
  }

  // Reload the index.
  hdoc::types::Index index;

  // Load and merge the index using deserializeFromJSON
  for (const auto& file : inputFiles) {
    const bool result = hdoc::serde::deserializeFromJSONFragment(index, cfg, file);
    if (!result) {
      spdlog::error("Failed to load index from {}", file);
      return EXIT_FAILURE;
    }
  }

  llvm::ThreadPool        pool(llvm::hardware_concurrency(cfg.numThreads));
  hdoc::serde::HTMLWriter htmlWriter(&index, &cfg, pool);
  htmlWriter.printFunctions();
  htmlWriter.printRecords();
  htmlWriter.printNamespaces();
  htmlWriter.printEnums();
  htmlWriter.printSearchPage();
  htmlWriter.processMarkdownFiles();
  htmlWriter.printProjectIndex();

  return EXIT_SUCCESS;
}
