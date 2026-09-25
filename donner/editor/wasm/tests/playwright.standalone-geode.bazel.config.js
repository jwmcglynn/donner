const baseConfig = require("./playwright.bazel.config.js");

// This lane serves the standalone renderer package, not the editor package used by the
// default browser matrix. Select only its spec even though the base config excludes it.
module.exports = {
  ...baseConfig,
  testIgnore: [],
  testMatch: "standalone-geode-browser-renderer.spec.ts",
};
