#!/usr/bin/env python3
"""
Script to filter out LCOV exclusion ranges from an LCOV file based on LCOV annotations in source files.
Only LCOV_EXCL_START and LCOV_EXCL_STOP annotations are supported.
Now also updates summary counters so that excluded lines are marked as non‑executable.
Also filters out multiline C++ comment lines.
"""

import argparse
import os
import re
import sys
from typing import List, Tuple

def is_empty_line(line: str) -> bool:
    """Check if a line is empty, or is a single-line comment starting with //."""
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
    """Filters source file lines based on LCOV exclusion annotations and comment-only lines.

    This class parses a source file to determine the line ranges that are marked for exclusion
    using LCOV_EXCL_START and LCOV_EXCL_STOP annotations. It also handles single line exclusions
    marked with LCOV_EXCL_LINE, UTILS_UNREACHABLE(), empty lines, and lines that are solely part of
    a multiline C++ comment.

    Args:
        source_path (str): Path to the source file to be parsed
        verbose (bool, optional): If True, print debug information. Defaults to False.

    Examples:
        >>> filter = SourceFilter("path/to/source.cpp")
        >>> filter.is_no_cov(10)  # Returns True if line 10 is excluded from coverage

    Notes:
        - LCOV_EXCL_START marks the beginning of a multi-line exclusion block
        - LCOV_EXCL_STOP marks the end of a multi-line exclusion block
        - LCOV_EXCL_LINE marks a single line for exclusion
        - Empty lines and lines that are entirely a comment (including multiline comments) are excluded
    """
    def __init__(self, source_path: str, verbose: bool = False) -> None:
        self._no_cov_ranges: List[LineRange] = []
        self._verbose = verbose
        self._parse_source(source_path)

    @staticmethod
    def _check_multiline_comment(source_line: str, in_multiline: bool) -> Tuple[bool, bool]:
        """
        Check whether the given source line is entirely part of a multiline C++ comment.

        Returns a tuple (is_comment, new_in_multiline) where:
         - is_comment is True if the entire line should be considered comment-only.
         - new_in_multiline reflects the updated state after processing this line.
        """
        stripped = source_line.strip()
        if in_multiline:
            # Already inside a multiline comment.
            if "*/" in stripped:
                # End of comment block found.
                idx = stripped.find("*/")
                after = stripped[idx+2:].strip()
                # If nothing follows the closing marker, treat the whole line as comment.
                return (after == "", False)
            else:
                return (True, True)
        else:
            # Not in a multiline comment.
            if stripped.startswith("/*"):
                if "*/" in stripped:
                    # Single-line multiline comment.
                    idx = stripped.find("*/")
                    after = stripped[idx+2:].strip()
                    return (after == "", False)
                else:
                    # Multiline comment begins here.
                    return (True, True)
            else:
                return (False, False)

    def _parse_source(self, source_path: str) -> None:
        """Parse the source file and build a list of exclusion ranges."""
        try:
            with open(source_path, 'r') as source_file:
                source_lines = source_file.readlines()
        except Exception as e:
            raise Exception(f"Error reading source file {source_path}: {e}")

        if self._verbose:
            print(f"Processing source file: {source_path}")

        in_exclusion = False
        in_multiline_comment = False
        start_line = 0
        for line_number, source_line in enumerate(source_lines, start=1):
            if "LCOV_EXCL_START" in source_line:
                start_line = line_number
                in_exclusion = True
            elif "LCOV_EXCL_STOP" in source_line:
                if in_exclusion:
                    # Add the range from start (inclusive) to stop (exclusive)
                    self._no_cov_ranges.append(LineRange(start_line, line_number))
                    if self._verbose:
                        print(f"Exclusion range {source_path}: {start_line} - {line_number}")
                    in_exclusion = False
            else:
                # Check for single line exclusions: explicit markers, unreachable code, or empty lines.
                if "LCOV_EXCL_LINE" in source_line or "UTILS_UNREACHABLE()" in source_line or is_empty_line(source_line):
                    if not in_exclusion:
                        self._no_cov_ranges.append(LineRange(line_number, line_number + 1))
                else:
                    # Check for lines that are entirely within a multiline comment.
                    comment_exclusion, in_multiline_comment = SourceFilter._check_multiline_comment(source_line, in_multiline_comment)
                    if comment_exclusion and not in_exclusion:
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

