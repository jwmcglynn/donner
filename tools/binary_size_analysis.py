import csv
import sys
import json
import os
from typing import Dict
from python.webtreemap import generate_webtreemap_html


def main(filename: str, output_filename: str):
    generate_webtreemap_html(filename, output_filename)
    print("Saved report to " + output_filename)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
