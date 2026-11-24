#pragma once
/// @file
/// Documents, edits and measurements shared by the CSS cascade benchmark and its perf tests.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/xml/XMLDocument.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/internal/RenderingDiagnostics.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::benchmarks {

/// Workspace-relative path of the splash document.
inline constexpr std::string_view kSplashPath = "donner_splash.svg";
/// Workspace-relative path of the bootstrap-icons sprite.
inline constexpr std::string_view kBootstrapIconsPath =
    "third_party/bootstrap-icons/bootstrap-icons.svg";
/// Workspace-relative path of the tiger illustration.
inline constexpr std::string_view kTigerPath = "donner/svg/renderer/testdata/Ghostscript_Tiger.svg";

/// Selector shape of the rules in \ref MakeGroupedRectsDocument.
enum class CascadeRuleKind {
  /// Per 20 rules: 14 class, 2 type, 3 descendant and 1 attribute rule.
  Mixed,
  /// `.cN`.
  Class,
  /// `.gK .cN`, with `K = N mod 97`.
  Descendant,
  /// One of eight element names, with `:nth-child(N)` from the ninth rule on.
  Type,
  /// `[data-kN]`, which no generated element matches.
  Attribute,
};

/**
 * Generate a document of `<g class="gK">` groups (`K` is the group index mod 97), each holding up
 * to ten `<rect id="eN" class="cM">` with no text between elements, and a `<style>` of
 * \p ruleCount rules of \p kind. Each rect's class is drawn from a fixed seed, so a given
 * argument list produces the same document on every platform.
 *
 * @param elementCount Groups plus rects.
 * @param ruleCount Stylesheet rules; zero omits the `<style>` element.
 * @param kind Selector shape of the rules.
 */
std::string MakeGroupedRectsDocument(int elementCount, int ruleCount, CascadeRuleKind kind);

/**
 * Generate a document with a `<symbol>` of \p symbolChildren `<rect class="cN">` drawn by
 * \p instanceCount `<use>` elements, and \p ruleCount class rules.
 *
 * @param instanceCount `<use>` elements.
 * @param symbolChildren Rects inside the symbol.
 * @param ruleCount Class rules `.c0` to `.c{ruleCount - 1}`.
 */
std::string MakeUseInstancesDocument(int instanceCount, int symbolChildren, int ruleCount);

/// The synthetic many-elements-by-many-rules document: 2,000 elements and 200 mixed rules.
std::string MakeSyntheticDocument();

/**
 * Generate a document of \p ruleCount `.a` rules and one rect whose class list repeats `a`
 * \p tokenCount times, which costs the cascade at most one gathered bucket however often the
 * class repeats.
 *
 * @param ruleCount Stylesheet rules, all `.a`.
 * @param tokenCount Repetitions of `a` in the rect's class list.
 */
std::string MakeRepeatedClassTokensDocument(int ruleCount, int tokenCount);

/// Renderer that records nothing, so a frame costs only document preparation.
class NullRenderer final : public svg::RendererInterface {
public:
  void draw(svg::SVGDocument& document) override;
  void draw(svg::SVGDocument& document, ParseWarningSink& warningSink) override;
  [[nodiscard]] int width() const override { return 0; }
  [[nodiscard]] int height() const override { return 0; }
  void beginFrame(const svg::RenderViewport&) override {}
  void endFrame() override {}
  void setTransform(const Transform2d&) override {}
  void pushTransform(const Transform2d&) override {}
  void popTransform() override {}
  void pushClip(const svg::ResolvedClip&) override {}
  void popClip() override {}
  void pushIsolatedLayer(double, svg::MixBlendMode) override {}
  void popIsolatedLayer() override {}
  void pushFilterLayer(const svg::components::FilterGraph&, const std::optional<Box2d>&) override {}
  void popFilterLayer() override {}
  void pushMask(const std::optional<Box2d>&) override {}
  void transitionMaskToContent() override {}
  void popMask() override {}
  bool beginPatternTile(const Box2d&, const Transform2d&) override { return true; }
  void endPatternTile(bool, std::optional<std::uint64_t>) override {}
  void setPaint(const svg::PaintParams&) override {}
  void drawPath(const svg::PathShape&, const svg::StrokeParams&) override {}
  void drawRect(const Box2d&, const svg::StrokeParams&) override {}
  void drawEllipse(const Box2d&, const svg::StrokeParams&) override {}
  void drawImage(const svg::ImageResource&, const svg::ImageParams&) override {}
  void drawText(Registry&, const svg::components::ComputedTextComponent&,
                const svg::TextParams&) override {}
  [[nodiscard]] svg::RendererBitmap takeSnapshot() const override { return {}; }
  [[nodiscard]] std::unique_ptr<svg::RendererInterface> createOffscreenInstance() const override {
    return std::make_unique<NullRenderer>();
  }
};

