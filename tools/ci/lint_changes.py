"""Select changed-file lint checks from immutable Git trees on hosted CI."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import quote

from build_defs.check_gpu_routing import checked_path as gpu_routing_path
from build_defs.check_text_tier_defines import is_governed as text_tier_governed
from tools.rust_boundary.rust_scopes import is_build_graph_path, is_rust_source_path

CPP = (".cc", ".h", ".cpp", ".hpp", ".mm")
JS = (".js", ".jsx", ".es6", ".mjs")
ZERO = "0" * 40


def git(*args):
    return subprocess.check_output(["git", *args])


def sha(value):
    if not isinstance(value, str) or not re.fullmatch("[0-9a-f]{40}", value):
        raise ValueError("Expected a full commit SHA")
    return value


def ensure_commit(commit):
    sha(commit)
    if subprocess.run(["git", "cat-file", "-e", commit + "^{commit}"], stderr=subprocess.DEVNULL).returncode:
        subprocess.run(["git", "fetch", "--no-tags", "--depth=1", "origin", commit], check=True)
    return commit


def empty_tree():
    return subprocess.check_output(["git", "hash-object", "-w", "-t", "tree", "--stdin"], input=b"").decode().strip()


def branch_fork_point(default, head):
    endpoint = "repos/{owner}/{repo}/compare/" + quote(default, safe="") + "..." + head
    response = subprocess.run(["gh", "api", endpoint], capture_output=True, text=True)
    if response.returncode:
        try:
            message = json.loads(response.stdout).get("message", "")
        except ValueError:
            message = ""
        if message.lower().startswith("no common ancestor between "):
            return empty_tree()
        response.check_returncode()
    return ensure_commit(sha(json.loads(response.stdout)["merge_base_commit"]["sha"]))


def pull_request_base(merge, pr_head):
    """Returns the first parent of GitHub's test merge of `pr_head`.

    GitHub merges the head into the base branch tip of when the run is created. The payload's
    `base.sha` is recorded earlier and lags that tip once the base branch advances, so it is never
    the comparison base. The second parent proves the checkout merges this event's head: a checkout
    of the head itself or of any other commit would lint the wrong set of files. The first parent
    is GitHub's choice, not pull request content, and it is not compared with the base branch: the
    depth-two checkout cannot prove ancestry without fetching history, and the branch can advance
    again before this step runs.
    """
    parents = git("show", "-s", "--format=%P", merge).decode().split()
    if len(parents) != 2 or parents[1] != pr_head:
        raise ValueError(f"Checkout {merge} is not a merge of pull request head {pr_head} "
                         f"(parents: {' '.join(parents) or 'none'})")
    return sha(parents[0])


def base_tree(event_name, event, head):
    checkout = git("rev-parse", "HEAD").decode().strip()
    if checkout != sha(head):
        raise ValueError(f"Checkout {checkout} is not the event commit {head}")
    if event_name == "pull_request":
        return pull_request_base(head, sha(event["pull_request"]["head"]["sha"]))
    if event_name != "push" or sha(event["after"]) != head:
        raise ValueError("Expected a matching push or pull request event")
    before = sha(event["before"])
    if before != ZERO:
        return ensure_commit(before)
    default = event["repository"]["default_branch"]
    if event["ref"] == "refs/heads/" + default:
        return empty_tree()
    return branch_fork_point(default, head)


def changed_paths(base):
    data = git("diff", "--no-renames", "--name-only", "-z", sha(base), "HEAD", "--")
    return sorted(os.fsdecode(p) for p in data.split(b"\0") if p)


def graph_input(path):
    name = path.rsplit("/", 1)[-1]
    return (name in ("BUILD", "BUILD.bazel", "WORKSPACE", "WORKSPACE.bazel", "MODULE.bazel", "MODULE.bazel.lock",
                     ".bazelignore", ".bazelrc", ".bazelversion") or path.endswith((".bzl", ".BUILD", ".BUILD.bazel")))


def fuzz_input(path):
    return graph_input(path) or path in (".github/workflows/fuzz.yml", "tools/ci/fuzz_lane_coverage.py")


def requires_bazel(paths):
    return any(fuzz_input(p) or (p.endswith(CPP) and not p.startswith("third_party/") and Path(p).is_file())
               or p in (".clang-format", "build_defs/check_banned_patterns.py", "tools/lint.sh", "tools/lint_braces_tests.py", "tools/format/clang_format.sh") for p in paths)


def batches(paths):
    batch, size = [], 0
    for path in paths:
        length = len(os.fsencode(path)) + 1
        if batch and size + length > 65536:
            yield batch
            batch, size = [], 0
        batch.append(path)
        size += length
    if batch:
        yield batch


def file_commands(name, args, paths):
    for group in batches(paths):
        yield name, [*args, *group]


def commands(paths, base):
    if not paths:
        return
    tracked = {os.fsdecode(p) for p in git("ls-files", "-z").split(b"\0") if p}
    existing = [p for p in paths if p in tracked]
    regular = [p for p in existing if Path(p).is_file() and not Path(p).is_symlink() and not p.startswith("third_party/")]
    py = sys.executable
    if "tools/check_private_details.py" in paths:
        yield "Check tracked files against changed privacy rules", [py, "tools/check_private_details.py"]
    else:
        yield from file_commands("Check changed tracked files for private details",
                                 [py, "tools/check_private_details.py", "--"], existing)
    yield from source_commands(paths, regular, base)
    yield from file_commands("Check changed Python", [py, "tools/check_python_quality.py", "--base-ref", base, "--"],
                             [p for p in existing if p.endswith(".py") and Path(p).is_file() and not Path(p).is_symlink()])
    if set(paths) - set(existing) or "build_defs/check_banned_patterns.py" in paths:
        yield "Check retained source exemptions", [py, "build_defs/check_banned_patterns.py", "--allowlist-only"]
    yield from relationship_commands(paths)


def tracked_cpp():
    paths = (os.fsdecode(p) for p in git("ls-files", "-z").split(b"\0") if p)
    return [p for p in paths if p.endswith(CPP) and not p.startswith("third_party/")
            and Path(p).is_file() and not Path(p).is_symlink()]


def source_commands(paths, regular, base):
    policy = {"build_defs/check_banned_patterns.py", ".clang-format", "tools/format/clang_format.sh"}
    if policy.intersection(paths):
        cpp = tracked_cpp()
        if "build_defs/check_banned_patterns.py" in paths:
            banned = [p for p in cpp if p.startswith(("donner/", "tools/", "examples/"))
                      and Path(p).name not in ("render_test.cc", "wasm_reproducer.cc")]
            yield from file_commands("Check sources against changed banned patterns",
                                     [sys.executable, "build_defs/check_banned_patterns.py", "--"], banned)
        if ".clang-format" in paths or "tools/format/clang_format.sh" in paths:
            yield from file_commands("Check changed formatting policy", ["tools/format/clang_format.sh",
                                     "--dry-run", "--Werror", "--style=file", "--"], cpp)
    yield from file_commands("Check changed C++ source and formatting",
                             ["tools/lint.sh", "--changed-cpp", base], [p for p in regular if p.endswith(CPP)])
    if set(paths) & (policy | {"tools/lint.sh", "tools/lint_braces_tests.py"}):
        yield "Check brace lint regression", [sys.executable, "tools/lint_braces_tests.py"]


def relationship_commands(paths):
    py = sys.executable
    changed = set(paths)
    docs = [p for p in paths if p.startswith("docs/design_docs/")]
    for label, script in [("provenance", "tools/check_design_doc_provenance.py"),
                          ("index", "tools/check_design_doc_index.py")]:
        title = "Check design document " + label
        if script in changed:
            yield title, [py, script, "--"]
        else:
            yield from file_commands(title, [py, script, "--"], docs)
    if any(p.endswith(JS) or p in ("tools/check_js_lint_coverage.py", "donner/editor/wasm/tests/BUILD.bazel") for p in paths):
        yield "Check JavaScript lint coverage", [py, "tools/check_js_lint_coverage.py"]
    yield from rust_commands(paths)
    if any(fuzz_input(p) for p in paths):
        yield "Check fuzzer soak coverage", [py, "tools/ci/fuzz_lane_coverage.py"]
    yield from gpu_commands(paths)
    yield from text_tier_commands(paths)
    if any(p.endswith(".bzl") or p == "build_defs/check_variant_transitions.py" for p in paths):
        yield "Check variant transitions", [sys.executable, "build_defs/check_variant_transitions.py"]


def gpu_commands(paths):
    py = sys.executable
    changed = set(paths)
    builds = [p for p in paths if Path(p).name in ("BUILD", "BUILD.bazel") and Path(p).is_file() and not Path(p).is_symlink()]
    if "build_defs/check_local_gpu_tags.py" in changed:
        yield "Check GPU scheduling policy", [py, "build_defs/check_local_gpu_tags.py"]
    else:
        yield from file_commands("Check changed GPU scheduling tags", [py, "build_defs/check_local_gpu_tags.py"], builds)
    # The routing rules relate files to each other (a platform, the config that selects it), so a
    # change to any routing file rechecks the whole tracked set.
    if "build_defs/check_gpu_routing.py" in changed or any(gpu_routing_path(p) for p in paths):
        yield "Check remote GPU routing", [py, "build_defs/check_gpu_routing.py"]


def text_tier_commands(paths):
    py = sys.executable
    if "build_defs/check_text_tier_defines.py" in paths:
        yield "Check text tier macro policy", [py, "build_defs/check_text_tier_defines.py"]
        return
    governed = [p for p in paths if text_tier_governed(p) and Path(p).is_file() and not Path(p).is_symlink()]
    yield from file_commands("Check changed text tier macros", [py, "build_defs/check_text_tier_defines.py"],
                             governed)


def rust_commands(paths):
    py = sys.executable
    changed = set(paths)
    rust = [p for p in paths if is_rust_source_path(p) or is_build_graph_path(p)]
    rust_command = [py, "tools/rust_boundary/check_no_rust_dependencies.py", "--blocking", "default"]
    if changed & {"tools/rust_boundary/rust_allowlist.json", "tools/rust_boundary/check_no_rust_dependencies.py",
                  "tools/rust_boundary/rust_scopes.py"}:
        yield "Check Rust boundary policy", rust_command
    else:
        yield from file_commands("Check changed Rust boundary inputs", [*rust_command, "--"], rust)


def run_checks(phases):
    for title, command in phases:
        # A privacy finding may be in a filename. Let the checker report it without echoing argv.
        status = subprocess.run(["bash", "-c", 'source tools/ci/lane.sh; lane_phase "$@"',
                                 "lint", title, *command]).returncode
        if status:
            raise SystemExit(status)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("base", "run"))
    parser.add_argument("base", nargs="?")
    args = parser.parse_args()
    if args.mode == "base":
        event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text())
        base = base_tree(os.environ["GITHUB_EVENT_NAME"], event, os.environ["GITHUB_SHA"])
        paths = changed_paths(base)
        with open(os.environ["GITHUB_OUTPUT"], "a") as out:
            print("base=" + base, file=out)
            print("needs_bazel=" + str(requires_bazel(paths)).lower(), file=out)
        print(f"{len(paths)} changed paths against {base}")
        return
    paths = changed_paths(args.base)
    run_checks(commands(paths, args.base))


if __name__ == "__main__":
    main()
