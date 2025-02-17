#!/usr/bin/env python3
"""
Script to filter out LCOV exclusion ranges from an LCOV file based on LCOV annotations in source files.
Only LCOV_EXCL_START and LCOV_EXCL_STOP annotations are supported.
"""

import argparse
import os
import re
import sys
from typing import List


def is_empty_line(line: str) -> bool:
    """Check if a line is empty, such as a comment-only line."""
    line = line.strip()
    return len(line) == 0 or line.startswith("//")

class LineRange:
    """
    Represents a range of line numbers to be excluded.

    The range is from start (inclusive) to end (exclusive).
    """
    def __init__(self, start: int = 0, end: int = 0) -> None:
        self.start = start
        self.end = end


class SourceFilter:
    """Filters source file lines based on LCOV exclusion annotations.

    This class parses a source file to determine the line ranges that are marked for exclusion
    using LCOV_EXCL_START and LCOV_EXCL_STOP annotations.
    """
    def __init__(self, source_path: str) -> None:
        self._no_cov_ranges: List[LineRange] = []
        self._parse_source(source_path)

    def _parse_source(self, source_path: str) -> None:
        """Parse the source file and build a list of exclusion ranges."""
        try:
            with open(source_path, 'r') as source_file:
                source_lines = source_file.readlines()
        except Exception as e:
            raise Exception(f"Error reading source file {source_path}: {e}")

        in_exclusion = False
        start_line = 0
        for line_number, source_line in enumerate(source_lines, start=1):
            if "LCOV_EXCL_START" in source_line:
                start_line = line_number
                in_exclusion = True
            elif "LCOV_EXCL_STOP" in source_line:
                # Add the range from start (exclusive) to stop (exclusive)
                if in_exclusion:
                  self._no_cov_ranges.append(LineRange(start_line, line_number))
                  in_exclusion = False
            elif "LCOV_EXCL_LINE" in source_line or "UTILS_UNREACHABLE()" in source_line or is_empty_line(source_line):
                # Add a single line exclusion
                if not in_exclusion:
                  self._no_cov_ranges.append(LineRange(line_number, line_number + 1))

    def is_no_cov(self, line_number: int) -> bool:
        """Determine whether a given line number is within an exclusion range.

        Uses binary search for efficient lookup assuming ranges are sorted.
        """
        low = 0
        high = len(self._no_cov_ranges) - 1

        while low <= high:
            mid = low + (high - low) // 2
            current_range = self._no_cov_ranges[mid]
            if current_range.start <= line_number < current_range.end:
                return True
            elif line_number >= current_range.end:
                low = mid + 1
            else:
                high = mid - 1

        return False


def main() -> None:
    """Main function to filter LCOV files based on LCOV exclusion annotations in source files."""
    parser = argparse.ArgumentParser(
        description="Filter LCOV file based on LCOV_EXCL_START and LCOV_EXCL_STOP annotations in source files."
    )
    parser.add_argument("--input", help="Path to the LCOV file")
    parser.add_argument("--output", help="Path to the output LCOV file")
    args = parser.parse_args()

    input_file = args.input
    output_file = args.output

    try:
        with open(input_file, 'r') as lcov_file:
            lcov_lines = lcov_file.readlines()
    except Exception as e:
        sys.exit(f"Error reading LCOV file {input_file}: {e}")

    try:
        with open(output_file, 'w') as lcov_out_file:
            source_filter: SourceFilter = None
            for line in lcov_lines:
                filter_out = False

                # When encountering a new source file, update the source filter.
                if line.startswith("SF:"):
                    source_path = line[3:].strip()
                    source_filter = SourceFilter(source_path)
                else:
                    # Process DA lines reporting execution counts.
                    match = re.search(r'^(BR)?DA:(\d+),', line)
                    if match:
                        line_number = int(match.group(2))
                        if source_filter is None:
                            raise Exception("Source filter not initialized for line: " + line)
                        filter_out = source_filter.is_no_cov(line_number)

                if not filter_out:
                    lcov_out_file.write(line)
    except Exception as e:
        sys.exit(f"Error processing LCOV file: {e}")


if __name__ == "__main__":
    main()
