// Copyright 2019-2023 hdoc
// SPDX-License-Identifier: AGPL-3.0-only

#include "support/ParallelExecutor.hpp"
#include "spdlog/spdlog.h"
#include "support/PathUtils.hpp"

#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "llvm/Support/VirtualFileSystem.h"

#include <filesystem>
#include <iostream>

namespace {

bool isPathIgnored(std::string path, const std::vector<std::string>& ignorePaths, const std::string& rootDir) {
  const std::string relPath = hdoc::utils::pathToRelative(path, rootDir);

  // Ignore paths outside of the rootDir
  // ".." is used as a janky way to determine if the path is outside of rootDir since the canonicalized path
  // should not have any ".."s in it
  if (relPath.find("..") != std::string::npos) {
    return true;
  }

  for (const auto& substr : ignorePaths) {
    if (relPath.find(substr) != std::string::npos) {
      return true;
    }
  }

  return false;
}

} // namespace

void hdoc::indexer::ParallelExecutor::execute(std::unique_ptr<clang::tooling::FrontendActionFactory> action) {
  std::mutex mutex;

  // Add a counter to track progress
  uint32_t i                = 0;
  auto     incrementCounter = [&]() {
    std::unique_lock<std::mutex> lock(mutex);
    return ++i;
  };

  std::vector<std::string> allFilesInCmpdb = this->cmpdb.getAllFiles();

  std::vector<std::string> allMatchingFiles;
  for (const std::string& file : allFilesInCmpdb) {
    if (isPathIgnored(file, this->ignorePaths, this->rootDir)) {
      continue;
    }

    allMatchingFiles.push_back(file);
  }

  if (this->debugLimitNumIndexedFiles > 0 && this->debugLimitNumIndexedFiles > allMatchingFiles.size()) {
    allMatchingFiles.resize(this->debugLimitNumIndexedFiles);
  }

  std::string totalNumFiles = std::to_string(allMatchingFiles.size());

  for (const std::string& file : allMatchingFiles) {
    this->pool.async(
        [&](const std::string path) {
          spdlog::info("[{}/{}] processing {}", incrementCounter(), totalNumFiles, path);

          // Each thread gets an independent copy of a VFS to allow different concurrent working directories
          llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FS = llvm::vfs::createPhysicalFileSystem().release();
          clang::tooling::ClangTool Tool(this->cmpdb, {path}, std::make_shared<clang::PCHContainerOperations>(), FS);

          // Append argument adjusters so that system includes and others are picked up on
          // TODO: determine if the -fsyntax-only flag actually does anything
          Tool.appendArgumentsAdjuster(clang::tooling::getClangStripOutputAdjuster());
          Tool.appendArgumentsAdjuster(clang::tooling::getClangStripDependencyFileAdjuster());
          Tool.appendArgumentsAdjuster(clang::tooling::getClangSyntaxOnlyAdjuster());
          Tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
              this->includePaths, clang::tooling::ArgumentInsertPosition::END));

          clang::TextDiagnosticPrinter stderrPrinter(llvm::errs(), new clang::DiagnosticOptions());
          Tool.setDiagnosticConsumer(&stderrPrinter);

          // Ignore all diagnostics that clang might throw. Clang often has weird diagnostic settings that don't
          // match what's in compile_commands.json, resulting in spurious errors. Instead of trying to change clang's
          // behavior, we'll ignore all diagnostics and assume that the user supplied a project that builds on their
          // machine.
          // clang::IgnoringDiagConsumer ignore;
          // Tool.setDiagnosticConsumer(&ignore);

          // Run the tool and print an error message if something goes wrong
          if (Tool.run(action.get())) {
            spdlog::error(
                "Clang failed to parse source file: {}. Information from this file may be missing from hdoc's output",
                path);
          }
        },
        file);
  }
  // Make sure all tasks have finished before resetting the working directory
  this->pool.wait();
}
