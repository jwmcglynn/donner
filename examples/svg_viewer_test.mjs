/**
 * @file svg_viewer_test.mjs
 * @brief Playwright test for the Donner SVG Viewer WASM package.
 *
 * Serves the built WASM package with COOP/COEP headers (required for
 * SharedArrayBuffer / pthreads), loads it headlessly in Chromium,
 * screenshots the canvas, and asserts that red and blue pixels from
 * the default SVG are present.
 *
 * Usage:
 *   node examples/svg_viewer_test.mjs
 *
 * Prerequisites:
 *   bazel build --config=wasm //examples:svg_viewer_web_package
 */

import { chromium } from "playwright";
import { createServer } from "http";
import { readFileSync, existsSync } from "fs";
import { join, extname, basename } from "path";
import { execSync } from "child_process";
import { createRequire } from "module";
import { PNG } from "pngjs";
import { writeFileSync } from "fs";

// ---------------------------------------------------------------------------
// Resolve the WASM output directory from Bazel.
// ---------------------------------------------------------------------------

const REPO_ROOT = new URL("..", import.meta.url).pathname.replace(/\/$/, "");

function findWasmDir() {
  // Try the Bazel symlink first.
  const symlinkPath = join(REPO_ROOT, "bazel-bin", "examples", "svg_viewer_web_package");
  if (existsSync(join(symlinkPath, "svg_viewer.js"))) {
    return symlinkPath;
  }
  // Fallback: ask Bazel for bin dir.
  try {
    const binDir = execSync("bazel info bazel-bin", { cwd: REPO_ROOT, encoding: "utf8" }).trim();
    const path = join(binDir, "examples", "svg_viewer_web_package");
    if (existsSync(join(path, "svg_viewer.js"))) {
      return path;
    }
  } catch (_) {
    // ignored
  }
  throw new Error("WASM package not found. Run: bazel build --config=wasm //examples:svg_viewer_web_package");
}

const WASM_DIR = findWasmDir();
const SHELL_HTML = join(REPO_ROOT, "examples", "svg_viewer_shell.html");

console.log(`WASM dir : ${WASM_DIR}`);
console.log(`Shell HTML: ${SHELL_HTML}`);

// ---------------------------------------------------------------------------
// Minimal HTTP server with COOP/COEP headers.
// ---------------------------------------------------------------------------

const MIME_TYPES = {
  ".html": "text/html",
  ".js": "application/javascript",
  ".wasm": "application/wasm",
  ".data": "application/octet-stream",
  ".map": "application/json",
};

function startServer(port) {
  return new Promise((resolve, reject) => {
    const server = createServer((req, res) => {
      // COOP + COEP headers required for SharedArrayBuffer (pthreads).
      res.setHeader("Cross-Origin-Opener-Policy", "same-origin");
      res.setHeader("Cross-Origin-Embedder-Policy", "require-corp");

      let filePath;
      const url = req.url.split("?")[0];
      if (url === "/" || url === "/index.html") {
        filePath = SHELL_HTML;
      } else {
        // Sanitize user-controlled path BEFORE any path-building. Strip to
        // the basename so there is no directory structure to traverse; the
        // WASM package is a flat directory of sibling files (svg_viewer.js,
        // svg_viewer.wasm, svg_viewer.data, etc.), so basename-only access
        // is sufficient. CodeQL (alert #21) flagged the unchecked join as
        // "uncontrolled data used in path expression" — this sanitizer runs
        // before the path is built, which makes the taint flow terminate.
        const safeName = basename(url);
        if (!safeName || safeName === "." || safeName === "..") {
          res.writeHead(403);
          res.end("Forbidden");
          return;
        }
        filePath = join(WASM_DIR, safeName);
      }

      try {
        const data = readFileSync(filePath);
        const ext = extname(filePath);
        res.writeHead(200, { "Content-Type": MIME_TYPES[ext] || "application/octet-stream" });
        res.end(data);
      } catch (err) {
        console.error(`404: ${filePath}`);
        res.writeHead(404);
        res.end("Not found");
      }
    });

    server.listen(port, "127.0.0.1", () => {
      console.log(`Serving on http://127.0.0.1:${port}`);
      resolve(server);
    });
    server.on("error", reject);
  });
}

// ---------------------------------------------------------------------------
// Pixel analysis helpers.
// ---------------------------------------------------------------------------

function analyzePixels(pngBuffer) {
  const png = PNG.sync.read(pngBuffer);
  const { data, width, height } = png;

  let redPixels = 0;
  let bluePixels = 0;
  let nonBlackPixels = 0;

  // Sample some pixels for diagnostics.
  const samplePixels = [];

  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const i = (y * width + x) * 4;
      const r = data[i];
      const g = data[i + 1];
      const b = data[i + 2];

      if (r > 50 || g > 50 || b > 50) {
        nonBlackPixels++;
      }
      // Red: high R, moderate G & B allowed (e.g. #e74c3c = rgb(231,76,60)).
      if (r > 180 && g < 120 && b < 120) {
        redPixels++;
      }
      // Blue: high B, low-ish R (e.g. #3498db = rgb(52,152,219)).
      if (b > 180 && r < 100) {
        bluePixels++;
      }

      // Collect samples across the entire image, including right panel.
      if (samplePixels.length < 40 && y % 100 === 50 && x % 100 === 50) {
        samplePixels.push({ x, y, r, g, b, a: data[i + 3] });
      }
    }
  }

  return { width, height, redPixels, bluePixels, nonBlackPixels, samplePixels };
}

