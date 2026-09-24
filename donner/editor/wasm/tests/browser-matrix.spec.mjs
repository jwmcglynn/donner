import assert from "node:assert/strict";
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { createRequire } from "node:module";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const require = createRequire(import.meta.url);
const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(testDirectory, "../../../..");

test("Playwright version stays synchronized across package locks and browser metadata", () => {
  const packageJson = JSON.parse(readFileSync(path.join(testDirectory, "package.json"), "utf8"));
  const packageLock = JSON.parse(
    readFileSync(path.join(testDirectory, "package-lock.json"), "utf8"),
  );
  const playwrightVersion = packageJson.devDependencies["@playwright/test"];
  assert.match(playwrightVersion, /^\d+\.\d+\.\d+$/);
  assert.equal(packageLock.packages[""].devDependencies["@playwright/test"], playwrightVersion);
  for (const packageName of ["@playwright/test", "playwright", "playwright-core"]) {
    assert.equal(
      packageLock.packages[`node_modules/${packageName}`].version,
      playwrightVersion,
      `${packageName} in package-lock.json must match package.json`,
    );
  }

  const pnpmLock = readFileSync(path.join(testDirectory, "pnpm-lock.yaml"), "utf8");
  for (
    const lockEntry of [
      `specifier: ${playwrightVersion}`,
      `version: ${playwrightVersion}`,
      `playwright@${playwrightVersion}:`,
      `playwright-core@${playwrightVersion}:`,
    ]
  ) {
    assert.ok(pnpmLock.includes(lockEntry), `pnpm-lock.yaml is missing ${lockEntry}`);
  }
  const scopedPackageKey = `@playwright/test@${playwrightVersion}`;
  assert.ok(
    pnpmLock.includes(`'${scopedPackageKey}':`) || pnpmLock.includes(`"${scopedPackageKey}":`),
    `pnpm-lock.yaml is missing the exact ${scopedPackageKey} key`,
  );

  const browserMetadataPath = path.join(
    testDirectory,
    `browsers.${playwrightVersion}.json`,
  );
  assert.ok(existsSync(browserMetadataPath), "browser metadata filename must match Playwright");
  const browserMetadata = JSON.parse(readFileSync(browserMetadataPath, "utf8"));
  assert.equal(browserMetadata.playwrightVersion, playwrightVersion);
  assert.equal(
    browserMetadata.comment,
    `Pinned from playwright-core ${playwrightVersion}; update with the npm lock.`,
  );
});

function parseBrowserLane(lane) {
  const laneName = /name = "([^"]+)"/.exec(lane)?.[1];
  const performanceLane = laneName === "browser_responsiveness_perf_test";
  const tags = [...(/tags = \[([\s\S]*?)\]/.exec(lane)?.[1] ?? "").matchAll(/"([^"]+)"/g)]
    .map(([, tag]) => tag);
  const specPattern = performanceLane
    ? /\$\(rootpath :([^)]+\.perf\.ts)\)/g
    : /\$\(rootpath :([^)]+\.spec\.ts)\)/g;
  const specFiles = [...lane.matchAll(specPattern)].map(([, spec]) => spec);
  return { lane, laneName, performanceLane, tags, specFiles };
}

function assertCommonBrowserLane({ lane, laneName, specFiles }) {
  assert.ok(laneName, "every browser lane must be named");
  assert.ok(specFiles.length > 0, `${laneName} must run a named spec`);
  for (const specFile of specFiles) {
    assert.ok(
      lane.includes(`"${specFile}"`),
      `${laneName} must list ${specFile} in its data`,
    );
    const spec = readFileSync(path.join(testDirectory, specFile), "utf8");
    for (const [, importedModule] of spec.matchAll(/from "\.\/([^"]+)"/g)) {
      assert.ok(
        lane.includes(`"${importedModule}.ts"`),
        `${laneName} is missing ${specFile} dependency ${importedModule}.ts`,
      );
    }
  }
  assert.match(
    lane,
    /target_compatible_with = \[[\s\S]*?"@platforms\/\/cpu:aarch64"[\s\S]*?"@platforms\/\/os:macos"[\s\S]*?\]/,
    "every browser lane must allow only macOS ARM64 execution",
  );
}

