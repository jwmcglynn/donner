// The JavaScript lint rules CodeFactor applies to this repository: its default ESLint
// configuration (https://github.com/codefactor-io/default-configs, `.eslintrc.json`), which is
// `eslint:recommended` with the rules below switched off or adjusted, and the environments and
// globals it declares. CodeFactor lints `*.js`, `*.jsx`, `*.es6` and `*.mjs` files outside
// `third_party/`; `:js_lint_test` runs these rules on those files, and
// tools/check_js_lint_coverage.py fails the Lint lane when a tracked file is missing from it.
// `eslint`, `@eslint/js` and `globals` are pinned in package.json: ESLint to the release
// CodeFactor lists for its analysis, and `globals` to the version that release's environments
// use. Errors fail the test; `no-unused-vars` stays a warning, as in CodeFactor's configuration.
//
// Files are parsed as ES modules. CodeFactor's configuration names script mode, but CodeFactor
// reports findings inside `.mjs` files that parse only as modules and no parse errors on them;
// on the files that parse either way, both modes give the same findings.
import js from "@eslint/js";
import globals from "globals";

export default [
  {
    files: ["**/*.js", "**/*.jsx", "**/*.es6", "**/*.mjs"],
    languageOptions: {
      parserOptions: { ecmaFeatures: { jsx: true } },
      globals: {
        ...globals.es2015,
        ...globals.node,
        ...globals.browser,
        ...globals.jquery,
        ...globals.jasmine,
        ...globals.mocha,
        ...globals.qunit,
        ActiveXObject: true,
        XDomainRequest: true,
        ScriptEngine: true,
        WSH: true,
        WScript: true,
        DocumentTouch: true,
      },
    },
    linterOptions: { reportUnusedDisableDirectives: "off" },
    rules: {
      ...js.configs.recommended.rules,
      "no-undef": "off",
      "no-fallthrough": "off",
      "no-mixed-spaces-and-tabs": "off",
      "no-redeclare": "off",
      "no-with": "off",
      "no-prototype-builtins": "off",
      "no-misleading-character-class": "off",
      "no-async-promise-executor": "off",
      "no-import-assign": "off",
      "no-sparse-arrays": "off",
      "no-useless-escape": "off",
      "no-empty": ["error", { allowEmptyCatch: true }],
      "no-unused-vars": ["warn", { vars: "local", args: "after-used", ignoreRestSiblings: false }],
    },
  },
];
