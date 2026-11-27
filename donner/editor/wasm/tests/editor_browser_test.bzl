"""Run one editor browser project against the size-optimized Geode package."""

load("@npm//donner/editor/wasm/tests:@playwright/test/package_json.bzl", playwright_bin = "bin")

_ARCHIVES = {
    "chromium": "@playwright_chromium_mac_arm64//file",
    "firefox": "@playwright-firefox-mac14-arm64//file",
    "webkit": "@playwright_webkit_mac_arm64//file",
}

def editor_browser_test(
        name,
        project,
        specs,
        sources,
        report_only = False,
        package = "//donner/editor/wasm:_wasm_web_package_for_serve",
        remote_only = False):
    """Runs the selected Playwright specs on a Darwin worker with a desktop session."""
    engine = "chromium" if project == "composited-chromium" else project.split("-")[0]
    archive = _ARCHIVES[engine]
    comparator = "//donner/editor/tests:standalone_geode_browser_png_compare"
    # The hosted producer runs manually selected tests on its own image.
    modes = [False] if remote_only else ([True] if report_only else [False, True])
    for hosted in modes:
        playwright_bin.playwright_test(
            name = ("hosted_" if hosted else "") + name,
            size = "large",
            timeout = "long",
            args = ["test"] + ["$(rootpath :%s)" % spec for spec in specs] + [
                "--config=$(rootpath :playwright.editor-ci.config.js)",
            ],
            copy_data_to_bin = False,
            data = specs + sources + [
                "package.json",
                "playwright.editor-ci.config.js",
                "remote-webserver.mjs",
                ":editor_browser_config_sources",
                ":node_modules/@playwright/test",
                ":node_modules/pngjs",
                archive,
                comparator,
                package,
            ],
            env = ({"DONNER_BROWSER_QUARANTINE_REPORT": "report"} if report_only else {}) | {
                "CI": "1",
                "DONNER_BROWSER_GOLDEN_COMPARE": "$(rootpath %s)" % comparator,
                "DONNER_EDITOR_BROWSER_PROJECT": project,
                "DONNER_WASM_BACKEND": "geode",
                "DONNER_WASM_EXPECTED_HEADLESS_BACKEND": "browser",
                "DONNER_WASM_PACKAGE_DIR": "$(rootpath %s)" % package,
                "DONNER_WASM_REQUIRE_WEBGPU": "1",
                "DONNER_WASM_TEST_SERVER": "$(rootpath :remote-webserver.mjs)",
                "DONNER_%s_ARCHIVE" % engine.upper(): "$(rootpath %s)" % archive,
                "NODE_OPTIONS": "",
            },
            tags = ["manual", "no-sandbox", "resources:local_gpu:1"] if hosted else [
                "editor-wasm-browser", "launches-browser", "no-local", "no-sandbox", "requires_desktop_session",
            ],
            target_compatible_with = ["@platforms//cpu:aarch64", "@platforms//os:macos"],
        )
