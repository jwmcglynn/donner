#include <gmock/gmock.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "donner/base/tests/Runfiles.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"

using testing::ValuesIn;

namespace donner::svg {

namespace {

struct TestDirectoryConfig {
  std::vector<std::string> relativePaths;
  std::map<std::string, ImageComparisonParams> overrides;
  ImageComparisonParams defaultParams;
};

ImageComparisonParams ensureDefaultCanvas(ImageComparisonParams params) {
  if (!params.canvasSize) {
    params.setCanvasSize(500, 500);
  }
  return params;
}

std::string buildDisplayName(const std::filesystem::path& testsDir,
                             const std::filesystem::path& svgPath) {
  std::string name = svgPath.lexically_relative(testsDir).generic_string();
  std::transform(name.begin(), name.end(), name.begin(), [](char c) {
    if (!isalnum(c)) {
      return '_';
    }
    return c;
  });
  return name;
}

std::vector<ImageComparisonTestcase> collectTests(const std::filesystem::path& testsDir,
                                                  const TestDirectoryConfig& config) {
  std::vector<ImageComparisonTestcase> testPlan;

  for (const auto& relativePath : config.relativePaths) {
    const std::filesystem::path dirPath = testsDir / relativePath;
    if (!std::filesystem::exists(dirPath)) {
      continue;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(dirPath)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".svg") {
        continue;
      }

      const std::string relativeSvgPath =
          entry.path().lexically_relative(testsDir).generic_string();

      ImageComparisonTestcase test;
      test.svgFilename = entry.path();
      test.displayName = buildDisplayName(testsDir, entry.path());
      test.params = ensureDefaultCanvas(config.defaultParams);

      if (auto it = config.overrides.find(relativeSvgPath); it != config.overrides.end()) {
        test.params = ensureDefaultCanvas(it->second);
      }

      testPlan.emplace_back(std::move(test));
    }
  }

