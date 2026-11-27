const crypto = require("node:crypto");
const fs = require("node:fs");
const { get } = require("node:http");
const path = require("node:path");

async function sha256(stream) {
  const hash = crypto.createHash("sha256");
  for await (const chunk of stream) hash.update(chunk);
  return hash.digest("hex");
}

/** Verify the module at this test server's loopback port against the declared Bazel package. */
async function verifyServedEditor(baseUrl, directory) {
  if (!/^http:\/\/127\.0\.0\.1:\d+$/.test(baseUrl)) throw new Error("Expected editor test server on loopback");
  await verifyModule(baseUrl, directory, "editor.wasm");
  if (fs.existsSync(path.join(directory, "editor-asyncify.wasm"))) {
    await verifyModule(baseUrl, directory, "editor-asyncify.wasm");
  }
}

async function verifyModule(baseUrl, directory, filename) {
  const response = await new Promise((resolve, reject) => {
    get(new URL(filename, baseUrl), { signal: AbortSignal.timeout(30_000) }, resolve)
      .once("error", reject);
  });
  if (response.statusCode !== 200) {
    response.resume();
    throw new Error("Editor test server returned HTTP " + response.statusCode);
  }
  const [expected, served] = await Promise.all([
    sha256(fs.createReadStream(path.join(directory, filename))),
    sha256(response),
  ]);
  if (served !== expected) throw new Error(`Served ${filename} differs from the declared package`);
}

module.exports = { verifyServedEditor };
