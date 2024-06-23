#!/bin/bash -e
#
# Watch for changes in an SVG file and run the donner renderer tool on each change, saving
# 'output.png' in the current working directory.
#
# Usage: tools/watch_svg.sh <path_to_svg_file>
#

# Get the location of this script.
SCRIPT_DIR="$( cd "$( dirname "$0" )" && pwd )"

# Check if an argument is provided
if [ $# -eq 0 ]; then
    echo "Please provide the path to the SVG file as an argument."
    echo "Usage: $0 <path_to_svg_file>"
    exit 1
fi

SVG_FILE="$1"
DONNER_DIR="$SCRIPT_DIR/../donner"

# Function to run the Bazel command
run_bazel_command() {
    echo "Running Bazel command..."
    bazel run --run_under="cd $PWD && " -c dbg //donner/svg/renderer:renderer_tool -- "$SVG_FILE" --quiet
}

# Run the command once at the start
run_bazel_command

# Watch for changes in both the SVG file and the donner directory
fswatch -o "$SVG_FILE" "$DONNER_DIR" | while read file event; do
    echo "Change detected in: $file"
    run_bazel_command
done
