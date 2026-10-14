/// @file
/// Unattended GoogleTest image suite with an explicit native renderer executable.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "tools/svg_suite/GtestSuite.h"
#include "tools/svg_suite/ProcessRenderer.h"
#include "tools/svg_suite/Provenance.h"
#include "tools/svg_suite/TextEncoding.h"

namespace donner::svg_suite {
namespace {
struct CommandLine {
  std::filesystem::path bundleRoot = ".";
  std::filesystem::path artifactRoot;
  ProcessRendererOptions process;
  std::vector<CorpusRoot> roots;
  bool extensionOnly = false;
  bool bundledCorpora = false;
  ProvenanceOptions provenance;
};

std::string AddCorpus(const std::string& value, CommandLine& command) {
  const size_t separator = value.find(':');
  if (separator == std::string::npos || separator == 0 || separator + 1 == value.size()) {
    return "Expected --corpus name:path";
  }
  command.roots.push_back({value.substr(0, separator), value.substr(separator + 1)});
  return {};
}

std::string ParseValueArgument(std::string_view argument, const std::string& value,
                               CommandLine& command) {
  if (argument == "--bundle-root") {
    command.bundleRoot = value;
  } else if (argument == "--renderer") {
    command.process.executable = value;
  } else if (argument == "--renderer-arg") {
    command.process.arguments.push_back(value);
  } else if (argument == "--artifact-dir") {
    command.artifactRoot = value;
  } else if (argument == "--work-dir") {
    command.process.workRoot = value;
  } else if (argument == "--engine-revision") {
    command.provenance.engineRevision = value;
  } else if (argument == "--backend") {
    command.provenance.backend = value;
  } else if (argument == "--build-flags") {
    command.provenance.buildFlags = value;
  } else if (argument == "--corpus") {
    return AddCorpus(value, command);
  } else {
    return "Unknown argument: " + std::string(argument);
  }
  return {};
}

std::string ParseArguments(int argc, char** argv, CommandLine& command) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--trusted-inputs") {
      command.process.trustedInputs = true;
      continue;
    }
    if (argument == "--extension-only") {
      command.extensionOnly = true;
      continue;
    }
    if (index + 1 >= argc) {
      return "Missing value for " + std::string(argument);
    }
    const std::string issue = ParseValueArgument(argument, argv[++index], command);
    if (!issue.empty()) {
      return issue;
    }
  }
  return {};
}

std::string ResolveDefaults(CommandLine& command) {
  if (command.process.executable.empty()) {
    return "--renderer is required";
  }
  if (command.artifactRoot.empty()) {
    if (const char* path = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
      command.artifactRoot = path;
    } else {
      return "--artifact-dir is required outside the test launcher";
    }
  }
  if (command.process.workRoot.empty()) {
    command.process.workRoot = command.artifactRoot / "work";
  }
  if (command.process.arguments.empty()) {
    command.process.arguments = {"--input", "{input}", "--output", "{output}",
                                 "--root",  "{root}",  "--fonts",  "{fonts}",
                                 "--width", "{width}", "--height", "{height}"};
  }
  return {};
}

std::string AddBundledCorpus(const char* name, const std::filesystem::path& sentinelPath,
                             CommandLine& command) {
  std::error_code error;
  const auto sentinel = std::filesystem::canonical(command.bundleRoot / sentinelPath, error);
  if (error) {
    return "Missing bundled " + std::string(name) + " corpus: " + error.message();
  }
  command.roots.push_back({name, sentinel.parent_path()});
  return {};
}

std::string ResolveCorpora(CommandLine& command) {
  command.bundledCorpora = command.roots.empty();
  if (!command.bundledCorpora) {
    return command.extensionOnly ? "--extension-only cannot be combined with explicit --corpus"
                                 : "";
  }
  if (!command.extensionOnly) {
    const auto issue = AddBundledCorpus("resvg", "third_party/resvg-test-suite/LICENSE", command);
    if (!issue.empty()) {
      return issue;
    }
  }
  return AddBundledCorpus("donner", "donner/svg/renderer/tests/donner_svg2_corpus/manifest.json",
                          command);
}

