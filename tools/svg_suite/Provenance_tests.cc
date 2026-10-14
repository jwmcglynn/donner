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

TEST_F(ProvenanceTest, StandardSha256KnownVectorsAndStreaming) {
  const std::vector<std::pair<std::string, std::string>> vectors = {
      {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
      {std::string(1000000, 'a'),
       "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"}};
  for (const auto& [input, expected] : vectors) {
    write("vector", input);
    const auto result = FingerprintFile(directory_ / "vector", 1000000);
    ASSERT_TRUE(result.fingerprint) << result.error;
    EXPECT_EQ(result.fingerprint->sha256, expected);
    EXPECT_EQ(result.fingerprint->bytes, input.size());
  }
}

TEST_F(ProvenanceTest, HashingRejectsNonregularMissingAndOversizedSources) {
  EXPECT_FALSE(FingerprintFile(directory_ / "corpus", 1024).fingerprint);
  EXPECT_FALSE(FingerprintFile(directory_ / "missing", 1024).fingerprint);
  EXPECT_FALSE(FingerprintFile(directory_ / "renderer", 1).fingerprint);
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

TEST_F(ProvenanceTest, SameLengthSourceMutationIsDetected) {
  const auto snapshot = FingerprintFile(directory_ / "corpus/tests/a.svg", 1024);
  ASSERT_TRUE(snapshot.fingerprint);
  write("corpus/tests/a.svg", "<svg >");
  EXPECT_THAT(VerifyFile(directory_ / "corpus/tests/a.svg", *snapshot.fingerprint),
              HasSubstr("changed"));
}

TEST_F(ProvenanceTest, FinalVerificationIsCreateOnlyAndRecordsRejectedRun) {
  const auto run = CreateRunProvenance(options(), directory_ / "output");
  ASSERT_TRUE(run.run) << run.error;
  ASSERT_THAT(WriteRunVerification(*run.run, "resource changed", 0), IsEmpty());
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
  ASSERT_THAT(WriteRunVerification(*run.run, "", 0), IsEmpty());
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
  write("output/report.xml", "<?xml version=\"1.0\"?><testsuites/>\n");
  RunExecutionSummary execution;
  execution.discoveredCases = 5;
  execution.selectedCases = 2;
  execution.completedCases = 2;
  execution.completedInvocations = 4;
  execution.repeatCount = 2;
  execution.xmlReport = directory_ / "output/report.xml";
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
  ASSERT_THAT(WriteRunVerification(*run.run, "", 0, execution), IsEmpty());
  std::ifstream input(directory_ / "output/run-verification.json");
  const auto record = nlohmann::json::parse(input);
  EXPECT_EQ(record["accepted"], false);
  EXPECT_EQ(record["execution"]["corpus_complete"], false);
  EXPECT_EQ(record["xml_report"], nullptr);
}

TEST_F(ProvenanceTest, XmlEvidenceFailuresRejectOtherwiseCompleteRuns) {
  for (const std::string_view mode : {"missing", "empty", "oversized", "unreadable", "directory"}) {
    SCOPED_TRACE(mode);
    const auto output = directory_ / std::string(mode);
    const auto run = CreateRunProvenance(options(), output);
    ASSERT_TRUE(run.run) << run.error;
    const auto reportPath = output / "report.xml";
    if (mode == "directory") {
      std::filesystem::create_directory(reportPath);
    } else if (mode != "missing") {
      std::ofstream(reportPath) << (mode == "empty" ? "" : "<testsuites/>");
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
