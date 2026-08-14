/// @file
/// Command-line wrapper for the in-memory Donner showcase generator.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>

#include "donner/editor/tools/GenerateShowcaseAsset.h"

namespace {

std::string ReadFile(const std::string& path, bool& ok) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    ok = false;
    return {};
  }

  std::ostringstream buffer;
  buffer << stream.rdbuf();
  ok = true;
  return buffer.str();
}

int Run(const std::string& inputPath, const std::string& outputPath) {
  std::error_code equivalentError;
  if (std::filesystem::equivalent(inputPath, outputPath, equivalentError) && !equivalentError) {
    std::cerr << "error: input and output must be different files\n";
    return 1;
  }

  std::error_code inputError;
  std::error_code outputError;
  const std::filesystem::path resolvedInput =
      std::filesystem::weakly_canonical(inputPath, inputError);
  const std::filesystem::path resolvedOutput =
      std::filesystem::weakly_canonical(outputPath, outputError);
  if (!inputError && !outputError && resolvedInput == resolvedOutput) {
    std::cerr << "error: input and output must be different files\n";
    return 1;
  }

  bool readOk = false;
  const std::string source = ReadFile(inputPath, readOk);
  if (!readOk) {
    std::cerr << "error: could not open input " << inputPath << "\n";
    return 1;
  }

  auto generated = donner::editor::GenerateShowcaseAsset(source);
  if (!generated.ok()) {
    std::cerr << "error: " << generated.error << "\n";
    return 1;
  }

  std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    std::cerr << "error: could not open output " << outputPath << " for writing\n";
    return 1;
  }

  output << generated.value;
  output.close();
  if (!output) {
    std::cerr << "error: failed while writing output " << outputPath << "\n";
    return 1;
  }

  std::cout << "Wrote generated showcase: " << outputPath << " (" << generated.value.size()
            << " bytes)\n";
  return 0;
}

}  // namespace

// Generate a temporary showcase SVG from an input document. This entry point
// is excluded from Doxygen's symbol index so ordinary references to the main
// branch do not autolink to the CLI.
int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: " << (argc > 0 ? argv[0] : "generate_showcase_asset")
              << " <input.svg> <output.svg>\n";
    return 2;
  }

  return Run(argv[1], argv[2]);
}
