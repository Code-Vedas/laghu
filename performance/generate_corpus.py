#!/usr/bin/env python3
# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Create deterministic benchmark inputs without network access."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import subprocess
from pathlib import Path


def write_sized(path: Path, prefix: str, target: int) -> None:
    body = (prefix + "\n").encode()
    with path.open("wb") as output:
        while output.tell() + len(body) <= target:
            output.write(body)
        output.write(body[: target - output.tell()])


def run_vips(output: Path, width: int, height: int, suffix: str, transparent: bool = False) -> bool:
    svg_path = output.with_suffix(".source.svg")
    source = output.with_suffix(".v")
    alpha_circle = (
        f'<circle cx="{width * 2 // 3}" cy="{height // 2}" r="{max(8, width // 5)}" '
        'fill="#ffffff" fill-opacity="0.35"/>'
        if transparent
        else ""
    )
    background = "" if transparent else f'<rect width="{width}" height="{height}" fill="url(#g)"/>'
    svg = (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">'
        '<defs><linearGradient id="g" x1="0" x2="1" y1="0" y2="1"><stop stop-color="#103050"/>'
        '<stop offset=".48" stop-color="#d77a32"/><stop offset="1" stop-color="#5ebf86"/></linearGradient></defs>'
        + background
        + f'<circle cx="{width // 3}" cy="{height // 2}" r="{max(8, width // 7)}" fill="#f5df7d"/>'
        + alpha_circle
        + f'<path d="M0 {height * 3 // 4} L{width // 3} {height // 3} L{width * 2 // 3} '
        f'{height * 4 // 5} L{width} {height // 4} V{height} H0Z" fill="#172638"/>'
        + f'<text x="{max(8, width // 20)}" y="{height * 9 // 10}" fill="white" font-size="{max(12, width // 16)}">Laghu benchmark</text></svg>'
    )
    svg_path.write_text(
        svg,
        encoding="utf-8",
    )
    subprocess.run(["vips", "svgload", str(svg_path), str(source)], check=True)
    command = {".png": ["vips", "pngsave"], ".jpg": ["vips", "jpegsave"], ".webp": ["vips", "webpsave"], ".avif": ["vips", "heifsave", "--compression", "av1"]}[suffix]
    completed = subprocess.run([*command, str(source), str(output)], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    source.unlink()
    svg_path.unlink()
    return completed.returncode == 0


def write_animated_gif(path: Path) -> None:
    """Write a deterministic two-frame animated GIF without another tool dependency."""
    path.write_bytes(base64.b64decode(
        "R0lGODlhAgACAPAAADQ0NAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQAAAAAACwAAAAAAgACAAAC"
        "AoRRACH5BAAKAAAALAAAAAACAAIAgP///wAAAAIChFEAOw=="
    ))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--seed", default="laghu-performance-v1")
    args = parser.parse_args()
    root = args.output
    root.mkdir(parents=True, exist_ok=True)
    sizes = {"1k": 1024, "10k": 10 * 1024, "100k": 100 * 1024, "1m": 1024 * 1024, "5m": 5 * 1024 * 1024}
    for name, size in sizes.items():
        write_sized(root / f"html-{name}.html", "<!doctype html><main><!-- removable --> seeded benchmark content</main>", size)
    for name, size in {"1k": 1024, "10k": 10 * 1024, "100k": 100 * 1024, "1m": 1024 * 1024, "2m": 2 * 1024 * 1024}.items():
        write_sized(root / f"css-{name}.css", ".card { color: #334455; margin: 0 0 0 0; }", size)
    for name, size in {"1k": 1024, "10k": 10 * 1024, "100k": 100 * 1024, "1m": 1024 * 1024, "2m": 2 * 1024 * 1024}.items():
        write_sized(root / f"js-{name}.js", "function seededBenchmark(longName) { return longName + 1; }", size)
    write_sized(root / "no-store.html", "<!doctype html><meta http-equiv=Cache-Control content=no-store><main>no store</main>", 1024)
    (root / "index.html").write_text(
        "<!doctype html><html><head><link rel=stylesheet href=/css-10k.css></head><body><h1>Laghu benchmark</h1>"
        "<!-- removable --><img src=/image-480.png width=480 height=320><script src=/js-10k.js></script></body></html>", encoding="utf-8"
    )
    dimensions = (100, 480, 768, 1440, 3840, 8000)
    formats = (".png", ".jpg", ".webp", ".avif")
    unavailable_formats: set[str] = set()
    for width in dimensions:
        height = max(1, width * 3 // 4)
        for suffix in formats:
            if not run_vips(root / f"image-{width}{suffix}", width, height, suffix):
                unavailable_formats.add(suffix)
    if not run_vips(
        root / "image-transparent-1440.png", 1440, 1080, ".png", transparent=True
    ):
        unavailable_formats.add(".png")
    (root / "image.svg").write_text("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\"><rect width=\"100\" height=\"100\" fill=\"#345\"/></svg>", encoding="utf-8")
    write_animated_gif(root / "image-animated.gif")
    manifest = {"seed": args.seed, "sha256": {}, "images": dimensions, "unavailable_formats": sorted(unavailable_formats)}
    for path in sorted(root.iterdir()):
        if path.is_file():
            manifest["sha256"][path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
