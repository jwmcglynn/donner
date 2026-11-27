import assert from "node:assert/strict";
import { execFileSync, spawnSync } from "node:child_process";
import {
  cpSync,
  existsSync,
  mkdirSync,
  mkdtempSync,
  readdirSync,
  readFileSync,
  rmSync,
  statSync,
  writeFileSync,
} from "node:fs";
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
  const performanceLane = laneName === "_browser_responsiveness_perf_driver";
  const tags = [...(/tags = \[([\s\S]*?)\]/.exec(lane)?.[1] ?? "").matchAll(/"([^"]+)"/g)]
    .map(([, tag]) => tag);
  const specPattern = performanceLane
    ? /\$\(rootpath :([^)]+\.perf\.ts)\)/g
    : /\$\(rootpath :([^)]+\.spec\.ts)\)/g;
  const specFiles = [...lane.matchAll(specPattern)].map(([, spec]) => spec);
  return { lane, laneName, performanceLane, tags, specFiles };
}

// Every form by which a test module loads a file beside it, with either quote style: static
// imports and re-exports (type-only ones included, a harmless over-requirement), side-effect and
// dynamic imports, CommonJS requires, require.resolve, and a URL resolved against the module.
// Matches inside comments and JSDoc count as requirements too. That fails closed: an extra `data`
// entry at worst, never a load the walk misses.
const kLocalFileReferences = [
  /\b(?:from|import)\s*(["'])(\.\.?\/[^"']+)\1/g,
  /\b(?:import|require|require\.resolve)\(\s*(["'])(\.\.?\/[^"']+)\1\s*\)/g,
  /\bnew URL\(\s*(["'])(\.\.?\/[^"']+)\1\s*,\s*import\.meta\.url\s*\)/g,
];
const kModuleFile = /\.[cm]?[jt]s$/;

function localFileReferences(source) {
  return kLocalFileReferences.flatMap((pattern) =>
    [...source.matchAll(pattern)].map(([, , specifier]) => specifier)
  );
}

// Resolves a reference to a file in this package: the exact path, else the module extensions in
// order, else a directory index. A file outside the package would be listed by label, which this
// contract does not follow, so reaching one fails instead of passing unchecked.
function resolveLocalFile(fromFile, specifier, via) {
  const target = path.posix.normalize(path.posix.join(path.posix.dirname(fromFile), specifier));
  const reference = `${via.join(" -> ")} -> ${specifier}`;
  assert.ok(
    !target.startsWith("../"),
    `${reference} leaves the test package; list it by label and extend this contract`,
  );
  const candidates = [
    target,
    ...[".ts", ".mjs", ".js", ".cjs"].map((extension) => `${target}${extension}`),
    ...["index.ts", "index.js"].map((index) => `${target}/${index}`),
  ];
  const resolved = candidates.find((candidate) => {
    const candidatePath = path.join(testDirectory, candidate);
    return existsSync(candidatePath) && statSync(candidatePath).isFile();
  });
  assert.ok(resolved, `${reference} resolves to no file; tried ${candidates.join(", ")}`);
  return resolved;
}

// The package modules a target names through location expansion in its args or env: its specs,
// its Playwright config and the servers it starts.
function locatedModules(target) {
  return [...target.matchAll(/\$\((?:rootpaths?|locations?|execpaths?) :([^)\s]+)\)/g)]
    .map(([, file]) => file)
    .filter((file) => kModuleFile.test(file));
}

// Requires `target` to list every package module it names through location expansion; a manual
// target is never analyzed by a pull request build, so Bazel's own check would come only nightly.
// Then walks every package file those modules and `entryPoints` load, directly or through one
// another, and requires `target` or one of `runfilesFrom` (targets whose runfiles it carries) to
// list each one. A file that only a listed helper or a config loads is just as absent from the
// runfiles, and the target then fails before any case runs. Each file keeps the chain that first
// reached it, for the failure.
function assertListsLoadedFiles(targetName, target, { entryPoints = [], runfilesFrom = [] } = {}) {
  const located = locatedModules(target);
  for (const module of located) {
    assert.ok(target.includes(`"${module}"`), `${targetName} must list ${module} in its data`);
  }
  const listedIn = [target, ...runfilesFrom];
  const chains = new Map([...entryPoints, ...located].map((root) => [root, [root]]));
  for (const [file, chain] of chains) {
    if (!kModuleFile.test(file)) continue;
    const filePath = path.join(testDirectory, file);
    assert.ok(existsSync(filePath), `${targetName} loads missing module ${chain.join(" -> ")}`);
    for (const specifier of localFileReferences(readFileSync(filePath, "utf8"))) {
      const dependency = resolveLocalFile(file, specifier, chain);
      if (chains.has(dependency)) continue;
      const dependencyChain = [...chain, dependency];
      chains.set(dependency, dependencyChain);
      assert.ok(
        listedIn.some((body) => body.includes(`"${dependency}"`)),
        `${targetName} is missing ${dependency}, loaded through ${dependencyChain.join(" -> ")}`,
      );
    }
  }
}

test("the browser lane walk follows every way a module loads a file beside it", () => {
  const source = [
    "import { a } from \"./static\";",
    "import type { B } from './type-only';",
    "export * from \"./re-export.mjs\";",
    "import \"./side-effect.js\";",
    "const lazy = await import('./dynamic.mjs');",
    "const helper = require(\"./common.cjs\");",
    "const setup = require.resolve(\"./setup.js\");",
    "const child = new URL(\"./child.cjs\", import.meta.url);",
    "const parent = require(\"../parent.js\");",
    "import { test } from \"@playwright/test\";",
  ].join("\n");
  assert.deepEqual(localFileReferences(source).sort(), [
    "../parent.js",
    "./child.cjs",
    "./common.cjs",
    "./dynamic.mjs",
    "./re-export.mjs",
    "./setup.js",
    "./side-effect.js",
    "./static",
    "./type-only",
  ]);
  const via = ["root.ts"];
  assert.equal(
    resolveLocalFile("browser-responsiveness.perf.ts", "./browser-gpu-residency", via),
    "browser-gpu-residency.ts",
  );
  assert.equal(
    resolveLocalFile("browser-watchdog.mjs", "./browser-process-snapshot.cjs", via),
    "browser-process-snapshot.cjs",
  );
  assert.throws(
    () => resolveLocalFile("root.ts", "../parent.js", via),
    /root\.ts -> \.\.\/parent\.js leaves the test package/,
  );
  assert.throws(
    () => resolveLocalFile("root.ts", "./absent", via),
    /root\.ts -> \.\/absent resolves to no file; tried absent, absent\.ts, .*absent\/index\.js/,
  );
  assert.deepEqual(
    locatedModules(
      "\"--config=$(rootpath :playwright.bazel.config.js)\", \"$(rootpath :smoke.spec.ts)\", "
        + "\"$(rootpath :remote-webserver.mjs)\", \"$(rootpath //donner/editor/wasm:pkg)\", "
        + "\"$(rootpath :_driver)\", \"$(rootpath @playwright//:chromium)/../\"",
    ),
    ["playwright.bazel.config.js", "smoke.spec.ts", "remote-webserver.mjs"],
  );
  // A target that lists its config and the config's base, but not the file the base requires.
  const target = [
    "name = \"synthetic_lane\",",
    "args = [\"--config=$(rootpath :playwright.standalone-geode.bazel.config.js)\"],",
    "data = [\"playwright.standalone-geode.bazel.config.js\", \"playwright.bazel.config.js\", "
    + "\"chromium-launch-env.cjs\"],",
  ].join("\n");
  assert.throws(() => assertListsLoadedFiles("synthetic_lane", target), {
    message: "synthetic_lane is missing playwright.config.js, loaded through "
      + "playwright.standalone-geode.bazel.config.js -> playwright.bazel.config.js -> "
      + "playwright.config.js",
  });
  assert.doesNotThrow(() =>
    assertListsLoadedFiles("synthetic_lane", target, {
      runfilesFrom: ["data = [\"playwright.config.js\", \"browser-launch-probe.js\", \"test-timing.cjs\"],"],
    })
  );
});

test("graph browser targets declare each project and all imports used by its specs", () => {
  const build = readFileSync(path.join(testDirectory, "BUILD.bazel"), "utf8");
  const groups = new Map([...build.matchAll(/filegroup\(\n {4}name = "(editor_browser_[^"]+)",([\s\S]*?)\n\)/g)]
    .map(([, name, body]) => [name, body]));
  const targets = [...build.matchAll(/editor_browser_test\(\n([\s\S]*?)\n\)/g)].map(([, body]) => body);
  const projects = new Map();
  for (const body of targets) {
    const name = /name = "([^"]+)"/.exec(body)[1];
    const project = /project = "([^"]+)"/.exec(body)[1];
    const specs = [.../specs = \[([^\]]+)\]/.exec(body)[1].matchAll(/"([^"]+)"/g)].map(([, file]) => file);
    projects.set(project, [...(projects.get(project) || []), ...specs]);
    const reach = new Set(["editor_browser_config_sources"]);
    for (const [, group] of body.matchAll(/":(editor_browser_[^"]+)"/g)) reach.add(group);
    const inputs = [];
    for (const group of reach) {
      const source = groups.get(group);
      assert.ok(source, `unknown source group ${group}`);
      inputs.push(source);
      for (const [, dep] of source.matchAll(/":(editor_browser_[^"]+)"/g)) reach.add(dep);
    }
    assertListsLoadedFiles(name, body, { entryPoints: ["playwright.editor-ci.config.js", ...specs], runfilesFrom: inputs });
  }
  assert.deepEqual([...projects.keys()].sort(), [
    "chromium-default", "composited-chromium", "firefox-composited-invariants", "firefox-geode-resize", "firefox-quarantine-report", "webkit-geode-carousel",
  ]);
  for (const browser of ["composited-chromium", "firefox-composited-invariants"]) {
    assert.deepEqual(projects.get(browser).sort(), ["composited-drag-invariants.spec.ts", "composited-invariants.spec.ts"]);
  }
  assert.deepEqual(projects.get("webkit-geode-carousel").sort(),
    ["browser-presentation-regression.spec.ts", "smoke.spec.ts"]);
  assert.deepEqual(projects.get("firefox-geode-resize").sort(), [
    "browser-backend-selection.spec.ts", "browser-presentation-regression.spec.ts",
    "browser-surface-recovery.spec.ts", "smoke.spec.ts",
  ]);
  const regular = readdirSync(testDirectory).filter((file) => file.endsWith(".spec.ts")
    && !require("./playwright.config.js").testIgnore.includes(file)
    && !["browser-surface-recovery.spec.ts", "static-host-isolation.spec.ts"].includes(file));
  assert.deepEqual(projects.get("chromium-default").sort(), regular.sort());
});

test("editor graph browser projects retain the committed overlay snapshot names", () => {
  const build = readFileSync(path.join(testDirectory, "BUILD.bazel"), "utf8");
  const inputs = /name = "editor_browser_presentation_sources",([\s\S]*?)\n\)/.exec(build)[1];
  for (const [lane, expected] of [["chromium-default", ""], ["firefox-geode-resize", "firefox-geode-resize"]]) {
    const project = execFileSync(process.execPath, ["-p", 'require("./playwright.editor-ci.config.js").projects[0].name'], {
      cwd: testDirectory,
      env: {
        ...process.env,
        DONNER_EDITOR_BROWSER_PROJECT: lane,
        DONNER_WASM_TEST_SERVER: path.join(testDirectory, "remote-webserver.mjs"),
        DONNER_WASM_PACKAGE_DIR: testDirectory,
        TEST_TMPDIR: process.env.TEST_TMPDIR || tmpdir(),
      },
      encoding: "utf8",
    }).trim();
    assert.equal(project, expected);
    const suffix = project ? "-" + project : "";
    const golden = "browser-presentation-regression.spec.ts-snapshots/basic-shapes-compositor-tile-overlay"
      + suffix + "-darwin.png";
    assert.ok(inputs.includes('"' + golden + '"'), golden);
  }
});

function assertCommonBrowserLane({ lane, laneName, specFiles }) {
  assert.ok(laneName, "every browser lane must be named");
  assert.ok(specFiles.length > 0, `${laneName} must run a named spec`);
  // Start from every module the lane names, its specs included: the Playwright config and the
  // web server it starts load package files of their own.
  assertListsLoadedFiles(laneName, lane);
  assert.match(
    lane,
    /target_compatible_with = \[[\s\S]*?"@platforms\/\/cpu:aarch64"[\s\S]*?"@platforms\/\/os:macos"[\s\S]*?\]/,
    "every browser lane must allow only macOS ARM64 execution",
  );
}

function assertPerformanceLane({ lane, tags, specFiles }) {
  // The driver runs only under the watchdog lane, which owns the `perf` tag. Perf discovery
  // selects every `perf`-tagged test, so the tag here would run the driver without its watchdog.
  assert.deepEqual(
    tags.sort(),
    ["manual", "no-sandbox"],
    "the responsiveness driver must stay opt-in, allow Firefox's own sandbox, and stay out of "
      + "perf discovery",
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

function assertDocsLane({ lane, tags, specFiles }) {
  assert.deepEqual(tags, ["launches-browser"]);
  assert.deepEqual(specFiles, ["docs-selector.spec.ts"]);
  assert.match(lane, /--config=\$\(rootpath :playwright\.docs\.config\.js\)/);
  assert.ok(lane.includes('"//tools:docs_selector_fixture_json"'));
  assert.match(lane, /"DONNER_DOCS_FIXTURE": "\$\(rootpath \/\/tools:docs_selector_fixture_json\)"/);
  assert.doesNotMatch(lane, /wasm_web_package|DONNER_WASM_REQUIRE_WEBGPU/);
}

// The Editor WASM chromium-default lane runs every case of smoke.spec.ts in Chromium: the default
// config discovers it, the lane runs that config with no filter, and a selected case has no skip
// that could leave Chromium out.
function assertChromiumDefaultLaneRuns(lane) {
  const defaultConfig = require("./playwright.config.js");
  assert.equal(defaultConfig.testMatch, "**/*.spec.ts");
  assert.ok(!defaultConfig.testIgnore.includes("smoke.spec.ts"));
  assert.equal(defaultConfig.projects, undefined);
  const browserCi = readFileSync(path.join(repositoryRoot, "tools/run-browser-ci.sh"), "utf8");
  assert.ok(browserCi.includes(
    "run_lane \"chromium-default\" \\\n  bash donner/editor/wasm/tests/run_tests.sh --headed\n",
  ));
  const spec = readFileSync(path.join(testDirectory, "smoke.spec.ts"), "utf8");
  const grep = /"--grep=([^"]+)"/.exec(lane)?.[1];
  assert.ok(grep, "the lane selects its cases with --grep");
  const pattern = new RegExp(grep);
  const declarations = [...spec.matchAll(/^test(?:\.\w+)?\(\s*"([^"]+)"/gm)];
  const selected = declarations.filter(([, title]) => pattern.test(title));
  assert.ok(selected.length > 0, `${grep} selects smoke cases`);
  for (const match of selected) {
    const next = spec.indexOf("\ntest", match.index + 1);
    const body = spec.slice(match.index, next === -1 ? undefined : next);
    assert.doesNotMatch(body, /test\.(?:skip|fixme)\(/, `${match[1]} runs in Chromium`);
  }
}

function assertAnimationLane({ lane, tags, specFiles }, grep) {
  assert.deepEqual(specFiles, ["smoke.spec.ts"]);
  assert.ok(tags.includes("launches-browser"));
  assert.ok(tags.includes("requires_desktop_session"));
  assert.ok(!tags.includes("manual") && !tags.includes("perf"));
  assert.match(lane, /--config=\$\(rootpath :playwright\.animation\.bazel\.config\.js\)/);
  assert.match(lane, grep);
  assert.doesNotMatch(lane, /--project(?:=|"|\s)/, "the config, not the lane, names the browser");
  assert.match(lane, /"DONNER_WASM_REQUIRE_WEBGPU": "1"/);
  assert.match(lane, /"NODE_OPTIONS": ""/);
  assert.ok(lane.includes("\"chromium-launch-env.cjs\""));
  // Firefox only: the Chromium copy of these cases is the Editor WASM chromium-default lane's.
  assert.ok(lane.includes("\"DONNER_FIREFOX_ARCHIVE\":"));
  assert.ok(lane.includes("\"@playwright-firefox-mac14-arm64//file\""));
  assert.doesNotMatch(lane, /DONNER_CHROMIUM_ARCHIVE|@playwright_chromium_mac_arm64|@playwright\/\/:chromium/);
  const config = readFileSync(
    path.join(testDirectory, "playwright.animation.bazel.config.js"),
    "utf8",
  );
  assert.deepEqual([...config.matchAll(/name: "([^"]+)"/g)].map((match) => match[1]), [
    "firefox-animation",
  ]);
  assert.match(config, /metadata: \{ pinnedBrowsers: \["firefox"\] \}/);
  assertChromiumDefaultLaneRuns(lane);
  assert.match(config, /globalSetup: require\.resolve\("\.\/prepare-browser-archives\.js"\)/);
}

function assertBrowserLane(lane) {
  const contract = parseBrowserLane(lane);
  assertCommonBrowserLane(contract);
  const { laneName, performanceLane, tags } = contract;
  if (performanceLane) {
    assertPerformanceLane(contract);
  } else if (laneName === "animation_preview_browser_test") {
    assertAnimationLane(contract, /--grep=animation\.preview\.plays/);
  } else if (laneName === "canvas_snapshot_browser_test") {
    assertAnimationLane(contract, /--grep=page screenshots keep the canvas/);
  } else if (laneName === "docs_selector_browser_tests") {
    assertDocsLane(contract);
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
  const packageJson = JSON.parse(readFileSync(path.join(testDirectory, "package.json"), "utf8"));
  const playwrightVersion = packageJson.devDependencies["@playwright/test"];
  const browserMetadata = JSON.parse(
    readFileSync(path.join(testDirectory, `browsers.${playwrightVersion}.json`), "utf8"),
  );
  const chromiumVersion = browserMetadata.browsers.find(
    (browser) => browser.name === "chromium",
  )?.browserVersion;
  assert.match(chromiumVersion ?? "", /^\d+\.\d+\.\d+\.\d+$/);
  assert.match(
    moduleFile,
    /bazel_dep\(name = "rules_playwright", version = "0\.5\.3"/,
    "MODULE.bazel must pin the browser repository rule",
  );
  assert.ok(
    moduleFile.includes(
      `browsers_json = "//donner/editor/wasm/tests:browsers.${playwrightVersion}.json"`,
    ),
    "the browser revisions must come from the Playwright version in package-lock.json",
  );
  assert.ok(
    moduleFile.includes(`builds/cft/${chromiumVersion}/mac-arm64/chrome-mac-arm64.zip`),
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
  assert.match(
    moduleFile,
    /"playwright-chromium-ubuntu24\.04-arm64": "playwright_chromium_linux_arm64"/,
    "the rules_playwright Linux ARM64 archive must be replaced hermetically",
  );

  const buildFilePath = path.join(testDirectory, "BUILD.bazel");
  assert.ok(existsSync(buildFilePath), "Wasm browser tests need a Bazel package");
  const buildFile = readFileSync(buildFilePath, "utf8");
  assert.match(buildFile, /playwright_bin\.playwright_test\(/);
  // Every Bazel-owned browser lane, checked one at a time rather than against
  // the file as a whole: a contract satisfied by some other lane in the same
  // file is not a contract. Each lane names a spec, a config and sometimes a
  // server, and every package file those load, directly or through another
  // file, has to be in that lane's `data` or the test runs against a runfiles
  // tree that is missing it. That includes the manual performance driver and
  // its watchdog, which no pull request runs.
  const lanes = [...buildFile.matchAll(/playwright_bin\.playwright_test\(([\s\S]*?)\n\)\n/g)]
    .map(([, body]) => body);
  assert.deepEqual(
    lanes.map((lane) => /name = "([^"]+)"/.exec(lane)?.[1]).sort(),
    [
      "_browser_responsiveness_perf_driver",
      "animation_preview_browser_test",
      "canvas_snapshot_browser_test",
      "docs_selector_browser_tests",
      "font_reference_probe",
      "standalone_geode_browser_renderer_test",
    ],
    "every playwright_test lane must be named and checked; update this contract when one is added",
  );
  for (const lane of lanes) {
    assertBrowserLane(lane);
  }
  // One watchdog test per responsiveness project, generated from one js_test body, so each
  // project runs under the config's whole globalTimeout rather than all four sharing it.
  const watchdogPattern =
    /\n {4}js_test\(\n([\s\S]*?)\n {4}\)\n {4}for project in RESPONSIVENESS_PERF_PROJECTS\n/;
  const watchdog = watchdogPattern.exec(buildFile)?.[1];
  assert.ok(watchdog, "the public responsiveness lanes must own their watchdog");
  assert.match(
    watchdog,
    /name = "browser_responsiveness_perf_test_" \+ project\.replace\("-", "_"\)/,
  );
  assert.match(watchdog, /args = \["--project=" \+ project\]/);
  const buildProjects = responsivenessPerfProjects();
  const responsivenessConfig = readFileSync(
    path.join(testDirectory, "playwright.responsiveness.bazel.config.js"),
    "utf8",
  );
  assert.deepEqual(
    buildProjects,
    [...responsivenessConfig.matchAll(/^ {6}name: "([^"]+)",$/gm)].map(([, project]) => project),
    "every responsiveness project, and only those, must have its own watchdog test",
  );
  assert.match(watchdog, /entry_point = "browser-watchdog\.mjs"/);
  assert.ok(
    watchdog.includes("\":_browser_responsiveness_perf_driver\""),
    "the watchdog must carry the driver and its runfiles",
  );
  // The watchdog's entry point loads its own helpers and spawns an anchor and a preload beside
  // it, then hands the driver the spec and config it names. Those run against the watchdog's
  // runfiles, which include the driver's.
  const driver = lanes.find((lane) =>
    lane.includes("name = \"_browser_responsiveness_perf_driver\"")
  );
  assertListsLoadedFiles(responsivenessPerfTarget("<project>"), watchdog, {
    entryPoints: ["browser-watchdog.mjs"],
    runfilesFrom: [driver],
  });
  assert.match(
    watchdog,
    /"DONNER_WATCHDOG_DRIVER": "\$\(rootpath :_browser_responsiveness_perf_driver\)"/,
  );
  assert.match(watchdog, /"DONNER_WATCHDOG_HEARTBEAT": "browser-watchdog-heartbeat"/);
  const watchdogTags = [
    ...(/tags = \[([\s\S]*?)\]/.exec(watchdog)?.[1] ?? "").matchAll(/"([^"]+)"/g),
  ]
    .map(([, tag]) => tag).sort();
  assert.deepEqual(watchdogTags, ["exclusive", "manual", "no-sandbox", "perf"]);

  const ciDarwin = readFileSync(path.join(repositoryRoot, "build_defs/ci_darwin.bzl"), "utf8");
  for (const project of ["firefox", "firefox-dpr1"]) {
    assert.ok(
      ciDarwin.includes(`"${responsivenessPerfTarget(project)}",`),
      `${project} responsiveness perf needs a remote Darwin qualification wrapper`,
    );
  }
  assert.match(
    ciDarwin,
    /if name in special\.get\("remote_performance", \[\]\):[\s\S]*?_variant\(ci, name, source, "desktop", True, True, "ci_darwin_perf_", \["external", "no-cache"\], \["exclusive"\]\)/,
    "remote Firefox perf wrappers must require a desktop GPU runner without inheriting the local-only exclusive tag",
  );
  assert.match(
    ciDarwin,
    /if "manual" in tags[\s\S]*?continue/,
    "manual performance roots must not also enter the ordinary Darwin variant set",
  );
  // Every editor lane serves the one production browser-selected package. The standalone
  // renderer has its own page and package, verified separately below.
  const editorPackage = "//donner/editor/wasm:_wasm_web_package_for_serve";
  for (const lane of lanes) {
    const laneName = /name = "([^"]+)"/.exec(lane)?.[1];
    if (laneName === "standalone_geode_browser_renderer_test") {
      assert.ok(
        lane.includes("\"//donner/svg/renderer/wasm:_geode_browser_test_package_for_playwright\""),
        "standalone renderer must serve its browser-selected Geode package",
      );
      assert.ok(
        lane.includes("--config=$(rootpath :playwright.standalone-geode.bazel.config.js)"),
        "standalone renderer must use its own Playwright config",
      );
      assert.ok(
        lane.includes("\"playwright.standalone-geode.bazel.config.js\""),
        "standalone renderer config must be in the Bazel runfiles",
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
    if (laneName === "font_reference_probe" || laneName === "docs_selector_browser_tests") {
      continue;
    }
    assert.ok(lane.includes(`"${editorPackage}"`), `${laneName} must serve the editor package`);
    // The boot lane checks both UI canvas and raster worker selection.
    assert.equal(
      lane.includes(`"DONNER_WASM_EXPECTED_HEADLESS_BACKEND": "browser"`),
      lane.includes("$(rootpath :browser-backend-selection.spec.ts)"),
      `${laneName} must expect the browser backend exactly when it checks package selection`,
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
    /editor_wasm_geode_transitioned_target\([\s\S]*?name = "_wasm_web_package_for_serve"[\s\S]*?browser_backend = True[\s\S]*?visibility = \["\/\/donner\/editor\/wasm\/tests:__pkg__", "\/\/tools\/ci:__pkg__"\]/,
    "the served production package must select the browser backend",
  );
  assert.doesNotMatch(editorBuildFile, /_wasm_web_package_browser_backend_for_serve/);
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

// The specs whose Chromium cases only the Editor WASM chromium-default lane runs. Bazel no longer
// runs a Chromium-only copy of them: the lane already ran every one of those cases on each pull
// request and main push that could change them.
const kChromiumDefaultLaneSpecs = [
  "boot-presentation.spec.ts",
  "browser-backend-selection.spec.ts",
  "browser-presentation-regression.spec.ts",
  "browser-surface-recovery.spec.ts",
  "smoke.spec.ts",
];
// Bazel lanes that may still name one of those specs, because they run a browser project the
// chromium-default lane does not.
const kNonDefaultBrowserLanesOverDefaultSpecs = {
  animation_preview_browser_test: "smoke.spec.ts",
  canvas_snapshot_browser_test: "smoke.spec.ts",
};

function assertDefaultLaneDiscovers(spec) {
  const defaultConfig = require("./playwright.config.js");
  assert.equal(defaultConfig.testMatch, "**/*.spec.ts", "the default lane discovers every spec");
  assert.ok(existsSync(path.join(testDirectory, spec)), `${spec} must exist`);
  assert.ok(
    !defaultConfig.testIgnore.includes(spec),
    `${spec} must stay in the chromium-default lane's discovery`,
  );
}

test("the Editor WASM chromium-default lane owns the Chromium editor specs", () => {
  const defaultConfig = require("./playwright.config.js");
  // One Chromium project, run whole: no project list or grep that could drop a case.
  assert.equal(defaultConfig.projects, undefined);
  assert.equal(defaultConfig.grep, undefined);
  assert.equal(defaultConfig.grepInvert, undefined);
  assert.equal(defaultConfig.use.channel, "chromium");
  // A committed test.only would otherwise narrow the lane to one case and still pass.
  assert.match(
    readFileSync(path.join(testDirectory, "playwright.config.js"), "utf8"),
    /\n {2}forbidOnly: Boolean\(process\.env\.CI\),\n/,
  );
  for (const spec of kChromiumDefaultLaneSpecs) assertDefaultLaneDiscovers(spec);

  const browserCi = readFileSync(path.join(repositoryRoot, "tools/run-browser-ci.sh"), "utf8");
  assert.ok(
    browserCi.includes(
      "run_lane \"chromium-default\" \\\n  bash donner/editor/wasm/tests/run_tests.sh --headed\n",
    ),
    "the chromium-default lane runs the default config with no filter",
  );
  // The environment the Bazel copies of these specs set, so the same cases run, and the overlay
  // pixel cases have their native comparator.
  for (
    const exported of [
      "export DONNER_WASM_REQUIRE_WEBGPU=\"1\"",
      "export DONNER_WASM_EXPECTED_HEADLESS_BACKEND=browser",
      "export DONNER_BROWSER_GOLDEN_COMPARE=\"${compare_copy}\"",
      "export CI=\"${CI:-true}\"",
    ]
  ) {
    assert.ok(browserCi.includes(exported), `tools/run-browser-ci.sh must ${exported}`);
  }
  assert.doesNotMatch(browserCi, /DONNER_HOSTED_MACOS_QUARANTINE/);

  const buildFile = readFileSync(path.join(testDirectory, "BUILD.bazel"), "utf8");
  for (const [, body] of buildFile.matchAll(/playwright_bin\.playwright_test\(([\s\S]*?)\n\)\n/g)) {
    const { laneName, specFiles } = parseBrowserLane(body);
    for (const spec of specFiles.filter((file) => kChromiumDefaultLaneSpecs.includes(file))) {
      assert.equal(
        kNonDefaultBrowserLanesOverDefaultSpecs[laneName],
        spec,
        `${laneName} runs ${spec}, whose Chromium cases the Editor WASM chromium-default lane `
          + "already runs; a Bazel lane over it must run only cases that lane does not",
      );
    }
  }
});

// A browser lane pays for a browser and a served package; a case that never takes a page pays
// for neither. Page-free presentation cases run under Node with the unit tests instead.
test("browser presentation cases that take no page run under Node", () => {
  const spec = readFileSync(path.join(testDirectory, "browser-presentation-regression.spec.ts"), "utf8");
  const nodeTests = readFileSync(path.join(testDirectory, "presentation-evidence_tests.mjs"), "utf8");
  const pageFree = [
    ...spec.matchAll(/^test\(\s*"([^"]+)",\s*(?:async\s*)?\(\s*\)\s*=>/gm),
  ].map(([, title]) => title);
  assert.deepEqual(pageFree, [], "move page-free cases to presentation-evidence_tests.mjs");
  const nodeTitles = [...nodeTests.matchAll(/^test\("([^"]+)"/gm)].map(([, title]) => title);
  assert.ok(nodeTitles.length >= 15, "the page-free presentation cases keep running");
  const buildFile = readFileSync(path.join(testDirectory, "BUILD.bazel"), "utf8");
  const target = buildFile
    .split("js_test(\n")
    .find((body) => body.startsWith("    name = \"presentation_evidence_tests\","))
    ?.split("\n)\n")[0];
  assert.ok(target, "the Node presentation cases need their own target");
  assert.match(target, /entry_point = "presentation-evidence_tests\.mjs"/);
  assert.doesNotMatch(target, /tags = |@playwright\/\/:|target_compatible_with/);
});

test("every Bazel Playwright config is loaded by a Bazel lane", () => {
  const buildFile = readFileSync(path.join(testDirectory, "BUILD.bazel"), "utf8");
  const bazelConfigs = readdirSync(testDirectory)
    .filter((name) => /^playwright\..*bazel\.config\.js$/.test(name));
  assert.ok(bazelConfigs.includes("playwright.bazel.config.js"));
  const editorRule = readFileSync(path.join(testDirectory, "editor_browser_test.bzl"), "utf8");
  const loaded = new Set(
    [...(buildFile + editorRule).matchAll(/--config=\$\(rootpath :([^)]+)\)/g)].map(([, config]) => config),
  );
  for (const config of [...loaded]) {
    for (const specifier of localFileReferences(readFileSync(path.join(testDirectory, config), "utf8"))) {
      loaded.add(resolveLocalFile(config, specifier, [config]));
    }
  }
  for (const config of bazelConfigs) {
    assert.ok(loaded.has(config), `${config} is loaded by no Bazel lane; delete it with its lane`);
  }
});

// A browser that cannot start must fail a Bazel lane once, before its first case, not once per
// case into the Bazel timeout. The Bazel base config names the launch probe; each lane's config
// either inherits it or names a setup that runs it. Manual lanes (the performance driver, which
// runs under its own watchdog, and the font reference probe) are run by hand and keep their own
// setup.
test("every Bazel browser lane probes its browser launch before the first case", () => {
  const base = readFileSync(path.join(testDirectory, "playwright.bazel.config.js"), "utf8");
  assert.match(base, /\n {2}globalSetup: require\.resolve\("\.\/browser-launch-probe\.js"\),\n/);
  const probing = new Set(["browser-launch-probe.js", "prepare-browser-archives.js"]);
  const buildFile = readFileSync(path.join(testDirectory, "BUILD.bazel"), "utf8");
  const configs = new Set();
  for (const [, body] of buildFile.matchAll(/playwright_bin\.playwright_test\(([\s\S]*?)\n\)\n/g)) {
    const { laneName, tags } = parseBrowserLane(body);
    if (tags.includes("manual")) continue;
    const config = /--config=\$\(rootpath :([^)]+)\)/.exec(body)?.[1];
    assert.ok(config, `${laneName} names its Playwright config`);
    configs.add(config);
  }
  for (const config of configs) {
    const source = readFileSync(path.join(testDirectory, config), "utf8");
    const setup = /globalSetup: require\.resolve\("\.\/([^"]+)"\)/.exec(source)?.[1];
    if (setup === undefined) {
      assert.match(source, /require\("\.\/playwright\.bazel\.config\.js"\)/, config);
      assert.match(source, /\.\.\.(?:base|baseConfig),/, `${config} inherits the base setup`);
      assert.doesNotMatch(source, /globalSetup/, config);
    } else {
      assert.ok(probing.has(setup), `${config} global setup ${setup} must probe the launch`);
    }
  }
});

// The probe launches every project of a lane's config. That matches what the lane runs only while
// no target narrows a probed config with a project filter. The performance targets pass one
// project each to their driver, whose config runs its own watchdog setup instead of the probe.
test("no project filter narrows a config that probes the browser launch", () => {
  const buildFile = readFileSync(path.join(testDirectory, "BUILD.bazel"), "utf8");
  const probing = new Set(["browser-launch-probe.js", "prepare-browser-archives.js"]);
  const configSetup = (config) => {
    const source = readFileSync(path.join(testDirectory, config), "utf8");
    const named = /globalSetup: require\.resolve\("\.\/([^"]+)"\)/.exec(source)?.[1];
    if (named !== undefined) return named;
    return /require\("\.\/playwright\.bazel\.config\.js"\)/.test(source)
      ? "browser-launch-probe.js"
      : undefined;
  };
  // Every BUILD block (a target, or a list of generated targets) that passes --project.
  const filtered = buildFile.split(/\n\n(?=\S)/).filter((block) => /--project\b/.test(block));
  assert.ok(filtered.length > 0, "the performance targets pass a project each");
  for (const block of filtered) {
    const configs = [...new Set(block.match(/playwright\.[\w.-]+\.config\.js/g) ?? [])];
    assert.ok(configs.length > 0, `a project filter without a config: ${block.slice(0, 80)}`);
    for (const config of configs) {
      assert.ok(
        !probing.has(configSetup(config)),
        `${config} probes every project, so no target may filter its projects`,
      );
    }
  }
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
  assert.ok(
    defaultConfig.testIgnore.includes("standalone-geode-browser-renderer.spec.ts"),
    "the editor package must not discover the standalone renderer's different test page",
  );
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
    "browser-surface-recovery.spec.ts",
    "browser-backend-selection.spec.ts",
  ]);

  const firefox = projects.get("firefox-geode-resize");
  assert.ok(firefox, "missing Firefox Geode resize project");
  assert.equal(firefox.use.browserName, "firefox");
  assert.match(String(firefox.grep), /ordinary document presentation stays on the GPU/);
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
  // The workflow's size-check step runs the lane script's `build` phase.
  assert.match(workflow, /\n {8}run: tools\/ci\/editor_wasm_lane\.sh build\n/);
  const lane = readFileSync(path.join(repositoryRoot, "tools/ci/editor_wasm_lane.sh"), "utf8");
  const normalizedLane = lane.replace(/\\\s*\n\s*/g, " ").replace(/\s+/g, " ");
  assert.ok(
    workflow.includes(
      "PAYLOAD_BUDGET_MODE: ${{ github.event_name == 'push' && github.ref == 'refs/heads/main' && 'report' || 'enforce' }}",
    ),
    "only a main-branch push may report rather than enforce numeric size ceilings",
  );
  assert.ok(lane.includes("budget_suite=//tools/ci:hosted_wasm_budgets"));
  assert.ok(lane.includes("report) budget_suite+=_report"));
  assert.match(normalizedLane, /bazelisk test .*--noskip_incompatible_explicit_targets/);
  assert.ok(lane.includes("//tools/ci:hosted_editor_wasm_graph"));
  for (const text of [workflow, lane]) {
    assert.doesNotMatch(
      text,
      /editor-wasm-tiny-skia|wasm_tiny_skia|package-tiny_skia|backend:\s*tiny_skia/,
    );
  }
  assert.doesNotMatch(workflow, /test:ios|playwright\\.ios/);

  // The browser job's lane sequence lives in tools/run-browser-ci.sh so that
  // one local command reproduces the job; the workflow step only supplies the
  // package directory and invokes the script. Both halves of that mirror are
  // checked here: the workflow must still call the script, and the script must
  // still carry every lane, in CI order.
  assert.match(normalizedWorkflow, /run: tools\/ci\/editor_wasm_lane\.sh build/);
  assert.match(lane, /hosted_editor_wasm_graph/);
  assert.match(workflow, /Setup Bazel for native browser comparison/);

  const browserCi = readFileSync(
    path.join(repositoryRoot, "tools/run-browser-ci.sh"),
    "utf8",
  );
  const normalizedBrowserCi = browserCi
    .replace(/\\\s*\n\s*/g, " ")
    .replace(/\s+/g, " ");
  assert.match(normalizedBrowserCi, /standalone_geode_browser_png_compare/);
  assert.match(normalizedBrowserCi, /export DONNER_BROWSER_GOLDEN_COMPARE/);
  // Suites that hash the served package read its directory, including in a local build.
  assert.match(normalizedBrowserCi, /export DONNER_WASM_PACKAGE_DIR="\$\{pkg_dir\}"/);
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
  assert.match(normalizedBrowserCi, /export DONNER_WASM_EXPECTED_HEADLESS_BACKEND=browser/);
  // Lanes share one Playwright output directory and Playwright empties it on
  // start, so the job can only keep a failing lane's evidence if the script
  // archives each lane's results per lane and the workflow uploads THAT.
  assert.match(normalizedBrowserCi, /archive_lane_results "\$\{name\}"/);
  assert.match(
    normalizedBrowserCi,
    /kFailureArchiveDir="\$\{kTestsDir\}\/playwright-failures"/,
  );
  assert.match(normalizedWorkflow, /path: \$\{\{ runner\.temp \}\}\/editor-wasm-browser-evidence/);
  // A hung lane must end in a recorded failure, not run into the job's own limit: every lane
  // runs under the stall guard, which writes into that lane's archived evidence.
  assert.match(normalizedBrowserCi, /guarded "\$\{name\}" "\$@" \|\| code=\$\?/);
  assert.match(
    normalizedBrowserCi,
    /--diagnostics-dir "\$\{kFailureArchiveDir\}\/\$\{name\}\/stall-guard"/,
  );
  assert.match(
    normalizedWorkflow,
    /run: tools\/ci\/editor_wasm_lane\.sh build/,
  );
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

test("composited probe evidence retains presentation loss only when published", async () => {
  const { stopCompositedProbe } = await import("./composited-probe-evidence.mjs");
  const temporary = mkdtempSync(path.join(tmpdir(), "donner-probe-loss-"));
  const attachments = [];
  const testInfo = {
    outputPath: (name) => path.join(temporary, name),
    attach: async (name, attachment) => attachments.push({ name, ...attachment }),
  };
  const originalWindow = globalThis.window;
  try {
    const deviceLoss = {
      firstObservedAbsoluteMs: 10123,
      pageTimeOriginMs: 10000,
      firstObservedAtMs: 123,
      publishedAtMs: 140,
      completedSerial: 7,
      submittedSerial: 11,
      oldestSerial: 9,
      timedOutSite: 2,
      timedOutElapsedMs: 250,
      browserReason: "browser reset",
    };
    globalThis.window = {
      __donnerCompositedProbe: {
        running: true,
        samples: [],
        drawFailures: 0,
        frames: 0,
        stop() {
          this.running = false;
        },
      },
      __donnerPresentationDeviceLoss: deviceLoss,
    };
    const page = { evaluate: async (callback) => callback() };
    const lost = await stopCompositedProbe(page, testInfo, {});
    assert.deepEqual(lost.presentation.deviceLoss, deviceLoss);

    delete globalThis.window.__donnerPresentationDeviceLoss;
    const healthy = await stopCompositedProbe(page, testInfo, {});
    assert.equal(healthy.presentation.deviceLoss, null);
  } finally {
    globalThis.window = originalWindow;
    rmSync(temporary, { recursive: true, force: true });
  }
});

test("drag candidates say whether the editor's completed frame or only the readback went back", async () => {
  const { presentedFrameOrdering } = await import("./composited-probe-evidence.mjs");
  const frame = (frameId, pointerX, pointerY) => ({
    frameId,
    captureId: 1,
    pointerX,
    pointerY,
    inputRepresented: true,
    completedSerial: frameId * 3,
    submittedSerial: frameId * 3 + 1,
    deviceCompletedSerial: frameId * 3,
    completedAtMs: frameId * 16,
  });
  // The drag moves right over the candidate's interval.
  const candidate = { predecessorIndex: 0, sampleIndex: 1, pointerDx: 10, pointerDy: 0 };
  const ordering = (before, after) =>
    presentedFrameOrdering([{ presentedFrame: before }, { presentedFrame: after }], candidate);

  const advanced = ordering(frame(40, 100, 50), frame(41, 108, 50));
  assert.equal(advanced.verdict, "readback-only");
  assert.equal(advanced.frameIdDelta, 1);
  assert.equal(advanced.representedPointerAlongDrag, 8);
  assert.deepEqual(advanced.predecessor, frame(40, 100, 50), "serials stay with each frame");
  assert.equal(ordering(frame(40, 100, 50), frame(40, 100, 50)).verdict, "readback-only");
  assert.equal(
    ordering(frame(41, 108, 50), frame(40, 100, 50)).verdict,
    "editor-frame-went-backwards",
    "a smaller frame id is the editor's completed frame going back",
  );
  // Just after a reversal the represented pointer, which lags the input, still moves the old way.
  assert.equal(
    ordering(frame(40, 108, 50), frame(41, 100, 50)).verdict,
    "editor-pointer-against-interval",
    "a represented pointer behind the last one is ambiguous, not proof of an editor bug",
  );
  const unrepresented = (id, x, y) => ({ ...frame(id, x, y), inputRepresented: false });
  assert.equal(
    ordering(frame(40, 100, 50), unrepresented(41, 108, 50)).verdict,
    "indeterminate",
    "a frame that represents no input says nothing about where its content is",
  );
  assert.equal(
    ordering(unrepresented(40, 100, 50), frame(41, 108, 50)).verdict,
    "indeterminate",
    "an unrepresented predecessor cannot clear the editor either",
  );
  assert.equal(
    ordering(frame(40, 108, 50), unrepresented(41, 100, 50)).verdict,
    "indeterminate",
    "validity comes before the pointer comparison",
  );
  assert.equal(
    ordering(unrepresented(41, 100, 50), unrepresented(40, 100, 50)).verdict,
    "editor-frame-went-backwards",
    "a smaller frame id needs no pointer",
  );
  const missing = ordering(null, frame(41, 108, 50));
  assert.equal(missing.verdict, "no-editor-frame");
  assert.equal(missing.frameIdDelta, null);
  assert.equal(
    presentedFrameOrdering([{}, {}], candidate).verdict,
    "no-editor-frame",
    "samples from before the probe kept frames still classify",
  );
});

test("composited probe samples keep the editor's completed frame for failure output", () => {
  const probe = readFileSync(path.join(testDirectory, "composited-probe.ts"), "utf8");
  const sampleOnce = /const sampleOnce = \(\): CapturedSample => \{[\s\S]*?\n {4}\};\n/.exec(probe)?.[0];
  assert.ok(sampleOnce, "the probe must define sampleOnce");
  assert.match(sampleOnce, /const presentedFrame = readPresentedFrame\(\);/);
  // Both the failed and the measured sample carry it.
  assert.equal(sampleOnce.match(/\n {8,10}presentedFrame,\n/g)?.length, 2);
  assert.match(probe, /__donnerPresentationQueueStats/);
  const spec = readFileSync(path.join(testDirectory, "composited-drag-invariants.spec.ts"), "utf8");
  const verdict = spec.indexOf("samples presented an OLDER frame than one already shown");
  const ordering = spec.indexOf("editorFrameOrdering(result.samples, violations)", verdict);
  assert.ok(
    verdict !== -1 && ordering !== -1 && ordering - verdict < 400,
    "the ordering verdict's message must carry the editor's frames",
  );
});

test("startup phases name a pre-navigation stall instead of the wait it starved", async () => {
  const {
    describePreNavigationStall,
    kPreNavigationStallShare,
    kStartupPhases,
    startupPhaseReport,
    startupWaitMessage,
  } = await import("./startup-phases.mjs");
  assert.equal(kStartupPhases[0], "testStart");
  const wait = "expected Geode to present the editor's first frame";
  const start = 1_000_000;
  // The observed failure: navigation began 29.3 s into a 30 s case.
  const stalled = {
    testStart: start,
    openEditor: start + 29_250,
    gotoStart: start + 29_300,
    gotoEnd: start + 29_420,
    wasmStartAllowed: start + 29_450,
    firstFrameWaitStart: start + 29_460,
  };
  const message = startupWaitMessage(stalled, 30_000, wait, start + 29_460);
  assert.match(
    startupWaitMessage({ ...stalled, firstCaseInWorker: 1 }, 30_000, wait, start + 29_460),
    /^pre-navigation stall: up to 29\.3 s of[\s\S]*openEditor to navigation 0\.1 s; the worker's first page case, so this may include the browser launch/,
  );
  assert.equal(
    message,
    "pre-navigation stall: 29.3 s of the 30.0 s test budget passed before navigation started"
      + " (test start to openEditor 29.3 s, openEditor to navigation 0.1 s);"
      + " 0.5 s of the budget was left when this wait started: " + wait,
  );
  const report = startupPhaseReport(stalled, 30_000, {
    navigationTimeOrigin: start + 29_310,
    now: start + 30_200,
  });
  assert.equal(report.preNavigationStall, true);
  assert.equal(report.preNavigationMs, 29_300);
  assert.equal(report.navigationTimeOriginMs, 29_310);
  assert.equal(report.reportedAtMs, 30_200);
  assert.deepEqual(Object.keys(report.sinceTestStartMs), Object.keys(stalled));

  // Every passing run reached navigation 0.5-2.1 s after the previous case ended.
  const prompt = { testStart: start, openEditor: start + 40, gotoStart: start + 2_100 };
  assert.equal(startupWaitMessage(prompt, 30_000, wait, start + 2_500), wait);
  assert.equal(describePreNavigationStall(startupPhaseReport(prompt, 30_000)), null);
  const threshold = 30_000 * kPreNavigationStallShare;
  assert.equal(
    startupPhaseReport({ testStart: start, gotoStart: start + threshold }, 30_000)
      .preNavigationStall,
    true,
  );

  // A case that reached openEditor but never navigated spent everything up to the report there.
  const never = startupPhaseReport({ testStart: start, openEditor: start + 50 }, 30_000, {
    now: start + 30_100,
  });
  assert.equal(never.preNavigationMs, 30_100);
  assert.match(describePreNavigationStall(never), /, navigation never started\)$/);
  // A case that navigates without openEditor has no navigation marks to judge.
  const elsewhere = startupPhaseReport({ testStart: start }, 30_000, { now: start + 30_100 });
  assert.equal(elsewhere.preNavigationMs, null);
  assert.equal(elsewhere.preNavigationStall, false);
  // No timeout (a debugger run) has no budget to stall.
  assert.equal(startupWaitMessage(stalled, 0, wait, start + 29_460), wait);
  // A case without marks (its spec does not keep them) keeps the wait's own message.
  assert.equal(startupWaitMessage({}, 30_000, wait), wait);
});

test("openEditor marks each startup phase before the wait it starts", () => {
  const smokeTest = readFileSync(path.join(testDirectory, "smoke.spec.ts"), "utf8");
  const start = smokeTest.indexOf("async function openEditor(");
  const body = smokeTest.slice(start, smokeTest.indexOf("\n}\n", start));
  const order = [
    "markStartupPhase(testInfo, \"openEditor\")",
    "markStartupPhase(testInfo, \"gotoStart\")",
    "await page.goto(",
    "markStartupPhase(testInfo, \"gotoEnd\")",
    "startupWait(testInfo, \"expected browser capabilities to permit Wasm startup\")",
    "markStartupPhase(testInfo, \"wasmStartAllowed\")",
    "markStartupPhase(testInfo, \"firstFrameWaitStart\")",
    "startupWait(testInfo, \"expected Geode to present the editor's first frame\")",
    "markStartupPhase(testInfo, \"firstFramePresented\")",
  ].map((needle) => [needle, body.indexOf(needle)]);
  for (const [needle, index] of order) assert.notEqual(index, -1, needle);
  assert.deepEqual(
    order.map(([needle]) => needle),
    [...order].sort((left, right) => left[1] - right[1]).map(([needle]) => needle),
  );
  const evidence = readFileSync(path.join(testDirectory, "failure-canvas-evidence.ts"), "utf8");
  const install = evidence.indexOf("export function installFailureCanvasEvidence(");
  assert.match(
    evidence.slice(install),
    /test\.beforeEach\(\(\) => \{\n {4}startupMarks\.set\(test\.info\(\)\.testId, \{\n {6}testStart: Date\.now\(\),/,
    "the first mark must come from a hook that sets up no fixture",
  );
  // A deadline that arrived outside the startup waits still names the stall, once, and only for a
  // case that already timed out.
  assert.match(
    evidence.slice(install),
    /stall !== null && testInfo\.status === "timedOut"\n\s*&& !testInfo\.errors\.some\(\(error\) => error\.message\?\.includes\("pre-navigation stall"\)\)/,
  );
  // Only a case armed earlier in the worker proves the browser was already up.
  assert.match(evidence, /firstCaseInWorker: armedCases === 0 \? 1 : 0/);
  assert.match(evidence, /report === null \|\| report\.firstCaseInWorker\n\s*\? null/);
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
    /sampleTimes\.at\(-1\) - sampleTimes\[0\] >= kMemoryDwellMs - 2 \* kMemorySampleIntervalMs/,
    "the gate must prove its sampler stayed alive throughout the five-minute dwell",
  );
  assert.match(
    harness,
    /Math\.max\(\.\.\.sampleGaps\) <= 3 \* kMemorySampleIntervalMs/,
    "the gate must prove its sampler never stalled during the dwell",
  );
  assert.match(
    harness,
    /dwellRss\.at\(-1\) <= Math\.min\(\.\.\.dwellRss\) \+ 128 \* 1024 \* 1024/,
    "the gate must reject WebContent RSS growth across the dwell",
  );
  assert.match(
    harness,
    /baselineLayers\.bitmapCount, 0[\s\S]*baselineLayers\.textureSnapshotCount,\s*baselineLayers\.rowCount/,
    "the memory baseline must require GPU-resident layer thumbnails",
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

test("latency gates enforce by default and only record a miss when set to report", async () => {
  const gates = await import("./latency-gates.mjs");
  assert.equal(gates.latencyGateMode({}), "enforce");
  assert.equal(gates.latencyGateMode({ [gates.kLatencyGatesEnv]: "enforce" }), "enforce");
  assert.equal(gates.latencyGateMode({ [gates.kLatencyGatesEnv]: "report" }), "report");
  assert.throws(() => gates.latencyGateMode({ [gates.kLatencyGatesEnv]: "off" }), /must be/);
  assert.deepEqual(
    Object.fromEntries(
      Object.entries(gates.kLatencyGates).map(([gate, { limitMs }]) => [
        gate,
        limitMs,
      ]),
    ),
    {
      settledClick: 100,
      inputP95: 50,
      inputStall: 250,
      completedStall: 250,
      completedResultHandoff: 1000,
    },
    "the gate limits are the targets, not a hosted-runner allowance",
  );
  for (const mode of ["enforce", "report"]) {
    const enforced = [];
    const lines = [];
    const result = gates.checkLatencyGate("inputP95", 447, {
      browser: "firefox",
      phase: "first-drag-193",
      mode,
      enforce: (...check) => enforced.push(check),
      log: (line) => lines.push(line),
    });
    assert.deepEqual(result, {
      gate: "inputP95",
      browser: "firefox",
      phase: "first-drag-193",
      valueMs: 447,
      limitMs: 50,
      met: false,
      mode,
    });
    assert.deepEqual(
      enforced,
      mode === "enforce" ? [[447, 50, gates.kLatencyGates.inputP95.message]] : [],
      `${mode} mode`,
    );
    assert.deepEqual(lines, [`${gates.kLatencyGateRecordPrefix}${JSON.stringify(result)}`]);
  }
  const summary = gates.latencyGateSummary([
    "responsiveness first-drag-193 chromium {}",
    `${gates.kLatencyGateRecordPrefix}${
      JSON.stringify({
        gate: "settledClick",
        browser: "chromium",
        phase: "settled-click",
        valueMs: 716.6,
        limitMs: 100,
        met: false,
        mode: "report",
      })
    }`,
  ].join("\n"));
  assert.match(
    summary,
    /\| chromium \| settled-click \| settledClick \| 716\.6 \| 100 \| missed \(reported\) \|/,
  );
  assert.equal(gates.latencyGateSummary(""), "No latency gate results were recorded.\n");
});

// The Perf workflow's matrix entries as [os, value of `key`] pairs.
function perfMatrixSetting(workflow, key) {
  return workflow.split(/\n {10}- os: /).slice(1).map((entry) => [
    entry.split("\n")[0].trim(),
    new RegExp(`\\n {12}${key}: (\\S+)`).exec(entry)?.[1],
  ]);
}

// The Playwright projects that BUILD.bazel gives one watchdog test each, in declaration order.
function responsivenessPerfProjects() {
  const buildFile = readFileSync(path.join(testDirectory, "BUILD.bazel"), "utf8");
  const list = /\nRESPONSIVENESS_PERF_PROJECTS = \[([\s\S]*?)\]/.exec(buildFile)?.[1] ?? "";
  return [...list.matchAll(/"([^"]+)"/g)].map(([, project]) => project);
}

// The Bazel test name BUILD.bazel generates for a responsiveness project.
function responsivenessPerfTarget(project) {
  return `browser_responsiveness_perf_test_${project.replaceAll("-", "_")}`;
}

// The body of the Perf workflow step named `name`, up to the next step.
function perfStep(workflow, name) {
  const start = workflow.indexOf(`- name: ${name}`);
  assert.ok(start >= 0, `Perf must have the step "${name}"`);
  const end = workflow.indexOf("\n      - name:", start);
  return workflow.slice(start, end < 0 ? undefined : end);
}

test("only the hosted macOS Perf job reports latency gates without enforcing them", () => {
  const workflow = readFileSync(path.join(repositoryRoot, ".github/workflows/perf.yml"), "utf8");
  assert.deepEqual(
    perfMatrixSetting(workflow, "latency_gates"),
    [["xcode-27", "report"], ["ubuntu-24.04", "enforce"]],
    "only the GitHub-hosted macOS entry, which runs the browser lane, may report gate misses",
  );
  assert.match(
    perfStep(workflow, "Test perf-tagged targets"),
    /--test_env=DONNER_BROWSER_LATENCY_GATES=\$\{\{ matrix\.latency_gates \}\}/,
  );
  const summaryStep = perfStep(workflow, "Summarize interaction latency gates");
  assert.ok(
    summaryStep.includes(
      "if: ${{ !cancelled() && matrix.target_tag == 'perf' && steps.perf.outcome != 'skipped' }}",
    ),
    "the summary must also follow a failed test step",
  );
  assert.ok(summaryStep.includes("PERF_OUTCOME: ${{ steps.perf.outcome }}\n"));
  assert.ok(
    summaryStep.includes(
      "if [[ \"$PERF_OUTCOME\" == \"success\" ]]; then\n            require=(--require-all)\n",
    ),
    "after a passing test step a missing project log must fail the summary",
  );
  assert.ok(summaryStep.includes("logs=bazel-testlogs/donner/editor/wasm/tests\n"));
  // One command reads every project's log and writes the job summary.
  const command = new RegExp(
    String.raw`\n {10}node donner/editor/wasm/tests/latency-gate-summary\.mjs `
      + String.raw`\$\{require\[@\]\+"\$\{require\[@\]\}"\} \\\n`
      + String.raw`((?: {12}"\$logs/\w+/test\.log" \\\n)+)`
      + String.raw` {12}>> "\$GITHUB_STEP_SUMMARY"\n`,
  ).exec(summaryStep);
  assert.ok(command, "the summary command must append its output to the job summary");
  assert.deepEqual(
    [...command[1].matchAll(/"\$logs\/(\w+)\/test\.log"/g)].map(([, target]) => target),
    responsivenessPerfProjects().map(responsivenessPerfTarget),
    "the summary must read the log of every responsiveness project, and only those",
  );
});

test("responsiveness latency gates go through the gate helper", async () => {
  const gates = await import("./latency-gates.mjs");
  const spec = readFileSync(path.join(testDirectory, "browser-responsiveness.perf.ts"), "utf8");
  assert.match(spec, /from "\.\/latency-gates\.mjs"/);
  assert.match(spec, /latencyGateMode\(\)/, "the mode must come from the run's environment");
  for (const [gate, { message }] of Object.entries(gates.kLatencyGates)) {
    assert.ok(spec.includes(`latencyGate("${gate}"`), `${gate} must be checked through the helper`);
    assert.ok(!spec.includes(message), `${gate} must not be enforced outside the helper`);
  }
});

test("the latency gate summary command prints the gates recorded in a test log", () => {
  const command = path.join(testDirectory, "latency-gate-summary.mjs");
  const directory = mkdtempSync(path.join(process.env.TEST_TMPDIR ?? tmpdir(), "latency-gates-"));
  try {
    const log = path.join(directory, "test.log");
    const record = {
      gate: "inputP95",
      browser: "firefox",
      phase: "first-drag-193",
      valueMs: 447.4,
      limitMs: 50,
      met: false,
      mode: "report",
    };
    writeFileSync(log, `[firefox] drag\nlatency-gate ${JSON.stringify(record)}\n`);
    assert.match(
      execFileSync(process.execPath, [command, log], { encoding: "utf8" }),
      /\| firefox \| first-drag-193 \| inputP95 \| 447\.4 \| 50 \| missed \(reported\) \|/,
    );
    const secondLog = path.join(directory, "second.log");
    writeFileSync(
      secondLog,
      `latency-gate ${JSON.stringify({ ...record, browser: "chromium", met: true })}\n`,
    );
    const combined = execFileSync(
      process.execPath,
      [command, log, path.join(directory, "missing.log"), secondLog],
      { encoding: "utf8" },
    );
    assert.match(combined, /\| firefox \| first-drag-193 \| inputP95 \| 447\.4 \| 50 \| missed/);
    assert.match(
      combined,
      /\| chromium \| first-drag-193 \| inputP95 \| 447\.4 \| 50 \| met \|/,
      "every project's log is summarized, skipping one that did not run",
    );
    // A test that did not run leaves no log, named by its target directory.
    const missing = path.join(directory, "browser_responsiveness_perf_test_firefox", "test.log");
    const notice = "Missing test log for browser_responsiveness_perf_test_firefox: "
      + "that test did not run.\n";
    assert.equal(
      execFileSync(process.execPath, [command, missing], { encoding: "utf8" }),
      `No latency gate results were recorded.\n${notice}`,
      "without --require-all a missing log is reported and the command still succeeds",
    );
    const required = spawnSync(process.execPath, [command, "--require-all", log, missing], {
      encoding: "utf8",
    });
    assert.equal(required.status, 1, "with --require-all a missing log fails the command");
    assert.ok(required.stdout.endsWith(notice), "the failing summary still names the missing log");
    assert.match(required.stdout, /\| firefox \| first-drag-193 \| inputP95 \|/);
    const complete = spawnSync(process.execPath, [command, "--require-all", log, secondLog], {
      encoding: "utf8",
    });
    assert.equal(complete.status, 0, "with every log present --require-all succeeds");
    assert.doesNotMatch(complete.stdout, /Missing test log/);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test("modules that the TypeScript specs import stay loadable as CommonJS", () => {
  // Playwright loads the TypeScript specs, and the modules they import, as CommonJS. There
  // import.meta is a syntax error that stops the whole spec file from loading.
  const imported = new Set();
  for (const file of readdirSync(testDirectory).filter((name) => name.endsWith(".ts"))) {
    const source = readFileSync(path.join(testDirectory, file), "utf8");
    for (const [, module] of source.matchAll(/from "\.\/([^"]+\.m?js)"/g)) imported.add(module);
  }
  assert.ok(imported.has("latency-gates.mjs"), "the responsiveness spec imports the gate module");
  for (const module of imported) {
    const source = readFileSync(path.join(testDirectory, module), "utf8");
    assert.ok(!source.includes("import.meta"), `${module} must not use import.meta`);
  }
});

test("completion checks enforce by default and report only tracked misses", async () => {
  const gates = await import("./latency-gates.mjs");
  assert.equal(gates.completionCheckMode({}), "enforce");
  assert.equal(gates.completionCheckMode({ [gates.kCompletionChecksEnv]: "report" }), "report");
  assert.throws(
    () => gates.completionCheckMode({ [gates.kCompletionChecksEnv]: "off" }),
    /must be/,
  );
  assert.equal(
    gates.completionCheckMode({ [gates.kLatencyGatesEnv]: "report" }),
    "enforce",
    "reporting latency gates must not also report completion checks",
  );
  assert.deepEqual(
    Object.fromEntries(
      Object.entries(gates.kCompletionChecks).map(([check, { report }]) => [check, report]),
    ),
    {
      coldZoomPresents: { browsers: ["chromium"], issue: "#1675", restoreWhen: "#1675 is fixed" },
      dragInputsComplete: { browsers: ["firefox"], issue: "#1676", restoreWhen: "#1676 is fixed" },
      heldDragPositions: {
        browsers: ["chromium"],
        phases: ["geode-crown"],
        issue: "#2106",
        restoreWhen: "#2106 is fixed",
      },
      heldDragFastPathResults: undefined,
      heldMaskedDragDamagePatch: undefined,
    },
    "a new or wider report scope is a reviewed change to this table",
  );
  const miss = new Error("every dispatched input must complete\n\nExpected: 60");
  const run = async (mode, browserName, assertion) => {
    const lines = [];
    const outcome = await gates.runCompletionCheck("dragInputsComplete", assertion, {
      browserName,
      project: `${browserName}-dpr1`,
      phase: "first-drag-193",
      detail: "58 of 60 inputs completed",
      mode,
      log: (line) => lines.push(line),
    }).then((result) => ({ result }), (error) => ({ error }));
    return { ...outcome, lines };
  };
  const reported = await run("report", "firefox", () => Promise.reject(miss));
  assert.deepEqual(reported.result, {
    check: "dragInputsComplete",
    browser: "firefox-dpr1",
    phase: "first-drag-193",
    detail: "58 of 60 inputs completed",
    met: false,
    mode: "report",
    error: "every dispatched input must complete",
  });
  assert.deepEqual(reported.lines, [
    `${gates.kCompletionCheckRecordPrefix}${JSON.stringify(reported.result)}`,
  ]);
  for (const [mode, browserName] of [["enforce", "firefox"], ["report", "chromium"]]) {
    const enforced = await run(mode, browserName, () => {
      throw miss;
    });
    assert.equal(enforced.error, miss, `${mode} mode in ${browserName} must rethrow the miss`);
    assert.equal(enforced.lines.length, 1, "an enforced miss is recorded too");
    assert.match(enforced.lines[0], /"mode":"enforce"/);
  }
  assert.equal((await run("enforce", "firefox", () => {})).result.met, true);
  const heldMiss = new Error("held drag must complete multiple represented positions");
  const runHeld = (browserName, phase) =>
    gates.runCompletionCheck("heldDragPositions", () => Promise.reject(heldMiss), {
      browserName,
      project: browserName,
      phase,
      detail: "1 held positions completed",
      mode: "report",
      log: () => {},
    }).then((result) => ({ result }), (error) => ({ error }));
  assert.equal((await runHeld("chromium", "geode-crown")).result?.mode, "report");
  const enforcedHeld = await gates.runCompletionCheck(
    "heldDragPositions",
    () => Promise.reject(heldMiss),
    {
      browserName: "chromium",
      project: "chromium",
      phase: "geode-crown",
      detail: "1 held positions completed",
      mode: "enforce",
      log: () => {},
    },
  ).then((result) => ({ result }), (error) => ({ error }));
  assert.equal(enforcedHeld.error, heldMiss, "an enforcing run rethrows a tracked miss");
  const enforcedPhases = [
    ["chromium", "first-drag-193"],
    ["chromium", "second-drag-193"],
    ["firefox", "geode-crown"],
  ];
  for (const [browserName, phase] of enforcedPhases) {
    assert.equal(
      (await runHeld(browserName, phase)).error,
      heldMiss,
      `a held-position miss in ${browserName} ${phase} stays enforced in report mode`,
    );
  }
  assert.equal(gates.completionCheckTracks("dragInputsComplete", "firefox", "any-phase"), true);
  assert.equal(gates.completionCheckTracks("heldDragPositions", "chromium", "geode-crown"), true);
  assert.equal(gates.completionCheckTracks("heldDragPositions", "firefox", "geode-crown"), false);
  for (const browserName of ["chromium", "firefox"]) {
    for (const check of ["heldDragFastPathResults", "heldMaskedDragDamagePatch"]) {
      assert.equal(
        gates.completionCheckTracks(check, browserName, "geode-crown"),
        false,
        `${check} stays enforced in ${browserName}`,
      );
    }
  }
  const damagePatchMiss = new Error("every held masked-child result must take the fast path");
  for (const project of responsivenessPerfProjects()) {
    const outcome = await gates.runCompletionCheck(
      "heldMaskedDragDamagePatch",
      () => Promise.reject(damagePatchMiss),
      {
        browserName: project.split("-")[0],
        project,
        phase: "geode-crown",
        detail: "8 held results, 0 slow path, 6 rasterized unpatched",
        mode: "report",
        log: () => {},
      },
    ).then((result) => ({ result }), (error) => ({ error }));
    assert.equal(
      outcome.error,
      damagePatchMiss,
      `a damage patch miss in the ${project} project stays enforced in report mode`,
    );
  }
  assert.equal(
    gates.completionCheckTracks("heldDragPositions", "chromium", "first-drag-193"),
    false,
  );
  await assert.rejects(
    gates.runCompletionCheck("anyOtherCheck", () => {}, {
      browserName: "firefox",
      project: "firefox",
      phase: "first-drag-193",
      detail: "",
      mode: "report",
      log: () => {},
    }),
    /unknown completion check/,
  );
  assert.ok(
    gates.latencyGateSummary(reported.lines.join("\n")).includes(
      "| first-drag-193 | dragInputsComplete | 58 of 60 inputs completed | missed (reported) |",
    ),
  );
});

test("only the hosted macOS Perf job reports the completion checks without enforcing them", () => {
  const workflow = readFileSync(path.join(repositoryRoot, ".github/workflows/perf.yml"), "utf8");
  assert.deepEqual(
    perfMatrixSetting(workflow, "completion_checks"),
    [["xcode-27", "report"], ["ubuntu-24.04", "enforce"]],
  );
  assert.match(
    perfStep(workflow, "Test perf-tagged targets"),
    /--test_env=DONNER_BROWSER_COMPLETION_CHECKS=\$\{\{ matrix\.completion_checks \}\}/,
  );
});

test("the masked drag counters are checked in candidate runs and only measured in baseline runs", async () => {
  const { summarizeMaskedDragResults } = await import("./masked-drag-counters.mjs");
  const published = (completedResults, activeDragResult) => ({
    completedResults,
    countersPublished: true,
    activeDragResult,
  });
  const records = [published(8, true), published(9, true), published(10, false), published(11, false)];
  for (const baselineOnly of [false, true]) {
    assert.deepEqual(summarizeMaskedDragResults(records, { baselineOnly }), {
      countersPublished: true,
      resultCount: 4,
      held: records.slice(0, 2),
      release: records.slice(2),
      checkHeldResults: !baselineOnly,
    });
  }
  // A historical bundle publishes no per-result counters; its results are not idle results.
  const historical = [8, 9, 10].map((completedResults) => ({
    completedResults,
    countersPublished: false,
    activeDragResult: null,
  }));
  assert.deepEqual(summarizeMaskedDragResults(historical, { baselineOnly: true }), {
    countersPublished: false,
    resultCount: 3,
    held: [],
    release: [],
    checkHeldResults: false,
  });
  assert.equal(
    summarizeMaskedDragResults(historical, { baselineOnly: false }).checkHeldResults,
    true,
    "a candidate bundle without the counters must still be checked, and fail",
  );
});

test("every completion check report scope names its issue and when it is removed", async () => {
  const gates = await import("./latency-gates.mjs");
  for (const [check, { observes, report }] of Object.entries(gates.kCompletionChecks)) {
    assert.ok(typeof observes === "string" && observes !== "", `${check} names what it observes`);
    if (report === undefined) continue;
    assert.ok(report.browsers.length > 0, `${check} must name the browsers it reports in`);
    assert.match(report.issue, /^#\d+$/, `${check} must name the issue that tracks its misses`);
    assert.ok(
      typeof report.restoreWhen === "string" && report.restoreWhen.includes(report.issue),
      `${check} must say, in terms of its issue, when enforcement returns`,
    );
  }
});

// Held masked-child result counters as the editor publishes them for one result.
function maskedDragResult(completedResults, counters = {}) {
  return {
    completedResults,
    countersPublished: true,
    activeDragResult: true,
    fastPathFrames: 0,
    slowPathFramesWithDirty: 0,
    noDirtyFrames: 0,
    damagePatchTileCount: 0,
    cachedTileCount: 0,
    immediateTileCount: 0,
    ...counters,
  };
}

// A drag-start result finds nothing dirty; a held move patches the damage of its one owning tile.
const kDragStartResult = { noDirtyFrames: 1 };
const kFastPathMove = { fastPathFrames: 1, cachedTileCount: 1, damagePatchTileCount: 1 };

test("the masked drag result shape and its fast-path progress are separate checks", async () => {
  const gates = await import("./latency-gates.mjs");
  const counters = await import("./masked-drag-counters.mjs");
  const summary = (...held) => ({ countersPublished: true, held });
  // How the spec runs both checks on the hosted job, which reports the checks' scoped misses: each
  // outcome is "met", "reported" or "failed".
  const check = async (held, project) => {
    const results = summary(...held);
    const run = (name, assertion) =>
      gates.runCompletionCheck(name, assertion, {
        browserName: project.split("-")[0],
        project,
        phase: "geode-crown",
        detail: "",
        mode: "report",
        log: () => {},
      }).then((result) => result.met ? "met" : "reported", () => "failed");
    return {
      shape: await run(
        "heldMaskedDragDamagePatch",
        () => assert.deepEqual(counters.heldMaskedDragShapeFailures(results), []),
      ),
      fastPath: await run(
        "heldDragFastPathResults",
        () => assert.ok(counters.heldFastPathResultCount(results) > 0, "no fast-path result"),
      ),
    };
  };
  const dragStart = maskedDragResult(8, kDragStartResult);
  for (const project of responsivenessPerfProjects()) {
    assert.deepEqual(
      await check([dragStart], project),
      { shape: "met", fastPath: "failed" },
      `${project}: a drag that completes only its drag-start result while held has the shape, and `
        + "fails the enforced fast-path result check",
    );
    assert.deepEqual(
      await check([dragStart, maskedDragResult(9, kFastPathMove), maskedDragResult(10)], project),
      { shape: "failed", fastPath: "met" },
      `${project}: a held result that neither takes the fast path nor finds nothing dirty fails`,
    );
    assert.deepEqual(
      await check([dragStart, maskedDragResult(9, kFastPathMove)], project),
      { shape: "met", fastPath: "met" },
      `${project}: a healthy held drag meets both`,
    );
    const slowPath = maskedDragResult(9, { slowPathFramesWithDirty: 1, cachedTileCount: 4 });
    assert.deepEqual(
      await check([dragStart, slowPath], project),
      { shape: "failed", fastPath: "failed" },
      `${project}: a slow-path held result fails the enforced shape check`,
    );
    const unpatched = maskedDragResult(9, { ...kFastPathMove, cachedTileCount: 2 });
    assert.deepEqual(
      await check([dragStart, unpatched], project),
      { shape: "failed", fastPath: "met" },
      `${project}: a held tile rasterized without a damage patch fails the enforced shape check`,
    );
  }
  assert.deepEqual(
    counters.heldMaskedDragShapeFailures(summary(
      dragStart,
      maskedDragResult(9, { slowPathFramesWithDirty: 1, cachedTileCount: 4 }),
      maskedDragResult(10, { ...kFastPathMove, cachedTileCount: 2, damagePatchTileCount: 2 }),
    )),
    [
      "held result 1 (completed result 9): 1 slow-path frames, neither the fast path nor a clean "
      + "frame, 4 tiles rasterized without a damage patch",
      "held result 2 (completed result 10): 2 damage-patched tiles",
    ],
  );
  assert.deepEqual(
    counters.heldMaskedDragShapeFailures({ countersPublished: false, held: [] }),
    ["the candidate bundle must publish per-result counters"],
  );
  // The recorder reads each counter with Number(), so a counter the editor stopped publishing or
  // renamed arrives as NaN; it fails, named, instead of passing every comparison.
  const withoutSlowPath = maskedDragResult(9, kFastPathMove);
  delete withoutSlowPath.slowPathFramesWithDirty;
  assert.deepEqual(
    counters.heldMaskedDragShapeFailures(summary(
      dragStart,
      withoutSlowPath,
      maskedDragResult(10, { ...kFastPathMove, damagePatchTileCount: Number(undefined) }),
    )),
    [
      "held result 1 (completed result 9): unreadable counters slowPathFramesWithDirty",
      "held result 2 (completed result 10): unreadable counters damagePatchTileCount",
    ],
  );
  // More damage-patched tiles than rasterized ones is not a clean result either.
  assert.deepEqual(
    counters.heldMaskedDragShapeFailures(summary(
      maskedDragResult(9, { ...kFastPathMove, cachedTileCount: 0 }),
    )),
    ["held result 0 (completed result 9): -1 tiles rasterized without a damage patch"],
  );
});

// Runs the in-page drag sampler against a scripted editor on a simulated clock. From the press
// until its release frame the editor completes a frame every `frameEveryMs` that represents no
// input, as the hosted Chromium runner does during the masked crown drag. It completes the release
// frame `releaseAfterMs` after the mouse up, representing `releasePoint` (the final pointer
// position unless set) at camera zoom `releaseZoom`, or never when `releaseAfterMs` is null.
async function sampleScriptedDrag({
  releaseFrameTimeoutMs,
  releaseAfterMs,
  frameEveryMs = 50,
  releasePoint,
  releaseZoom = 1.5,
  presentationOnlyHeldBurst = false,
}) {
  const { kPresentationSamplePollMs, streamDragInputs } = await import("./drag-input-stream.mjs");
  let now = 1000;
  let nextTimer = 0;
  const timers = [];
  const schedule = (fn, delayMs) => {
    timers.push({ id: ++nextTimer, atMs: now + Math.max(0, delayMs ?? 0), fn });
    return nextTimer;
  };
  const page = {
    __donnerFrameLoopStats: { renderedFrames: 1, lastFrameAtMs: now },
    __donnerPresentationQueueStats: {
      completedSerial: 1,
      completedAtMs: now,
      inputRepresented: true,
      mouseDown: false,
      pointerX: 0,
      pointerY: 0,
      viewportZoom: 1.5,
    },
    __donnerInteractionStats: { pointerX: 0, pointerY: 0, dragging: false, selectedCount: 1 },
    __donnerViewportStats: { zoom: 1.5 },
  };
  const complete = (
    pointer,
    mouseDown,
    inputRepresented,
    viewportZoom = 1.5,
    advancesFrameLoop = true,
  ) => {
    if (advancesFrameLoop) {
      const loop = page.__donnerFrameLoopStats;
      page.__donnerFrameLoopStats = { renderedFrames: loop.renderedFrames + 1, lastFrameAtMs: now };
    }
    page.__donnerPresentationQueueStats = {
      completedSerial: page.__donnerPresentationQueueStats.completedSerial + 1,
      completedAtMs: now,
      inputRepresented,
      mouseDown,
      pointerX: pointer.x,
      pointerY: pointer.y,
      viewportZoom,
    };
  };
  let held = false;
  let released = false;
  const laggingFrame = () => {
    if (released) return;
    complete({ x: 0, y: 0 }, held, false);
    schedule(laggingFrame, frameEveryMs);
  };
  const canvas = {
    dispatchEvent(event) {
      const pointer = { x: event.clientX, y: event.clientY };
      page.__donnerInteractionStats = {
        ...page.__donnerInteractionStats,
        pointerX: pointer.x,
        pointerY: pointer.y,
        dragging: event.type !== "mouseup",
      };
      if (event.type === "mousedown") {
        held = true;
        schedule(laggingFrame, frameEveryMs);
        if (presentationOnlyHeldBurst) {
          schedule(() => complete({ x: pointer.x + 1, y: pointer.y }, true, true, 1.5, false), 99);
          schedule(
            () => complete({ x: pointer.x + 1, y: pointer.y + 1 }, true, true, 1.5, false),
            107,
          );
        }
      } else if (event.type === "mouseup") {
        held = false;
        if (releaseAfterMs !== null) {
          schedule(() => {
            released = true;
            complete(releasePoint ?? pointer, false, true, releaseZoom);
          }, releaseAfterMs);
        }
      }
    },
  };
  const globals = {
    window: page,
    document: { querySelector: () => canvas },
    MouseEvent: class {
      constructor(type, init) {
        Object.assign(this, init, { type });
      }
    },
    performance: { now: () => now },
    setTimeout: schedule,
    clearTimeout: (id) => {
      const index = timers.findIndex((timer) => timer.id === id);
      if (index >= 0) timers.splice(index, 1);
    },
    requestAnimationFrame: (fn) => schedule(() => fn(now), 16),
    cancelAnimationFrame: (id) => {
      const index = timers.findIndex((timer) => timer.id === id);
      if (index >= 0) timers.splice(index, 1);
    },
  };
  const previous = Object.fromEntries(
    Object.keys(globals).map((name) => [name, Object.getOwnPropertyDescriptor(globalThis, name)]),
  );
  const flush = () => new Promise((resolve) => setImmediate(resolve));
  for (const [name, value] of Object.entries(globals)) {
    Object.defineProperty(globalThis, name, { configurable: true, writable: true, value });
  }
  try {
    let settled = false;
    const stream = streamDragInputs({
      start: { x: 400, y: 300 },
      direction: 0.25,
      presentationSamplePollMs: kPresentationSamplePollMs,
      releaseFrameTimeoutMs,
    }).finally(() => {
      settled = true;
    });
    await flush();
    while (!settled) {
      assert.ok(timers.length > 0, "the sampler must still be waiting on a timer");
      timers.sort((a, b) => a.atMs - b.atMs || a.id - b.id);
      const timer = timers.shift();
      now = timer.atMs;
      timer.fn();
      await flush();
    }
    return await stream;
  } finally {
    for (const [name, descriptor] of Object.entries(previous)) {
      if (descriptor) Object.defineProperty(globalThis, name, descriptor);
      else Reflect.deleteProperty(globalThis, name);
    }
  }
}

test("the drag sampler retains presentation completions between animation frames", async () => {
  const metrics = await import("./drag-input-stream.mjs");
  assert.doesNotMatch(
    String(metrics.streamDragInputs),
    /\bkPresentationSamplePollMs\b/,
    "the serialized in-page sampler must read its interval only from the Playwright argument",
  );
  const stream = await sampleScriptedDrag({
    releaseFrameTimeoutMs: metrics.kReleaseFrameTimeoutMs,
    releaseAfterMs: 0,
    frameEveryMs: 1000,
    presentationOnlyHeldBurst: true,
  });
  const completed = metrics.completedDragFrames(stream, 1);
  assert.equal(
    metrics.heldDragPositionCount(completed),
    2,
    "both held presentations published between rAF callbacks remain measurable",
  );
});

test("completed drag frames reject a matching coordinate dispatched in the future", async () => {
  const metrics = await import("./drag-input-stream.mjs");
  const presentation = {
    completedSerial: 2,
    completedAtMs: 150,
    inputRepresented: true,
    mouseDown: true,
    pointerX: 2,
    pointerY: 3,
    viewportZoom: 1,
  };
  const stream = {
    startedAtMs: 100,
    pressDispatchedAtMs: 110,
    finishedAtMs: 250,
    dispatchTimes: [200],
    dispatchPoints: [{ x: 2, y: 3 }],
    frames: [{ frame: 1, atMs: 150, presentation }],
    start: { x: 0, y: 0 },
    end: { x: 2, y: 3 },
    releasedAtMs: 240,
    releaseFrame: null,
  };
  assert.deepEqual(
    metrics.completedDragFrames(stream, 1),
    [],
    "a coordinate does not count before the matching input was dispatched",
  );
  stream.dispatchTimes[0] = 140;
  presentation.pointerX += 1e-9;
  assert.deepEqual(metrics.completedDragFrames(stream, 1), [stream.frames[0]]);
});

test("held drag motion includes its dispatched press and a later represented move", async () => {
  const metrics = await import("./drag-input-stream.mjs");
  const presentation = (serial, point) => ({
    frame: serial,
    atMs: serial * 10,
    presentation: {
      completedSerial: serial,
      completedAtMs: serial * 10,
      inputRepresented: true,
      mouseDown: true,
      pointerX: point.x,
      pointerY: point.y,
      viewportZoom: 1,
    },
  });
  const stream = {
    startedAtMs: 10,
    pressDispatchedAtMs: 15,
    finishedAtMs: 40,
    dispatchTimes: [20],
    dispatchPoints: [{ x: 12, y: 13 }],
    frames: [presentation(2, { x: 10, y: 11 }), presentation(3, { x: 12, y: 13 })],
    start: { x: 10, y: 11 },
    end: { x: 12, y: 13 },
    releasedAtMs: 40,
    releaseFrame: null,
  };
  const completed = metrics.completedDragFrames(stream, 1);
  const pressOnly = metrics.completedDragFrames({
    ...stream,
    dispatchTimes: [],
    dispatchPoints: [],
    frames: stream.frames.slice(0, 1),
  }, 1);
  assert.equal(metrics.heldDragPositionCount(pressOnly), 1, "a press without displayed motion fails");
  const latePress = metrics.completedDragFrames({ ...stream, pressDispatchedAtMs: 21 }, 1);
  assert.equal(
    metrics.heldDragPositionCount(latePress),
    1,
    "a press stamped after its presented completion cannot establish motion",
  );
  assert.equal(metrics.heldDragPositionCount(completed), 2);
});

test("the drag sampler observes the drag until the one release stall guard", async () => {
  const stream = await import("./drag-input-stream.mjs");
  const completed = (sampled) => stream.dragInputLatencies(sampled, 1).length;
  const deadlineMs = stream.kReleaseFrameTimeoutMs;
  // Every held frame lags behind the pointer, so only the release frame completes any input. A
  // release frame 900 ms after the mouse up is slow but complete; a sampler that stopped observing
  // after 500 ms read it as no input completed.
  const late = await sampleScriptedDrag({ releaseFrameTimeoutMs: deadlineMs, releaseAfterMs: 900 });
  assert.equal(completed(late), 60, "a slow but complete drag completes every input");
  assert.equal(late.releaseFrame?.completedAtMs, late.releasedAtMs + 900);
  assert.equal(late.frames.at(-1).presentation.mouseDown, false);
  // Just inside the stall guard the release is met; just past it the sampler reports no release
  // frame, which the spec fails as a stalled release before it reads the inputs.
  const inside = await sampleScriptedDrag({
    releaseFrameTimeoutMs: deadlineMs,
    releaseAfterMs: deadlineMs - 10,
  });
  assert.equal(inside.releaseFrame?.completedAtMs, inside.releasedAtMs + deadlineMs - 10);
  assert.equal(completed(inside), 60);
  const past = await sampleScriptedDrag({
    releaseFrameTimeoutMs: deadlineMs,
    releaseAfterMs: deadlineMs + 10,
  });
  assert.equal(past.releaseFrame, null, "a release frame past the deadline is not the release");
  assert.ok(past.finishedAtMs - past.releasedAtMs < deadlineMs + 10, "no wait past the deadline");
  // A release frame at another camera than the current one does not meet the release.
  const otherCamera = await sampleScriptedDrag({
    releaseFrameTimeoutMs: deadlineMs,
    releaseAfterMs: 100,
    releaseZoom: 1.25,
  });
  assert.equal(otherCamera.releaseFrame, null);
  // The sampler stops at the release frame rather than waiting out the deadline.
  const prompt = await sampleScriptedDrag({
    releaseFrameTimeoutMs: deadlineMs,
    releaseAfterMs: 120,
  });
  assert.equal(completed(prompt), 60);
  assert.ok(prompt.finishedAtMs - prompt.releasedAtMs < 200, "the sampler stops at the release");
  // Held frames that change on every animation frame fill the stream before a late release; the
  // release frame is still kept.
  const busy = await sampleScriptedDrag({
    releaseFrameTimeoutMs: deadlineMs,
    releaseAfterMs: 4500,
    frameEveryMs: 16,
  });
  assert.equal(busy.frames.length, 257, "256 samples and the release frame");
  assert.equal(completed(busy), 60);
  assert.equal(
    deadlineMs,
    500 + 15000 + 5000,
    "the stall guard equals the release's previous bound: sampler window, idle wait, release poll",
  );
});

test("the drag sampler still reports inputs that never complete", async () => {
  const stream = await import("./drag-input-stream.mjs");
  const deadlineMs = stream.kReleaseFrameTimeoutMs;
  const lost = await sampleScriptedDrag({
    releaseFrameTimeoutMs: deadlineMs,
    releaseAfterMs: null,
  });
  assert.equal(lost.releaseFrame, null);
  assert.equal(stream.dragInputLatencies(lost, 1).length, 0, "no frame completed an input");
  const waitedMs = lost.finishedAtMs - lost.releasedAtMs;
  assert.ok(
    waitedMs >= deadlineMs && waitedMs < deadlineMs + 10,
    `the sampler gives up at the release stall guard, after ${waitedMs} ms`,
  );
  // The editor dropped the last moves: its release frame represents the 45th of 60 positions.
  const partial = await sampleScriptedDrag({
    releaseFrameTimeoutMs: deadlineMs,
    releaseAfterMs: 100,
    releasePoint: { x: Math.round(400 + 0.25 * 120 * 0.75), y: Math.round(300 + 0.25 * 72 * 0.75) },
  });
  assert.equal(partial.releaseFrame, null, "a frame at an earlier position is not the release");
  const completed = stream.dragInputLatencies(partial, 1).length;
  assert.ok(completed > 40 && completed < 50, `only inputs before the dropped moves: ${completed}`);
});

test("the release deadline is checked once, before the drag's completion checks", () => {
  const spec = readFileSync(path.join(testDirectory, "browser-responsiveness.perf.ts"), "utf8");
  const release = spec.indexOf(
    "expect(stream.releaseFrame, "
      + "\"release must reach a current paired frame at the requested camera\")",
  );
  assert.ok(release >= 0, "the spec must fail a drag whose sampler saw no release frame");
  assert.equal(spec.split("release must reach a current paired frame").length, 2);
  assert.ok(release < spec.indexOf("\"dragInputsComplete\","), "the release is checked first");
  const settled = spec.indexOf(
    "the presented frame after idle must still be the release at the requested camera",
  );
  assert.ok(
    settled > release && settled < spec.indexOf("\"dragInputsComplete\","),
    "after idle the presented frame is checked once, without waiting",
  );
  assert.equal(
    spec.split("kReleaseFrameTimeoutMs").length,
    3,
    "the deadline goes only to the sampler, so no poll restarts it",
  );
});

test("only the tracked responsiveness checks go through the completion check helper", () => {
  const spec = readFileSync(path.join(testDirectory, "browser-responsiveness.perf.ts"), "utf8");
  assert.match(spec, /completionCheckMode\(\)/, "the mode must come from the run's environment");
  const calls = [...spec.matchAll(/completionCheck\(\s*"(\w+)"/g)];
  assert.deepEqual(
    calls.map(([, check]) => check),
    [
      "heldDragPositions",
      "dragInputsComplete",
      "coldZoomPresents",
      "heldMaskedDragDamagePatch",
      "heldDragFastPathResults",
    ],
  );
  const [held, drag, coldZoom, damagePatch, fastPath] = calls.map(({ index }) => index);
  const damagePatchAssertion = spec.indexOf("() => expectHeldMaskedDragDamagePatch(results)");
  assert.match(
    spec.slice(damagePatch - 60, damagePatch),
    /if \(results\.checkHeldResults\) \{\s*await $/,
    "the damage patch check must run only when the summary asks for it",
  );
  assert.equal(
    spec.split('baselineOnly: process.env.DONNER_BROWSER_BASELINE === "1"').length,
    2,
    "the masked drag summary must take baseline mode from the run's environment",
  );
  assert.ok(
    damagePatchAssertion > damagePatch && damagePatchAssertion - damagePatch < 300,
    "the damage patch check must run its assertion through the helper",
  );
  const fastPathMessage = "the held drag must render a fast-path result for a move before release";
  assert.equal(spec.split(fastPathMessage).length, 2, "the fast-path check must appear once");
  assert.ok(
    spec.indexOf(fastPathMessage) > fastPath && spec.indexOf(fastPathMessage) - fastPath < 300,
    "the fast-path result check must run through the helper",
  );
  assert.ok(
    spec.slice(damagePatch, fastPath).includes(".catch((error) => checkFailures.push(error));"),
    "the fast-path result check must run even when the damage patch check misses",
  );
  const damagePatchMessage =
    "every held masked-child result must take the fast path and only patch its owning tile";
  assert.equal(spec.split(damagePatchMessage).length, 2, "the damage patch check must appear once");
  const assertionDefinition = spec.indexOf("function expectHeldMaskedDragDamagePatch(");
  assert.ok(
    assertionDefinition >= 0 && spec.indexOf(damagePatchMessage) > assertionDefinition
      && spec.indexOf(damagePatchMessage) - assertionDefinition < 300,
    "the damage patch message must belong to the helper the check runs",
  );
  assert.match(
    spec.slice(assertionDefinition, assertionDefinition + 300),
    /expect\(\s*heldMaskedDragShapeFailures\(summary\),/,
    "the damage patch helper must assert the shared shape evaluation",
  );
  assert.equal(
    spec.split('"geode-crown"').length,
    2,
    "only the masked crown drag may use the phase whose held-position miss is reported",
  );
  const heldMessage = "held drag must complete multiple represented positions";
  assert.equal(spec.split(heldMessage).length, 2, "the held-position check must appear once");
  const heldMessageAt = spec.indexOf(heldMessage);
  assert.ok(
    heldMessageAt > held && heldMessageAt - held < 300,
    "the held-position check must run through the helper",
  );
  assert.match(
    spec.slice(coldZoom - 80, coldZoom),
    /if \(phase === "cold-zoom"\) \{\s*await $/,
    "only the cold-zoom phase may report its presentation check",
  );
  const message =
    "every dispatched input, including the final one, must complete in the measured stream";
  assert.equal(spec.split(message).length, 2, "the drag input check must appear once");
  const messageAt = spec.indexOf(message);
  assert.ok(
    messageAt > drag && messageAt - drag < 400,
    "the drag input check must run through the helper",
  );
});

test("catalog browser gate admits resource failure and warm update acceptance without dropping cases", () => {
  const spec = readFileSync(path.join(testDirectory, "smoke.spec.ts"), "utf8");
  const titles = [...spec.matchAll(/^test(?:\.\w+)?\(\s*"([^"]+)"/gm)].map(([, title]) => title);
  const expected = [
    "catalog font loading adopts deferred WOFF2 without authored edits",
    "welcome picker paints before asynchronously rendering real SVG thumbnails",
    "WGPU diagnostics do not block the first carousel interaction",
    "required resource failure stays readable and prevents application startup",
    "code-only updates transfer no unchanged essential resource bodies from a populated cache",
  ];
  const pureTitle = "catalog font loading records only fixed GPU acquisition markers";
  const unitSpec = readFileSync(path.join(testDirectory, "browser-diagnostic-policy.spec.mjs"), "utf8");
  const selectors = readFileSync(path.join(testDirectory, "selector-tests.mjs"), "utf8");
  assert.ok(!titles.includes(pureTitle), "pure marker validation must not start a browser");
  assert.ok(unitSpec.includes(pureTitle), "the marker assertion must remain in the Node suite");
  assert.ok(selectors.includes('./browser-diagnostic-policy.spec.mjs'), "normal Node tests must include it");
  for (const title of expected) assert.ok(titles.includes(title), `${title} must remain declared`);
  // The Editor WASM chromium-default lane runs every case of smoke.spec.ts, these included (see
  // "the Editor WASM chromium-default lane owns the Chromium editor specs").
  assertDefaultLaneDiscovers("smoke.spec.ts");
});

// The first-boot case takes each browser's one-time startup cost, so the animation cases after it
// budget only their own boot and probes. That holds only while the lane runs one worker, in
// declaration order, with every case selected by the lane's grep.
test("each animation preview browser boots in its own case before the timed cases", () => {
  const spec = readFileSync(path.join(testDirectory, "smoke.spec.ts"), "utf8");
  const titles = [
    "animation preview plays on the browser's first boot",
    "animation preview plays at its playback cadence",
    "animation preview plays and dispatches mouse events",
    "animation preview plays through cancellation and shutdown while raster task yields are held",
  ];
  const declarations = titles.map((title) => {
    const declared = spec.indexOf(`test(\n  ${JSON.stringify(title)},`);
    assert.notEqual(declared, -1, `${title} is a top-level case`);
    return declared;
  });
  assert.deepEqual([...declarations].sort((a, b) => a - b), declarations, "declaration order");
  const animationCases = [...spec.matchAll(/^test(?:\.\w+)?\(\s*"(animation preview [^"]*)"/gm)]
    .map(([, title]) => title);
  assert.deepEqual(animationCases, titles, "no other animation preview case runs before it");

  const buildFile = readFileSync(path.join(testDirectory, "BUILD.bazel"), "utf8");
  const lane = buildFile
    .split("playwright_bin.playwright_test(\n")
    .find((body) => body.startsWith("    name = \"animation_preview_browser_test\","))
    ?.split("\n)\n")[0];
  assert.ok(lane, "the animation lane is declared");
  const grep = /"--grep=([^"]+)"/.exec(lane)?.[1];
  assert.ok(grep, "the animation lane selects its cases with --grep");
  for (const title of titles) assert.match(title, new RegExp(grep), `${title} matches the lane`);
  assert.doesNotMatch(lane, /shard_count/, "the animation lane is not sharded");

  const configs = [
    "playwright.animation.bazel.config.js",
    "playwright.bazel.config.js",
    "playwright.config.js",
  ].map((file) => readFileSync(path.join(testDirectory, file), "utf8"));
  for (const config of configs) assert.doesNotMatch(config, /fullyParallel/);
  assert.match(configs[1], /^ {2}workers: 1,$/m, "the lane runs one Playwright worker");
  assert.doesNotMatch(configs[0], /workers/, "the animation config keeps the one worker");
  assert.doesNotMatch(spec, /describe\.configure|test\.describe\.parallel/);
});

// A Firefox launched with only the private home can still wait for AppKit window restoration.
test("every config that uses the Firefox launch helper takes the full launch options", () => {
  const configs = readdirSync(testDirectory)
    .filter((file) => /^playwright\..*config\.js$/.test(file));
  const users = configs.filter((file) =>
    readFileSync(path.join(testDirectory, file), "utf8").includes("firefox-launch-env.cjs")
  );
  assert.ok(users.includes("playwright.animation.bazel.config.js"), "the scan finds the configs");
  for (const file of users) {
    const source = readFileSync(path.join(testDirectory, file), "utf8");
    assert.doesNotMatch(source, /firefoxLaunchEnvironment/, `${file} uses the full launch options`);
    assert.match(source, /\.\.\.firefoxLaunchOptions\(/, `${file} spreads the launch options`);
  }
});

test("hosted macOS quarantines read only the hosted job's switch and name their issue", () => {
  const definition = "const kHostedMacosQuarantine = "
    + "process.env.DONNER_HOSTED_MACOS_QUARANTINE === \"1\";";
  const quarantined = [
    [
      "smoke.spec.ts",
      "welcome picker paints before asynchronously rendering real SVG thumbnails",
      "#1702",
    ],
    [
      "smoke.spec.ts",
      "production Geode wasm presents visible editor pixels after delayed canvas GPU"
      + " work",
      "#1691",
    ],
    [
      "browser-presentation-regression.spec.ts",
      "Basic Shapes gated setup reaches visible pixels in the browser journey",
      "#1702",
    ],
  ];
  for (const spec of new Set(quarantined.map(([file]) => file))) {
    const source = readFileSync(path.join(testDirectory, spec), "utf8");
    assert.equal(source.split(definition).length - 1, 1, `${spec} defines the switch once`);
    assert.equal(
      source.split("DONNER_HOSTED_MACOS_QUARANTINE").length - 1,
      1,
      `${spec} reads the switch only through kHostedMacosQuarantine`,
    );
    const uses = source.split("    kHostedMacosQuarantine,\n").length - 1;
    assert.equal(uses, quarantined.filter(([file]) => file === spec).length, spec);
  }
  for (const [spec, title, issue] of quarantined) {
    const source = readFileSync(path.join(testDirectory, spec), "utf8");
    const opening = [...source.matchAll(/\ntest\(\s*"([^"\n]+)"/g)]
      .find((match) => match[1] === title);
    assert.ok(opening, title);
    const start = opening.index;
    const body = source.slice(start, source.indexOf("\ntest(", start + 1));
    const skip = body.indexOf("    kHostedMacosQuarantine,\n");
    assert.notEqual(skip, -1, `${title} must be skipped only by the hosted switch`);
    assert.match(body.slice(skip, skip + 200), new RegExp(`\\(${issue}\\)",`), title);
    assert.ok(skip < body.indexOf("await openEditor("), `${title} must skip before any work`);
  }
  // Nothing else may set or read the switch: a BUILD env entry or an export in the browser CI
  // script would quarantine every lane, including self-hosted and local runs.
  const scanned = /\.(?:[cm]?js|ts|json|bazel)$/;
  const readers = readdirSync(testDirectory)
    .filter((name) => name !== "browser-matrix.spec.mjs" && scanned.test(name))
    .filter((name) =>
      readFileSync(path.join(testDirectory, name), "utf8")
        .includes("DONNER_HOSTED_MACOS_QUARANTINE")
    );
  assert.deepEqual(readers.sort(), ["browser-presentation-regression.spec.ts", "smoke.spec.ts"]);
  assert.doesNotMatch(
    readFileSync(path.join(repositoryRoot, "tools/run-browser-ci.sh"), "utf8"),
    /HOSTED_MACOS_QUARANTINE/,
  );
});

// Viewports the hosted macOS Perf job recorded for "held drag advances UI submissions while
// composited pixels are sampled" when its "selecting D" click selected nothing (#1683): the editor
// was still publishing the previous document's viewport when the case aimed, and settled on the
// splash right after.
const kPreviousDocumentViewport = {
  paneX: 40,
  paneY: 31,
  paneWidth: 1131,
  paneHeight: 861,
  documentX: 285.5,
  documentY: 261.5,
  documentWidth: 640,
  documentHeight: 400,
  zoom: 1,
};
const kSplashViewport = {
  paneX: 40,
  paneY: 31,
  paneWidth: 1131,
  paneHeight: 861,
  documentX: 159.5,
  documentY: 205.5,
  documentWidth: 892,
  documentHeight: 512,
  zoom: 1,
};

test("the D aim waits for the splash, not for the previous document's viewport", async () => {
  const { showsSplashDocument, splashDPoint } = await import("./splash-aim.mjs");
  // The recorded failure: the aim from the previous document's viewport is the pointer position
  // the failing run reported, not the D point once the splash is displayed.
  assert.deepEqual(splashDPoint(kPreviousDocumentViewport), { x: 488, y: 566 });
  assert.deepEqual(splashDPoint(kSplashViewport), { x: 442, y: 596 });
  assert.equal(showsSplashDocument(kPreviousDocumentViewport), false);
  assert.equal(showsSplashDocument(kSplashViewport), true);
  // A zoomed splash still counts; a missing or degenerate viewport does not.
  const zoomed = {
    ...kSplashViewport,
    documentWidth: 892 * 1.93,
    documentHeight: 512 * 1.93,
    zoom: 1.93,
  };
  assert.equal(showsSplashDocument(zoomed), true);
  assert.equal(showsSplashDocument(undefined), false);
  assert.equal(showsSplashDocument({ ...kSplashViewport, zoom: 0 }), false);
});

test("responsiveness cases wait for the splash to be displayed before using it", () => {
  const spec = readFileSync(path.join(testDirectory, "browser-responsiveness.perf.ts"), "utf8");
  // The sample attribute changes before the editor displays the splash, and an idle wait taken
  // then can finish on the previous document (#1683).
  assert.doesNotMatch(
    spec,
    /"data-active-sample-id",\s*"donner-splash",\s*\);\s*(?:checkpoint\([^)]*\);\s*)?await waitForIdle\(page\);/,
  );
  assert.equal(spec.split("await waitForSplashDisplayed(page);").length - 1, 4);
});

test("responsiveness cases render at their project's device scale", () => {
  const spec = readFileSync(path.join(testDirectory, "browser-responsiveness.perf.ts"), "utf8");
  // A case that sets its own deviceScaleFactor measures another project's configuration under
  // this project's name (#2470): the scale is read only from the project.
  assert.doesNotMatch(
    spec.replaceAll("info.project.use.deviceScaleFactor", ""),
    /deviceScaleFactor/,
    "only the project may declare a responsiveness case's device scale",
  );
  // Every passing case then checks that scale against the page's own pixel ratio and canvas.
  const start = spec.indexOf("test.afterEach(async ({ page }, info) => {");
  assert.ok(start >= 0, "the device scale check must run after every case");
  const check = spec.slice(start, spec.indexOf("\n});", start));
  for (
    const statement of [
      'if (info.status !== "passed") return;',
      "const scale = projectDeviceScale(info);",
      "const raster = await rasterIdentity(page);",
      "dpr: scale,",
      "backingWidth: raster.cssWidth * scale,",
      "backingHeight: raster.cssHeight * scale,",
    ]
  ) {
    assert.ok(check.includes(statement), `the device scale check must keep \`${statement}\``);
  }
});

test("a presented-frame drag summary counts presented frames, not GPU submissions", async () => {
  const { kHeldDragPresentationBounds, presentedDragSummary, presentedDragFailures } = await import(
    "./presented-frame-samples.mjs"
  );
  const stream = {
    firstInputAt: 0,
    lastInputAt: 1000,
    start: { x: 442, y: 596 },
    end: { x: 322, y: 524 },
  };
  // One sample per page frame; each presented editor frame completes several GPU submissions.
  const sample = (t, frameId, step, inputRepresented = true) => ({
    t,
    frameId,
    completedSerial: frameId * 5,
    pointerX: 442 - 120 * step,
    pointerY: 596 - 72 * step,
    inputRepresented,
  });

  // A drag that presents a frame per input and follows the pointer.
  const healthy = [];
  for (let i = 0; i < 40; ++i) healthy.push(sample(10 + i * 20, 100 + i, i / 40));
  const good = presentedDragSummary(healthy, stream);
  assert.equal(good.activeSamples, 40);
  assert.equal(good.presentedFrames, 39, "frame transitions inside the input window");
  assert.equal(good.advancingFrames, 39);
  assert.deepEqual(presentedDragFailures(good, kHeldDragPresentationBounds), []);

  // Two presented frames, the second halfway along the drag, then a stall while GPU serials keep
  // climbing. Counting serials passed this; counting presented frames must not.
  const stalled = [sample(10, 100, 0), sample(30, 101, 0.5)];
  for (let i = 2; i < 40; ++i) {
    stalled.push({ ...sample(10 + i * 20, 101, 0.5), completedSerial: 505 + i * 7 });
  }
  const stall = presentedDragSummary(stalled, stream);
  assert.equal(stall.presentedFrames, 1);
  assert.deepEqual(
    presentedDragFailures(stall, kHeldDragPresentationBounds).map((failure) => failure.metric),
    ["presentedFrames", "advancingFrames"],
  );

  // Frames that keep presenting but represent the pointer moving only once do not pass either.
  const frozen = [];
  for (let i = 0; i < 40; ++i) frozen.push(sample(10 + i * 20, 100 + i, i === 0 ? 0 : 0.5));
  const freeze = presentedDragSummary(frozen, stream);
  assert.equal(freeze.advancingFrames, 1);
  assert.deepEqual(
    presentedDragFailures(freeze, kHeldDragPresentationBounds).map((failure) => failure.metric),
    ["advancingFrames"],
  );

  // Unrepresented positions and travel against the drag do not count; samples outside the input
  // window are ignored.
  const outside = presentedDragSummary([
    sample(-5, 1, 1),
    sample(10, 2, 0.4, false),
    sample(20, 3, -0.5),
    sample(1500, 4, 1),
  ], stream);
  assert.deepEqual([outside.activeSamples, outside.travelFraction], [2, 0]);
  assert.equal(presentedDragSummary([], stream).activeSamples, 0);
});

test("the editor-opening specs echo the GPU acquisition trace into the test log", () => {
  const library = readFileSync(
    path.join(repositoryRoot, "donner/gpu/browser/library_donner_gpu.js"),
    "utf8",
  );
  const helper = readFileSync(path.join(testDirectory, "gpu-session-console.ts"), "utf8");
  const prefix = /kTracePrefix: ['"](\[Geode\/browser\/gpu-trace\])['"]/.exec(library)?.[1];
  assert.equal(prefix, "[Geode/browser/gpu-trace]");
  assert.ok(helper.includes(`export const kGpuTracePrefix = "${prefix}";`));
  // A trace line must never read as the acquisition failures the smoke spec treats as fatal, nor
  // as the "[Geode/browser]" lines the backend-selection spec refuses.
  assert.ok(!prefix.startsWith("[Geode/browser/acquire]"));
  assert.ok(!prefix.includes("[Geode/browser]"));
  for (
    const spec of [
      "smoke.spec.ts",
      "browser-presentation-regression.spec.ts",
      "browser-responsiveness.perf.ts",
    ]
  ) {
    const source = readFileSync(path.join(testDirectory, spec), "utf8");
    const helperImport = /import \{ echoGpuSessionConsole \} from "\.\/gpu-session-console";/;
    assert.match(source, helperImport, spec);
    assert.match(source, /\n {2}echoGpuSessionConsole\(page\);\n/, spec);
  }
});

test("quarantine mode defaults to skipping and accepts only skip or report", async () => {
  const quarantine = await import("./quarantine-report.mjs");
  const variable = quarantine.kQuarantineReportEnv;
  assert.equal(variable, "DONNER_BROWSER_QUARANTINE_REPORT");
  assert.equal(quarantine.quarantineMode({}), "skip");
  assert.equal(quarantine.quarantineMode({ [variable]: "" }), "skip");
  assert.equal(quarantine.quarantineMode({ [variable]: "skip" }), "skip");
  assert.equal(quarantine.quarantineMode({ [variable]: "report" }), "report");
  assert.equal(quarantine.quarantineStillSkips({}), true);
  assert.equal(quarantine.quarantineStillSkips({ [variable]: "report" }), false);
  assert.throws(() => quarantine.quarantineMode({ [variable]: "1" }), /"skip" or "report"/);
});

test("the quarantine report summary records a forced failure with its evidence", async () => {
  const { quarantineReportSummary } = await import("./quarantine-report.mjs");
  const report = {
    suites: [{
      file: "smoke.spec.ts",
      specs: [{
        title: "Firefox keeps Basic Shapes resize pixels and outline synchronized",
        file: "smoke.spec.ts",
        tests: [{ results: [{ status: "passed", errors: [], attachments: [] }] }],
      }],
      suites: [{
        file: "browser-presentation-regression.spec.ts",
        specs: [{
          title: "Firefox keeps the dragged shape and its selection outline in every drag frame",
          file: "browser-presentation-regression.spec.ts",
          tests: [{
            results: [{
              status: "failed",
              errors: [{ message: "\nError: forced | failure\n    at frame 3" }],
              attachments: [
                { name: "failure-page-screenshot" },
                { name: "failure-canvas-readback" },
                { name: "failure-canvas-evidence" },
              ],
            }],
          }],
        }, {
          title: "Geode Wasm View overlays render tile metadata and sparse Slug triangle edges",
          file: "browser-presentation-regression.spec.ts",
          tests: [{ results: [{ status: "timedOut", errors: [{ message: "Test timeout" }] }] }],
        }],
      }],
    }],
  };
  const summary = quarantineReportSummary(report);
  assert.match(summary, /^### Quarantined cases \(report only\)\n/);
  const row = (title) => summary.split("\n").find((line) => line.startsWith(`| ${title} |`));
  assert.equal(
    row("Firefox keeps Basic Shapes resize pixels and outline synchronized"),
    "| Firefox keeps Basic Shapes resize pixels and outline synchronized | smoke.spec.ts"
      + " | passed |  |",
  );
  assert.equal(
    row("Firefox keeps the dragged shape and its selection outline in every drag frame"),
    "| Firefox keeps the dragged shape and its selection outline in every drag frame"
      + " | browser-presentation-regression.spec.ts | failed (reported)"
      + " | Error: forced \\| failure; evidence: failure-page-screenshot,"
      + " failure-canvas-readback, failure-canvas-evidence |",
  );
  assert.match(
    row("Geode Wasm View overlays render tile metadata and sparse Slug triangle edges"),
    /\| timedOut \(reported\) \| Test timeout \|$/,
  );
  assert.match(quarantineReportSummary(null), /wrote no results/);
  assert.match(quarantineReportSummary({ suites: [] }), /ran no cases/);

  const unfinished = quarantineReportSummary({
    errors: [{ message: "Timed out waiting 360s for the test suite to run" }],
    suites: [{
      file: "smoke.spec.ts",
      specs: [
        { title: "a <case>", file: "smoke.spec.ts", tests: [{ results: [] }] },
        {
          title: "still quarantined",
          file: "smoke.spec.ts",
          tests: [{ results: [{ status: "skipped" }] }],
        },
      ],
    }],
  });
  assert.match(unfinished, /\| a &lt;case&gt; \| smoke\.spec\.ts \| not run \| {2}\|/);
  assert.match(
    unfinished,
    /\| still quarantined \| smoke\.spec\.ts \| skipped \| unexpected in report mode/,
  );
  assert.match(unfinished, /\nRun error: Timed out waiting 360s for the test suite to run\n$/);
});

test("the quarantine summary command tolerates a missing or unreadable report", () => {
  const scratch = mkdtempSync(path.join(tmpdir(), "quarantine-summary-"));
  try {
    const command = path.join(testDirectory, "quarantine-report-summary.mjs");
    const missing = execFileSync(process.execPath, [command, path.join(scratch, "absent.json")], {
      encoding: "utf8",
    });
    assert.match(missing, /wrote no results/);
    const unreadable = path.join(scratch, "bad.json");
    writeFileSync(unreadable, "{ not json");
    assert.match(
      execFileSync(process.execPath, [command, unreadable], { encoding: "utf8" }),
      /Unreadable report/,
    );
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
});

test("only a report-only lane lifts the Firefox quarantine, and it never fails the job", () => {
  const config = require("./playwright.quarantine-report.config.js");
  assert.equal(config.projects.length, 1);
  const [project] = config.projects;
  // The overlays case resolves its golden by project name, so the report project must keep the
  // compatibility Firefox project's name and settings.
  const compatibility = require("./playwright.compatibility.config.js");
  const firefox = compatibility.projects.find((entry) => entry.name === "firefox-geode-resize");
  assert.equal(project.name, firefox.name);
  assert.deepEqual(project.use, firefox.use);
  assert.equal(project.snapshotPathTemplate, firefox.snapshotPathTemplate);
  assert.equal(config.snapshotPathTemplate, compatibility.snapshotPathTemplate);
  assert.equal(project.use.browserName, "firefox");
  const titles = [
    "Geode Wasm View overlays render tile metadata and sparse Slug triangle edges",
    "Firefox keeps Basic Shapes resize pixels and outline synchronized",
    "Firefox keeps the dragged shape and its selection outline in every drag frame",
  ];
  assert.equal(project.grep.length, titles.length);
  const specTitles = config.testMatch.flatMap((spec) =>
    [...readFileSync(path.join(testDirectory, spec), "utf8").matchAll(/\n\s*test\(\s*"([^"]+)"/g)]
      .map(([, title]) => title)
  );
  assert.deepEqual(
    specTitles.filter((title) => project.grep.some((pattern) => pattern.test(title))).sort(),
    [...titles].sort(),
    "the report lane must run exactly the quarantined cases",
  );
  assert.ok(
    config.globalTimeout > 0 && config.globalTimeout <= 6 * 60_000,
    "the report lane must stay within a few minutes of the nightly job",
  );
  assert.deepEqual(config.reporter.map(([name]) => name), ["list", "json"]);

  const browserCi = readFileSync(path.join(repositoryRoot, "tools/run-browser-ci.sh"), "utf8");
  const normalized = browserCi.replace(/\\\s*\n\s*/g, " ").replace(/\s+/g, " ");
  assert.equal(
    browserCi.split("DONNER_BROWSER_QUARANTINE_REPORT=report").length - 1,
    1,
    "only the report lane may lift the quarantine",
  );
  const gate = normalized.indexOf(
    "if [[ \"${DONNER_BROWSER_QUARANTINE_REPORT_LANE:-}\" == \"1\" ]]; then",
  );
  assert.notEqual(gate, -1, "the report lane must be gated on the workflow's schedule-only flag");
  const lane = normalized.slice(gate, normalized.indexOf(" fi ", gate));
  assert.ok(
    lane.includes(
      "DONNER_BROWSER_QUARANTINE_REPORT=report"
        + " DONNER_QUARANTINE_REPORT_JSON=\"${kQuarantineReportDir}/quarantine-report.json\""
        + " guarded \"${kQuarantineReportLane}\" bash donner/editor/wasm/tests/run_tests.sh"
        + " --headed --config=playwright.quarantine-report.config.js || report_lane_code=$?",
    ),
    "the report lane must lift the quarantine for its own command only",
  );
  assert.doesNotMatch(lane, /overall_status|run_lane /, "the report lane must not fail the job");
  assert.match(lane, /archive_lane_results "\$\{kQuarantineReportLane\}" \|\| echo/);
  // The lane's code is only ever printed: it never reaches overall_status or the exit.
  for (const line of browserCi.split("\n").filter((text) => text.includes("report_lane_code"))) {
    assert.doesNotMatch(line, /overall_status|^\s*exit\b/, line);
  }
  assert.match(browserCi, /\nexit "\$\{overall_status\}"\n$/);
  assert.ok(
    normalized.indexOf("unset DONNER_BROWSER_QUARANTINE_REPORT")
      < normalized.indexOf("run_lane \"chromium-default\""),
    "an inherited quarantine mode must not reach the regular lanes",
  );

  const workflow = readFileSync(
    path.join(repositoryRoot, ".github/workflows/editor_wasm.yml"),
    "utf8",
  );
  assert.ok(
    workflow.includes(
      "node donner/editor/wasm/tests/quarantine-report-summary.mjs"
        + ' "$RUNNER_TEMP/editor-wasm-browser-evidence/firefox-quarantine-report/quarantine-report.json" >> "$GITHUB_STEP_SUMMARY"',
    ),
    "the scheduled run must summarize the report lane",
  );
  assert.match(
    workflow,
    /path: \$\{\{ runner\.temp \}\}\/editor-wasm-browser-evidence\/firefox-quarantine-report\n/,
  );

  const specs = [["smoke.spec.ts", 1], ["browser-presentation-regression.spec.ts", 2]];
  for (const [spec, count] of specs) {
    const source = readFileSync(path.join(testDirectory, spec), "utf8");
    const quarantines = source.split(
      "browserName === \"firefox\" && quarantineStillSkips(),\n"
        + "    \"Quarantined: Firefox can capture a blank editor page (#1634)\",",
    ).length - 1;
    assert.equal(quarantines, count, `${spec} must lift its #1634 quarantine only in report mode`);
    assert.match(source, /\ninstallFailureCanvasEvidence\(test\);\n/);
  }
  // Each quarantined case arms the failure evidence right after its quarantine.
  for (const title of titles) {
    const spec = title.startsWith("Firefox keeps Basic Shapes")
      ? "smoke.spec.ts"
      : "browser-presentation-regression.spec.ts";
    const source = readFileSync(path.join(testDirectory, spec), "utf8");
    const start = source.indexOf(`\ntest("${title}"`);
    assert.notEqual(start, -1, title);
    const body = source.slice(start, source.indexOf("\ntest(", start + 1));
    const quarantine = body.indexOf("(#1634)");
    const armed = body.indexOf("armFailureCanvasEvidence(page, test.info());");
    assert.ok(quarantine !== -1 && armed > quarantine && armed - quarantine < 80, title);
  }
});

test("a report-lane archive that cannot copy warns instead of passing silently", () => {
  const script = readFileSync(path.join(repositoryRoot, "tools/run-browser-ci.sh"), "utf8");
  const archive = /\narchive_lane_results\(\) \{\n[\s\S]*?\n\}\n/.exec(script)?.[0];
  assert.ok(archive, "the browser CI script must define archive_lane_results");
  const callPattern =
    /\n {2}archive_lane_results "\$\{kQuarantineReportLane\}" \\\n {4}\|\| echo "[^"\n]*"\n/;
  const call = callPattern.exec(script)?.[0];
  assert.ok(call, "the report lane must warn when its archive fails");
  const scratch = mkdtempSync(path.join(tmpdir(), "archive-lane-"));
  try {
    const results = path.join(scratch, "test-results");
    mkdirSync(results);
    writeFileSync(path.join(results, "failure-canvas-readback.png"), "evidence");
    // A file where the archive directory belongs makes both mkdir and cp fail.
    const archiveRoot = path.join(scratch, "playwright-failures");
    writeFileSync(archiveRoot, "not a directory");
    const output = execFileSync("bash", [
      "-c",
      [
        "set -euo pipefail",
        `kResultsDir=${JSON.stringify(results)}`,
        `kFailureArchiveDir=${JSON.stringify(archiveRoot)}`,
        "kQuarantineReportLane=firefox-quarantine-report",
        archive,
        call,
        "echo lane-finished",
      ].join("\n"),
    ], { encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] });
    assert.match(output, /warning: could not archive the firefox-quarantine-report results/);
    assert.doesNotMatch(output, /Archived firefox-quarantine-report/);
    assert.match(output, /lane-finished/, "a failed archive must not stop the script");
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
});

test("the delayed-completion baseline waits for the picker's last publication frame", async () => {
  const { pickerPresentationSettled } = await import("./presentation-baseline.mjs");
  const spec = readFileSync(path.join(testDirectory, "browser-responsiveness.perf.ts"), "utf8");
  const start = spec.indexOf("\"delayed UI completion callbacks do not permanently disable");
  const body = spec.slice(start, spec.indexOf("await application.evaluate(", start));
  // The baseline is read only from a settled poll after the surface worker is found.
  assert.equal(body.split("pickerPresentationSettled(").length - 1, 2);
  assert.doesNotMatch(body, /report\.before = await snapshot\(page\);/);

  const thumbnails = {
    drained: true,
    pending: false,
    active: false,
    resultReady: false,
    ready: 5,
    rendered: 5,
    requested: 6,
    completed: 6,
  };
  const idle = { completedSerial: 19, submittedSerial: 19, framesInFlight: 0, oldestSerial: 0 };
  // Values recorded by the hosted Perf run whose Firefox baseline read caught the picker's last
  // thumbnail-publication frame in flight.
  const recordedInFlight = {
    thumbnails: { ...thumbnails, publishedAtMs: 6237.56 },
    presentation: {
      completedSerial: 18,
      submittedSerial: 19,
      framesInFlight: 1,
      oldestSerial: 19,
      completedAtMs: 6169.88,
    },
  };
  const settled = {
    thumbnails: { ...thumbnails, publishedAtMs: 4521.64 },
    presentation: { ...idle, completedAtMs: 4546.45 },
  };
  const withThumbnails = (changes) => ({
    ...settled,
    thumbnails: { ...settled.thumbnails, ...changes },
  });
  assert.equal(pickerPresentationSettled(recordedInFlight), false);
  assert.equal(pickerPresentationSettled(settled), true);
  // A publication whose frame has not completed still owes a submission.
  assert.equal(
    pickerPresentationSettled({
      ...settled,
      presentation: { ...idle, completedAtMs: 4500 },
    }),
    false,
  );
  // A rendered thumbnail that is not yet published still owes its publication frame.
  assert.equal(pickerPresentationSettled(withThumbnails({ ready: 4 })), false);
  for (const field of ["pending", "active", "resultReady"]) {
    assert.equal(pickerPresentationSettled(withThumbnails({ [field]: true })), false, field);
  }
  assert.equal(pickerPresentationSettled(withThumbnails({ drained: false })), false);
  assert.equal(pickerPresentationSettled({ presentation: settled.presentation }), false);
});

test("Playwright evidence upload retries only transport while retaining terminal failure", () => {
  const workflow = readFileSync(
    path.join(repositoryRoot, ".github/workflows/editor_wasm.yml"),
    "utf8",
  );
  const uploads = [...workflow.matchAll(
    / {6}- name: Upload Playwright evidence[^\n]*\n([\s\S]*?)(?=\n {6}- name:|$)/g,
  )].map(([text]) => ({
    id: text.match(/ {8}id: ([\w-]+)/)?.[1],
    condition: text.match(/ {8}if: \$\{\{ (.*?) \}\}/)?.[1],
    continueOnError: text.includes("        continue-on-error: true"),
    action: text.match(/ {8}uses: (.*)/)?.[1],
    name: text.match(/ {10}name: (.*)/)?.[1],
    path: text.match(/ {10}path: (.*)/)?.[1],
    retention: text.match(/ {10}retention-days: (.*)/)?.[1],
    missing: text.match(/ {10}if-no-files-found: (.*)/)?.[1],
    overwrite: text.match(/ {10}overwrite: (.*)/)?.[1],
  }));
  assert.equal(uploads.length, 3, "bound transport attempts to three");
  assert.equal(
    new Set(uploads.map(({ name }) => name)).size,
    3,
    "preserve uncertain prior artifacts",
  );
  assert.deepEqual(uploads.map(({ continueOnError }) => continueOnError), [true, true, false]);
  for (const upload of uploads) {
    assert.equal(upload.action, "actions/upload-artifact@v7");
    assert.equal(upload.path, "${{ runner.temp }}/editor-wasm-browser-evidence");
    assert.equal(upload.retention, "14");
    assert.equal(upload.missing, "warn");
    assert.notEqual(upload.overwrite, "true");
  }
  for (
    const scenario of [
      { outcomes: ["success"], attempts: 1, failed: false },
      { outcomes: ["failure", "success"], attempts: 2, failed: false },
      { outcomes: ["failure", "failure", "success"], attempts: 3, failed: false },
      { outcomes: ["failure", "failure", "failure"], attempts: 3, failed: true },
      { outcomes: ["failure"], cancelAfter: 1, attempts: 1, failed: false },
      { outcomes: [], cancelAfter: 0, attempts: 0, failed: false },
      { outcomes: ["failure", "success"], browserFailed: true, attempts: 2, failed: true },
    ]
  ) {
    const steps = {};
    let attempts = 0;
    let failed = scenario.browserFailed ?? false;
    for (const upload of uploads) {
      const cancelled = scenario.cancelAfter !== undefined && attempts >= scenario.cancelAfter;
      const expression = upload.condition.replaceAll("cancelled()", "cancelled")
        .replace(/steps\.([\w-]+)\.outcome/g, (_, id) => `steps[${JSON.stringify(id)}].outcome`);
      if (!new Function("steps", "cancelled", `return (${expression});`)(steps, cancelled)) {
        continue;
      }
      const outcome = scenario.outcomes[attempts++];
      assert.ok(outcome, "no extra upload attempt after recovery or exhaustion");
      steps[upload.id] = { outcome };
      failed ||= outcome === "failure" && !upload.continueOnError;
    }
    assert.equal(attempts, scenario.attempts, JSON.stringify(scenario));
    assert.equal(failed, scenario.failed, JSON.stringify(scenario));
  }
});

test("Chromium context diagnostics stay bounded, manual, and independent of the editor", () => {
  const buildFile = readFileSync(path.join(testDirectory, "BUILD.bazel"), "utf8");
  const diagnostic = buildFile.match(
    /js_test\(\n {4}name = "chromium_context_lifecycle_diagnostic",([\s\S]*?)\n\)/,
  );
  assert.ok(diagnostic, "expected the explicit browser-only diagnostic target");
  assert.match(diagnostic[1], /timeout = "short"/);
  assert.match(diagnostic[1], /args = \["--contexts=40"\]/);
  assert.deepEqual(parseBrowserLane(diagnostic[1]).tags.sort(), ["launches-browser", "manual", "no-sandbox"]);
  assert.doesNotMatch(diagnostic[1], /requires_desktop_session|wasm_web_package/);
  for (const input of ["playwright.config.js", "test-timing.cjs", "prepare-browser-archives.js"]) {
    assert.ok(diagnostic[1].includes(`"${input}"`), `${input} must be a declared runfile`);
  }
  const source = readFileSync(
    path.join(testDirectory, "chromium-context-lifecycle-diagnostic.cjs"),
    "utf8",
  );
  assert.match(source, /channel: baseConfig\.use\.channel/);
  assert.match(source, /headless: baseConfig\.use\.headless/);
  assert.match(source, /\.\.\.baseConfig\.use\.launchOptions/);
  assert.doesNotMatch(source, /DONNER_WASM_PACKAGE|error\.stack|error\.message|process\.getuid/);
});
