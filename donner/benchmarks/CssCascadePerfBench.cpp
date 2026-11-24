/// @file
/// CSS cascade benchmark: style time and style work for document loads, single-element edits,
/// stylesheet keystrokes and synthetic element-by-rule scaling.
///
/// Usage:
/// ```
/// bazel run -c opt //donner/benchmarks:css_cascade_perf_bench -- \
///     [--mode=all|load|edits|keystrokes|scaling] [--no-renderer]
/// ```
///
/// Load cases parse the document afresh for each of 9 repetitions; edit cases apply the edit 19
/// times to one loaded document and discard the first; keystroke cases apply 20 source edits and
/// discard the first two. An edit that alternates between two documents with different work, such
/// as appending and removing a rule, prints one line per direction from its alternate repetitions.
/// Each line has the median, minimum and maximum of every timing and the exact style work counted
/// by the style system for one repetition; the run fails if repetitions of a line differ in work.
/// "style" is the time inside the outermost style computation calls of the null-renderer frame.
/// For stable timings pin the process to one core, for example with `taskset -c 3`.

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/benchmarks/CssCascadeScenes.h"

namespace {

using donner::benchmarks::CascadeEdit;
using donner::benchmarks::CascadeEditTarget;
using donner::benchmarks::CascadeKeystroke;
using donner::benchmarks::CascadeMeasurement;
using donner::benchmarks::CascadeRepetitions;
using donner::benchmarks::CascadeRuleKind;
using donner::benchmarks::kEditRepetitions;
using donner::benchmarks::kKeystrokeRepetitions;
using donner::benchmarks::kLoadRepetitions;
using donner::benchmarks::MakeGroupedRectsDocument;
using donner::benchmarks::MakeUseInstancesDocument;
using donner::benchmarks::OneDirection;
using donner::benchmarks::RecordedRepetitions;
using donner::benchmarks::TimingSummary;

std::optional<std::string> ReadFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void PrintTiming(const char* name, const TimingSummary& timing) {
  std::printf(" %s_ms=%.3f %s_min_ms=%.3f %s_max_ms=%.3f", name, timing.medianMs, name,
              timing.minMs, name, timing.maxMs);
}

/// Print one case; returns false when the case could not be measured or its recorded repetitions
/// did different work.
bool PrintResult(std::string_view kind, std::string_view name,
                 const std::optional<CascadeMeasurement>& measurement, const char* setupName) {
  if (!measurement) {
    std::fprintf(stderr, "case %.*s could not be measured\n", static_cast<int>(name.size()),
                 name.data());
    return false;
  }
  const auto& counters = measurement->counters;
  std::printf("RESULT kind=%.*s case=\"%.*s\" samples=%zu", static_cast<int>(kind.size()),
              kind.data(), static_cast<int>(name.size()), name.data(),
              measurement->nullFrame.samples);
  PrintTiming(setupName, measurement->setup);
  PrintTiming("null_frame", measurement->nullFrame);
  PrintTiming("style", measurement->style);
  if (measurement->rendererFrame) {
    PrintTiming("renderer_frame", *measurement->rendererFrame);
  }
  std::printf(" elements_cascaded=%" PRIu64 " rules_tested=%" PRIu64 " rules_matched=%" PRIu64
              " declarations_applied=%" PRIu64 " full_clears=%" PRIu64 " budget_rejections=%" PRIu64
              " registry_copies=%" PRIu64 " matched_properties_cache_hits=%" PRIu64
              " matched_properties_cache_misses=%" PRIu64
              " matched_properties_cache_uncacheable=%" PRIu64
              " declaration_parses_skipped=%" PRIu64
              " distinct_computed_style_records=%zu counters_stable=%s\n",
              counters.elementsCascaded, counters.rulesTested, counters.rulesMatched,
              counters.declarationsApplied, counters.fullClears, counters.budgetRejections,
              counters.registryCopies, counters.matchedPropertiesCacheHits,
              counters.matchedPropertiesCacheMisses, counters.matchedPropertiesCacheUncacheable,
              counters.declarationParsesSkipped, measurement->distinctComputedStyleRecords,
              measurement->countersStable ? "yes" : "no");
  std::fflush(stdout);
  return measurement->countersStable;
}

struct Document {
  std::string name;
  std::string source;
};

/// Documents the cases run against.
struct Documents {
  Document splash;
  Document icons;
  Document tiger;
  Document synthetic;
};

struct Options {
  std::string mode = "all";
  bool withRenderer = true;
};

std::optional<Options> ParseOptions(int argc, char* argv[]) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg.starts_with("--mode=")) {
      options.mode = std::string(arg.substr(7));
      constexpr std::string_view kModes[] = {"all", "load", "edits", "keystrokes", "scaling"};
      if (std::find(std::begin(kModes), std::end(kModes), options.mode) == std::end(kModes)) {
        return std::nullopt;
      }
    } else if (arg == "--no-renderer") {
      options.withRenderer = false;
    } else {
      return std::nullopt;
    }
  }
  return options;
}

