import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";

/** Hash the exact JS/Wasm files served by the browser fixture, without publishing their paths. */
export function packageHashes(directory = process.env.DONNER_WASM_PACKAGE_DIR) {
  if (!directory) throw new Error("the browser fixture must name its packaged module directory");
  const files = [
    ["wasmSha256", "editor.wasm"],
    ["jsSha256", "editor.js"],
  ];
  if (fs.existsSync(path.join(directory, "editor-asyncify.wasm"))) {
    files.push(["asyncifyWasmSha256", "editor-asyncify.wasm"],
               ["asyncifyJsSha256", "editor-asyncify.js"]);
  }
  return Object.fromEntries(files.map(([key, filename]) => [
    key,
    crypto.createHash("sha256").update(
      fs.readFileSync(path.join(directory, filename)),
    ).digest("hex"),
  ]));
}