void SetProvenanceOptions(CommandLine& command) {
  for (const auto& corpus : command.roots) {
    ProvenanceCorpus identity{corpus.name, corpus.path};
    if (command.bundledCorpora && corpus.name == "resvg") {
      identity.revision = "d8e064337faf01bc5a9579187a56dbdbe3eacc72";
      identity.revisionEvidence =
          "repository vendor record; not an independent upstream byte verification";
    }
    command.provenance.corpora.push_back(std::move(identity));
  }
  command.provenance.renderer = command.process.executable;
  command.provenance.rendererArguments = command.process.arguments;
  command.provenance.workRoot = command.process.workRoot;
  command.provenance.timeoutMs = command.process.timeout.count();
  command.provenance.trustedInputs = command.process.trustedInputs;
}

bool ValidInitialText(int argc, char** argv) {
  if (argc < 0 || static_cast<size_t>(argc) > kMaxArguments) {
    return false;
  }
  size_t bytes = 0;
  for (int index = 0; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (!IsValidIdentityText(argument) || argument.size() > kMaxArgumentBytes - bytes) {
      return false;
    }
    bytes += argument.size();
  }
  for (const char* name : {"GTEST_OUTPUT", "TEST_UNDECLARED_OUTPUTS_DIR"}) {
    if (const char* value = std::getenv(name); value && !IsValidIdentityText(value)) {
      return false;
    }
  }
  return true;
}

int ReportError(const std::string& issue) {
  std::cerr << EscapeLogText(issue) << '\n';
  return 2;
}

void ConfigureDefaultGtestOutput(int argc, char** argv) {
  if (!GTEST_FLAG_GET(output).empty()) {
    return;
  }
  const char* environment = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  std::filesystem::path artifacts = environment ? environment : "";
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument.starts_with("--gtest_") || argument == "--trusted-inputs" ||
        argument == "--extension-only") {
      continue;
    }
    if (index + 1 < argc) {
      const char* value = argv[++index];
      if (argument == "--artifact-dir") {
        artifacts = value;
      }
    }
  }
  // GoogleTest installs its report listener during initialization; explicit flags override this.
  if (!artifacts.empty()) {
    GTEST_FLAG_SET(output, "xml:" + (artifacts / "gtest.xml").string());
  }
}

int ExecuteSuite(CommandLine& command) {
  auto discovery = DiscoverCases(command.roots);
  if (!discovery.error.empty()) {
    return ReportError(discovery.error);
  }
  SetProvenanceOptions(command);
  const auto provenance = CreateRunProvenance(command.provenance, command.artifactRoot);
  if (!provenance.run) {
    return ReportError(provenance.error);
  }
  command.process.executable = provenance.run->renderer;
  command.process.workRoot = provenance.run->workRoot;
  for (auto& test : discovery.cases) {
    test.provenance = provenance.run;
  }
  const std::string error = RegisterTests(
      discovery, MakeProcessRenderer(std::move(command.process)), command.artifactRoot);
  if (!error.empty()) {
    return ReportError(error);
  }
  RunExecutionSummary execution;
  const int result = RunSelectedTests(&execution);
  const auto verification = VerifyRunProvenance(*provenance.run);
  const auto evidenceError = WriteRunVerification(*provenance.run, verification, result, execution);
  if (!verification.empty() || !evidenceError.empty()) {
    return ReportError("Run provenance rejected: " + verification + " " + evidenceError);
  }
  return result;
}
}  // namespace
}  // namespace donner::svg_suite

int main(int argc, char** argv) {
  namespace suite = donner::svg_suite;
  if (!suite::ValidInitialText(argc, argv)) {
    return suite::ReportError(
        "command arguments and output environment require bounded NUL-free UTF-8");
  }
  suite::CommandLine command;
  command.provenance.invocation.assign(argv, argv + argc);
  suite::ConfigureDefaultGtestOutput(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  std::string issue = suite::ParseArguments(argc, argv, command);
  if (issue.empty()) {
    issue = suite::ResolveDefaults(command);
  }
  if (issue.empty()) {
    issue = suite::ResolveCorpora(command);
  }
  return issue.empty() ? suite::ExecuteSuite(command) : suite::ReportError(issue);
}