/// Read a workspace-relative document, named after its file.
std::optional<Document> ReadDocument(std::string_view path) {
  const std::string pathString(path);
  std::optional<std::string> source = ReadFile(pathString);
  if (!source) {
    std::fprintf(stderr, "unable to read %s; run from the repository root or with bazel run\n",
                 pathString.c_str());
    return std::nullopt;
  }
  return Document{pathString.substr(pathString.rfind('/') + 1), std::move(*source)};
}

std::optional<Documents> LoadDocuments() {
  std::optional<Document> splash = ReadDocument(donner::benchmarks::kSplashPath);
  std::optional<Document> icons = ReadDocument(donner::benchmarks::kBootstrapIconsPath);
  std::optional<Document> tiger = ReadDocument(donner::benchmarks::kTigerPath);
  if (!splash || !icons || !tiger) {
    return std::nullopt;
  }
  return Documents{std::move(*splash), std::move(*icons), std::move(*tiger),
                   Document{"synthetic 2000 elements x 200 mixed rules",
                            donner::benchmarks::MakeSyntheticDocument()}};
}

bool RunLoadCases(const Documents& documents, bool withRenderer) {
  const Document generated[] = {
      {"synthetic 200 use x 20 symbol rects, 20 rules", MakeUseInstancesDocument(200, 20, 20)},
      {"synthetic 1000 elements x 100 descendant rules",
       MakeGroupedRectsDocument(1000, 100, CascadeRuleKind::Descendant)},
  };
  bool ok = true;
  for (const Document* document : {&documents.splash, &documents.icons, &documents.tiger,
                                   &documents.synthetic, &generated[0], &generated[1]}) {
    ok &=
        PrintResult("load", document->name,
                    MeasureCascadeLoad(document->source, kLoadRepetitions, withRenderer), "parse");
  }
  return ok;
}

/// One reported edit: the edit and the repetitions whose frames it reports.
struct EditRow {
  CascadeEdit edit;
  const char* name;
  CascadeRepetitions repetitions;
};

bool RunEditCases(const Documents& documents, bool withRenderer) {
  const EditRow kSymmetricRows[] = {
      {CascadeEdit::Class, "class", kEditRepetitions},
      {CascadeEdit::Fill, "fill", kEditRepetitions},
      {CascadeEdit::Style, "style", kEditRepetitions},
  };
  const EditRow kStylesheetRows[] = {
      {CascadeEdit::AppendRule, "append rule",
       OneDirection(kEditRepetitions, RecordedRepetitions::Odd)},
      {CascadeEdit::AppendRule, "remove appended rule",
       OneDirection(kEditRepetitions, RecordedRepetitions::Even)},
  };
  struct EditCase {
    const Document* document;
    CascadeEditTarget target;
    bool stylesheetEdits;
  };
  const EditCase cases[] = {
      {&documents.splash, donner::benchmarks::SplashEditTarget(), true},
      {&documents.synthetic, donner::benchmarks::SyntheticEditTarget(), true},
      {&documents.icons, donner::benchmarks::BootstrapIconsEditTarget(), false},
  };
  bool ok = true;
  for (const EditCase& editCase : cases) {
    std::vector<EditRow> rows(std::begin(kSymmetricRows), std::end(kSymmetricRows));
    if (editCase.stylesheetEdits) {
      rows.insert(rows.end(), std::begin(kStylesheetRows), std::end(kStylesheetRows));
    }
    for (const EditRow& row : rows) {
      ok &= PrintResult("edit", editCase.document->name + ", " + row.name,
                        MeasureCascadeEdit(editCase.document->source, editCase.target, row.edit,
                                           row.repetitions, withRenderer),
                        "edit");
    }
  }
  return ok;
}

