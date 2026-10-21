const { chromium } = require("@playwright/test");
const baseConfig = require("./playwright.config.js");
const { prepareBrowserArchives, browserExecutablePaths } = require("./prepare-browser-archives.js");
const { chromiumLaunchEnvironment } = require("./chromium-launch-env.cjs");

async function main() {
  const requested = process.argv.slice(2).filter((argument) => argument.startsWith("--contexts="));
  const contexts = Number(requested.at(-1)?.split("=")[1] ?? 40);
  if (![1, 40].includes(contexts)) throw new Error("contexts must be 1 or 40");
  if (baseConfig.use.headless !== true || baseConfig.use.channel !== "chromium") {
    throw new Error("expected the presentation lane's modern headless Chromium configuration");
  }
  prepareBrowserArchives(process.env);
  const browser = await chromium.launch({
    ...baseConfig.use.launchOptions,
    channel: baseConfig.use.channel,
    headless: baseConfig.use.headless,
    executablePath: browserExecutablePaths(process.env).chromium,
    env: chromiumLaunchEnvironment(process.env),
  });
  let closing = false;
  let completed = 0;
  browser.on("disconnected", () => console.log(JSON.stringify({ event: "disconnected", closing })));
  try {
    for (let iteration = 0; iteration < contexts; ++iteration) {
      const context = await browser.newContext({ viewport: { width: 1280, height: 720 } });
      const page = await context.newPage();
      await page.setContent("<canvas width='640' height='400'></canvas>");
      await page.evaluate(async () => {
        const context = document.querySelector("canvas").getContext("2d");
        context.fillStyle = "blue";
        context.fillRect(0, 0, 640, 400);
        await new Promise(requestAnimationFrame);
      });
      await context.close();
      ++completed;
      console.log(JSON.stringify({ event: "context-closed", completed }));
    }
  } finally {
    closing = true;
    await browser.close();
  }
  console.log(JSON.stringify({ event: "completed", contexts: completed }));
}
main().catch((error) => {
  console.error(JSON.stringify({ event: "failure", name: error.name }));
  process.exitCode = 1;
});
