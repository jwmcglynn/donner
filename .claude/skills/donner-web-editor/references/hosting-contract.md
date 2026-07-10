# Donner Web Editor Hosting Contract

Read this file before creating a deployment candidate, changing static-host configuration, or
publishing the browser editor.

## Runtime contract

The deployable Geode site contains these required files at one origin and release boundary:

- `index.html`
- `editor.js`
- `editor.wasm`
- `enable-threads.js`
- `donner_icon.svg`

Additional files require an explicit artifact review. Reject accidental source maps, build logs,
test output, caches, credentials, repository metadata, local certificates, and unrelated Bazel
outputs.

The site requires:

- HTTPS outside the localhost secure-context exception.
- Browser WebAssembly, WebGPU, service workers, and pthread support.
- `SharedArrayBuffer` and cross-origin isolation.
- `Content-Type: application/wasm` for `editor.wasm` and correct JavaScript, HTML, and SVG MIME
  types with `X-Content-Type-Options: nosniff`.
- Same-origin delivery for the shell, loader, Wasm binary, icon, and service worker unless every
  cross-origin response has compatible CORP/CORS policy.

Prefer host response headers:

```text
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

`enable-threads.js` provides a secure-origin service-worker fallback that returns COOP/COEP
headers and may require a reload before the page becomes isolated. Treat that as compatibility,
not permission to ignore host configuration. Verify first load, the isolation reload, a normal
reload, and service-worker update behavior. Deploy on a dedicated origin or deliberately scoped
path so the worker cannot control unrelated applications.

## Cache and atomicity contract

The package uses stable filenames. Never upload files in place where clients can observe a new
`index.html` with an old `editor.js` or `editor.wasm`.

- Prefer content-addressed provider versions with atomic activation.
- Use validation-friendly caching such as `no-cache` for stable URLs unless the provider gives
  each release a unique immutable URL.
- If assets are marked immutable, prove their URLs change when their bytes change.
- Keep the previous reviewed provider version and header configuration available for rollback.
- Purge or switch caches atomically with the release. Do not purge the old rollback version.

## CSP contract

Derive CSP from the exact generated site and observed browser behavior. The current shell contains
inline script and style, creates the loader dynamically, compiles WebAssembly, and starts pthread
workers. A policy may need hashes or nonces, `wasm-unsafe-eval`, and a narrowly scoped `worker-src`
that permits the runtime's worker mechanism. Do not paste a generic permissive policy or add
`unsafe-eval` without evidence.

Start with report-only policy when changing CSP. Promote to enforcement only after the headed
Geode suite and parity interactions pass with zero unexplained violations.

## Candidate record

Bind these fields before promotion:

- Source revision and branch or tag.
- Dependency lockfile digest.
- CI workflow run and successful Geode plus TinySkia browser-test evidence.
- Bazel configuration and target.
- GitHub artifact digest and per-file SHA-256 manifest.
- Security review result and unresolved finding dispositions.
- Static-host provider version, origin/path, access mode, response-header configuration, and cache
  policy.
- Previous reviewed artifact and configuration selected for rollback.

Do not approve a candidate when any field points at a different revision or artifact.

## Security review

Review the exact candidate and deployment configuration. At minimum cover:

- Threat model: untrusted SVG/XML/CSS input, resource references, parser and renderer memory
  safety, browser-origin boundaries, service-worker scope, and denial-of-service paths.
- Exposed interfaces: static routes only, no accidental directory listing, source map, repository
  metadata, diagnostics endpoint, write API, secret, or private hostname.
- Authentication and authorization: owner-only by default when public access was not requested;
  verify the effective policy before deploying.
- Input and resource limits: Wasm maximum memory, worker count, pathological SVG behavior, and
  external resource loading.
- Supply chain: reviewed source revision, dependency lock state, pinned workflow actions, build
  provenance, artifact hashes, and no deployment-time dependency substitution.
- Secrets and privacy: static bytes and generated JavaScript contain no tokens, credentials,
  private paths, user data, or private infrastructure references.
- Analysis evidence: relevant unit/integration tests, `bazel test //...`, sanitizers, fuzzing,
  CodeQL/static analysis, and headed browser tests are current for the candidate.
- CI/CD permissions: least-privilege artifact production and deployment credentials; untrusted
  pull requests cannot publish.
- Negative exposure tests: unknown files return 404, directory listing is disabled, headers apply
  to errors where relevant, and public access matches the operator's request.
- Rollback: the retained prior artifact and compatible configuration are known by digest and can
  be reactivated without rebuilding.

Critical and high findings block deployment. Lower findings need an explicit disposition, owner,
and follow-up.

## Post-deployment checks

Check the exact deployed URL:

1. `index.html` is HTTPS and returns the intended access policy.
2. `editor.wasm` returns HTTP 200 and `application/wasm`.
3. The page reports `window.isSecureContext === true`.
4. After any isolation reload, `window.crossOriginIsolated === true` and
   `typeof SharedArrayBuffer !== "undefined"`.
5. `navigator.gpu` exists and Geode acquires an adapter without validation errors.
6. The loading status clears and the uninstrumented canvas contains visible editor pixels.
7. Selection, Inspector/source focus, zoom, drag, DOM writeback, overlays, and thumbnails work.
8. A normal reload does not loop service-worker registration or mix release files.
9. Unknown and sensitive-looking paths return 404 without directory listings.

## Failure signatures

| Symptom                                    | First check                                                                |
| ------------------------------------------ | -------------------------------------------------------------------------- |
| Threaded WebAssembly unavailable           | HTTPS, COOP/COEP, service-worker scope, then isolation reload              |
| `editor.wasm` compile or streaming error   | MIME type, status code, content encoding, and release-file match           |
| Loading clears but canvas is black         | Browser animation-frame presentation, not diagnostic readback              |
| Geode unavailable                          | Secure context, `navigator.gpu`, adapter errors, and browser support       |
| First load works only after manual refresh | Host COOP/COEP missing or service-worker activation flow broken            |
| Reload loops                               | Service-worker update/claim logic or mixed cached release files            |
| DOM/source edits diverge                   | Apply `donner-editor-debugging`; do not patch hosting around an editor bug |
