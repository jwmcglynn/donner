import * as esbuild from "esbuild";
import { copyFileSync, mkdirSync } from "fs";
import { join, dirname } from "path";
import { fileURLToPath } from "url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const isWatch = process.argv.includes("--watch");

/** @type {esbuild.BuildOptions} */
const extensionConfig = {
  entryPoints: [join(__dirname, "src/extension.ts")],
  bundle: true,
  outfile: join(__dirname, "dist/extension.js"),
  external: ["vscode"],
  format: "cjs",
  platform: "node",
  target: "es2020",
  sourcemap: true,
  minify: !isWatch,
};

/** @type {esbuild.BuildOptions} */
const webviewConfig = {
  entryPoints: [join(__dirname, "webview/boot.ts")],
  bundle: true,
  outfile: join(__dirname, "dist/webview/boot.js"),
  format: "iife",
  platform: "browser",
  target: "es2020",
  sourcemap: true,
  minify: !isWatch,
};

function copyStaticAssets() {
  const outDir = join(__dirname, "dist/webview");
  mkdirSync(outDir, { recursive: true });
  copyFileSync(join(__dirname, "webview/style.css"), join(outDir, "style.css"));
}

async function build() {
  if (isWatch) {
    const extCtx = await esbuild.context(extensionConfig);
    const webCtx = await esbuild.context(webviewConfig);
    await Promise.all([extCtx.watch(), webCtx.watch()]);
    copyStaticAssets();
    console.log("Watching for changes...");
  } else {
    await Promise.all([esbuild.build(extensionConfig), esbuild.build(webviewConfig)]);
    copyStaticAssets();
    console.log("Build complete.");
  }
}

build().catch((err) => {
  console.error(err);
  process.exit(1);
});
