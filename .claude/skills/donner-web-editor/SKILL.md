---
name: donner-web-editor
description: >-
  Build, serve, test, package, security-review, deploy, verify, and roll back Donner's browser
  editor static site. Use when asked to build or host the Donner WASM editor; prepare the Geode or
  TinySkia web package; run browser parity tests; create or inspect an immutable deployment
  candidate; configure HTTPS, WebGPU, SharedArrayBuffer, COOP/COEP, MIME, CSP, or cache headers;
  publish the editor to a static host; or diagnose a deployed editor that is blank, not
  cross-origin isolated, or unable to load its pthread or WebGPU assets.
---

# Donner Web Editor

Build and deploy the browser editor as a static, cross-origin-isolated application. Treat Geode as
the production backend and TinySkia as a tested fallback, not as an interchangeable release.

## Route the request

- For a local build or preview, stop after the local browser suite passes.
- For a deployment, read `references/hosting-contract.md` before changing hosting configuration or
  publishing any artifact.
- For a browser editor bug, apply `donner-editor-debugging` and `donner-bugfix-discipline` first.
  Preserve the MCP-first, failing-test-first, root-cause workflow.
- For a PR or CI failure, apply `donner-pr-ci`. Never merge without explicit operator approval.
- For a product release or tag, apply `donner-release`; deploying this site is not permission to
  cut a Donner release.

Query live BUILD files, workflows, and test scripts before acting. Paths and target names below are
the current contract, but source wins when they drift.

## 1. Establish the source revision

Use a clean, isolated worktree. Record the exact source revision and dependency lock state. Do not
build a deployable candidate from a dirty checkout or mix files from different builds.

The production package is Geode/WebGPU:

```sh
bazel build --config=editor-wasm //donner/editor/wasm:wasm_web_package
```

Build the software fallback separately:

```sh
bazel build --config=editor-wasm-tiny-skia \
  //donner/editor/wasm:wasm_tiny_skia_web_package
```

Do not copy outputs between the backends. Both packages use the same public filenames, so mixing
their files creates a plausible-looking but invalid site.

## 2. Serve and verify locally

Serve Geode on localhost in one terminal:

```sh
bazel run --config=editor-wasm //donner/editor/wasm:serve_http -- \
  --no-https --host 127.0.0.1
```

Install the pinned browser-test dependency and run the headed Geode suite:

```sh
cd donner/editor/wasm/tests
npm ci
npx playwright install chromium
cd ../../../..
DONNER_WASM_BASE_URL=http://127.0.0.1:8000 \
DONNER_WASM_BACKEND=geode \
  bash donner/editor/wasm/tests/run_tests.sh --headed
```

Use the URL printed by the server if port 8000 was occupied. Repeat against TinySkia with
`serve_http_tiny_skia`, `--config=editor-wasm-tiny-skia`, and
`DONNER_WASM_BACKEND=tiny_skia`.

Require all of the following before packaging:

- The uninstrumented production canvas has visible pixels. Do not enable diagnostic GPU readback
  to make a blank canvas pass.
- Startup, colored document rendering, layer thumbnails, and zoom gestures pass.
- A real browser check covers selection, Inspector/source focus, zoom, drag, DOM-first source
  writeback, overlays, and the resulting pixels.
- There are no runtime aborts, uncaught exceptions, WebGPU validation errors, or service-worker
  registration loops.

## 3. Create the deployment candidate in CI

Local builds are preview evidence, not production artifacts. Dispatch `.github/workflows/editor_wasm.yml`
at the reviewed source revision. The workflow must build and test both backends, then upload the
Geode candidate named `donner-editor-wasm-geode-<source-sha>`.

Download that artifact without rebuilding it. Verify `SHA256SUMS` from the artifact root and check
that `provenance.json` names the expected source revision, Bazel configuration, and target. Treat
the uploaded artifact digest, per-file hashes, source revision, lockfile state, test run, security
review, hosting configuration, and access mode as one candidate record.

If the workflow does not produce this evidence, fix the workflow before deploying. Do not replace
the missing CI artifact with a local rebuild.

## 4. Complete the deployment gate

Before every production deployment, including an owner-only deployment:

1. Confirm the operator explicitly requested deployment.
2. Complete the security review in `references/hosting-contract.md` against the exact candidate.
3. Resolve every critical or high finding. Record an owner and disposition for lower findings.
4. Confirm the host can satisfy the HTTPS, cross-origin-isolation, MIME, cache, and atomic-release
   contract.
5. Retain the currently deployed artifact and configuration as the rollback candidate.

Default to owner-only access when the user did not request a public site and the host supports it.
Changing access to public or shared is a separate explicit action.

## 5. Deploy without rebuilding

Promote the verified `site/` directory from the CI candidate. Do not run Bazel, npm, minifiers,
asset optimizers, or bundlers in the deployment environment. Deploy the exact bytes atomically and
record the provider version plus candidate digest.

Keep `index.html`, `editor.js`, `editor.wasm`, `enable-threads.js`, and `donner_icon.svg` on one
release boundary. Stable filenames make partial or in-place uploads unsafe.

## 6. Verify the deployed editor

Verify the exact deployed URL, not a local server:

- Check HTTPS, `Content-Type: application/wasm`, COOP/COEP behavior, and cache headers.
- On first load and one reload, verify secure context, cross-origin isolation,
  `SharedArrayBuffer`, WebGPU availability, service-worker stability, and no console abort.
- Run the production Geode browser suite against the deployed URL when access controls permit.
- Repeat the parity interactions from local verification and confirm visible production pixels
  without diagnostic query parameters.
- Record the deployed version, URL, access mode, candidate digest, verification result, and
  rollback version.

If verification fails, stop promotion and reactivate the retained reviewed version. Roll back by
digest and compatible configuration; never rebuild old source to manufacture a rollback artifact.

## Hard rules

- Geode is the production site. Deploy TinySkia only when the operator explicitly selects the
  fallback and the deployment record says so.
- Never deploy a dirty, locally rebuilt, or partially copied package.
- Never hide a blank production canvas with readback diagnostics, retries, sleeps, or broader
  timeouts. Root-cause presentation scheduling and browser lifecycle failures.
- Never expose credentials, private infrastructure references, local paths, source maps, test
  caches, or unrelated build outputs in the static artifact.
- Never publish or promote without the security review and explicit operator authorization.