// ---------------------------------------------------------------------------
// Main test.
// ---------------------------------------------------------------------------

const PORT = 8787;
let server;
let browser;
let exitCode = 1;

try {
  server = await startServer(PORT);

  browser = await chromium.launch({
    headless: true,
    args: [
      "--disable-web-security",
      "--enable-features=SharedArrayBuffer",
    ],
  });

  const context = await browser.newContext();
  const page = await context.newPage();

  // Collect console messages for diagnostics.
  const consoleMsgs = [];
  page.on("console", (msg) => {
    consoleMsgs.push(`[${msg.type()}] ${msg.text()}`);
  });
  page.on("pageerror", (err) => {
    consoleMsgs.push(`[pageerror] ${err.message}`);
  });

  console.log("Navigating to page...");
  await page.goto(`http://127.0.0.1:${PORT}/`, { waitUntil: "networkidle" });

  // Wait for rendering to stabilize — the viewer prints "Donner SVG Viewer started."
  // when the main loop begins. Give it a few frames to render.
  console.log("Waiting for rendering to stabilize...");
  try {
    await page.waitForFunction(() => {
      // The canvas should have non-zero dimensions and content drawn.
      const canvas = document.getElementById("canvas");
      return canvas && canvas.width > 0 && canvas.height > 0;
    }, { timeout: 30000 });
  } catch (e) {
    console.error("Canvas did not become ready within 30s");
  }

  // Give a few extra frames for the ImGui render + SVG parse/render cycle.
  await page.waitForTimeout(3000);

  // Check canvas dimensions.
  const canvasInfo = await page.evaluate(() => {
    const c = document.getElementById("canvas");
    return {
      cssWidth: c.clientWidth,
      cssHeight: c.clientHeight,
      bufWidth: c.width,
      bufHeight: c.height,
      dpr: window.devicePixelRatio,
    };
  });
  console.log(`Canvas CSS: ${canvasInfo.cssWidth}x${canvasInfo.cssHeight}, ` +
              `buffer: ${canvasInfo.bufWidth}x${canvasInfo.bufHeight}, dpr: ${canvasInfo.dpr}`);

  // Use page screenshot — WebGL canvas content is captured via compositor.
  const screenshot = await page.screenshot({ type: "png" });
  console.log(`Screenshot: ${screenshot.length} bytes`);

  // Save for manual inspection.
  const screenshotPath = join(REPO_ROOT, "examples", "svg_viewer_screenshot.png");
  writeFileSync(screenshotPath, screenshot);
  console.log(`Saved screenshot to ${screenshotPath}`);

  // Analyze pixels.
  const analysis = analyzePixels(screenshot);
  console.log(`Resolution  : ${analysis.width}x${analysis.height}`);
  console.log(`Non-black px: ${analysis.nonBlackPixels}`);
  console.log(`Red pixels  : ${analysis.redPixels}`);
  console.log(`Blue pixels : ${analysis.bluePixels}`);

  // Show sample pixels for debugging.
  if (analysis.samplePixels.length > 0) {
    console.log("\nSample pixels:");
    for (const p of analysis.samplePixels) {
      console.log(`  (${p.x},${p.y}): rgba(${p.r},${p.g},${p.b},${p.a})`);
    }
  }

  // Diagnostics: print console messages.
  if (consoleMsgs.length > 0) {
    console.log("\nBrowser console:");
    for (const msg of consoleMsgs.slice(0, 30)) {
      console.log(`  ${msg}`);
    }
    if (consoleMsgs.length > 30) {
      console.log(`  ... (${consoleMsgs.length - 30} more)`);
    }
  }

  // Assertions.
  const RED_THRESHOLD = 50;
  const BLUE_THRESHOLD = 50;

  const passed = analysis.redPixels >= RED_THRESHOLD && analysis.bluePixels >= BLUE_THRESHOLD;
  if (passed) {
    console.log("\n✅ PASS — red and blue pixels detected from rendered SVG.");
    exitCode = 0;
  } else {
    console.error(`\n❌ FAIL — expected ≥${RED_THRESHOLD} red and ≥${BLUE_THRESHOLD} blue pixels.`);
    console.error(`  Got: red=${analysis.redPixels}, blue=${analysis.bluePixels}`);
    exitCode = 1;
  }
} catch (err) {
  console.error("Test error:", err);
  exitCode = 1;
} finally {
  if (browser) await browser.close();
  if (server) server.close();
  process.exit(exitCode);
}
