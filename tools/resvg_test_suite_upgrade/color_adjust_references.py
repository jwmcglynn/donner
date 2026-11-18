"""Reconstruct the color-adjust references from upstream pixels and two outlined-label masks."""

import argparse
from pathlib import Path
import xml.etree.ElementTree as ET

from defusedxml.ElementTree import parse
from PIL import Image


ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "third_party/resvg-test-suite/tests/filters/filter-functions"
SVG = "http://www.w3.org/2000/svg"
PREFIX = "color-adjust-functions-"
# Each pair uses the same filter values written in two different ways.
STEMS = {
    "0percent": "0percent",
    "100percent": "100percent",
    "2": "200percent",
    "200percent": "200percent",
    "50percent": "50percent",
    "default-value": "100percent",
    "negative": "negative",
}
LABEL_BANDS = [(38, y, 463, y + 29) for y in (151, 301, 451)]


def label_scene(case: str) -> bytes:
    root = parse(FIXTURES / f"{PREFIX}{case}.svg", forbid_dtd=True).getroot()
    for child in list(root):
        if child.tag != f"{{{SVG}}}text":
            root.remove(child)
        else:
            child.tail = None
    if len(root) != 7:
        raise ValueError(f"Expected seven text labels in {case}")
    root.text = None
    root.set("width", "500")
    root.set("height", "500")
    ET.register_namespace("", SVG)
    return ET.tostring(root)


def compose(mask_file: Path, output: Path) -> None:
    mask = Image.open(mask_file).convert("RGBA")
    if mask.size != (500, 500):
        raise ValueError(f"Unexpected mask size in {mask_file}")
    alpha = mask.getchannel("A")
    for box in LABEL_BANDS:
        if not alpha.crop(box).getbbox():
            raise ValueError(f"Missing label band in {mask_file}")
        alpha.paste(0, box)
    if alpha.getbbox():
        raise ValueError(f"Found artwork outside the label bands in {mask_file}")

    output.mkdir(parents=True, exist_ok=True)
    generated = {}
    for case, stem in STEMS.items():
        image = Image.open(FIXTURES / f"{PREFIX}{case}.png").convert("RGBA")
        if image.size != mask.size:
            raise ValueError(f"Unexpected upstream reference size for {case}")
        for box in LABEL_BANDS:
            image.paste(mask.crop(box), box)
        if stem in generated:
            if image.tobytes() != generated[stem]:
                raise ValueError(f"{case} no longer matches {stem}")
        else:
            generated[stem] = image.tobytes()
            image.save(output / f"donner-{PREFIX}{stem}.png")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    labels = commands.add_parser("labels", help="Extract the common text scene for usvg")
    labels.add_argument("output", type=Path)
    images = commands.add_parser("compose", help="Combine the masks with the upstream PNGs")
    images.add_argument("cpu_mask", type=Path)
    images.add_argument("geode_mask", type=Path)
    images.add_argument("output", type=Path)
    args = parser.parse_args()

    scene = label_scene("0percent")
    if any(label_scene(case) != scene for case in STEMS):
        raise ValueError("The seven fixtures no longer have identical text and viewport settings")
    if args.command == "labels":
        args.output.write_bytes(scene)
    else:
        compose(args.cpu_mask, args.output)
        compose(args.geode_mask, args.output / "geode")


if __name__ == "__main__":
    main()
