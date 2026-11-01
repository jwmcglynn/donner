#!/usr/bin/env python3
"""Prepare bounded CMake compiler/source caches without reusing build directories."""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request
import uuid
import xml.parsers.expat as expat

VERSION = "4.14.2"
RELEASE = "https://github.com/ccache/ccache/releases/download/v" + VERSION + "/"
ARTIFACTS = {
    ("Darwin", "arm64"): ("darwin.tar.gz", "cf240009630b605b775c7c5c7b7105a3b196c79f31af322c1557969aa28003fc"),
    ("Darwin", "x86_64"): ("darwin.tar.gz", "cf240009630b605b775c7c5c7b7105a3b196c79f31af322c1557969aa28003fc"),
    ("Linux", "aarch64"): ("linux-aarch64-musl-static.tar.xz", "b1b583cd76980b291c4a25b95d8cb2cf1637b4a58083b9c53014780c7548f8d0"),
    ("Linux", "x86_64"): ("linux-x86_64-musl-static.tar.xz", "b20ff6415151226ac5a10dfd6839700c694bd67d1871c8fddb266d366ff25bfb"),
}
MAX_ARCHIVE = 64 * 1024 * 1024
DOWNLOAD_BUDGET = 1024 * 1024 * 1024
ALLOWED_HOSTS = {"github.com", "codeload.github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com"}
FLAG_VARIABLES = {"CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "SDKROOT", "DEVELOPER_DIR", "MACOSX_DEPLOYMENT_TARGET", "DONNER_CI_XCODE_IDENTITY", "CMAKE_TOOLCHAIN_FILE", "SOURCE_DATE_EPOCH"}


class CacheError(Exception):
    pass


def digest(data):
    return hashlib.sha256(data).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def read_file(path, limit=MAX_ARCHIVE):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size > limit:
            raise CacheError("invalid cache file")
        with os.fdopen(fd, "rb", closefd=False) as source:
            data = source.read(limit + 1)
        if len(data) != info.st_size:
            raise CacheError("cache file changed while reading")
        return data
    finally:
        os.close(fd)


def write_new(path, data, mode=0o600):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, mode)
    with os.fdopen(fd, "wb") as output:
        output.write(data)
    os.chmod(path, mode)


def command(args, env=None):
    return subprocess.check_output(args, env=env, stderr=subprocess.PIPE, text=True, timeout=60).strip()


def validate_url(url):
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != "https" or parsed.hostname not in ALLOWED_HOSTS or parsed.username or parsed.password:
        raise CacheError("cache download URL is not an approved HTTPS origin")


class SafeRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        validate_url(newurl)
        return super().redirect_request(request, fp, code, message, headers, newurl)


def download(url):
    validate_url(url)
    with urllib.request.build_opener(SafeRedirect()).open(url, timeout=60) as response:
        data = response.read(MAX_ARCHIVE + 1)
    if len(data) > MAX_ARCHIVE:
        raise CacheError("cache download exceeds the size bound")
    return data


def install_tool(directory):
    key = (platform.system(), platform.machine())
    if key not in ARTIFACTS:
        raise CacheError("no pinned compiler cache for this platform")
    suffix, expected = ARTIFACTS[key]
    data = download(RELEASE + "ccache-" + VERSION + "-" + suffix)
    if digest(data) != expected:
        raise CacheError("compiler cache archive checksum mismatch")
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        members = [m for m in archive if m.name.endswith("/ccache")]
        if len(members) != 1 or not members[0].isfile() or members[0].size > MAX_ARCHIVE:
            raise CacheError("invalid compiler cache executable archive")
        binary = archive.extractfile(members[0]).read()
    path = directory / "ccache"
    write_new(path, binary, 0o700)
    return str(path)


def relevant_environment(env):
    return {k: v for k, v in env.items() if k in FLAG_VARIABLES or k.startswith("NIX_")}


def compiler_identity(value):
    words = shlex.split(value)
    path = shutil.which(words[0]) if words else None
    if path is None:
        raise CacheError("compiler is unavailable")
    actual = Path(path).resolve(strict=True)
    return {"command": words, "path": str(actual), "content": digest(actual.read_bytes()),
            "version": command([path, *words[1:], "--version"])}


