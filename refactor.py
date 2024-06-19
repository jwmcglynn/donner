import os
import re

# Define the mapping of source to destination directories and filenames
mappings = {
    "base": "base",
    "css": "css",
    "svg": "svg",
    "experimental/wasm": "experimental/wasm",
}


def capitalize(word):
    if word == "svg":
        return "SVG"
    if word == "BUILD":
        return "BUILD"
    return word.capitalize()


# Function to generate new file names with UpperCamelCase and special case for SVG
def to_upper_camel_case(name):
    return "".join(capitalize(word) for word in name.split("_"))


# Function to convert UpperCamelCase to snake_case
def to_snake_case(name):
    s1 = re.sub("([A-Z]+)([A-Z][a-z])", r"\1_\2", name)
    s2 = re.sub("([a-z\d])([A-Z])", r"\1_\2", s1)
    return s2.replace("SVG", "svg").lower()


# List of directories to rename files with UpperCamelCase
directories_to_rename = ["base", "css", "svg"]

# Generate git mv commands
commands = []
for src_dir, dest_dir in mappings.items():
    for root, dirs, files in os.walk(src_dir):
        for file in files:
            # For all header file includes
            if not file.endswith(".h"):
                continue

            # Construct old and new file paths
            current_path = os.path.join(root, file)
            relative_path = os.path.relpath(current_path, src_dir)

            legacy_filename = (
                to_snake_case(os.path.splitext(file)[0]) + os.path.splitext(file)[1]
            )
            legacy_path = os.path.join(
                dest_dir, os.path.dirname(relative_path), legacy_filename
            )

            if any(dest in legacy_path for dest in directories_to_rename):
                legacy_path = os.path.join(
                    dest_dir, os.path.dirname(relative_path), legacy_filename
                )

            # Add sed command to the list for updating include paths
            old_include_path = os.path.join(src_dir, relative_path).replace("\\", "/")
            new_include_path = os.path.join(
                dest_dir, os.path.dirname(relative_path), legacy_filename
            ).replace("\\", "/")
            sed_command = f'sed -i \'s|#include "{old_include_path}"|#include <donner/{new_include_path}>|g\' $(grep -rl "#include \\"{old_include_path}\\"")'
            commands.append(sed_command)

# Print commands
for command in commands:
    print(command)