def process_record(record_lines: List[str], source_filter: SourceFilter, verbose: bool = False) -> List[str]:
    """
    Process one LCOV record (from "SF:" until "end_of_record").
    Removes DA and BRDA lines for excluded source lines and recalculates summary counters.

    Args:
        record_lines (List[str]): List of lines in the record
        source_filter (SourceFilter): Source filter object for the source file
        verbose (bool, optional): If True, print debug information. Defaults to False.
    """
    # Separate out the body from the record terminator.
    record_body = []
    end_record_line = None
    for line in record_lines:
        if line.strip() == "end_of_record":
            end_record_line = line
        else:
            record_body.append(line)

    processed_body = []
    # For recalculating summary counters.
    da_lines = []    # stores DA lines (e.g. "DA:<line>,<hits>")
    brda_lines = []  # stores BRDA lines (e.g. "BRDA:<line>,...,<hits>")

    # Process each line in the record body.
    for line in record_body:
        if line.startswith("DA:") or line.startswith("BRDA:"):
            # Extract the line number.
            m = re.match(r'^(BRDA|DA):(\d+),(.*)', line)
            if m:
                prefix = m.group(1)
                line_number = int(m.group(2))
                # If the line is excluded, skip it.
                if source_filter and source_filter.is_no_cov(line_number):
                    if verbose:
                        print(f"Excluding line: {line.strip()}")
                    continue
                else:
                    processed_body.append(line)
                    if prefix == "DA":
                        da_lines.append(line)
                    else:
                        brda_lines.append(line)
            else:
                processed_body.append(line)
        elif (line.startswith("LF:") or line.startswith("LH:") or 
              line.startswith("BRF:") or line.startswith("BRH:")):
            # Skip existing summary lines; we'll recalc them.
            continue
        else:
            processed_body.append(line)

    # Recalculate summary for DA lines.
    lf = len(da_lines)
    lh = 0
    for da in da_lines:
        try:
            # Expected format: DA:<line>,<hits>
            parts = da.strip().split(',')
            count = int(parts[1].strip())
            if count > 0:
                lh += 1
        except (IndexError, ValueError):
            pass

    # Recalculate summary for branch lines if any.
    brf = len(brda_lines)
    brh = 0
    for brda in brda_lines:
        try:
            # Expected format: BRDA:<line>,<block>,<branch>,<hits>
            parts = brda.strip().split(',')
            hits = int(parts[3].strip())
            if hits > 0:
                brh += 1
        except (IndexError, ValueError):
            pass

    # Append the new summary lines.
    processed_body.append(f"LF:{lf}\n")
    processed_body.append(f"LH:{lh}\n")
    if brda_lines:
        processed_body.append(f"BRF:{brf}\n")
        processed_body.append(f"BRH:{brh}\n")

    # Append the record terminator if it existed.
    if end_record_line:
        processed_body.append(end_record_line)
    return processed_body

def main() -> None:
    """
    Main function to filter LCOV files based on LCOV exclusion annotations in source files.
    Processes records and recalculates summary counters so that excluded lines are marked as non-executable.
    """
    parser = argparse.ArgumentParser(
        description="Filter LCOV file based on LCOV_EXCL_START and LCOV_EXCL_STOP annotations in source files."
    )
    parser.add_argument("--input", help="Path to the LCOV file", required=True)
    parser.add_argument("--output", help="Path to the output LCOV file", required=True)
    parser.add_argument("--verbose", action="store_true", help="Print debug information")
    args = parser.parse_args()

    input_file = args.input
    output_file = args.output
    verbose = args.verbose

    try:
        with open(input_file, 'r') as lcov_file:
            lcov_lines = lcov_file.readlines()
    except Exception as e:
        sys.exit(f"Error reading LCOV file {input_file}: {e}")

    try:
        with open(output_file, 'w') as lcov_out_file:
            record_lines: List[str] = []
            current_source_filter = None
            for line in lcov_lines:
                record_lines.append(line)
                if line.startswith("SF:"):
                    source_path = line[3:].strip()
                    try:
                        current_source_filter = SourceFilter(source_path, verbose)
                    except Exception as e:
                        sys.exit(f"Error reading source file {source_path}: {e}")
                # LCOV records are terminated by the line "end_of_record"
                if line.strip() == "end_of_record":
                    processed_record = process_record(record_lines, current_source_filter, verbose)
                    for processed_line in processed_record:
                        lcov_out_file.write(processed_line)
                    # Reset for the next record.
                    record_lines = []
                    current_source_filter = None
    except Exception as e:
        sys.exit(f"Error processing LCOV file: {e}")

if __name__ == "__main__":
    main()
