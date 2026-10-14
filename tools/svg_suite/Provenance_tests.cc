#include "tools/svg_suite/Provenance.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>

#include "tools/svg_suite/StaticSuite.h"

namespace donner::svg_suite {
namespace {
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Not;

class ProvenanceTest : public testing::Test {
protected:
  void SetUp() override {
    static std::atomic<unsigned> serial{0};
    const char* base = std::getenv("TEST_TMPDIR");
    directory_ = (base ? std::filesystem::path(base) : std::filesystem::temp_directory_path()) /
                 ("svg-provenance-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                  "-" + std::to_string(serial++));
    std::filesystem::create_directories(directory_ / "corpus/tests");
    std::filesystem::create_directories(directory_ / "corpus/fonts");
    std::filesystem::create_directories(directory_ / "corpus/resources");
    write("renderer", "trusted executable fixture");
    write("corpus/tests/a.svg", "<svg/>");
    write("corpus/fonts/font.ttf", "font bytes");
    write("corpus/resources/resource.dat", "resource bytes");
    ASSERT_THAT(WritePng(directory_ / "corpus/tests/a.png", view(red())), IsEmpty());
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  void write(const std::filesystem::path& relative, std::string_view text) {
    std::ofstream output(directory_ / relative, std::ios::binary);
    output << text;
  }

  static Image red() { return {1, 1, 1, {255, 0, 0, 255}}; }
  static ImageView view(const Image& image) {
    return {image.width, image.height, image.strideInPixels, image.data};
  }

  ProvenanceOptions options() const {
    ProvenanceOptions result;
    result.corpora = {{"fixture", directory_ / "corpus"}};
    result.renderer = directory_ / "renderer";
    result.workRoot = directory_ / "output/work";
    result.invocation = {"native-suite", "--renderer", result.renderer.string(),
                         "--gtest_filter=*fixture*"};
    result.rendererArguments = {"--input", "{input}", "--output", "{output}"};
    result.engineRevision = "claimed-revision";
    result.backend = "claimed-backend";
    result.buildFlags = "-DCLAIMED_FLAG";
    return result;
  }

  std::filesystem::path directory_;
};

TEST_F(ProvenanceTest, InvalidUtf8RevisionIsRejectedBeforeCapture) {
  auto config = options();
  config.engineRevision = std::string("invalid-") + char(0xFF);
  const auto result = CreateRunProvenance(config, directory_ / "output");
  EXPECT_FALSE(result.run);
  EXPECT_THAT(result.error, HasSubstr("UTF-8"));
  EXPECT_FALSE(std::filesystem::exists(directory_ / "output/run.json"));
}

TEST_F(ProvenanceTest, AllDeclaredIdentitiesRejectInvalidEncodingBeforeSourceCapture) {
  for (int field = 0; field < 8; ++field) {
    auto config = options();
    config.invocation = {"suite"};
    config.rendererArguments = {"{input}"};
    const std::vector<std::string*> fields = {
        &config.engineRevision,      &config.backend,
        &config.buildFlags,          &config.corpora[0].name,
        &config.corpora[0].revision, &config.corpora[0].revisionEvidence,
        &config.invocation[0],       &config.rendererArguments[0]};
    *fields[field] = std::string("invalid-") + char(0xFF);
    config.renderer = directory_ / "missing-renderer";
    const auto result = CreateRunProvenance(config, directory_ / "output");
    EXPECT_FALSE(result.run);
    EXPECT_THAT(result.error, HasSubstr("UTF-8")) << field;
  }
}

TEST_F(ProvenanceTest, UnicodeAndLiteralEscapesKeepDistinctExactIdentities) {
  auto config = options();
  config.engineRevision = "revision-\xC3\xA9";
  config.backend = "\\xff";
  config.buildFlags = "\xEF\xBF\xBD";
  write("corpus/resources/unicode-\xC3\xA9.dat", std::string(1, char(0xFF)));
  const auto result = CreateRunProvenance(config, directory_ / "output");
  ASSERT_TRUE(result.run) << result.error;
  std::ifstream input(result.run->recordPath);
  const auto record = nlohmann::json::parse(input);
  EXPECT_EQ(record["renderer"]["declared"]["revision"], config.engineRevision);
  EXPECT_EQ(record["renderer"]["declared"]["backend"], config.backend);
  EXPECT_EQ(record["renderer"]["declared"]["build_flags"], config.buildFlags);
  EXPECT_TRUE(record["corpora"][0]["files"].contains("resources/unicode-\xC3\xA9.dat"));
  EXPECT_THAT(VerifyRunProvenance(*result.run), IsEmpty());
}

TEST_F(ProvenanceTest, InvalidVerificationDiagnosticsRemainSafeRejectedEvidence) {
  const auto run = CreateRunProvenance(options(), directory_ / "output");
  ASSERT_TRUE(run.run) << run.error;
  RunExecutionSummary execution;
  execution.gtestOutput = std::string("xml:") + char(0xFF);
  execution.xmlReport = directory_ / std::string("bad-").append(1, char(0xFF));
  const auto issue =
      WriteRunVerification(*run.run, std::string("changed-") + char(0xFF), 0, execution);
  EXPECT_THAT(issue, HasSubstr("UTF-8"));
  std::ifstream input(directory_ / "output/run-verification.json");
  const auto record = nlohmann::json::parse(input);
  EXPECT_EQ(record["accepted"], false);
  EXPECT_EQ(record["complete_corpus_accepted"], false);
  EXPECT_EQ(record["execution"]["gtest_output"], nullptr);
  EXPECT_EQ(record["xml_report"], nullptr);
  EXPECT_EQ(record["verification_error"], "changed-\xEF\xBF\xBD");
  EXPECT_EQ(record["diagnostics_encoding"], "utf8-with-replacement");
}

#if defined(__linux__)
TEST_F(ProvenanceTest, InvalidNativeResourceNameRejectsCaptureAndFinalVerification) {
  write("corpus/resources/bad-\\xff", "literal backslash escape");
  write("corpus/resources/bad-\xEF\xBF\xBD", "valid replacement scalar");
  const auto run = CreateRunProvenance(options(), directory_ / "output");
  ASSERT_TRUE(run.run) << run.error;
  std::ifstream initial(run.run->recordPath);
  const auto captured = nlohmann::json::parse(initial);
  EXPECT_TRUE(captured["corpora"][0]["files"].contains("resources/bad-\\xff"));
  EXPECT_TRUE(captured["corpora"][0]["files"].contains("resources/bad-\xEF\xBF\xBD"));
  write(std::filesystem::path("corpus/resources") / std::string("bad-").append(1, char(0xFF)),
        "data");
  const auto rejected = CreateRunProvenance(options(), directory_ / "other-output");
  EXPECT_FALSE(rejected.run);
  EXPECT_THAT(rejected.error, HasSubstr("UTF-8"));
  const auto issue = VerifyRunProvenance(*run.run);
  EXPECT_THAT(issue, HasSubstr("UTF-8"));
  EXPECT_THAT(WriteRunVerification(*run.run, issue, 0), Not(IsEmpty()));
  std::ifstream input(directory_ / "output/run-verification.json");
  EXPECT_EQ(nlohmann::json::parse(input)["accepted"], false);
}
#endif

TEST_F(ProvenanceTest, HandcraftedInvalidCaseProvenanceNeverInvokesCallback) {
  auto discovery = DiscoverCases({{"fixture", directory_ / "corpus"}});
  ASSERT_THAT(discovery.error, IsEmpty());
  auto run = std::make_shared<RunProvenance>();
  run->runId = std::string("bad-") + char(0xFF);
  run->recordPath = directory_ / "run.json";
  run->workRoot = directory_ / "work";
  discovery.cases.front().provenance = run;
  bool called = false;
  const auto result = RunCase(
      discovery.cases.front(),
      [&called](const StaticCase&, int, int) {
        called = true;
        return ImageResult{red(), {}};
      },
      directory_ / "output");
  EXPECT_FALSE(called);
  EXPECT_FALSE(result.passed);
  EXPECT_THAT(result.error, HasSubstr("UTF-8"));
  EXPECT_TRUE(result.artifactDirectory.empty());
}

TEST_F(ProvenanceTest, CallbackDiagnosticReplacementCannotAlterIdentityFields) {
  const auto run = CreateRunProvenance(options(), directory_ / "output");
  ASSERT_TRUE(run.run) << run.error;
  auto discovery = DiscoverCases({{"fixture", directory_ / "corpus"}});
  ASSERT_THAT(discovery.error, IsEmpty());
  auto test = discovery.cases.front();
  test.provenance = run.run;
  const auto result = RunCase(
      test,
      [](const StaticCase&, int, int) { return ImageResult{{}, std::string(65537, char(0xFF))}; },
      directory_ / "output");
  EXPECT_FALSE(result.passed);
  EXPECT_FALSE(result.mismatchedPixels);
  std::ifstream input(result.artifactDirectory / "metrics.json");
  const auto record = nlohmann::json::parse(input);
  EXPECT_EQ(record["run_id"], run.run->runId);
  EXPECT_EQ(record["test_id"], test.id);
  EXPECT_EQ(record["input"], "tests/a.svg");
  EXPECT_EQ(record["status"], "error");
  EXPECT_EQ(record["mismatched_pixels"], nullptr);
  EXPECT_EQ(record["diagnostics_encoding"], "utf8-with-replacement");
  EXPECT_EQ(record["diagnostics_truncated"], true);
  EXPECT_EQ(record["diagnostics"].get<std::string>().size(),
            65536u * 3u + std::string("[diagnostic truncated]").size());
}

TEST_F(ProvenanceTest, InvalidRunIdentityStillWritesRejectedEvidenceAtValidDestination) {
  const auto created = CreateRunProvenance(options(), directory_ / "output");
  ASSERT_TRUE(created.run) << created.error;
  auto run = *created.run;
  run.runId = std::string("invalid-") + char(0xFF);
  EXPECT_THAT(WriteRunVerification(run, "", 0), HasSubstr("UTF-8"));
  std::ifstream input(directory_ / "output/run-verification.json");
  const auto record = nlohmann::json::parse(input);
  EXPECT_EQ(record["run_id"], nullptr);
  EXPECT_EQ(record["accepted"], false);
  EXPECT_EQ(record["complete_corpus_accepted"], false);
  EXPECT_THAT(record["identity_error"].get<std::string>(), HasSubstr("UTF-8"));
}

TEST_F(ProvenanceTest, SourceErrorReturnsFailureEvenWithValidXmlEvidence) {
  const auto run = CreateRunProvenance(options(), directory_ / "output");
  ASSERT_TRUE(run.run) << run.error;
  write("output/report.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><testsuites></testsuites>\n");
  RunExecutionSummary execution;
  execution.discoveredCases = execution.selectedCases = execution.completedCases = 1;
  execution.completedInvocations = 1;
  execution.gtestOutput = "xml";
  execution.xmlReport = directory_ / "output/report.xml";
  EXPECT_THAT(WriteRunVerification(*run.run, std::string("changed-") + char(0xFF), 0, execution),
              HasSubstr("changed-"));
  std::ifstream input(directory_ / "output/run-verification.json");
  const auto record = nlohmann::json::parse(input);
  EXPECT_EQ(record["accepted"], false);
  EXPECT_FALSE(record["xml_report"].is_null());
}

TEST_F(ProvenanceTest, RunBindsActualBytesAndKeepsLabelsUnverified) {
  const auto run = CreateRunProvenance(options(), directory_ / "output");
  ASSERT_TRUE(run.run) << run.error;
  EXPECT_EQ(run.run->files.size(), 4u);
  EXPECT_THAT(VerifyRunProvenance(*run.run), IsEmpty());
  std::ifstream input(run.run->recordPath);
  const auto record = nlohmann::json::parse(input);
  EXPECT_EQ(record["run_id"], run.run->runId);
  EXPECT_EQ(record["renderer"]["declared"]["verification"], "caller-declared, unverified");
  EXPECT_EQ(record["renderer"]["declared"]["revision"], "claimed-revision");
  EXPECT_EQ(record["invocation_argv"], options().invocation);
  EXPECT_TRUE(record["corpora"][0]["files"].contains("fonts/font.ttf"));
  EXPECT_TRUE(record["corpora"][0]["files"].contains("resources/resource.dat"));
  EXPECT_THAT(CreateRunProvenance(options(), directory_ / "output").error,
              HasSubstr("already exists"));
}

TEST_F(ProvenanceTest, FileAndAggregateBudgetsFailClosed) {
  auto config = options();
  config.limits.maxFiles = 2;
  EXPECT_FALSE(CreateRunProvenance(config, directory_ / "output").run);
  config = options();
  config.limits.maxTotalBytes = 4;
  EXPECT_FALSE(CreateRunProvenance(config, directory_ / "output").run);
  config = options();
  config.limits.maxFileBytes = 4;
  EXPECT_FALSE(CreateRunProvenance(config, directory_ / "output").run);
  EXPECT_FALSE(std::filesystem::exists(directory_ / "output/run.json"));
}

TEST_F(ProvenanceTest, CorpusEscapesAndOutputInsideCorpusAreRejected) {
  std::filesystem::create_symlink(directory_ / "renderer", directory_ / "corpus/resources/escape");
  EXPECT_FALSE(CreateRunProvenance(options(), directory_ / "output").run);
  std::filesystem::remove(directory_ / "corpus/resources/escape");
  EXPECT_FALSE(CreateRunProvenance(options(), directory_ / "corpus/output").run);
}

TEST_F(ProvenanceTest, FinalVerificationIsCreateOnlyAndRecordsRejectedRun) {
  const auto run = CreateRunProvenance(options(), directory_ / "output");
  ASSERT_TRUE(run.run) << run.error;
  ASSERT_THAT(WriteRunVerification(*run.run, "resource changed", 0), HasSubstr("XML"));
  std::ifstream input(directory_ / "output/run-verification.json");
  const auto record = nlohmann::json::parse(input);
  EXPECT_EQ(record["accepted"], false);
  EXPECT_EQ(record["sources_unchanged_at_verification"], false);
  EXPECT_EQ(record["run_id"], run.run->runId);
  EXPECT_THAT(WriteRunVerification(*run.run, "", 0), HasSubstr("already exists"));
}

TEST_F(ProvenanceTest, MissingExecutionObservationsCannotCertifySuccess) {
  const auto run = CreateRunProvenance(options(), directory_ / "output");
  ASSERT_TRUE(run.run) << run.error;
  ASSERT_THAT(WriteRunVerification(*run.run, "", 0), HasSubstr("XML"));
  std::ifstream input(directory_ / "output/run-verification.json");
  const auto record = nlohmann::json::parse(input);
  EXPECT_EQ(record["accepted"], false);
  EXPECT_EQ(record["complete_corpus_accepted"], false);
  EXPECT_EQ(record["execution"]["completed_cases"], 0);
  EXPECT_EQ(record["xml_report"], nullptr);
}

TEST_F(ProvenanceTest, FilteredExecutionRecordsSelectionScopeAndReportFingerprint) {
  const auto run = CreateRunProvenance(options(), directory_ / "output");
  ASSERT_TRUE(run.run) << run.error;
  write("output/report.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><testsuites></testsuites>\n");
  RunExecutionSummary execution;
  execution.discoveredCases = 5;
  execution.selectedCases = 2;
  execution.completedCases = 2;
  execution.completedInvocations = 4;
  execution.repeatCount = 2;
  execution.xmlReport = directory_ / "output/report.xml";
  execution.gtestOutput = "xml:" + execution.xmlReport.string();
  ASSERT_THAT(WriteRunVerification(*run.run, "", 0, execution), IsEmpty());
  std::ifstream input(directory_ / "output/run-verification.json");
  const auto record = nlohmann::json::parse(input);
  EXPECT_EQ(record["accepted"], true);
  EXPECT_EQ(record["accepted_scope"], "selected-cases");
  EXPECT_EQ(record["complete_corpus_accepted"], false);
  EXPECT_EQ(record["execution"]["discovered_cases"], 5);
  EXPECT_EQ(record["execution"]["selected_cases"], 2);
  EXPECT_EQ(record["execution"]["completed_cases"], 2);
  EXPECT_EQ(record["execution"]["completed_invocations"], 4);
  EXPECT_EQ(record["execution"]["requested_repeat_count"], 2);
  const auto report = FingerprintFile(execution.xmlReport, 1024);
  ASSERT_TRUE(report.fingerprint);
  EXPECT_EQ(record["xml_report"]["sha256"], report.fingerprint->sha256);
  EXPECT_EQ(record["xml_report"]["bytes"], report.fingerprint->bytes);
}

TEST_F(ProvenanceTest, UnexecutedRunCannotClaimExistingXmlAsGeneratedEvidence) {
  const auto run = CreateRunProvenance(options(), directory_ / "output");
  ASSERT_TRUE(run.run) << run.error;
  write("output/stale.xml", "<testsuites/>\n");
  RunExecutionSummary execution;
  execution.discoveredCases = 1;
  execution.selectedCases = 1;
  execution.repeatCount = 0;
  execution.xmlReport = directory_ / "output/stale.xml";
  ASSERT_THAT(WriteRunVerification(*run.run, "", 0, execution), HasSubstr("XML"));
  std::ifstream input(directory_ / "output/run-verification.json");
  const auto record = nlohmann::json::parse(input);
  EXPECT_EQ(record["accepted"], false);
  EXPECT_EQ(record["execution"]["corpus_complete"], false);
  EXPECT_EQ(record["xml_report"], nullptr);
}

TEST_F(ProvenanceTest, XmlEvidenceFailuresRejectOtherwiseCompleteRuns) {
  for (const std::string_view mode :
       {"missing", "empty", "oversized", "unreadable", "directory", "non-xml", "truncated"}) {
    SCOPED_TRACE(mode);
    const auto output = directory_ / std::string(mode);
    const auto run = CreateRunProvenance(options(), output);
    ASSERT_TRUE(run.run) << run.error;
    const auto reportPath = output / "report.xml";
    if (mode == "directory") {
      std::filesystem::create_directory(reportPath);
    } else if (mode != "missing") {
      std::ofstream(reportPath) << (mode == "empty" ? ""
                                    : mode == "truncated"
                                        ? "<?xml version=\"1.0\" encoding=\"UTF-8\"?><testsuites>"
                                        : "not XML");
      if (mode == "oversized") {
        std::filesystem::resize_file(reportPath, 32u * 1024u * 1024u + 1u);
      } else if (mode == "unreadable") {
        std::filesystem::permissions(reportPath, std::filesystem::perms::none);
      }
    }
    RunExecutionSummary execution;
    execution.discoveredCases = 1;
    execution.selectedCases = 1;
    execution.completedCases = 1;
    execution.completedInvocations = 1;
    execution.gtestOutput = "xml:" + reportPath.string();
    execution.xmlReport = reportPath;
    EXPECT_THAT(WriteRunVerification(*run.run, "", 0, execution), Not(IsEmpty()));
    std::ifstream input(output / "run-verification.json");
    const auto record = nlohmann::json::parse(input);
    EXPECT_EQ(record["accepted"], false);
    EXPECT_EQ(record["complete_corpus_accepted"], false);
    EXPECT_EQ(record["xml_report"], nullptr);
    EXPECT_THAT(record["xml_report_error"].get<std::string>(), Not(IsEmpty()));
  }
}

TEST_F(ProvenanceTest, ResourceMutationRejectsAggregateRun) {
  const auto run = CreateRunProvenance(options(), directory_ / "output");
  ASSERT_TRUE(run.run) << run.error;
  write("corpus/resources/resource.dat", "Resource bytes");
  EXPECT_THAT(VerifyRunProvenance(*run.run), HasSubstr("changed"));
}

TEST_F(ProvenanceTest, AddedResourceRejectsAggregateRun) {
  const auto run = CreateRunProvenance(options(), directory_ / "output");
  ASSERT_TRUE(run.run) << run.error;
  write("corpus/resources/new.dat", "new resource");
  EXPECT_THAT(VerifyRunProvenance(*run.run), HasSubstr("membership"));
}

TEST_F(ProvenanceTest, EngineMutationRejectsAggregateRun) {
  const auto run = CreateRunProvenance(options(), directory_ / "output");
  ASSERT_TRUE(run.run) << run.error;
  write("renderer", "different engine");
  EXPECT_THAT(VerifyRunProvenance(*run.run), HasSubstr("renderer provenance"));
}

TEST_F(ProvenanceTest, ChangedSourceIsNotRenderedOrReportedAsPassing) {
  const auto run = CreateRunProvenance(options(), directory_ / "output");
  ASSERT_TRUE(run.run) << run.error;
  auto discovery = DiscoverCases({{"fixture", directory_ / "corpus"}});
  ASSERT_EQ(discovery.cases.size(), 1u);
  discovery.cases[0].provenance = run.run;
  write("corpus/tests/a.svg", "<svg >");
  bool rendered = false;
  const auto result = RunCase(
      discovery.cases[0],
      [&](const StaticCase&, int, int) {
        rendered = true;
        return ImageResult{red(), {}};
      },
      directory_ / "output");
  EXPECT_FALSE(rendered);
  EXPECT_FALSE(result.passed);
  EXPECT_THAT(result.error, HasSubstr("changed"));
  std::ifstream input(result.artifactDirectory / "metrics.json");
  const auto record = nlohmann::json::parse(input);
  EXPECT_EQ(record["run_id"], run.run->runId);
  EXPECT_EQ(record["status"], "error");
  EXPECT_EQ(record["input_fingerprint"]["sha256"],
            FindFingerprint(*run.run, discovery.cases[0].svgPath)->sha256);
  EXPECT_THAT(record["process_record"].get<std::string>(), HasSubstr("process/process.json"));
}

TEST_F(ProvenanceTest, ReferenceChangedByRendererCannotBlessItsOutput) {
  const auto run = CreateRunProvenance(options(), directory_ / "output");
  ASSERT_TRUE(run.run) << run.error;
  auto discovery = DiscoverCases({{"fixture", directory_ / "corpus"}});
  ASSERT_EQ(discovery.cases.size(), 1u);
  discovery.cases[0].provenance = run.run;
  const auto result = RunCase(
      discovery.cases[0],
      [&](const StaticCase& test, int, int) {
        Image blue{1, 1, 1, {0, 0, 255, 255}};
        EXPECT_THAT(WritePng(test.expectedPath, view(blue)), IsEmpty());
        return ImageResult{red(), {}};
      },
      directory_ / "output");
  EXPECT_FALSE(result.passed);
  EXPECT_THAT(result.error, HasSubstr("changed"));
}
}  // namespace
}  // namespace donner::svg_suite
