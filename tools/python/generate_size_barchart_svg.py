import sys
from html import escape
from typing import Dict
from webtreemap import read_bloaty_csv, Node, generate_webtreemap_html


# Aggregate each directory into a table that sums up the sizes of the files inside.
# Create a group for everything that is not a directory.
def aggregate_sizes(node: Node, path: str = "") -> Dict[str, int]:
    sizes: Dict[str, int] = {}
    parent_path = path if path else node.id
    current_path = f"{path}/{node.id}" if path else node.id

    if node.children:
        for child in node.children:
            child_sizes = aggregate_sizes(child, current_path)

            for k, v in child_sizes.items():
                sizes[k] = sizes.get(k, 0) + v
    else:
        parent_path = path if path else node.id
        sizes[parent_path] = sizes.get(parent_path, 0) + node.size

    # Combine entries in sizes which start with a '[' into an 'Everything else' category. Remove the original entry.
    simplified_sizes: Dict[str, int] = {}
    everything_else = 0

    for k, v in sizes.items():
        if k.startswith("[") and k.endswith("]"):
            everything_else += v
        else:
            simplified_sizes[k] = v

    if everything_else != 0:
        simplified_sizes["Everything else"] = everything_else

    return simplified_sizes


# Generate a color bar graph for the directory structure, where each directory has a unique
# color and takes a % of the width. Output as a simple <svg>, size 512x128.
def generate_color_bar(aggregated_sizes: Dict[str, int]) -> str:
    total_size = sum(aggregated_sizes.values())
    width = 512
    height = 128
    key_height = 20 * len(aggregated_sizes)
    x_offset = 0
    colors = [
        "#FF5733",
        "#33FF57",
        "#3357FF",
        "#FF33A1",
        "#FF8633",
        "#33FFD7",
        "#8333FF",
        "#FF3381",
        "#33FF83",
        "#A133FF",
        "#FF8333",
        "#3381FF",
        "#FF33D7",
    ]

    svg_elements = []
    key_elements = []
    color_index = 0
    key_y_offset = height + 20

    sorted_items = {
        k: v for k, v in sorted(aggregated_sizes.items(), key=lambda item: -item[1])
    }

    def to_human_readable(size: int):
        for unit in ["B", "KB", "MB", "GB"]:
            if size < 1024:
                return f"{size:.2f} {unit}"
            size /= 1024

        return f"{size:.2f} TB"

    for directory, size in sorted_items.items():
        readable_size = to_human_readable(size)
        sanitized_directory = escape(directory)

        bar_width = (size / total_size) * width
        color = colors[color_index % len(colors)]
        svg_elements.append(
            f'  <rect x="{x_offset}" y="0" width="{bar_width}" height="{height}" fill="{color}" />\n'
        )
        key_elements.append(
            f'  <rect x="0" y="{key_y_offset}" width="20" height="20" fill="{color}" />\n'
        )
        key_elements.append(
            f'  <text x="25" y="{key_y_offset + 15}" fill="black" font-size="12">{sanitized_directory} - {readable_size}</text>\n'
        )
        x_offset += bar_width
        color_index += 1
        key_y_offset += 20

    svg_content = f'<svg width="{width}" height="{height + key_height}" xmlns="http://www.w3.org/2000/svg">\n'
    svg_content += f'  <rect x="0" y="0" width="{width}" height="{height + key_height}" fill="white" />\n'
    svg_content += "".join(svg_elements)
    svg_content += "".join(key_elements)
    svg_content += "</svg>"

    return svg_content


def generate_svg(filename: str):
    tree: Node = read_bloaty_csv(filename)

    aggregated_sizes = aggregate_sizes(tree)
    svg_output = generate_color_bar(aggregated_sizes)
    print(svg_output)


def main(filename: str):
    generate_svg(filename)


if __name__ == "__main__":
    main(sys.argv[1])
