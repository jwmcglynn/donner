const prepareBrowsers = require("./prepare-browser-archives.js");

const { verifyServedEditor } = require("./served-editor-package.cjs");

module.exports = async function setup(config) {
  await verifyServedEditor(process.env.DONNER_WASM_BASE_URL, process.env.DONNER_WASM_PACKAGE_DIR);
  await prepareBrowsers(config);
};