  std::sort(testPlan.begin(), testPlan.end());
  return testPlan;
}

std::vector<ImageComparisonTestcase> getResvgTests() {
  const std::filesystem::path testsDir =
      Runfiles::instance().RlocationExternal("resvg-test-suite", "tests");

  std::vector<ImageComparisonTestcase> testPlan;

  // TODO: Add coverage for filters and text once the renderer supports those features.

  const TestDirectoryConfig paintingConfig{
      .relativePaths =
          {
              "painting/color",
              "painting/display",
              "painting/fill",
              "painting/fill-opacity",
              "painting/marker",
              "painting/opacity",
              "painting/overflow",
              "painting/shape-rendering",
              "painting/stroke",
              "painting/stroke-dasharray",
              "painting/stroke-dashoffset",
              "painting/stroke-linecap",
              "painting/stroke-linejoin",
              "painting/stroke-opacity",
              "painting/stroke-width",
              "painting/visibility",
          },
      .overrides =
          {
              {"painting/display/none-on-tref.svg", ImageComparisonParams::Skip()},
              // Not impl: <tref>
              {"painting/display/none-on-tspan-1.svg", ImageComparisonParams::Skip()},
              // Not impl: <tspan>
              {"painting/display/none-on-tspan-2.svg", ImageComparisonParams::Skip()},
              // Not impl: <tspan>
              {"painting/fill/icc-color.svg", ImageComparisonParams::Skip()},
              // ICC color
              {"painting/fill/linear-gradient-on-text.svg", ImageComparisonParams::Skip()},
              // Not impl: <text>
              {"painting/fill/pattern-on-text.svg", ImageComparisonParams::Skip()},
              // Not impl: <pattern>, <text>
              {"painting/fill/radial-gradient-on-text.svg", ImageComparisonParams::Skip()},
              // Not impl: <text>
              {"painting/fill/rgb-int-int-int.svg", ImageComparisonParams::Skip()},
              // UB: rgb(int int int)
              {"painting/fill/valid-FuncIRI-with-a-fallback-ICC-color.svg",
               ImageComparisonParams::Skip()},
              // Not impl: Fallback with icc-color
              {"painting/fill-opacity/on-text.svg", ImageComparisonParams::Skip()},
              // Not impl: <text>
              {"painting/fill-opacity/with-pattern.svg", ImageComparisonParams::Skip()},
              // Not impl: `fill-opacity` affects pattern
              {"painting/stroke-dasharray/em-units.svg", ImageComparisonParams::Skip()},
              // Not impl: "em" units
              {"painting/stroke-dasharray/negative-sum.svg", ImageComparisonParams::Skip()},
              // UB: Negative sum
              {"painting/stroke-dasharray/negative-values.svg", ImageComparisonParams::Skip()},
              // UB: Negative values
              {"painting/stroke-dasharray/on-a-circle.svg",
               ImageComparisonParams::WithThreshold(0.13f)},
              // Larger threshold due to anti-aliasing artifacts
              {"painting/stroke-dashoffset/em-units.svg", ImageComparisonParams::Skip()},
              // Not impl: "em" units
              {"painting/stroke-linejoin/arcs.svg", ImageComparisonParams::Skip()},
              // UB: `arcs` (SVG 2)
              {"painting/stroke-linejoin/miter-clip.svg", ImageComparisonParams::Skip()},
              // UB: `miter-clip` (SVG 2)
              {"painting/stroke-opacity/on-text.svg", ImageComparisonParams::Skip()},
              // Not impl: <text>
              {"painting/stroke-opacity/with-pattern.svg", ImageComparisonParams::Skip()},
              // Not impl: <pattern> / stroke interaction
              {"painting/stroke/pattern-on-text.svg", ImageComparisonParams::Skip()},
              // Not impl: <pattern> / <text>
              {"painting/stroke/radial-gradient-on-text.svg", ImageComparisonParams::Skip()},
              // Not impl: <text>
              {"painting/stroke/linear-gradient-on-text.svg", ImageComparisonParams::Skip()},
              // Not impl: <text>
              {"painting/stroke-width/zero.svg", ImageComparisonParams::Skip()},
              // UB: Nothing should be rendered
          },
  };

  const TestDirectoryConfig paintServerConfig{
      .relativePaths =
          {
              "paint-servers/linearGradient",
              "paint-servers/pattern",
              "paint-servers/radialGradient",
              "paint-servers/stop",
          },
      .overrides =
          {
              {"paint-servers/pattern/'overflow=visible.svg'",
               ImageComparisonParams::Skip()},  // UB: overflow
              {"paint-servers/pattern/invalid-patternTransform.svg",
               ImageComparisonParams::Skip()},  // UB: invalid transform
              {"paint-servers/pattern/self-recursive.svg", ImageComparisonParams::Skip()},
              // UB: Self-recursive pattern
              {"paint-servers/pattern/self-recursive-on-child.svg",
               ImageComparisonParams::Skip()},
              // UB: Self-recursive pattern on child
              {"paint-servers/pattern/recursive.svg", ImageComparisonParams::Skip()},
              // UB: Recursive pattern
              {"paint-servers/pattern/recursive-on-child.svg", ImageComparisonParams::Skip()},
              // UB: Recursive pattern on child
              {"paint-servers/pattern/tiny-pattern-upscaled.svg",
               ImageComparisonParams::WithThreshold(0.2f)},  // Anti-aliasing artifacts
              {"paint-servers/pattern/transform-and-patternTransform.svg",
               ImageComparisonParams::WithThreshold(0.02f)},  // Anti-aliasing artifacts
          },
      .defaultParams = ImageComparisonParams::WithThreshold(0.02f),
  };

  const TestDirectoryConfig shapesConfig{
      .relativePaths =
          {
              "shapes/circle",
              "shapes/ellipse",
              "shapes/line",
              "shapes/path",
              "shapes/polygon",
              "shapes/polyline",
              "shapes/rect",
          },
      .overrides =
          {
              {"shapes/rect/ch-values.svg", ImageComparisonParams::Skip()},   // ch units
              {"shapes/rect/em-values.svg", ImageComparisonParams::Skip()},   // em units
              {"shapes/rect/ex-values.svg", ImageComparisonParams::Skip()},   // ex units
              {"shapes/rect/rem-values.svg", ImageComparisonParams::Skip()},  // rem units
              {"shapes/rect/vmin-and-vmax-values.svg", ImageComparisonParams::Skip()},
              // Not impl: vmin/vmax units
              {"shapes/rect/vw-and-vh-values.svg", ImageComparisonParams::Skip()},
              // Not impl: vw/vh units
          },
  };

  const TestDirectoryConfig structureConfig{
      .relativePaths =
          {
              "structure/defs",
              "structure/g",
              "structure/style",
              "structure/style-attribute",
              "structure/svg",
              "structure/symbol",
              "structure/transform",
              "structure/use",
          },
      .overrides =
          {
              {"structure/defs/style-inheritance-on-text.svg",
               ImageComparisonParams::Skip()},  // Not impl: <text>
              {"structure/style/external-CSS.svg", ImageComparisonParams::Skip()},
              // Not impl: External CSS import
              {"structure/style/invalid-type.svg", ImageComparisonParams::Skip()},
              // UB: Invalid style type
              {"structure/svg/funcIRI-parsing.svg", ImageComparisonParams::Skip()},
              // UB: FuncIRI parsing
              {"structure/svg/funcIRI-with-invalid-characters.svg", ImageComparisonParams::Skip()},
              // UB: FuncIRI with invalid characters
              {"structure/svg/invalid-id-attribute-1.svg", ImageComparisonParams::Skip()},
              // UB: Invalid id attribute
              {"structure/svg/invalid-id-attribute-2.svg", ImageComparisonParams::Skip()},
              // UB: Invalid id attribute
              {"structure/svg/nested-svg-with-overflow-auto.svg", ImageComparisonParams::Skip()},
              // Not impl: overflow
              {"structure/svg/nested-svg-with-overflow-visible.svg", ImageComparisonParams::Skip()},
              // Not impl: overflow
              {"structure/svg/rect-inside-a-non-SVG-element.svg", ImageComparisonParams::Skip()},
              // Bug? Rect inside unknown element
              {"structure/svg/xmlns-validation.svg", ImageComparisonParams::Skip()},
              // Bug? xmlns validation
              {"structure/symbol/with-transform.svg", ImageComparisonParams::Skip()},
              // SVG2: Transform on <symbol>
              {"structure/symbol/with-transform-on-use.svg", ImageComparisonParams::Skip()},
              // SVG2: Transform on <symbol> via <use>
              {"structure/symbol/with-transform-on-use-no-size.svg", ImageComparisonParams::Skip()},
              // SVG2: Transform on <symbol> via <use>
          },
  };

  const TestDirectoryConfig structureImageConfig{
      .relativePaths =
          {
              "structure/image",
          },
      .overrides =
          {
              {"structure/image/embedded-svg-with-text.svg",
               ImageComparisonParams::Skip()},  // Not impl: nested SVG with <text>
              {"structure/image/embedded-svg-without-mime.svg",
               ImageComparisonParams::Skip()},  // Not impl: embedded SVG mime
              {"structure/image/embedded-svg.svg", ImageComparisonParams::Skip()},
              // Not impl: Embedded .svg image
              {"structure/image/embedded-svgz.svg", ImageComparisonParams::Skip()},
              // Not impl: Embedded .svgz image
              {"structure/image/external-svg-with-transform.svg",
               ImageComparisonParams::Skip()},  // External SVGs
              {"structure/image/external-svg.svg", ImageComparisonParams::Skip()},
              // Not impl: External SVG image
              {"structure/image/external-svgz.svg", ImageComparisonParams::Skip()},
              // Not impl: External SVGZ image
              {"structure/image/float-size.svg", ImageComparisonParams::Skip()},
              // UB: Float sizes
              {"structure/image/image-with-float-size-scaling.svg",
               ImageComparisonParams::Skip()},
              // UB: Float sizes
              {"structure/image/url-to-svg.svg", ImageComparisonParams::Skip()},
              // Not impl: External SVG image
              {"structure/image/with-zero-width-and-height.svg",
               ImageComparisonParams::Skip()},
              // UB: Empty size
          },
      .defaultParams = ImageComparisonParams::WithThreshold(0.2f).disableDebugSkpOnFailure(),
  };

  const TestDirectoryConfig maskingConfig{
      .relativePaths =
          {
              "masking/clipPath",
              "masking/mask",
          },
      .overrides =
          {
              {"masking/clipPath/clip-path-with-transform-on-text.svg",
               ImageComparisonParams::Skip()},  // Not impl: <text>
              {"masking/clipPath/clipping-with-complex-text-1.svg",
               ImageComparisonParams::Skip()},  // Not impl: <text>
              {"masking/clipPath/clipping-with-complex-text-2.svg",
               ImageComparisonParams::Skip()},  // Not impl: <text>
              {"masking/clipPath/clipping-with-complex-text-and-clip-rule.svg",
               ImageComparisonParams::Skip()},  // Not impl: <text>
              {"masking/clipPath/clipping-with-text.svg",
               ImageComparisonParams::Skip()},  // Not impl: <text>
              {"masking/clipPath/on-the-root-svg-without-size.svg",
               ImageComparisonParams::Skip()},  // UB: on root `<svg>` without size
              {"masking/clipPath/recursive-on-child.svg",
               ImageComparisonParams::Skip()},  // UB: Recursive on child
              {"masking/clipPath/recursive-on-self.svg",
               ImageComparisonParams::Skip()},  // UB: Recursive on self
              {"masking/clipPath/recursive.svg", ImageComparisonParams::Skip()},  // UB: Recursive
              {"masking/clipPath/self-recursive.svg",
               ImageComparisonParams::Skip()},  // UB: Recursive
              {"masking/clipPath/with-marker-on-clip.svg",
               ImageComparisonParams::Skip()},  // Not impl: <marker>
              {"masking/clipPath/with-use-child.svg",
               ImageComparisonParams::Skip()},  // Not impl: <use> child
              {"masking/mask/'color-interpolation=linearRGB.svg'",
               ImageComparisonParams::Skip()},  // Not impl: color-interpolation
              {"masking/mask/mask-on-child.svg",
               ImageComparisonParams::Skip()},  // BUG: Mask on child doesn't apply
              {"masking/mask/mask-on-self.svg",
               ImageComparisonParams::Skip()},  // BUG: Mask on self, also a bug in browsers
              {"masking/mask/mask-on-self-with-mixed-mask-type.svg",
               ImageComparisonParams::Skip()},  // BUG: Rendering issue, mask is clipped. Repros in
                                                // renderer_tool but not viewer.
              {"masking/mask/mask-on-self-with-mask-type=alpha.svg",
               ImageComparisonParams::Skip()},  // BUG: Mask on self, also a bug in browsers
              {"masking/mask/recursive-on-child.svg",
               ImageComparisonParams::Skip()},  // UB: Recursive on child
              {"masking/mask/recursive-on-self.svg",
               ImageComparisonParams::Skip()},  // UB: Recursive
              {"masking/mask/recursive.svg",
               ImageComparisonParams::Skip()},  // BUG: Crashes on serializing the skp
              {"masking/mask/self-recursive.svg",
               ImageComparisonParams::Skip()},  // BUG: Crashes on serializing the skp
          },
  };

  for (const auto& config : {paintingConfig, paintServerConfig, shapesConfig, structureConfig,
                             structureImageConfig, maskingConfig}) {
    auto tests = collectTests(testsDir, config);
    testPlan.insert(testPlan.end(), std::make_move_iterator(tests.begin()),
                    std::make_move_iterator(tests.end()));
  }

  std::sort(testPlan.begin(), testPlan.end());
  testPlan.erase(std::unique(testPlan.begin(), testPlan.end(),
                             [](const auto& lhs, const auto& rhs) {
                               return lhs.svgFilename == rhs.svgFilename;
                             }),
                 testPlan.end());

  return testPlan;
}

}  // namespace

TEST_P(ImageComparisonTestFixture, ResvgTest) {
  const ImageComparisonTestcase& testcase = GetParam();
  const std::filesystem::path testSuiteRoot =
      Runfiles::instance().RlocationExternal("resvg-test-suite", "");

  std::filesystem::path goldenFilename = testcase.svgFilename;
  if (testcase.params.overrideGoldenFilename.empty()) {
    goldenFilename.replace_extension(".png");
  } else {
    goldenFilename = testcase.params.overrideGoldenFilename;
  }

  SVGDocument document = loadSVG(testcase.svgFilename.string().c_str(), testSuiteRoot);
  renderAndCompare(document, testcase.svgFilename, goldenFilename.string().c_str());
}

INSTANTIATE_TEST_SUITE_P(Resvg, ImageComparisonTestFixture, ValuesIn(getResvgTests()),
                         TestNameFromFilename);

}  // namespace donner::svg
