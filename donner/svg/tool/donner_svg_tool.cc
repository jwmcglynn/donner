/// @file

#include <cstdlib>
#include <filesystem>
#include <iostream>

#include "donner/base/FailureSignalHandler.h"
#include "donner/svg/tool/DonnerSvgTool.h"

int main(int argc, char* argv[]) {
  donner::InstallFailureSignalHandler();

  // When launched via `bazel run`, Bazel sets BUILD_WORKING_DIRECTORY to the
  // directory where the user invoked the command.  Changing to that directory
  // lets relative paths (input files, --output) resolve naturally without
  // requiring --run_under.
  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::filesystem::current_path(bwd);
  }

  return donner::svg::RunDonnerSvgTool(argc, argv, std::cout, std::cerr);
}
