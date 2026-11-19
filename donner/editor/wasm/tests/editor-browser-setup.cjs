const prepareBrowsers = require("./prepare-browser-archives.js");

module.exports = async function setup(config) {
  const { verifyServedEditor } = await import("./package-identity.mjs");
  await verifyServedEditor(process.env.DONNER_WASM_BASE_URL, process.env.DONNER_WASM_PACKAGE_DIR);
  await prepareBrowsers(config);
};