def sdk_identity():
    if platform.system() != "Darwin":
        return {}
    sdk = Path(command(["xcrun", "--show-sdk-path"]))
    compiler = Path(command(["xcrun", "--find", "clang++"]))
    return {"sdk": str(sdk), "settings": digest((sdk / "SDKSettings.json").read_bytes()),
            "compiler": digest(compiler.read_bytes())}


def fingerprint(source, env):
    value = {"schema": 1, "platform": platform.system(), "architecture": platform.machine(),
             "workspace": str(source.resolve()), "flags_sha256": digest(canonical(relevant_environment(env))), "sdk": sdk_identity(),
             "c": compiler_identity(env.get("CC", "cc")), "cxx": compiler_identity(env.get("CXX", "c++")),
             "configuration": {"generator": "Ninja", "renderer": "tiny_skia"}}
    return value, digest(canonical(value))


def check_directory(path):
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode):
        raise CacheError("cache directory is not a real directory")


def cache_root(work):
    configured = os.environ.get("DONNER_CMAKE_CACHE_ROOT")
    hosted = os.environ.get("RUNNER_ENVIRONMENT") == "github-hosted"
    if configured:
        root = Path(configured)
    else:
        default = Path.home() / ("Library/Caches" if platform.system() == "Darwin" else ".cache")
        root = Path(os.environ.get("XDG_CACHE_HOME", str(default))) / "donner-cmake-v1"
    try:
        root.mkdir(mode=0o700, parents=True, exist_ok=True)
        check_directory(root)
        if root.stat().st_mode & 0o022:
            raise CacheError("cache root is writable by another user")
        for name in ("compiler", "downloads"):
            (root / name).mkdir(mode=0o700, exist_ok=True)
            check_directory(root / name)
        probe = root / "compiler" / ("probe-" + work.name)
        write_new(probe, b"probe")
        probe.unlink()
        return root, not hosted and (bool(configured) or platform.system() == "Darwin"
                                      or os.environ.get("GITHUB_ACTIONS") != "true")
    except (OSError, CacheError):
        print("::warning::Persistent CMake cache unavailable; continuing with a job-local cache.")
        root = work / "local-cache"
        root.mkdir()
        (root / "compiler").mkdir()
        (root / "downloads").mkdir()
        return root, False


# Only these generated dependencies have been qualified without Git metadata/submodules.
SOURCE_REPOSITORIES = {
    "googletest": "https://github.com/google/googletest.git",
    "nlohmann_json": "https://github.com/nlohmann/json.git",
    "zlib": "https://github.com/madler/zlib.git",
    "rules_cc": "https://github.com/bazelbuild/rules_cc.git",
    "pixelmatch-cpp17": "https://github.com/jwmcglynn/pixelmatch-cpp17.git",
    "absl": "https://github.com/abseil/abseil-cpp.git",
    "entt": "https://github.com/skypjack/entt.git",
}


def source_declarations(source):
    text = (source / "CMakeLists.txt").read_text()
    for match in re.finditer(r"FetchContent_Declare\(\s*([A-Za-z0-9_-]+)\s+(.*?)\n\)", text, re.DOTALL):
        name, body = match.groups()
        git = re.fullmatch(r"\s*GIT_REPOSITORY\s+(https://github\.com/[\w.-]+/[\w.-]+)\s+GIT_TAG\s+([0-9a-f]{40})\s*", body)
        if git and SOURCE_REPOSITORIES.get(name) == git[1]:
            yield name, {"kind": "git", "repository": git[1], "revision": git[2]}
        elif name == "brotli":
            entry = re.fullmatch(
                r'\s*URL\s+(https://github\.com/google/brotli/archive/refs/tags/[^\s]+)'
                r'\s+URL_HASH\s+SHA256=([0-9a-f]{64})'
                r'(?:\s+SOURCE_DIR\s+"[^"\n]+"\s+BINARY_DIR\s+"[^"\n]+")?\s*', body)
            if entry:
                yield name, {"kind": "archive", "url": entry[1], "sha256": entry[2]}


