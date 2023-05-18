import hashlib
import requests

"""
Fetches a new version of scip-clang from the github repo, and provides the .bzl snippet to paste
into defs.bzl.

Usage:
```sh
bazel run //third_party/scip-clang:fetch_new_version -- <tag>

# Example:
bazel run //third_party/scip-clang:fetch_new_version -- v0.1.1
```
"""

def _human_readable_size(size):
    """Converts a size in bytes to a human-readable size."""
    units = ["B", "KB", "MB", "GB", "TB"]
    i = 0
    while size >= 1024 and i < len(units) - 1:
        size /= 1024
        i += 1
    return f"{size:.2f} {units[i]}"

def main(tag: str):
    release_url = f"https://github.com/sourcegraph/scip-clang/releases/download/{tag}"
    all_os = ["darwin", "linux"]

    sha256sums = {}
    print(
        f"Downloading version {tag}.")
    print("The latest version can be found at https://github.com/sourcegraph/scip-clang/releases\n")

    for os in all_os:
        print(f"Downloading scip-clang-{os}")
        url = f"{release_url}/scip-clang-x86_64-{os}"
        response = requests.get(url)

        if response.status_code != 200:
            sys.stderr.write(f"Error downloading {url}: {response.status_code} - {response.reason}\n")
            continue

        hash = hashlib.sha256(response.content).hexdigest()
        sha256sums[os] = hash

        size = _human_readable_size(len(response.content))
        print(f"-> {size}, sha256sum: {hash}")

    if len(sha256sums) != len(all_os):
        sys.stderr.write(f"\nError: Could not download all files for {tag}.\n" + \
            "Please check the release page for the latest version.")

        print(f"\n\n(Partial) update to third_party/scip-clang/defs.bzl:\n")
    else:
        print(f"\nUpdate the following in third_party/scip-clang/defs.bzl:\n")

    print(f"\"{tag}\": {{")
    for os, sum in sha256sums.items():
        print(f"    \"{os}\": \"{sha256sums[os]}\",")
    print("},")

if __name__ == "__main__":
    import sys
    main(sys.argv[1])