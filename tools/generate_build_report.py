import subprocess
import os
import argparse


def run_command(command):
    print("Running command: " + command)
    try:
        result = subprocess.run(command, shell=True, capture_output=True, text=True)
    except KeyboardInterrupt:
        print("Interrupted! Exiting...")
        return None

    print("Done, return code: " + str(result.returncode))
    return result.stdout.strip()


def create_build_report():
    report = "# Donner build report\n\n"

    # Lines of code report
    report += "## Lines of code\n```\n"
    report += "$ tools/cloc.sh\n"

    cloc_output = run_command("tools/cloc.sh")
    if not cloc_output:
        return report

    report += cloc_output
    report += "\n```\n\n"

    # Binary size report
    report += "## Binary size\n```\n"
    report += "$ tools/binary_size.sh\n"

    binsize_output = run_command("tools/binary_size.sh")
    if not binsize_output:
        return report

    report += binsize_output
    report += "\n```\n\n"

    # Code coverage report
    report += "## Code coverage\n```\n"
    report += "$ tools/coverage.sh --quiet\n"

    coverage_output = run_command("tools/coverage.sh --quiet")
    if not coverage_output:
        return report

    report += coverage_output
    report += "\n```\n\n"

    return report


def main():
    parser = argparse.ArgumentParser(
        description="Generate a build report for a C++/Bazel project."
    )
    parser.add_argument("--save", type=str, help="Path to save the build report")

    args = parser.parse_args()

    report = create_build_report()

    if args.save:
        with open(args.save, "w") as file:
            file.write(report)
        print(f"Saved build report to {args.save}")
    else:
        print(report)


if __name__ == "__main__":
    main()