def source_descriptor(declaration):
    value = dict(declaration)
    if value["kind"] == "git":
        value["commit"] = value["revision"]
        if not re.fullmatch(r"[0-9a-f]{40}", value["commit"]):
            raise CacheError("dependency needs an immutable commit")
        repo = value["repository"].removeprefix("https://github.com/").removesuffix(".git")
        value["url"] = "https://codeload.github.com/" + repo + "/tar.gz/" + value["commit"]
    validate_url(value["url"])
    return value


def cached_archive(directory, descriptor):
    key = digest(canonical(descriptor))
    path, metadata = directory / (key + ".tar"), directory / (key + ".json")
    try:
        record = json.loads(read_file(metadata))
        data = read_file(path)
        if record["descriptor"] == descriptor and digest(data) == record["sha256"]:
            if descriptor.get("sha256", record["sha256"]) == record["sha256"]:
                os.utime(path, None, follow_symlinks=False)
                return data, True
    except (OSError, ValueError, KeyError, TypeError, CacheError):
        pass
    data = download(descriptor["url"])
    if descriptor.get("sha256", digest(data)) != digest(data):
        raise CacheError("dependency archive checksum mismatch")
    record = {"descriptor": descriptor, "sha256": digest(data)}
    for target, content in ((path, data), (metadata, canonical(record))):
        temporary = directory / (key + ".tmp-" + uuid.uuid4().hex)
        write_new(temporary, content, 0o666)
        temporary.replace(target)
    return data, False


def archive_members(archive):
    members = archive.getmembers()
    if len(members) > 100000 or sum(m.size for m in members) > 512 * 1024 * 1024:
        raise CacheError("dependency source exceeds extraction bounds")
    prefixes = {Path(m.name).parts[0] for m in members if Path(m.name).parts}
    if len(prefixes) != 1:
        raise CacheError("dependency archive needs one root directory")
    for member in members:
        path = Path(member.name)
        if path.is_absolute() or ".." in path.parts or not (member.isfile() or member.isdir()):
            raise CacheError("unsupported dependency archive path or link")
    return members


