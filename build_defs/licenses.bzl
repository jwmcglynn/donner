"""Donner license aggregation rules.

Builds on `@rules_license//rules:license.bzl` to produce an aggregated NOTICE
text file (plus a JSON manifest) for embedding in applications.

Unlike the upstream `gather_licenses_info` aspect, which threads license info
through the whole dependency graph and requires every target to declare
`applicable_licenses`, `donner_notice_file` takes an *explicit* list of
`license()` labels. This trades automatic discovery for control: the list of
licenses is maintained manually in `//third_party/licenses:BUILD.bazel` and
kept in sync with the build variants surfaced by the build report.

For targets that need their NOTICE to follow the actual dependency graph,
`donner_notice_file_auto` walks the target's `deps` graph with a lightweight
aspect, collects normalized dependency keys (external repo names plus local
`//third_party/...` package names), and selects matching `license()` rules
from a provided catalog.

Two outputs are produced per rule instance:

  * `<name>.txt` — concatenated human-readable NOTICE suitable for embedding
    into an application (e.g. via `cc_embed_data` or a resource loader).
  * `<name>.json` — machine-readable manifest listing each dependency's
    package metadata, SPDX kinds and workspace-relative license text path.
    Consumed by `tools/generate_build_report.py` to annotate dependencies.
"""

load("@rules_license//rules:providers.bzl", "LicenseInfo")

_CollectedLicenseKeysInfo = provider(fields = ["keys"])

def _normalize_repo_name(repo):
    if not repo:
        return ""
    parts = [part for part in repo.split("+") if part]
    return parts[-1] if parts else repo

def _candidate_keys_from_label(label):
    keys = []

    repo = _normalize_repo_name(label.workspace_name)
    if repo:
        keys.append(repo.lower())
    elif label.package.startswith("third_party/"):
        keys.append(label.package[len("third_party/"):].lower())

    if label.package:
        package_leaf = label.package.split("/")[-1]
        if package_leaf:
            keys.append(package_leaf.lower())

    if label.name:
        keys.append(label.name.lower())

    return depset(keys)

def _collect_license_keys_aspect_impl(target, ctx):
    transitive = []
    for dep in getattr(ctx.rule.attr, "deps", []):
        if _CollectedLicenseKeysInfo in dep:
            transitive.append(dep[_CollectedLicenseKeysInfo].keys)

    return [ _CollectedLicenseKeysInfo(keys = depset(
        transitive = [_candidate_keys_from_label(target.label)] + transitive,
    )) ]

_collect_license_keys_aspect = aspect(
    implementation = _collect_license_keys_aspect_impl,
    attr_aspects = ["deps"],
)

def _fallback_package_name(label):
    # Prefer the external repo name (e.g. `libpng`) for licenses declared in
    # upstream BCR overlays that don't set `package_name`. Bazel's canonical
    # workspace names take forms like `libpng+` (direct BCR module) or
    # `+non_bcr_deps+skia` (module extension), so take the last non-empty
    # "+"-delimited component to get the human-readable repo name.
    repo = label.workspace_name
    if repo:
        parts = [part for part in repo.split("+") if part]
        if parts:
            return parts[-1]
    if label.package:
        return label.package
    return label.name

def _license_candidate_keys(info):
    keys = []

    package_name = info.package_name or _fallback_package_name(info.label)
    if package_name:
        lowered = package_name.lower()
        keys.append(lowered)
        keys.append(lowered.replace(" ", "-"))

    if info.label.name:
        keys.append(info.label.name.lower())

    repo = _normalize_repo_name(info.label.workspace_name)
    if repo:
        keys.append(repo.lower())

    return depset(keys)

def _write_notice_outputs(ctx, variant, targets):
    entries = []
    license_text_files = []
    for target in targets:
        info = target[LicenseInfo]
        license_text_files.append(info.license_text)
        entries.append(struct(
            label = str(info.label),
            package_name = info.package_name or _fallback_package_name(info.label),
            package_url = info.package_url,
            package_version = info.package_version,
            copyright_notice = info.copyright_notice,
            license_kinds = [k.name for k in info.license_kinds],
            license_text = info.license_text.short_path,
        ))

    manifest = ctx.actions.declare_file(ctx.label.name + ".json")
    ctx.actions.write(
        output = manifest,
        content = json.encode_indent(
            struct(variant = variant, licenses = entries),
            indent = "  ",
        ),
    )

    notice = ctx.actions.declare_file(ctx.label.name + ".txt")
    args = ctx.actions.args()
    args.add(manifest)
    args.add(notice)
    for f in license_text_files:
        args.add("--license-text")
        args.add(f.short_path + "=" + f.path)

    ctx.actions.run(
        executable = ctx.executable._render,
        inputs = [manifest] + license_text_files,
        outputs = [notice],
        arguments = [args],
        mnemonic = "DonnerNotice",
        progress_message = "Aggregating license notices for %s" % ctx.label,
    )

    return [
        DefaultInfo(files = depset([notice, manifest])),
        OutputGroupInfo(
            notice = depset([notice]),
            manifest = depset([manifest]),
        ),
    ]

def _donner_notice_file_impl(ctx):
    return _write_notice_outputs(ctx, ctx.attr.variant, ctx.attr.licenses)

def _donner_notice_file_auto_impl(ctx):
    dep_keys = {}
    for key in ctx.attr.target[_CollectedLicenseKeysInfo].keys.to_list():
        if key and not key.startswith("@"):
            dep_keys[key] = True

    selected = []
    seen = {}
    for target in ctx.attr.available_licenses:
        info = target[LicenseInfo]
        candidate_keys = _license_candidate_keys(info).to_list()
        matched = False
        for key in candidate_keys:
            if key in dep_keys:
                matched = True
                break

        if matched:
            label = str(info.label)
            if label not in seen:
                seen[label] = True
                selected.append(target)

    return _write_notice_outputs(ctx, ctx.attr.variant, selected)

donner_notice_file = rule(
    implementation = _donner_notice_file_impl,
    doc = ("Aggregates an explicit list of `license()` targets into a NOTICE " +
           "text file suitable for embedding, plus a JSON manifest consumed " +
           "by tooling (e.g. the build report)."),
    attrs = {
        "variant": attr.string(
            doc = "Human-readable name of the build variant this notice covers.",
            mandatory = True,
        ),
        "licenses": attr.label_list(
            doc = "`license()` targets to aggregate. Each must provide LicenseInfo.",
            providers = [LicenseInfo],
            mandatory = True,
        ),
        "_render": attr.label(
            default = "//tools:render_notice",
            executable = True,
            cfg = "exec",
        ),
    },
)

donner_notice_file_auto = rule(
    implementation = _donner_notice_file_auto_impl,
    doc = ("Aggregates licenses detected from a target's actual `deps` graph " +
           "into a NOTICE text file suitable for embedding, plus a JSON manifest."),
    attrs = {
        "variant": attr.string(
            doc = "Human-readable name of the build variant this notice covers.",
            mandatory = True,
        ),
        "target": attr.label(
            doc = "Target whose `deps` graph should be inspected for license selection.",
            mandatory = True,
            aspects = [_collect_license_keys_aspect],
        ),
        "available_licenses": attr.label_list(
            doc = "Catalog of license() targets that may match the inspected dependency graph.",
            providers = [LicenseInfo],
            mandatory = True,
        ),
        "_render": attr.label(
            default = "//tools:render_notice",
            executable = True,
            cfg = "exec",
        ),
    },
)
