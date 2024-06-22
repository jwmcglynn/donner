#!/bin/bash
# Based on: https://schneegans.github.io/tutorials/2020/08/16/badges

# This script counts the lines of code and comments in all source files
# and prints the results to the command line. It uses the commandline tool
# "cloc".

set -e

# Get the location of this script.
SCRIPT_DIR="$( cd "$( dirname "$0" )" && pwd )"

# Function to run cloc with common arguments and parse the output
run_cloc() {
    local extra_args="$@"
    local output
    output=$(cloc "${SCRIPT_DIR}"/../donner --include-lang="C++,C/C++ Header" --timeout=60 --diff-timeout=60 --csv $extra_args)
    
    # Extract the summary line (last line) and parse it
    local summary
    summary=$(echo "$output" | tail -1)

    IFS=',' read -r files language blank comment code <<< "$summary"
    
    # Return the parsed values
    echo "$code,$comment"
}

# Run cloc for all files, product files, and test files, and extract values
ALL_STATS=$(run_cloc "")
IFS=',' read -r LINES_OF_CODE COMMENT_LINES <<< "$ALL_STATS"

PRODUCT_STATS=$(run_cloc "--not-match-f=_tests\.cc$" --exclude-dir=tests)
IFS=',' read -r PRODUCT_LOC PRODUCT_COMMENTS <<< "$PRODUCT_STATS"

TEST_STATS=$(run_cloc "--match-f=_tests\.cc$")
IFS=',' read -r TEST_LOC TEST_COMMENTS <<< "$TEST_STATS"

COMMENT_PERCENT=$(bc <<< "scale=2; $COMMENT_LINES/$LINES_OF_CODE*100")

# Function to print formatted output
print_formatted() {
    local label="$1"
    local value="$2"
    local unit="$3"
    printf "%-25s %6.1f%s\n" "$label:" "$value" "$unit"
}

# Function to calculate and format value
calc_and_format() {
    local value="$1"
    local divisor="$2"
    if [[ "$value" =~ ^[0-9]+$ ]] && [[ "$divisor" =~ ^[0-9]+$ ]]; then
        bc <<< "scale=1; $value/$divisor" || echo "0.0"
    else
        echo "0.0"
    fi
}

print_formatted "Lines of source code" $(calc_and_format $LINES_OF_CODE 1000) "k"
print_formatted "Lines of comments" $(calc_and_format $COMMENT_LINES 1000) "k"
print_formatted "Comment percentage" "$COMMENT_PERCENT" "%"
print_formatted "Product lines of code" $(calc_and_format $PRODUCT_LOC 1000) "k"
print_formatted "Test lines of code" $(calc_and_format $TEST_LOC 1000) "k"
