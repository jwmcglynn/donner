// A platform or capability skip must never turn an entire selected browser target green.
module.exports = class EditorBrowserCoverage {
  onBegin(_config, suite) {
    this.suite = suite;
  }

  onEnd(result) {
    if (result.status !== "passed") return;
    const cases = this.suite.allTests().filter((test) =>
      test.expectedStatus === "passed" && test.results.some((attempt) => attempt.status === "passed")
    );
    if (!cases.length) {
      console.error("No editor browser case passed in this target.");
      return { status: "failed" };
    }
    console.log(`Editor browser cases passed: ${cases.length}`);
  }
};
