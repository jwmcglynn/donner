import argparse
import subprocess
import tempfile
from pathlib import Path
from python.runfiles import runfiles


def test_parity() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--header", required=True)
    args, remaining = parser.parse_known_args()

    r = runfiles.Create()
    font = Path(r.Rlocation("donner/third_party/public-sans/PublicSans-Medium.otf"))
    script = Path(r.Rlocation("donner/tools/embed_resources"))
    diff_tool = Path(r.Rlocation("donner/tools/diff_files"))
    bazel_header = Path(args.header)
    bazel_cpp = bazel_header.with_name("PublicSans_Medium_otf.cpp")

    with tempfile.TemporaryDirectory() as tmp_dir:
        tmp = Path(tmp_dir)
        subprocess.run(
            [
                str(script),
                "--out",
                str(tmp),
                "--header",
                bazel_header.name,
                f"kPublicSansMediumOtf={font}",
            ],
            check=True,
        )

        produced_cpp = tmp / "PublicSans_Medium_otf.cpp"
        produced_header = tmp / bazel_header.name

        subprocess.run([str(diff_tool), str(bazel_cpp), str(produced_cpp)], check=True)
        subprocess.run([str(diff_tool), str(bazel_header), str(produced_header)], check=True)

