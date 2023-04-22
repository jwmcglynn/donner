import hashlib
import requests

tag = "v0.0.6"
release_url = f"https://github.com/sourcegraph/scip-clang/releases/download/{tag}"
all_os = ["darwin", "linux"]

sha256sums = {}
print(
    f"Downloading version {tag}, please update fetch_new_version.py to change.")
print("The latest version can be found at https://github.com/sourcegraph/scip-clang/releases\n")

for os in all_os:
    print(f"Downloading scip-clang-{os}")
    response = requests.get(f"{release_url}/scip-clang-x86_64-{os}")
    hash = hashlib.sha256(response.content).hexdigest()
    sha256sums[os] = hash
    print(f"-> sha256sum: {hash}")


print(f"\nUpdate the following in third_party/scip-clang/defs.bzl:\n")
print(f"\"{tag}\": {{")
for os in all_os:
    print(f"    \"{os}\": \"{sha256sums[os]}\",")
print("},")
