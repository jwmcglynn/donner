/// @file
/// Exercise the GoogleTest runner in fresh processes with real image-case completion.

#include "tools/svg_suite/GtestSuite.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tools/svg_suite/ProcessRenderer.h"
#include "tools/svg_suite/Provenance.h"

namespace donner::svg_suite {
namespace {
std::filesystem::path executable;

class SkipSecondCase : public testing::EmptyTestEventListener {
public:
  void OnTestStart(const testing::TestInfo& test) override {
    if (std::string_view(test.test_suite_name()) == "SvgSuite" &&
        std::string_view(test.name()).ends_with("_7365636f6e642e737667")) {
      GTEST_SKIP() << "intentional second-case skip";
    }
  }
};

bool CreateFixtureCorpus(const std::filesystem::path& root, const Image& image) {
  std::filesystem::create_directories(root / "corpus");
  for (const char* name : {"first", "second"}) {
    std::ofstream(root / "corpus" / (std::string(name) + ".svg")) << "<svg/>";
    if (!WritePng(root / "corpus" / (std::string(name) + ".png"), {1, 1, 1, image.data}).empty()) {
      return false;
    }
  }
  return true;
}

ProvenanceOptions FixtureOptions(const std::filesystem::path& renderer,
                                 const std::filesystem::path& root, std::string_view mode) {
  ProvenanceOptions options;
  options.corpora = {{"fixture", root / "corpus"}};
  options.renderer = renderer;
  options.workRoot = root / "work";
  if (mode == "--fixture-process" || mode == "--fixture-spoof-diagnostics" ||
      mode == "--fixture-invalid-diagnostics") {
    options.rendererArguments = {
        mode == "--fixture-spoof-diagnostics" ? "--fixture-spoof-log" : "--fixture-render-png",
        "{output}"};
    if (mode == "--fixture-invalid-diagnostics") {
      options.rendererArguments.front() = "--fixture-invalid-log";
    }
    options.trustedInputs = true;
  }
  return options;
}

Renderer FixtureRenderer(const ProvenanceOptions& options, const Image& image,
                         std::string_view mode) {
  if (!options.rendererArguments.empty()) {
    return MakeProcessRenderer({.executable = options.renderer,
                                .workRoot = options.workRoot,
                                .arguments = options.rendererArguments,
                                .trustedInputs = true});
  }
  return [image, mode, calls = std::make_shared<int>(0)](const StaticCase& test, int,
                                                         int) -> ImageResult {
    if (mode == "--fixture-fail-first" && test.svgPath.stem() == "first") {
      return {{}, "intentional fixture failure"};
    }
    if (mode == "--fixture-fail-first-iteration" && ++*calls == 1) {
      return {Image{1, 1, 1, {0, 0, 255, 255}}, {}};
    }
    return {image, {}};
  };
}

int RunFixture(int argc, char** argv) {
  const std::filesystem::path renderer = std::filesystem::absolute(argv[0]);
  const std::filesystem::path root(argv[2]);
  const std::string_view fixtureMode = argc > 3 ? argv[3] : "";
  if (fixtureMode == "--fixture-empty-xml") {
    std::ofstream("test_detail.xml") << "stale-report-sentinel";
  }
  const Image image{1, 1, 1, {255, 0, 0, 255}};
  if (!CreateFixtureCorpus(root, image)) {
    return 3;
  }
  argc -= 2;
  argv += 2;
  GTEST_FLAG_SET(output, "xml:" + (root / "artifacts/gtest.xml").string());
  testing::InitGoogleTest(&argc, argv);
  if (fixtureMode == "--fixture-skip-second") {
    testing::UnitTest::GetInstance()->listeners().Append(new SkipSecondCase);
  }
  auto discovery = DiscoverCases({{"fixture", root / "corpus"}});
  const auto options = FixtureOptions(renderer, root, fixtureMode);
  const auto provenance = CreateRunProvenance(options, root / "artifacts");
  if (!provenance.run) {
    return 3;
  }
  Renderer callback = FixtureRenderer(options, image, fixtureMode);
  if (!options.rendererArguments.empty()) {
    for (auto& test : discovery.cases) {
      test.provenance = provenance.run;
    }
  }
  const auto issue = RegisterTests(discovery, callback, root / "artifacts");
  if (!issue.empty()) {
    return 3;
  }
  if (fixtureMode == "--fixture-direct-run") {
    return RUN_ALL_TESTS();
  }
  RunExecutionSummary execution;
  const int result = RunSelectedTests(&execution);
  if (fixtureMode == "--fixture-remove-xml") {
    std::filesystem::remove(execution.xmlReport);
  }
  const auto sourceError = fixtureMode == "--fixture-source-error" ? "source changed" : "";
  const auto error = WriteRunVerification(*provenance.run, sourceError, result, execution);
  return error.empty() || result != 0 ? result : 3;
}

class GtestSuiteTest : public testing::Test {
protected:
  void SetUp() override {
    const char* temporary = std::getenv("TEST_TMPDIR");
    ASSERT_NE(temporary, nullptr);
    std::string pattern = (std::filesystem::path(temporary) / "gtest-suite-XXXXXX").string();
    ASSERT_NE(mkdtemp(pattern.data()), nullptr);
    root_ = pattern;
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  int run(std::vector<std::string> flags) {
    std::vector<std::string> arguments{executable.string(), "--fixture-suite", root_.string()};
    arguments.insert(arguments.end(), flags.begin(), flags.end());
    std::vector<char*> pointers;
    for (auto& argument : arguments) {
      pointers.push_back(argument.data());
    }
    pointers.push_back(nullptr);
    const std::string childDirectory = root_.string();
    const std::string logPath = (root_ / "fixture.log").string();
    const pid_t child = fork();
    if (child < 0) {
      return -1;
    }
    if (child == 0) {
      // GoogleTest records its original working directory during static initialization.
      if (chdir(childDirectory.c_str()) != 0) {
        _exit(126);
      }
      const int log = open(logPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
      if (log < 0 || dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0) {
        _exit(126);
      }
      close(log);
      execv(pointers.front(), pointers.data());
      _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
      return -1;
    }
    return WEXITSTATUS(status);
  }

  std::string output() const {
    std::ifstream input(root_ / "fixture.log");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  std::string verification() const {
    std::ifstream input(root_ / "artifacts/run-verification.json");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  std::vector<std::string> reports(const std::filesystem::path& directory,
                                   std::string_view filename) const {
    std::vector<std::filesystem::path> paths;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(directory, error), end;
         !error && it != end; it.increment(error)) {
      if (it->path().filename() == filename) {
        paths.push_back(it->path());
      }
    }
    std::sort(paths.begin(), paths.end());
    std::vector<std::string> contents;
    for (const auto& path : paths) {
      std::ifstream input(path);
      contents.emplace_back(std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>());
    }
    return contents;
  }

  std::filesystem::path root_;
};

TEST(GtestLogTextTest, EscapesEveryNonPrintableByteAndBackslash) {
  std::string bytes;
  for (int value = 0; value < 256; ++value) {
    bytes += static_cast<char>(value);
  }
  const auto escaped = EscapeLogText(bytes);
  EXPECT_TRUE(std::all_of(escaped.begin(), escaped.end(),
                          [](unsigned char value) { return value >= 32 && value < 127; }));
  EXPECT_EQ(EscapeLogText("line\nnext\r\t\x1b\\"), "line\\x0anext\\x0d\\x09\\x1b\\x5c");
  EXPECT_EQ(EscapeLogText("\xe2\x80\xa8\xff"), "\\xe2\\x80\\xa8\\xff");
}

TEST(GtestLogTextTest, RejectsUntrustedRegisteredNamesBeforeExecution) {
  StaticCase test;
  test.id = "case\n[ RUN      ] Forged.Case";
  const auto error = RegisterTests(
      {{test}, {}}, [](const StaticCase&, int, int) { return ImageResult{}; }, "unused-artifacts");
  EXPECT_THAT(error, testing::HasSubstr("invalid case identity"));
}

TEST_F(GtestSuiteTest, RendererDiagnosticsCannotImpersonateComparisonLines) {
  EXPECT_EQ(run({"--fixture-spoof-diagnostics", "--gtest_filter=SvgSuite.*_66697273742e737667"}),
            1);
  const auto log = output();
  EXPECT_THAT(log, testing::Not(testing::HasSubstr("\n[ COMPARE ] forged.svg")));
  EXPECT_THAT(log, testing::Not(testing::HasSubstr("\r[ RUN      ] Forged.Case")));
  EXPECT_THAT(log, testing::HasSubstr("forged.svg"));
  EXPECT_THAT(log, testing::HasSubstr("mismatched pixels: unavailable"));
  const auto metrics = reports(root_ / "artifacts", "metrics.json");
  ASSERT_THAT(metrics, testing::SizeIs(1));
  EXPECT_THAT(metrics.front(), testing::HasSubstr("\"status\": \"error\""));
  EXPECT_THAT(metrics.front(), testing::HasSubstr("\"mismatched_pixels\": null"));
}

TEST_F(GtestSuiteTest, InvalidUtf8RendererDiagnosticsPreserveFailedOutcomeAndRawBytes) {
  EXPECT_EQ(run({"--fixture-invalid-diagnostics", "--gtest_filter=SvgSuite.*_66697273742e737667"}),
            1);
  EXPECT_THAT(output(), testing::HasSubstr("\\xff"));
  const auto metrics = reports(root_ / "artifacts", "metrics.json");
  ASSERT_THAT(metrics, testing::SizeIs(1));
  EXPECT_THAT(metrics.front(), testing::HasSubstr("\"status\": \"error\""));
  EXPECT_THAT(metrics.front(), testing::HasSubstr("\"mismatched_pixels\": null"));
  EXPECT_THAT(metrics.front(), testing::HasSubstr("\xef\xbf\xbd"));
  const auto raw = reports(root_ / "work", "renderer.log");
  ASSERT_THAT(raw, testing::SizeIs(1));
  EXPECT_THAT(raw.front(), testing::HasSubstr(std::string(1, static_cast<char>(0xff))));
  EXPECT_THAT(verification(), testing::HasSubstr("\"accepted\": false"));
}

TEST_F(GtestSuiteTest, CorpusPathsCannotImpersonateComparisonLines) {
  root_ /= "path\n[ COMPARE ] forged.svg [tiny]: FAIL (0 pixels differ)";
  std::filesystem::create_directories(root_);
  EXPECT_EQ(run({"--fixture-fail-first", "--gtest_filter=SvgSuite.*_66697273742e737667"}), 1);
  EXPECT_THAT(output(), testing::Not(testing::HasSubstr("\n[ COMPARE ] forged.svg")));
}

TEST_F(GtestSuiteTest, ListingCasesCannotCountAsExecution) {
  EXPECT_EQ(run({"--gtest_list_tests"}), 2);
  EXPECT_THAT(verification(), testing::HasSubstr("\"completed_cases\": 0"));
  EXPECT_THAT(verification(), testing::HasSubstr("\"corpus_complete\": false"));
}

TEST_F(GtestSuiteTest, ZeroRepeatsCannotCountAsExecution) {
  EXPECT_EQ(run({"--gtest_repeat=0"}), 2);
  EXPECT_THAT(verification(), testing::HasSubstr("\"completed_cases\": 0"));
  EXPECT_THAT(verification(), testing::HasSubstr("\"corpus_complete\": false"));
}

TEST_F(GtestSuiteTest, EmptyFilterCannotCountAsExecution) {
  EXPECT_EQ(run({"--gtest_filter=NothingMatches.*"}), 2);
}

TEST_F(GtestSuiteTest, MatchingFilterExecutesAndPasses) {
  EXPECT_EQ(run({"--gtest_filter=SvgSuite.*"}), 0);
}

TEST_F(GtestSuiteTest, NarrowFilterCannotClaimCompleteCorpus) {
  EXPECT_EQ(run({"--gtest_filter=SvgSuite.*_66697273742e737667"}), 0);
  const std::string record = verification();
  EXPECT_THAT(record, testing::HasSubstr("\"accepted\": true"));
  EXPECT_THAT(record, testing::HasSubstr("\"accepted_scope\": \"selected-cases\""));
  EXPECT_THAT(record, testing::HasSubstr("\"discovered_cases\": 2"));
  EXPECT_THAT(record, testing::HasSubstr("\"selected_cases\": 1"));
  EXPECT_THAT(record, testing::HasSubstr("\"completed_cases\": 1"));
  EXPECT_THAT(record, testing::HasSubstr("\"complete_corpus_accepted\": false"));
}

TEST_F(GtestSuiteTest, FullCorpusRecordsCompletionAndDefaultXml) {
  EXPECT_EQ(run({"--gtest_filter=SvgSuite.*"}), 0);
  const std::string record = verification();
  EXPECT_THAT(record, testing::HasSubstr("\"discovered_cases\": 2"));
  EXPECT_THAT(record, testing::HasSubstr("\"selected_cases\": 2"));
  EXPECT_THAT(record, testing::HasSubstr("\"completed_cases\": 2"));
  EXPECT_THAT(record, testing::HasSubstr("\"completed_invocations\": 2"));
  EXPECT_THAT(record, testing::HasSubstr("\"complete_corpus_accepted\": true"));
  EXPECT_THAT(record, testing::HasSubstr("\"accepted_scope\": \"complete-corpus\""));
  EXPECT_THAT(record, testing::HasSubstr("artifacts/gtest.xml"));
  EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "artifacts/gtest.xml"));
}

TEST_F(GtestSuiteTest, RepeatsDoNotCountAsDistinctCompletedCases) {
  EXPECT_EQ(run({"--gtest_filter=SvgSuite.*_66697273742e737667", "--gtest_repeat=2"}), 0);
  const std::string record = verification();
  EXPECT_THAT(record, testing::HasSubstr("\"completed_cases\": 1"));
  EXPECT_THAT(record, testing::HasSubstr("\"completed_invocations\": 2"));
  EXPECT_THAT(record, testing::HasSubstr("\"requested_repeat_count\": 2"));
  EXPECT_THAT(record, testing::HasSubstr("\"corpus_complete\": false"));
}

TEST_F(GtestSuiteTest, DirectGoogleTestRunnerRetainsEveryIteration) {
  EXPECT_EQ(run({"--fixture-direct-run", "--gtest_filter=SvgSuite.*_66697273742e737667",
                 "--gtest_repeat=2"}),
            0);
  EXPECT_THAT(reports(root_ / "artifacts", "metrics.json"), testing::SizeIs(2));
}

TEST_F(GtestSuiteTest, RepeatedFullCorpusKeepsUniqueCounts) {
  EXPECT_EQ(run({"--fixture-process", "--gtest_filter=SvgSuite.*", "--gtest_repeat=2"}), 0);
  EXPECT_THAT(reports(root_ / "artifacts", "metrics.json"), testing::SizeIs(4));
  EXPECT_THAT(reports(root_ / "work", "process.json"), testing::SizeIs(4));
  EXPECT_THAT(verification(), testing::HasSubstr("\"completed_cases\": 2"));
  EXPECT_THAT(verification(), testing::HasSubstr("\"completed_invocations\": 4"));
  EXPECT_THAT(verification(), testing::HasSubstr("\"complete_corpus_accepted\": true"));
}

TEST_F(GtestSuiteTest, RepeatedProcessesRetainEveryInvocation) {
  EXPECT_EQ(run({"--fixture-process", "--gtest_filter=SvgSuite.*_66697273742e737667",
                 "--gtest_repeat=2"}),
            0);
  const auto cases = reports(root_ / "artifacts", "metrics.json");
  const auto processes = reports(root_ / "work", "process.json");
  ASSERT_THAT(cases, testing::SizeIs(2));
  ASSERT_THAT(processes, testing::SizeIs(2));
  for (size_t index = 0; index < 2; ++index) {
    const std::string marker = "\"iteration\": " + std::to_string(index + 1);
    EXPECT_THAT(cases[index], testing::HasSubstr(marker));
    EXPECT_THAT(processes[index], testing::HasSubstr(marker));
    EXPECT_THAT(cases[index], testing::HasSubstr("iteration-" + std::to_string(index + 1)));
  }
  EXPECT_THAT(verification(), testing::HasSubstr("\"accepted\": true"));
}

TEST_F(GtestSuiteTest, RepeatedFailureSurvivesALaterPassingImage) {
  EXPECT_EQ(run({"--fixture-fail-first-iteration", "--gtest_filter=SvgSuite.*_66697273742e737667",
                 "--gtest_repeat=2"}),
            1);
  const auto cases = reports(root_ / "artifacts", "metrics.json");
  ASSERT_THAT(cases, testing::SizeIs(2));
  EXPECT_THAT(cases[0], testing::HasSubstr("\"status\": \"fail\""));
  EXPECT_THAT(cases[1], testing::HasSubstr("\"status\": \"pass\""));
  EXPECT_THAT(verification(), testing::HasSubstr("\"accepted\": false"));
}

TEST_F(GtestSuiteTest, FailFastSkipsDoNotCountAsCompletedCases) {
  EXPECT_EQ(run({"--fixture-fail-first", "--gtest_filter=SvgSuite.*", "--gtest_fail_fast"}), 1);
  const std::string record = verification();
  EXPECT_THAT(record, testing::HasSubstr("\"selected_cases\": 2"));
  EXPECT_THAT(record, testing::HasSubstr("\"completed_cases\": 1"));
  EXPECT_THAT(record, testing::HasSubstr("\"skipped_invocations\": 1"));
  EXPECT_THAT(record, testing::HasSubstr("\"corpus_complete\": false"));
}

TEST_F(GtestSuiteTest, InvalidUtf8XmlDestinationRejectsBeforeAnyCaseExecutes) {
  EXPECT_EQ(run({"--gtest_filter=SvgSuite.*",
                 "--gtest_output=xml:invalid-" + std::string(1, char(0xFF))}),
            2);
  const auto record = verification();
  EXPECT_THAT(record, testing::HasSubstr("\"accepted\": false"));
  EXPECT_THAT(record, testing::HasSubstr("\"completed_invocations\": 0"));
  EXPECT_THAT(record, testing::HasSubstr("\"gtest_output\": null"));
  EXPECT_TRUE(reports(root_ / "artifacts", "metrics.json").empty());
}

TEST_F(GtestSuiteTest, UnicodeXmlDestinationKeepsExactEvidenceIdentity) {
  const auto path = root_ / "chosen-\xC3\xA9.xml";
  EXPECT_EQ(run({"--gtest_filter=SvgSuite.*", "--gtest_output=xml:" + path.string()}), 0);
  EXPECT_TRUE(std::filesystem::is_regular_file(path));
  EXPECT_THAT(verification(), testing::HasSubstr("chosen-\xC3\xA9.xml"));
}

TEST_F(GtestSuiteTest, SourceErrorWithValidXmlReturnsNonzero) {
  EXPECT_EQ(run({"--fixture-source-error", "--gtest_filter=SvgSuite.*"}), 3);
  const auto record = verification();
  EXPECT_THAT(record, testing::HasSubstr("\"accepted\": false"));
  EXPECT_THAT(record, testing::HasSubstr("\"completed_invocations\": 2"));
  EXPECT_THAT(record, testing::HasSubstr("\"xml_report\": {"));
}

TEST_F(GtestSuiteTest, ExplicitXmlOutputIsPreserved) {
  const auto path = root_ / "chosen.xml";
  EXPECT_EQ(run({"--gtest_filter=SvgSuite.*", "--gtest_output=xml:" + path.string()}), 0);
  EXPECT_TRUE(std::filesystem::is_regular_file(path));
  EXPECT_FALSE(std::filesystem::exists(root_ / "artifacts/gtest.xml"));
  EXPECT_THAT(verification(), testing::HasSubstr("chosen.xml"));
}

TEST_F(GtestSuiteTest, SuccessfulAndSkippedSelectionCannotReturnSuccess) {
  EXPECT_EQ(run({"--fixture-skip-second", "--gtest_filter=SvgSuite.*"}), 2);
  const std::string record = verification();
  EXPECT_THAT(record, testing::HasSubstr("\"selected_cases\": 2"));
  EXPECT_THAT(record, testing::HasSubstr("\"completed_cases\": 1"));
  EXPECT_THAT(record, testing::HasSubstr("\"skipped_invocations\": 1"));
  EXPECT_THAT(record, testing::HasSubstr("\"accepted\": false"));
}

TEST_F(GtestSuiteTest, JsonOutputCannotCertifyWithoutXml) {
  const auto output = root_ / "chosen.json";
  EXPECT_EQ(run({"--gtest_filter=SvgSuite.*", "--gtest_output=json:" + output.string()}), 2);
  EXPECT_FALSE(std::filesystem::exists(output));
  EXPECT_THAT(verification(), testing::HasSubstr("\"accepted\": false"));
  EXPECT_THAT(verification(), testing::HasSubstr("\"complete_corpus_accepted\": false"));
  EXPECT_THAT(verification(), testing::HasSubstr("\"xml_report\": null"));
}

TEST_F(GtestSuiteTest, DirectoryXmlOutputCannotCertifyAnUnknownReport) {
  const auto output = root_ / "reports";
  EXPECT_EQ(run({"--gtest_filter=SvgSuite.*", "--gtest_output=xml:" + output.string() + "/"}), 2);
  EXPECT_THAT(verification(), testing::HasSubstr("\"accepted\": false"));
  EXPECT_THAT(verification(), testing::HasSubstr("\"xml_report\": null"));
}

TEST_F(GtestSuiteTest, ExistingExplicitXmlCannotBeOverwrittenOrCertified) {
  const auto output = root_ / "existing.xml";
  std::ofstream(output) << "existing-report-sentinel";
  EXPECT_EQ(run({"--gtest_filter=SvgSuite.*", "--gtest_output=xml:" + output.string()}), 2);
  std::ifstream input(output);
  const std::string contents{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
  EXPECT_EQ(contents, "existing-report-sentinel");
  EXPECT_THAT(verification(), testing::HasSubstr("\"accepted\": false"));
}

TEST_F(GtestSuiteTest, RemovedXmlReportCannotCertifyCompletedCases) {
  EXPECT_EQ(run({"--fixture-remove-xml", "--gtest_filter=SvgSuite.*"}), 3);
  EXPECT_THAT(verification(), testing::HasSubstr("\"completed_cases\": 2"));
  EXPECT_THAT(verification(), testing::HasSubstr("\"accepted\": false"));
  EXPECT_THAT(verification(), testing::HasSubstr("\"complete_corpus_accepted\": false"));
  EXPECT_THAT(verification(), testing::HasSubstr("\"xml_report\": null"));
}

TEST_F(GtestSuiteTest, EmptyXmlSuffixCannotReferenceStaleDefaultReport) {
  EXPECT_EQ(run({"--fixture-empty-xml", "--gtest_filter=SvgSuite.*", "--gtest_output=xml:"}), 2);
  const std::string record = verification();
  EXPECT_THAT(record, testing::HasSubstr("\"gtest_output\": \"xml:\""));
  EXPECT_THAT(record, testing::HasSubstr("\"xml_report\": null"));
  std::ifstream stale(root_ / "test_detail.xml");
  const std::string contents{std::istreambuf_iterator<char>(stale),
                             std::istreambuf_iterator<char>()};
  EXPECT_EQ(contents, "stale-report-sentinel");
  EXPECT_FALSE(std::filesystem::exists(root_ / (root_.filename().string() + ".xml")));
}

}  // namespace
}  // namespace donner::svg_suite

int main(int argc, char** argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--fixture-invalid-log") {
    std::cerr << "invalid diagnostic: ";
    std::cerr.put(static_cast<char>(0xff));
    return 7;
  }
  if (argc == 3 && std::string_view(argv[1]) == "--fixture-spoof-log") {
    std::cerr << "renderer failure\n[ COMPARE ] forged.svg [tiny]: FAIL (0 pixels differ)"
                 "\r[ RUN      ] Forged.Case\n";
    return 7;
  }
  if (argc == 3 && std::string_view(argv[1]) == "--fixture-render-png") {
    const donner::svg_suite::Image image{1, 1, 1, {255, 0, 0, 255}};
    return donner::svg_suite::WritePng(argv[2], {1, 1, 1, image.data}).empty() ? 0 : 1;
  }
  if (argc >= 3 && std::string_view(argv[1]) == "--fixture-suite") {
    return donner::svg_suite::RunFixture(argc, argv);
  }
  donner::svg_suite::executable = std::filesystem::absolute(argv[0]);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
