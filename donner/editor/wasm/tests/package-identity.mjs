import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";

/** Hash the exact JS/Wasm files served by the browser fixture, without publishing their paths. */
export function packageHashes(directory = process.env.DONNER_WASM_PACKAGE_DIR) {
  if (!directory) throw new Error("the browser fixture must name its packaged module directory");
  return Object.fromEntries(["wasm", "js"].map((extension) => [
    `${extension}Sha256`,
    crypto.createHash("sha256").update(
      fs.readFileSync(path.join(directory, `editor.${extension}`)),
    ).digest("hex"),
  ]));
}