def extract_sources(data, destination):
    destination.mkdir()
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        members = archive_members(archive)
        for member in members:
            parts = Path(member.name).parts[1:]
            if not parts:
                continue
            target = destination.joinpath(*parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                write_new(target, archive.extractfile(member).read(), member.mode & 0o755)
                os.utime(target, (member.mtime, member.mtime))


def verify_sources(data, destination):
    check_directory(destination)
    expected = set()
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        members = archive_members(archive)
        if any(p.is_symlink() for p in destination.rglob("*")):
            raise CacheError("extracted dependency contains a link")
        for member in members:
            parts = Path(member.name).parts[1:]
            if not parts or member.isdir():
                continue
            path = destination.joinpath(*parts)
            if read_file(path) != archive.extractfile(member).read():
                raise CacheError("extracted dependency source changed")
            expected.add(path)
    paths = list(destination.rglob("*"))
    if any(p.is_symlink() for p in paths):
        raise CacheError("extracted dependency contains a link")
    observed = {p for p in paths if not p.is_dir()}
    if observed != expected:
        raise CacheError("extracted dependency source inventory changed")


def trim_downloads(directory):
    entries = []
    for path in directory.glob("*.tar"):
        if not re.fullmatch(r"[0-9a-f]{64}\.tar", path.name):
            continue
        try:
            info = path.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISREG(info.st_mode) and info.st_nlink == 1:
            entries.append((info.st_mtime, info.st_size, path))
    total = sum(row[1] for row in entries)
    for _, size, path in sorted(entries):
        if total <= DOWNLOAD_BUDGET:
            break
        path.unlink(missing_ok=True)
        path.with_suffix(".json").unlink(missing_ok=True)
        total -= size


def prepare_sources(source, directory, work):
    names = re.findall(r"FetchContent_Declare\(\s*([A-Za-z0-9_-]+)", (source / "CMakeLists.txt").read_text())
    arguments = ["-DFETCHCONTENT_SOURCE_DIR_" + name.upper() + "=" for name in names]
    hits, misses = 0, 0
    for name, declaration in source_declarations(source):
        try:
            descriptor = source_descriptor(declaration)
            data, hit = cached_archive(directory, descriptor)
            destination = work / (name + "-" + digest(data)[:20])
            if not destination.exists():
                extract_sources(data, destination)
            else:
                verify_sources(data, destination)
            arguments.append("-DFETCHCONTENT_SOURCE_DIR_" + name.upper() + "=" + str(destination))
            hits += int(hit)
            misses += int(not hit)
        except (OSError, ValueError, CacheError, tarfile.TarError, subprocess.SubprocessError):
            print("::warning::A dependency will use its normal fresh FetchContent path.")
    trim_downloads(directory)
    return arguments, hits, misses


def prepare(source, context):
    work = Path(tempfile.mkdtemp(prefix="donner-cmake-", dir=str(context.parent)))
    root, persistent = cache_root(work)
    identity, namespace = fingerprint(source, os.environ)
    try:
        tool = install_tool(work)
    except (OSError, CacheError, tarfile.TarError):
        print("::warning::Compiler cache unavailable; continuing with normal compilation.")
        tool = None
    identity_path = work / "compiler-identity.json"
    write_new(identity_path, canonical(identity))
    config = work / "ccache.conf"
    write_new(config, b"hard_link = false\nfile_clone = false\n")
    env = {"CCACHE_DIR": str(root / "compiler"), "CCACHE_CONFIGPATH": str(config),
           "CCACHE_MAXSIZE": "2GiB", "CCACHE_COMPILERCHECK": "content", "CCACHE_EXTRAFILES": str(identity_path),
           "CCACHE_SLOPPINESS": "",
           "CCACHE_UMASK": "000" if root.stat().st_uid != os.getuid() else "077",
           "CCACHE_STATSLOG": str(work / "statistics.log"), "CCACHE_TEMPDIR": str(work)}
    extra = os.environ.get("DONNER_CMAKE_CACHE_NAMESPACE", "")
    if not re.fullmatch(r"[A-Za-z0-9_-]{0,80}", extra):
        raise CacheError("invalid cache qualification namespace")
    env["CCACHE_NAMESPACE"] = namespace + ("-" + extra if extra else "")
    sources = source / "build" / ".cmake-sources"
    sources.mkdir(parents=True, exist_ok=True)
    check_directory(sources)
    arguments, hits, misses = prepare_sources(source, root / "downloads", sources)
    value = {"schema": 1, "source": str(source.resolve()), "tool": tool, "environment": env,
             "configure": arguments, "persistent": persistent, "source_hits": hits, "source_misses": misses}
    write_new(context, canonical(value))


def load_context(path):
    value = json.loads(read_file(path))
    if value.get("schema") != 1:
        raise CacheError("invalid cache context")
    return value


def run_context(context, args, configure=False):
    value = load_context(context)
    env = {k: v for k, v in os.environ.items() if not k.startswith("CCACHE_")}
    env.update(value["environment"])
    if configure:
        launcher = value["tool"] or ""
        args += ["-DCMAKE_C_COMPILER_LAUNCHER=" + launcher,
                 "-DCMAKE_CXX_COMPILER_LAUNCHER=" + launcher, *value["configure"]]
    return subprocess.call(args, env=env)


def ctest_counts(path):
    counts = {"testcase": 0, "failure": 0, "error": 0, "skipped": 0}
    parser = expat.ParserCreate()
    def start(name, attributes):
        if name in counts:
            counts[name] += 1
    def reject(*_):
        raise CacheError("CTest XML declarations are forbidden")
    parser.StartElementHandler = start
    parser.StartDoctypeDeclHandler = reject
    parser.EntityDeclHandler = reject
    parser.ExternalEntityRefHandler = lambda *_: 0
    parser.Parse(read_file(path, 4 * 1024 * 1024), True)
    return counts


def replace_receipt(path, data):
    try:
        info = path.lstat()
    except FileNotFoundError:
        info = None
    if info is not None and (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
                             or info.st_nlink != 1 or info.st_mode & 0o022):
        raise CacheError("existing receipt is not a private owned regular file")
    temporary = path.with_name(path.name + ".tmp-" + uuid.uuid4().hex)
    write_new(temporary, data)
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def statistics(context, receipt):
    value = load_context(context)
    env = {k: v for k, v in os.environ.items() if not k.startswith("CCACHE_")}
    env.update(value["environment"])
    counters = {"direct_cache_hit": 0, "preprocessed_cache_hit": 0, "cache_miss": 0}
    if value["tool"] and Path(value["environment"]["CCACHE_STATSLOG"]).is_file():
        counters = json.loads(command([value["tool"], "--print-log-stats", "--format=json"], env))
    result = {"persistent": value["persistent"] and bool(value["tool"]), "enabled": bool(value["tool"]), "source_hits": value["source_hits"], "source_misses": value["source_misses"],
              "hits": counters["direct_cache_hit"] + counters["preprocessed_cache_hit"], "misses": counters["cache_miss"]}
    result["cacheable_calls"] = result["hits"] + result["misses"]
    tests = ctest_counts(Path(value["source"]) / "build/cmake-consumer-tests.xml")
    result["tests"] = tests["testcase"]
    result["test_failures"] = tests["failure"] + tests["error"]
    result["test_skips"] = tests["skipped"]
    try:
        result["checkout_sha"] = command(["git", "rev-parse", "HEAD"])
    except subprocess.SubprocessError:
        if os.environ.get("GITHUB_ACTIONS") == "true":
            raise
        result["checkout_sha"] = "not-a-git-checkout"
    event_path = os.environ.get("GITHUB_EVENT_PATH")
    event = json.loads(Path(event_path).read_text()) if event_path else {}
    result["head_sha"] = event.get("pull_request", {}).get("head", {}).get("sha", result["checkout_sha"])
    result["run_id"] = os.environ.get("GITHUB_RUN_ID", "")
    result["run_attempt"] = os.environ.get("GITHUB_RUN_ATTEMPT", "")
    replace_receipt(receipt, canonical(result))
    print("CMake compiler cache: %d hits, %d misses; dependency sources: %d hits, %d misses" %
          (result["hits"], result["misses"], result["source_hits"], result["source_misses"]))
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as summary:
            summary.write("\nCompiler cache: %d hits, %d misses. Source cache: %d hits, %d misses.\n" %
                          (result["hits"], result["misses"], result["source_hits"], result["source_misses"]))


def qualify(receipt, role, producer=None):
    current = json.loads(read_file(receipt))
    if not (current["persistent"] and current["enabled"] and current["cacheable_calls"] > 0
            and current["tests"] == 2 and current["test_failures"] == 0 and current["test_skips"] == 0):
        raise CacheError("qualification requires persistent caching and both fresh CTest cases")
    if role == "producer":
        if current["hits"] != 0 or current["misses"] == 0:
            raise CacheError("producer namespace must start cold")
    else:
        previous = json.loads(read_file(producer))
        for key in ("checkout_sha", "head_sha", "run_id", "run_attempt"):
            if not current[key] or current[key] != previous[key]:
                raise CacheError("producer/consumer identity differs")
        if current["hits"] < current["cacheable_calls"] * 0.8 or current["source_hits"] == 0:
            raise CacheError("consumer needs at least 80 percent compiler hits and source reuse")
    print("CMake cache " + role + " qualification passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("prepare", "run", "configure", "statistics", "qualify"))
    parser.add_argument("--context", type=Path)
    parser.add_argument("--source", type=Path, default=Path.cwd())
    parser.add_argument("--receipt", type=Path)
    parser.add_argument("--role", choices=("producer", "consumer"))
    parser.add_argument("--producer", type=Path)
    args, command_args = parser.parse_known_args()
    if args.operation == "qualify":
        if command_args or args.receipt is None or args.role is None or (args.role == "consumer" and args.producer is None):
            parser.error("qualification requires receipt, role and consumer producer receipt")
        qualify(args.receipt, args.role, args.producer)
        return
    if args.context is None:
        parser.error("--context is required")
    if args.operation in ("prepare", "statistics") and command_args:
        parser.error("unexpected command arguments")
    if args.operation == "statistics" and args.receipt is None:
        parser.error("statistics requires --receipt")
    if args.operation == "prepare":
        prepare(args.source, args.context)
        return
    if args.operation == "statistics":
        statistics(args.context, args.receipt)
        return
    command_args = command_args[1:] if command_args[:1] == ["--"] else command_args
    if not command_args:
        parser.error("a command is required")
    raise SystemExit(run_context(args.context, command_args, args.operation == "configure"))


if __name__ == "__main__":
    main()