function assertPerformanceLane({ lane, tags, specFiles }) {
  assert.deepEqual(
    tags.sort(),
    ["manual", "no-sandbox", "perf"],
    "responsiveness timing must remain opt-in while allowing Firefox's own sandbox",
  );
  assert.match(lane, /--config=\$\(rootpath :playwright\.responsiveness\.bazel\.config\.js\)/);
  assert.ok(lane.includes("\"playwright.responsiveness.bazel.config.js\""));
  assert.doesNotMatch(
    lane,
    /"@playwright\/\/:(?:chromium|firefox)"/,
    "macOS application symlinks must not travel inside Bazel tree artifacts",
  );
  assert.match(lane, /"DONNER_CHROMIUM_ARCHIVE":/);
  assert.match(lane, /"DONNER_FIREFOX_ARCHIVE":/);
  assert.ok(lane.includes("\"prepare-browser-archives.js\""));
  assert.deepEqual(specFiles, ["browser-responsiveness.perf.ts"]);
}

function assertCompositedLane({ lane, tags, specFiles }, browser) {
  assert.deepEqual(
    tags.sort(),
    ["manual", "no-local"],
    "extended composited browser diagnostics are opt-in and remote-only",
  );
  assert.deepEqual(specFiles, [
    "composited-invariants.spec.ts",
    "composited-drag-invariants.spec.ts",
  ]);
  assert.match(
    lane,
    browser === "firefox"
      ? /--config=\$\(rootpath :playwright\.composited-firefox\.bazel\.config\.js\)/
      : /--config=\$\(rootpath :playwright\.composited-chromium\.bazel\.config\.js\)/,
  );
  assert.ok(lane.includes(`"@playwright//:${browser}"`));
  assert.match(lane, /"DONNER_WASM_REQUIRE_WEBGPU": "1"/);
  assert.ok(tags.includes("no-local"), "the composited browser gate must use remote execution");
  assert.doesNotMatch(
    lane,
    /--grep/,
    "the composited lane must include the classifier controls",
  );
}

function assertFontReferenceLane({ lane, tags, specFiles }) {
  assert.deepEqual(
    tags.sort(),
    ["manual", "no-local"],
    "native font reference evidence must remain opt-in and remote-only",
  );
  assert.deepEqual(specFiles, ["font-reference.spec.ts"]);
  assert.match(lane, /--config=\$\(rootpath :playwright\.font-reference\.config\.js\)/);
  for (
    const dependency of [
      "playwright.font-reference.config.js",
      "//third_party/resvg-test-suite:fonts",
      "@playwright//:chromium",
      "@playwright//:firefox",
    ]
  ) {
    assert.ok(
      lane.includes(`"${dependency}"`),
      `font reference probe is missing ${dependency}`,
    );
  }
  assert.match(
    lane,
    /"DONNER_REFERENCE_FONT": "third_party\/resvg-test-suite\/fonts\/NotoSans-Regular\.ttf"/,
  );
  assert.match(lane, /"NODE_OPTIONS": ""/);
  assert.match(
    lane,
    /"PLAYWRIGHT_BROWSERS_PATH": "\$\(rootpath @playwright\/\/:chromium\)\/\.\.\/"/,
  );
}

function assertBrowserLane(lane) {
  const contract = parseBrowserLane(lane);
  assertCommonBrowserLane(contract);
  const { laneName, performanceLane, tags } = contract;
  if (performanceLane) {
    assertPerformanceLane(contract);
  } else if (laneName === "firefox_composited_invariants_test") {
    assertCompositedLane(contract, "firefox");
  } else if (laneName === "chromium_composited_invariants_test") {
    assertCompositedLane(contract, "chromium");
  } else if (laneName === "font_reference_probe") {
    assertFontReferenceLane(contract);
  } else {
    assert.ok(
      !tags.includes("manual") && !tags.includes("perf"),
      `${laneName} must remain a regression gate`,
    );
  }
}