bool RunKeystrokeCases(const Documents& documents, bool withRenderer) {
  constexpr CascadeRepetitions kForward =
      OneDirection(kKeystrokeRepetitions, RecordedRepetitions::Even);
  constexpr CascadeRepetitions kUndo =
      OneDirection(kKeystrokeRepetitions, RecordedRepetitions::Odd);
  struct KeystrokeRow {
    CascadeKeystroke keystroke;
    const char* name;
    CascadeRepetitions repetitions;
  };
  constexpr KeystrokeRow kRows[] = {
      {CascadeKeystroke::DeclarationValue, "style declaration value", kKeystrokeRepetitions},
      {CascadeKeystroke::Selector, "style selector, letter inserted", kForward},
      {CascadeKeystroke::Selector, "style selector, letter deleted", kUndo},
      {CascadeKeystroke::ClosingBrace, "style closing brace deleted", kForward},
      {CascadeKeystroke::ClosingBrace, "style closing brace restored", kUndo},
      {CascadeKeystroke::ClassAttribute, "class attribute, letter inserted", kForward},
      {CascadeKeystroke::ClassAttribute, "class attribute, letter deleted", kUndo},
  };
  bool ok = true;
  for (const KeystrokeRow& row : kRows) {
    ok &= PrintResult("keystroke", documents.splash.name + ", " + row.name,
                      MeasureCascadeKeystroke(documents.splash.source, row.keystroke,
                                              row.repetitions, withRenderer),
                      "source_edit");
  }
  return ok;
}

bool RunScalingCases() {
  struct ScalingCase {
    const char* kindName;
    CascadeRuleKind kind;
    int elements;
    int rules;
  };
  std::vector<ScalingCase> cases;
  for (const int elements : {1000, 4000}) {
    for (const int rules : {0, 10, 100, 1000}) {
      cases.push_back({"mixed", CascadeRuleKind::Mixed, elements, rules});
    }
  }
  cases.push_back({"class", CascadeRuleKind::Class, 1000, 1000});
  cases.push_back({"descendant", CascadeRuleKind::Descendant, 1000, 1000});
  cases.push_back({"type", CascadeRuleKind::Type, 1000, 1000});
  cases.push_back({"attribute", CascadeRuleKind::Attribute, 1000, 1000});
  bool ok = true;
  for (const ScalingCase& scaling : cases) {
    const std::string name = "grouped rects " + std::to_string(scaling.elements) + " x " +
                             std::to_string(scaling.rules) + " " + scaling.kindName;
    ok &= PrintResult(
        "scaling", name,
        MeasureCascadeLoad(MakeGroupedRectsDocument(scaling.elements, scaling.rules, scaling.kind),
                           kLoadRepetitions, /*withRenderer=*/false),
        "parse");
  }
  return ok;
}

}  // namespace

int main(int argc, char* argv[]) {
  const std::optional<Options> options = ParseOptions(argc, argv);
  if (!options) {
    std::fprintf(stderr,
                 "usage: css_cascade_perf_bench [--mode=all|load|edits|keystrokes|scaling] "
                 "[--no-renderer]\n");
    return 2;
  }
  const std::optional<Documents> documents = LoadDocuments();
  if (!documents) {
    return 2;
  }
  const auto runs = [&](std::string_view mode) {
    return options->mode == "all" || options->mode == mode;
  };
  bool ok = true;
  if (runs("load")) {
    ok &= RunLoadCases(*documents, options->withRenderer);
  }
  if (runs("edits")) {
    ok &= RunEditCases(*documents, options->withRenderer);
  }
  if (runs("keystrokes")) {
    ok &= RunKeystrokeCases(*documents, options->withRenderer);
  }
  if (runs("scaling")) {
    ok &= RunScalingCases();
  }
  return ok ? 0 : 1;
}
