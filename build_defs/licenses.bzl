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

`donner_closure_notice_file` is the strict form for a shipped product. It walks
the product's configured closure (dependencies, headers, sources, embedded
assets, data, aliases and linker inputs after transitions, but not build tools),
selects the catalog entry of every third-party component it reaches, and also
writes a coverage report that names any component with neither a catalog entry
nor reviewed exemptions for all of its labels, and any linker option that links
a library or file without a reviewed exemption; `tools/notice_coverage_tests.py`
fails on either.

Two outputs are produced per rule instance (`donner_closure_notice_file` adds a
third, its `<name>.coverage.json` report):

  * `<name>.txt` - concatenated human-readable NOTICE suitable for embedding
    into an application (e.g. via `cc_embed_data` or a resource loader).
  * `<name>.json` - machine-readable manifest listing each dependency's
    package metadata, SPDX kinds and workspace-relative license text path.
    Consumed by `tools/generate_build_report.py` to annotate dependencies.
"""

load("@rules_license//rules:providers.bzl", "LicenseInfo")
load("//build_defs:dep_audit.bzl", "ShippedClosureInfo", "shipped_closure_aspect")

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
    # `+non_bcr_deps+harfbuzz` (module extension), so take the last non-empty
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

def _closure_component(label):
    """Returns the third-party component that a configured closure label belongs to.

    An external repository is one component, named like the license catalog's
    repository keys. A `//third_party/<name>` subpackage is the vendored
    component `<name>`, and so is a file under `third_party/<name>/` that has no
    BUILD file of its own and therefore belongs to the `//third_party` package.
    Other files and aliases directly in `//third_party` are build overlays and
    patches for external repositories, which the closure reaches in their own
    right, so they are not components. Other first-party labels return None.
    """
    repo = _normalize_repo_name(label.workspace_name)
    if repo:
        return repo.lower()
    if label.package.startswith("third_party/"):
        return label.package.split("/")[1].lower()
    if label.package == "third_party" and "/" in label.name:
        return label.name.split("/")[0].lower()
    return None

def _links_a_library(option):
    """Whether a linker option names a library or file, which the label walk cannot see."""
    if option.startswith(("-l", "-L", "-Wl,-l", "-Wl,-L")) or not option.startswith("-"):
        return True
    return "/" in option or option.endswith((".a", ".o", ".so", ".dylib", ".lib")) or ".so." in option

def _donner_closure_notice_file_impl(ctx):
    target = ctx.attr.target
    closure = target[ShippedClosureInfo]

    components = {}
    for label in closure.labels.to_list():
        component = _closure_component(label)
        if component:
            components.setdefault(component, []).append(str(label))

    selected = []
    covered = {}
    for license_target in ctx.attr.available_licenses:
        info = license_target[LicenseInfo]
        matches = [key for key in _license_candidate_keys(info).to_list() if key in components]
        if matches:
            selected.append(license_target)
            for key in matches:
                covered.setdefault(key, []).append(str(info.label))

    # An exemption names one label, so a component is exempt only while every label of it that
    # the closure reaches has a reviewed reason.
    exemptions = {}
    for label, reason in ctx.attr.exempt_labels.items():
        if not reason.strip():
            fail("exemption for {} needs a reason".format(label))
        exemptions[str(Label(label))] = reason

    exempt = {}
    uncovered = {}
    for component, labels in components.items():
        if component in covered:
            continue
        unexempt = [label for label in labels if label not in exemptions]
        if unexempt:
            # Name the shortest labels first, which are usually the targets rather than files.
            uncovered[component] = sorted(unexempt, key = lambda label: (len(label), label))[:5]
        else:
            exempt[component] = {label: exemptions[label] for label in sorted(labels)}
    reached = {label: True for labels in components.values() for label in labels}

    # Options such as `-lz` or a library path link code that no label in the closure names. Each
    # such option needs a reviewed exemption.
    for option, reason in ctx.attr.exempt_linkopts.items():
        if not reason.strip():
            fail("exemption for linker option {} needs a reason".format(option))
    unexempt_linkopts = []
    for entry in closure.linkopts.to_list():
        option = entry.partition(" ")[2]
        if _links_a_library(option) and option not in ctx.attr.exempt_linkopts:
            unexempt_linkopts.append(entry)

    coverage = ctx.actions.declare_file(ctx.label.name + ".coverage.json")
    ctx.actions.write(
        output = coverage,
        content = json.encode_indent(
            struct(
                target = str(target.label),
                variant = ctx.attr.variant,
                components = sorted(components.keys()),
                component_labels = {key: sorted(value) for key, value in components.items()},
                covered = {key: sorted(value) for key, value in covered.items()},
                exempt = exempt,
                uncovered = uncovered,
                # Exemptions are configuration dependent, so one this build does not reach is
                # reported rather than failed.
                unreached_exemptions = sorted([label for label in exemptions if label not in reached]),
                linkopts = sorted(closure.linkopts.to_list()),
                unexempt_linkopts = sorted(unexempt_linkopts),
            ),
            indent = "  ",
        ) + "\n",
    )

    providers = _write_notice_outputs(ctx, ctx.attr.variant, selected)
    default_info, output_groups = providers
    return [
        DefaultInfo(files = depset([coverage], transitive = [default_info.files])),
        OutputGroupInfo(
            notice = output_groups.notice,
            manifest = output_groups.manifest,
            coverage = depset([coverage]),
        ),
    ]

donner_closure_notice_file = rule(
    implementation = _donner_closure_notice_file_impl,
    doc = ("Aggregates the licenses of every third-party component in a product's " +
           "configured closure into a NOTICE text file and JSON manifest, and writes a " +
           "coverage report naming components with no catalog entry or exemption."),
    attrs = {
        "variant": attr.string(
            doc = "Human-readable name of the product this notice covers.",
            mandatory = True,
        ),
        "target": attr.label(
            doc = "Shipped product whose configured closure is attributed.",
            mandatory = True,
            aspects = [shipped_closure_aspect],
        ),
        "available_licenses": attr.label_list(
            doc = "Catalog of license() targets that may match the closure's components.",
            providers = [LicenseInfo],
            mandatory = True,
        ),
        "exempt_linkopts": attr.string_dict(
            doc = ("Linker options that name a library or file but put no third-party code in " +
                   "the product that needs attribution, each with the reason."),
        ),
        "exempt_labels": attr.string_dict(
            doc = ("Labels that put no third-party code or data in the product, each with " +
                   "the reason. A component is exempt only when all its reached labels are."),
        ),
        "_render": attr.label(
            default = "//tools:render_notice",
            executable = True,
            cfg = "exec",
        ),
    },
)