test("Bazel owns hermetic browser regression and manual performance lanes", () => {
  const moduleFile = readFileSync(path.join(repositoryRoot, "MODULE.bazel"), "utf8");
  assert.match(
    moduleFile,
    /bazel_dep\(name = "rules_playwright", version = "0\.5\.3"/,
    "MODULE.bazel must pin the browser repository rule",
  );
  assert.match(
    moduleFile,
    /browsers_json = "\/\/donner\/editor\/wasm\/tests:browsers\.1\.62\.1\.json"/,
    "the browser revisions must come from the Playwright version in package-lock.json",
  );
  assert.match(
    moduleFile,
    /builds\/cft\/151\.0\.7922\.34\/mac-arm64\/chrome-mac-arm64\.zip/,
    "macOS remote tests must use Playwright's Chrome-for-Testing artifact path",
  );
  assert.match(
    moduleFile,
    /"playwright-chromium-mac14-arm64": "playwright_chromium_mac_arm64"/,
    "the default rules_playwright macOS archive must be replaced hermetically",
  );
  assert.match(
    moduleFile,
    /"playwright-chromium-mac15-arm64": "playwright_chromium_mac_arm64"/,
    "the latest supported rules_playwright macOS archive must be replaced hermetically",
  );

  const buildFilePath = path.join(testDirectory, "BUILD.bazel");
  assert.ok(existsSync(buildFilePath), "Wasm browser tests need a Bazel package");
  const buildFile = readFileSync(buildFilePath, "utf8");
  assert.match(buildFile, /playwright_bin\.playwright_test\(/);
  assert.match(buildFile, /name = "chromium_remote_smoke"/);
  assert.match(buildFile, /name = "browser_presentation_regression_test"/);
  // Every Bazel-owned browser lane, checked one at a time rather than against
  // the file as a whole: a contract satisfied by some other lane in the same
  // file is not a contract. Each lane names a spec, and the spec's own local
  // imports have to be in that lane's `data` or the test runs against a
  // runfiles tree that is missing them.
  const lanes = [...buildFile.matchAll(/playwright_bin\.playwright_test\(([\s\S]*?)\n\)\n/g)]
    .map(([, body]) => body);
  assert.deepEqual(
    lanes.map((lane) => /name = "([^"]+)"/.exec(lane)?.[1]).sort(),
    [
      "boot_presentation_browser_backend_test",
      "boot_presentation_test",
      "browser_presentation_regression_browser_backend_test",
      "browser_presentation_regression_test",
      "browser_responsiveness_perf_test",
      "catalog_font_loading_browser_backend_test",
      "catalog_font_loading_test",
      "chromium_composited_invariants_test",
      "chromium_remote_smoke",
      "chromium_remote_smoke_browser_backend",
      "firefox_composited_invariants_test",
      "font_reference_probe",
      "standalone_geode_browser_renderer_test",
    ],
    "every playwright_test lane must be named and checked; update this contract when one is added",
  );
  for (const lane of lanes) {
    assertBrowserLane(lane);
  }
  // A browser-backend lane serves only the package that selects that backend, and the production
  // package serves every other lane, so neither can stand in for the other. One of them also runs
  // the check that the worker selected the browser backend at all.
  const browserBackendPackage = "//donner/editor/wasm:_wasm_web_package_browser_backend_for_serve";
  for (const lane of lanes) {
    const laneName = /name = "([^"]+)"/.exec(lane)?.[1];
    const browserBackendLane = /browser_backend/.test(laneName);
    if (laneName === "standalone_geode_browser_renderer_test") {
      assert.ok(
        lane.includes("\"//donner/svg/renderer/wasm:_geode_browser_test_package_for_playwright\""),
        "standalone renderer must serve its browser-selected Geode package",
      );
      assert.ok(
        lane.includes("\"//donner/editor/tests:standalone_geode_browser_png_compare\""),
        "standalone renderer pixels must use the shared pixelmatch helper",
      );
      assert.ok(
        lane.includes(
          "\"//donner/editor/tests:testdata/browser/standalone_geode_browser_renderer.png\"",
        ),
        "standalone renderer must carry its committed golden",
      );
      continue;
    }
    assert.equal(
      lane.includes(`"${browserBackendPackage}"`),
      browserBackendLane,
      `${laneName} must serve the browser-backend package exactly when it is a browser-backend lane`,
    );
    if (browserBackendLane) {
      assert.ok(
        !lane.includes(`"//donner/editor/wasm:_wasm_web_package_for_serve"`),
        `${laneName} must not serve the production package`,
      );
    }
    // The selection check reads which backend to expect from its lane, so only a lane serving the
    // browser-backend package may expect that one, and every other lane checks it was not chosen.
    assert.equal(
      lane.includes(`"DONNER_WASM_EXPECTED_HEADLESS_BACKEND": "browser"`),
      browserBackendLane && lane.includes("$(rootpath :browser-backend-selection.spec.ts)"),
      `${laneName} must expect the browser backend exactly when it checks the browser-backend package`,
    );
  }
  for (const laneName of ["boot_presentation_test", "boot_presentation_browser_backend_test"]) {
    const lane = lanes.find((body) => body.includes(`name = "${laneName}"`));
    assert.ok(
      lane?.includes("$(rootpath :browser-backend-selection.spec.ts)"),
      `${laneName} must check which backend the raster worker selected`,
    );
  }
  assert.match(buildFile, /"@playwright\/\/:chromium"/);
  assert.match(
    buildFile,
    /"\/\/donner\/editor\/wasm:_wasm_web_package_for_serve"/,
    "the browser lane must own the editor-Wasm transition instead of requiring caller flags",
  );
  const editorBuildFile = readFileSync(path.join(testDirectory, "..", "BUILD.bazel"), "utf8");
  assert.match(
    editorBuildFile,
    /editor_wasm_geode_transitioned_target\([\s\S]*?name = "_wasm_web_package_for_serve"[\s\S]*?visibility = \["\/\/donner\/editor\/wasm\/tests:__pkg__"\]/,
    "only the browser-test package should be able to consume the transitioned web package",
  );
  assert.match(
    buildFile,
    /"PLAYWRIGHT_BROWSERS_PATH": "\$\(rootpath @playwright\/\/:chromium\)\/\.\.\/"/,
  );
  // Linux is excluded deliberately, not by omission: headless Chromium there
  // cannot present a WebGPU OffscreenCanvas swapchain on the SwiftShader
  // adapter, so the presented-pixel assertions can never pass. Restoring the
  // platform needs a GPU-backed runner or upstream swapchain support, not just
  // a constraint edit, so re-adding it here should fail this contract first.
  for (const lane of lanes) {
    assert.doesNotMatch(
      lane,
      /"@platforms\/\/os:linux"/,
      "presented-pixel browser tests cannot run on Linux until WebGPU swapchain presentation exists there",
    );
  }

  const chromiumConfig = readFileSync(path.join(testDirectory, "playwright.config.js"), "utf8");
  assert.match(
    chromiumConfig,
    /channel:\s*"chromium"/,
    "the regular Chromium build preserves WebGPU presentation without opening a window",
  );
});

