import type { test as baseTest } from "@playwright/test";
import { recordBrowserStall } from "./browser-stall-diagnostics.mjs";

/**
 * When a test ends at its deadline, record the browser processes it leaves
 * behind before fixture teardown closes or kills them.
 *
 * `afterEach` hooks run after the test body ends, including after a timeout,
 * and before the page and context fixtures are torn down, so a browser that
 * stopped answering is still alive here. The recorder asks the operating system
 * rather than the browser, which may not respond, and the hook requests no
 * fixtures, so other tests do no extra work and a test without a page starts no
 * browser. Nothing is recorded unless the hosted CI job enabled it; see
 * `shouldRecordBrowserStall`.
 */
export function installBrowserStallDiagnostics(test: typeof baseTest): void {
  test.afterEach(async () => {
    await recordBrowserStall(test.info());
  });
}
