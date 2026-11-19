import crypto from "node:crypto";
import fs from "node:fs";
import { get } from "node:http";
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

async function sha256(stream) {
  const hash = crypto.createHash("sha256");
  for await (const chunk of stream) hash.update(chunk);
  return hash.digest("hex");
}

/** Verify the module at this test server's loopback port against the declared Bazel package. */
export async function verifyServedEditor(baseUrl, directory) {
  if (!/^http:\/\/127\.0\.0\.1:\d+$/.test(baseUrl)) throw new Error("Expected editor test server on loopback");
  const response = await new Promise((resolve, reject) => {
    get(new URL("/editor.wasm", baseUrl), { signal: AbortSignal.timeout(30_000) }, resolve)
      .once("error", reject);
  });
  if (response.statusCode !== 200) {
    response.resume();
    throw new Error("Editor test server returned HTTP " + response.statusCode);
  }
  const [expected, served] = await Promise.all([
    sha256(fs.createReadStream(path.join(directory, "editor.wasm"))),
    sha256(response),
  ]);
  if (served !== expected) throw new Error("Served editor.wasm differs from the declared package");
}
