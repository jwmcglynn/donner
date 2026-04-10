"""Donner license aggregation rules.

Builds on `@rules_license//rules:license.bzl` to produce an aggregated NOTICE
text file (plus a JSON manifest) for embedding in applications.

Unlike the upstream `gather_licenses_info` aspect, which threads license info
through the whole dependency graph and requires every target to declare
`applicable_licenses`, `donner_notice_file` takes an *explicit* list of
`license()` labels. This trades automatic discovery for control: the list of
licenses is maintained manually in `//third_party/licenses:BUILD.bazel` and
kept in sync with the build variants surfaced by the build report.

Two outputs are produced per rule instance:

  * `<name>.txt` — concatenated human-readable NOTICE suitable for embedding
    into an application (e.g. via `cc_embed_data` or a resource loader).
  * `<name>.json` — machine-readable manifest listing each dependency's
    package metadata, SPDX kinds and workspace-relative license text path.
    Consumed by `tools/generate_build_report.py` to annotate dependencies.
"""

load("@rules_license//rules:providers.bzl", "LicenseInfo")

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

def _donner_notice_file_impl(ctx):
    entries = []
    license_text_files = []
    for target in ctx.attr.licenses:
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
            struct(variant = ctx.attr.variant, licenses = entries),
            indent = "  ",
        ),
    )

    notice = ctx.actions.declare_file(ctx.label.name + ".txt")
    args = ctx.actions.args()
    args.add(manifest)
    args.add(notice)
    for f in license_text_files:
        # Pass the short_path→actual path mapping so the renderer can locate
        # each license text file inside the action sandbox.
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