/**
 * Parse \p source with warnings disabled.
 *
 * @param source SVG source.
 * @return The document, or \c std::nullopt when it does not parse.
 */
std::optional<svg::SVGDocument> ParseCascadeDocument(std::string_view source);

/// Style work and time recorded for a document so far, read from its rendering diagnostics.
struct StyleCascadeSample {
  svg::components::StyleCascadeCounters counters;  ///< Exact work counts.
  std::chrono::nanoseconds styleTime{0};           ///< Time spent in style computation.
};

/**
 * Read the style work recorded for \p document so far.
 *
 * @param document Document to inspect.
 */
StyleCascadeSample CaptureStyleCascade(const svg::SVGDocument& document);

/// Edit applied to one element, or to the document's first `<style>`, before a frame.
enum class CascadeEdit {
  Class,  ///< `class` alternates between two values.
  Fill,   ///< `fill` presentation attribute alternates between two colors.
  Style,  ///< `style` attribute alternates between two `fill` declarations.
  /// Odd repetitions append one rule to the first `<style>`; even ones restore its text.
  AppendRule,
};

/// One element of a document to edit, and the two classes a \ref CascadeEdit::Class edit uses.
struct CascadeEditTarget {
  std::string selector;  ///< Selector whose first match is the edited element.
  std::string classA;    ///< Class written on odd repetitions.
  std::string classB;    ///< Class written on even repetitions.
};

/// The first `.cls-71` path of the splash document, toggled between `cls-71` and `cls-88`.
CascadeEditTarget SplashEditTarget();
/// Element `#e1000` of \ref MakeSyntheticDocument, toggled between `c3` and `c7`.
CascadeEditTarget SyntheticEditTarget();
/// The first `symbol > path` of the bootstrap-icons sprite, toggled between `a` and `b`.
CascadeEditTarget BootstrapIconsEditTarget();

/**
 * Applies one \ref CascadeEdit to a loaded document, alternating between two values so every
 * repetition changes the document.
 */
class CascadeEditor {
public:
  /**
   * Resolve the edited element, and for \ref CascadeEdit::AppendRule the first `<style>` and its
   * original text.
   *
   * @param document Loaded document.
   * @param target Element to edit.
   * @param edit Edit kind.
   * @return The editor, or \c std::nullopt when the element or a needed `<style>` is missing.
   */
  static std::optional<CascadeEditor> Create(svg::SVGDocument& document,
                                             const CascadeEditTarget& target, CascadeEdit edit);

  /**
   * Apply the edit for repetition \p repetition.
   *
   * @param repetition Repetition index; odd and even repetitions write different values.
   */
  void apply(int repetition);

private:
  CascadeEditor(svg::SVGElement element, std::optional<svg::SVGElement> style,
                CascadeEditTarget target, CascadeEdit edit, std::string originalStylesheet);

  svg::SVGElement element_;
  std::optional<svg::SVGElement> style_;
  CascadeEditTarget target_;
  CascadeEdit edit_;
  std::string originalStylesheet_;
};

/// One-character source edit on the splash document, made as the editor's source pane does.
enum class CascadeKeystroke {
  DeclarationValue,  ///< The last hex digit of `#fffffc` in the `.cls-2` rule alternates.
  Selector,          ///< A letter is inserted after, then deleted from, the first `.cls-2`.
  ClosingBrace,      ///< The `.cls-2` rule's closing brace is deleted, then restored.
  ClassAttribute,    ///< A letter is inserted into, then deleted from, the first `cls-71` class.
};

/**
 * Build the source edit for \p keystroke at repetition \p repetition, against the document's
 * current source.
 *
 * @param document Splash document.
 * @param keystroke Keystroke kind.
 * @param repetition Even repetitions insert or change, odd repetitions undo.
 * @param replacementStorage Storage the returned intent's replacement points into.
 * @return The intent, or \c std::nullopt when the target text is missing.
 */
std::optional<xml::XMLEditIntent> MakeCascadeKeystroke(const svg::SVGDocument& document,
                                                       CascadeKeystroke keystroke, int repetition,
                                                       std::string& replacementStorage);