test("default browser discovery excludes the manual font reference probe", () => {
  const defaultConfig = require("./playwright.config.js");
  const referenceConfig = readFileSync(
    path.join(testDirectory, "playwright.font-reference.config.js"),
    "utf8",
  );
  assert.ok(
    defaultConfig.testIgnore.includes("font-reference.spec.ts"),
    "default discovery must not load a manual probe that requires explicit font inputs",
  );
  assert.match(referenceConfig, /testMatch: "font-reference\.spec\.ts"/);
});

test("default browser discovery excludes the Node selector aggregator", () => {
  const buildFile = readFileSync(path.join(testDirectory, "BUILD.bazel"), "utf8");
  const selectorTarget = buildFile.match(
    /name = "selector_tests"[\s\S]*?entry_point = "([^"]+)"/,
  );
  assert.ok(selectorTarget, "selector_tests must keep an explicit Node entry point");
  assert.equal(selectorTarget[1], "selector-tests.mjs");
  assert.doesNotMatch(
    selectorTarget[1],
    /\.spec\.[cm]?[jt]s$/,
    "the Node aggregator must not match Playwright's default test-file discovery",
  );
  assert.ok(existsSync(path.join(testDirectory, selectorTarget[1])));
});

test("browser diagnostics do not manufacture fatal adapter failures", () => {
  const smokeTest = readFileSync(path.join(testDirectory, "smoke.spec.ts"), "utf8");
  assert.doesNotMatch(
    smokeTest,
    /requestAdapter\(\{ forceFallbackAdapter: true \}\)/,
    "an optional fallback-adapter probe must not emit a fatal-class browser warning",
  );
  assert.match(
    smokeTest,
    /const recordFatalMessage[\s\S]*?fatalMessages\.push[\s\S]*?console\.error[\s\S]*?page\.on\("console"[\s\S]*?recordFatalMessage[\s\S]*?page\.on\("pageerror"[\s\S]*?recordFatalMessage/,
    "startup failures must be emitted while they are captured, before openEditor can time out",
  );
});

test("remote browser server preserves bounded request failures", () => {
  const server = readFileSync(path.join(testDirectory, "remote-webserver.mjs"), "utf8");
  assert.match(
    server,
    /function describeServerError[\s\S]*?replaceAll\(root, "<package>"\)[\s\S]*?if \(error\?\.code !== "ENOENT"\) \{[\s\S]*?console\.error/,
    "the remote webserver must preserve bounded package and request failures in test.log",
  );
  assert.match(
    server,
    /\.replace\(\/\(\?:\\\/\[\^\\s\/:\]\+\)\{2,\}\/g, "<path>"\)/,
    "server diagnostics must scrub absolute paths outside the package root",
  );
  assert.match(
    server,
    /\.slice\(0, 512\)/,
    "server diagnostics must stay bounded in remote test logs",
  );
  assert.match(
    server,
    /console\.error\(`remote-webserver request failed: \$\{describeServerError\(error\)\}`\)/,
    "non-ENOENT failures must emit the sanitized diagnostic",
  );
});

test("CI discovers Firefox, WebKit, and real Safari compatibility regressions", () => {
  const config = require("./playwright.compatibility.config.js");
  const projects = new Map(config.projects.map((project) => [project.name, project]));
  assert.deepEqual(config.testMatch, [
    "smoke.spec.ts",
    "browser-presentation-regression.spec.ts",
  ]);

  const firefox = projects.get("firefox-geode-resize");
  assert.ok(firefox, "missing Firefox Geode resize project");
  assert.equal(firefox.use.browserName, "firefox");
  assert.match(
    String(firefox.grep),
    /Firefox keeps Basic Shapes resize pixels and outline synchronized/,
  );
  assert.match(
    String(firefox.grep),
    /Firefox keeps the dragged shape and its selection outline in every drag frame/,
  );
  assert.match(
    String(firefox.grep),
    /Firefox never exposes the checkerboard while dragging a Splash letter/,
  );

  const webkit = projects.get("webkit-geode-carousel");
  assert.ok(webkit, "missing WebKit Geode carousel project");
  assert.equal(webkit.use.browserName, "webkit");
  assert.match(
    String(webkit.grep),
    /carousel loads Basic Shapes on the first interactive frame/,
  );
  assert.match(
    String(webkit.grep),
    /WebKit Geode survives a burst of drag wakeups without fatal errors/,
  );

  const packageJson = JSON.parse(readFileSync(path.join(testDirectory, "package.json"), "utf8"));
  assert.equal(
    packageJson.scripts["test:compatibility"],
    "playwright test --config=playwright.compatibility.config.js",
  );
  assert.equal(
    packageJson.scripts["test:safari-geode"],
    "node safari-geode-regression.mjs",
  );
  assert.match(packageJson.scripts["test:selector"], /browser-matrix\.spec\.mjs/);

  const workflow = readFileSync(
    path.join(repositoryRoot, ".github/workflows/editor_wasm.yml"),
    "utf8",
  );
  const normalizedWorkflow = workflow.replace(/\\\s*\n\s*/g, " ").replace(/\s+/g, " ");
  assert.match(
    normalizedWorkflow,
    /bazelisk test --config=editor-wasm --test_output=errors --remote_download_outputs=all --test_arg=--payload-budget-mode=measure \/\/tools\/ci:editor_wasm_size_tests/,
  );
  assert.match(
    normalizedWorkflow,
    /bazelisk test --test_output=errors \/\/tools\/ci:editor_wasm_audits/,
  );
  assert.doesNotMatch(
    workflow,
    /editor-wasm-tiny-skia|wasm_tiny_skia|package-tiny_skia|backend:\s*tiny_skia/,
  );
  assert.doesNotMatch(workflow, /test:ios|playwright\\.ios/);

  // The browser job's lane sequence lives in tools/run-browser-ci.sh so that
  // one local command reproduces the job; the workflow step only supplies the
  // package directory and invokes the script. Both halves of that mirror are
  // checked here: the workflow must still call the script, and the script must
  // still carry every lane, in CI order.
  assert.match(normalizedWorkflow, /run: bash tools\/run-browser-ci\.sh/);

  const browserCi = readFileSync(
    path.join(repositoryRoot, "tools/run-browser-ci.sh"),
    "utf8",
  );
  const normalizedBrowserCi = browserCi
    .replace(/\\\s*\n\s*/g, " ")
    .replace(/\s+/g, " ");
  const laneCommands = [
    "run_lane \"chromium-default\" bash donner/editor/wasm/tests/run_tests.sh --headed",
    "run_lane \"firefox-geode-resize\" npm --prefix donner/editor/wasm/tests"
    + " run test:compatibility -- --project=firefox-geode-resize --headed",
    "run_lane \"webkit-geode-carousel\" npm --prefix donner/editor/wasm/tests"
    + " run test:compatibility -- --project=webkit-geode-carousel --headed",
    "run_lane \"firefox-composited-invariants\" bash"
    + " donner/editor/wasm/tests/run_tests.sh --headed"
    + " --config=playwright.composited-firefox.config.js",
    "run_lane \"composited-chromium\" bash"
    + " donner/editor/wasm/tests/run_tests.sh --headed"
    + " --config=playwright.composited-chromium.config.js",
  ];
  let searchFrom = 0;
  for (const laneCommand of laneCommands) {
    const found = normalizedBrowserCi.indexOf(laneCommand, searchFrom);
    assert.notEqual(
      found,
      -1,
      `tools/run-browser-ci.sh is missing this lane, or runs it out of CI order: ${laneCommand}`,
    );
    searchFrom = found + laneCommand.length;
  }
  // The suites scale their timing bounds by kCiTimeScale when CI is set, so a
  // local run measures CI's thresholds only if the script exports it.
  assert.match(normalizedBrowserCi, /export CI="\$\{CI:-true\}"/);
  // Lanes share one Playwright output directory and Playwright empties it on
  // start, so the job can only keep a failing lane's evidence if the script
  // archives each lane's results per lane and the workflow uploads THAT.
  assert.match(normalizedBrowserCi, /archive_lane_results "\$\{name\}"/);
  assert.match(
    normalizedBrowserCi,
    /kFailureArchiveDir="\$\{kTestsDir\}\/playwright-failures"/,
  );
  assert.match(normalizedWorkflow, /path: donner\/editor\/wasm\/tests\/playwright-failures/);
});

test("composited probe evidence survives lane archival and the next lane", async () => {
  const { stopCompositedProbe } = await import("./composited-probe-evidence.mjs");
  const temporary = mkdtempSync(path.join(tmpdir(), "donner-probe-evidence-"));
  const outputDirectory = path.join(temporary, "test-results", "failed-drag");
  const archiveDirectory = path.join(temporary, "archived-lane");
  mkdirSync(outputDirectory, { recursive: true });
  const result = {
    samples: [{ t: 10, coloredCentroidX: 8, coloredCentroidY: 4, drawOk: true }],
    drawFailures: 0,
    frames: 1,
    readbackRetries: 0,
    readbackRescues: 0,
  };
  const gesture = { trace: [[10, 20, 30]] };
  const attachments = [];
  const testInfo = {
    outputPath: (name) => path.join(outputDirectory, name),
    attach: async (name, attachment) => attachments.push({ name, ...attachment }),
  };
  try {
    const observed = await stopCompositedProbe({ evaluate: async () => result }, testInfo, gesture);
    assert.deepEqual(observed, result);
    assert.equal(attachments.length, 1);
    assert.equal(attachments[0].name, "composited-probe");
    assert.equal(attachments[0].contentType, "application/json");
    assert.equal(typeof attachments[0].path, "string", "evidence must be an archived output file");
    const relative = path.relative(outputDirectory, attachments[0].path);
    assert.equal(relative, path.basename(relative), "evidence must stay inside this test's output");
    cpSync(outputDirectory, archiveDirectory, { recursive: true });
    rmSync(outputDirectory, { recursive: true });
    assert.deepEqual(JSON.parse(readFileSync(path.join(archiveDirectory, relative), "utf8")), {
      gesture,
      result,
    });
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
});

test("drag failure readbacks are archived after sampling with exact frame identities", async () => {
  const { attachCompositedReadbacks } = await import("./composited-probe-evidence.mjs");
  const temporary = mkdtempSync(path.join(tmpdir(), "donner-readback-evidence-"));
  const png = Buffer.from(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScLbtAAAAABJRU5ErkJggg==",
    "base64",
  );
  const attachments = [];
  const testInfo = {
    outputPath: (name) => path.join(temporary, name),
    attach: async (name, attachment) => attachments.push({ name, ...attachment }),
  };
  try {
    await attachCompositedReadbacks(
      {
        evaluate: async (_callback, request) => {
          assert.deepEqual(request.indices, [0, 340, 341, 342]);
          return request.indices.map((index) => ({ index, png: png.toString("base64") }));
        },
      },
      testInfo,
      [0, 340, 341, 342, 341],
    );
    assert.deepEqual(attachments.map((item) => item.name), [
      "composited-frame-0",
      "composited-frame-340",
      "composited-frame-341",
      "composited-frame-342",
    ]);
    for (const attachment of attachments) {
      assert.equal(attachment.contentType, "image/png");
      assert.deepEqual(readFileSync(attachment.path), png);
      assert.equal(path.dirname(attachment.path), temporary);
    }
    await assert.rejects(
      () => attachCompositedReadbacks({ evaluate: async () => [] }, testInfo, [341]),
      /missing.*341/i,
    );
    await assert.rejects(
      () =>
        attachCompositedReadbacks(
          {
            evaluate: async () => [{
              index: 341,
              png: Buffer.alloc(8 * 1024 * 1024 + 1).toString("base64"),
            }],
          },
          testInfo,
          [341],
        ),
      /budget/i,
    );
    assert.equal(attachments.length, 4, "invalid evidence must not attach a partial set");
    assert.deepEqual(readFileSync(path.join(temporary, "composited-frame-341.png")), png);
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
});

test("real Safari gate pins the served Wasm and scopes every visibility probe", () => {
  const harness = readFileSync(
    path.join(testDirectory, "safari-geode-regression.mjs"),
    "utf8",
  );

  assert.match(harness, /DONNER_SAFARI_EXPECTED_WASM_SHA256/);
  assert.match(harness, /DONNER_SAFARI_ALLOW_UNPINNED_PACKAGE/);
  assert.match(harness, /\^\[0-9a-f\]\{64\}\$/);
  assert.match(
    harness,
    /result\.packageArtifacts\.wasm\.sha256,[\s\S]*expectedWasmSha256/,
    "the expected digest must be compared with bytes fetched from the served editor.wasm",
  );
  assert.match(
    harness,
    /async function requireVisibleSafariAnimationFrame[\s\S]*finally\s*\{[\s\S]*cleanupVisibleSafariAnimationFrameProbe/,
    "the timer/rAF probe must be cleaned up on success, timeout, and error",
  );
  assert.match(
    harness,
    /\/refresh[\s\S]*installErrorCapture\(driver\);[\s\S]*requireVisibleSafariAnimationFrame\(driver\);[\s\S]*waitForEditor/,
    "a reload must pass the same visible-rAF preflight as initial navigation",
  );
});

test("real Safari memory gate clicks Donner Splash and dwells for five minutes", () => {
  const harness = readFileSync(
    path.join(testDirectory, "safari-geode-regression.mjs"),
    "utf8",
  );

  assert.match(
    harness,
    /kMemoryOnly[\s\S]*id:\s*"donner-splash"[\s\S]*await click\(driver,\s*sampleClickPoint\.x,\s*sampleClickPoint\.y\)[\s\S]*activeSample\?\.sampleId === sample\.id/,
    "the memory gate must enter the Splash through the trusted carousel click path",
  );
  assert.match(
    harness,
    /const kMemoryDwellMs = 5 \* 60 \* 1_000;/,
    "the Safari significant-memory gate must cover the five-minute reload window",
  );
  assert.match(
    harness,
    /result\.memoryDwell\.length >= kMemoryDwellMs \/ kMemorySampleIntervalMs/,
    "the gate must prove its sampler stayed alive throughout the five-minute dwell",
  );
  assert.match(
    harness,
    /const kMemoryMaxWebContentRssBytes = 512 \* 1024 \* 1024;/,
    "the gate must fail before Safari's WebContent process reaches termination pressure",
  );
  assert.match(
    harness,
    /result\.memoryWebContentSamples\.push[\s\S]*rssBytes[\s\S]*kMemoryMaxWebContentRssBytes/,
    "the gate must observe WebContent RSS outside the page's Wasm and WebGPU counters",
  );
  assert.match(
    harness,
    /Safari WebContent process exited during the memory dwell/,
    "a WebContent process replacement must be reported as a reload or termination",
  );
  assert.match(
    harness,
    /__donnerSafariRegressionPageLifetimeToken[\s\S]*finalState\.pageLifetimeToken[\s\S]*kPageLifetimeToken/,
    "reload detection must use a stable page token rather than Safari's drifting time origin",
  );
});

test("macOS perf Firefox archive has a content integrity pin", () => {
  const packageJson = JSON.parse(readFileSync(path.join(testDirectory, "package.json"), "utf8"));
  const version = packageJson.devDependencies["@playwright/test"];
  const browserMetadata = JSON.parse(
    readFileSync(path.join(testDirectory, `browsers.${version}.json`), "utf8"),
  );
  const firefox = browserMetadata.browsers.find((browser) => browser.name === "firefox");
  assert.ok(firefox, "the browser metadata must include Firefox");
  const moduleSource = readFileSync(path.join(repositoryRoot, "MODULE.bazel"), "utf8");
  const integrityMap = /integrity_path_map\s*=\s*\{([\s\S]*?)\}/.exec(moduleSource)?.[1] ?? "";
  const entries = new Map(
    [...integrityMap.matchAll(/"([^"\n]+)":\s*"([^"\n]+)"/g)].map((match) => [match[1], match[2]]),
  );
  const archive = `builds/firefox/${firefox.revision}/firefox-mac-arm64.zip`;
  assert.match(
    entries.get(archive) ?? "",
    /^sha256-[A-Za-z0-9+/]{43}=$/,
    "the native extractor must receive a content-authenticated Firefox archive",
  );
});
