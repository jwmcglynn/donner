import type { Page } from "@playwright/test";

/**
 * The browser GPU library's acquisition trace (`library_donner_gpu.js`): one line per settled step
 * of a device request, with its elapsed time, `late=1` when the editor had already given up on it,
 * a `still_pending` line when nothing settled within a minute, and a line when a device is lost.
 */
export const kGpuTracePrefix = "[Geode/browser/gpu-trace]";

/** Bound on echoed lines per page, so a looping failure cannot flood the test log. */
const kMaxEchoedLinesPerPage = 32;

const echoingPages = new WeakSet<Page>();

/**
 * Echo the page's GPU acquisition trace and a page crash to the test log.
 *
 * A Chromium session whose GPU work never arrives then says, in the log of the run that hit it,
 * how long its device request took and whether it ever settled. Observation only: nothing a case
 * asserts on changes, and calling this again for the same page adds nothing.
 *
 * @param page The page that opens the editor.
 */
export function echoGpuSessionConsole(page: Page): void {
  if (echoingPages.has(page)) return;
  echoingPages.add(page);
  let echoed = 0;
  page.on("console", (message) => {
    const text = message.text();
    if (!text.startsWith(kGpuTracePrefix) || echoed >= kMaxEchoedLinesPerPage) return;
    echoed += 1;
    console.log(text.slice(0, 400));
  });
  page.on("crash", () => {
    console.log(`${kGpuTracePrefix} step=page outcome=crashed`);
  });
}