/// Median, minimum and maximum of a set of samples, in milliseconds.
struct TimingSummary {
  double medianMs = 0;      ///< Median; the upper middle sample for an even count.
  double minMs = 0;         ///< Smallest sample.
  double maxMs = 0;         ///< Largest sample.
  std::size_t samples = 0;  ///< Sample count.
};

/**
 * Summarize timing samples.
 *
 * @param samplesMs Samples in milliseconds.
 */
TimingSummary SummarizeTimings(std::vector<double> samplesMs);

/// Frame timings and style work for one measured case.
struct CascadeMeasurement {
  /// Parse time for load cases, or the edit or source-edit call for edit and keystroke cases.
  TimingSummary setup;
  TimingSummary nullFrame;  ///< Frame drawn with \ref NullRenderer.
  TimingSummary style;      ///< Style time inside the null-renderer frame.
  /// Frame drawn with the default renderer, when measured.
  std::optional<TimingSummary> rendererFrame;
  /// Style work of the last measured null-renderer repetition.
  svg::components::StyleCascadeCounters counters;
  /// Whether every measured null-renderer repetition did the same style work.
  bool countersStable = true;
};

/// Print the style work of a measurement, for test failure output.
std::ostream& operator<<(std::ostream& os, const CascadeMeasurement& measurement);

/// Which repetitions after the warmup a measurement records.
enum class RecordedRepetitions {
  All,   ///< Every repetition.
  Even,  ///< Even repetitions: the forward edit of an edit that alternates with its undo.
  Odd,   ///< Odd repetitions: the undo, or the appended rule of \ref CascadeEdit::AppendRule.
};

/// Repetition counts for a measurement.
struct CascadeRepetitions {
  int warmup = 0;    ///< Unrecorded repetitions run first.
  int measured = 9;  ///< Repetitions run after the warmup.
  /// Repetitions recorded among the measured ones. An edit whose two directions do different
  /// work is measured one direction at a time.
  RecordedRepetitions recorded = RecordedRepetitions::All;
};

/// Freshly parsed documents: 9 repetitions.
constexpr CascadeRepetitions kLoadRepetitions{.warmup = 0, .measured = 9};
/// Edits on one loaded document: the first of 19 repetitions is discarded.
constexpr CascadeRepetitions kEditRepetitions{.warmup = 1, .measured = 18};
/// Source keystrokes on one loaded document: the first two of 20 repetitions are discarded.
constexpr CascadeRepetitions kKeystrokeRepetitions{.warmup = 2, .measured = 18};

/**
 * Restrict \p repetitions to one direction of an alternating edit.
 *
 * @param repetitions Repetition counts.
 * @param recorded Direction to record.
 */
constexpr CascadeRepetitions OneDirection(CascadeRepetitions repetitions,
                                          RecordedRepetitions recorded) {
  repetitions.recorded = recorded;
  return repetitions;
}

/**
 * Measure the first frame of freshly parsed documents: each repetition parses \p source and draws
 * one null-renderer frame, then, when \p withRenderer is set, draws a second fresh parse with the
 * default renderer.
 *
 * @param source SVG source.
 * @param repetitions Repetition counts.
 * @param withRenderer Also time the default renderer's first frame.
 */
std::optional<CascadeMeasurement> MeasureCascadeLoad(std::string_view source,
                                                     CascadeRepetitions repetitions,
                                                     bool withRenderer);

/**
 * Measure the frame after an edit: each repetition applies \p edit to a loaded document and draws
 * one frame, on one document per renderer.
 *
 * @param source SVG source.
 * @param target Element to edit.
 * @param edit Edit kind.
 * @param repetitions Repetition counts.
 * @param withRenderer Also time the default renderer's frame.
 */
std::optional<CascadeMeasurement> MeasureCascadeEdit(std::string_view source,
                                                     const CascadeEditTarget& target,
                                                     CascadeEdit edit,
                                                     CascadeRepetitions repetitions,
                                                     bool withRenderer);

/**
 * Measure the frame after a source keystroke on the splash document, applied through
 * \ref svg::SVGDocument::applySourceEdit.
 *
 * @param source Splash document source.
 * @param keystroke Keystroke kind.
 * @param repetitions Repetition counts.
 * @param withRenderer Also time the default renderer's frame.
 */
std::optional<CascadeMeasurement> MeasureCascadeKeystroke(std::string_view source,
                                                          CascadeKeystroke keystroke,
                                                          CascadeRepetitions repetitions,
                                                          bool withRenderer);

}  // namespace donner::benchmarks
