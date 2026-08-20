#!/usr/bin/env python3
# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import hashlib
import json
import subprocess
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from performance.generate_corpus import MANIFEST_SCHEMA
from performance.run_k6_rail import load_corpus_manifest


ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "performance" / "generate_corpus.py"


class CorpusTest(unittest.TestCase):
    def generate(self, destination: Path) -> dict:
        subprocess.run(["python3", str(GENERATOR), "--output", str(destination)], check=True)
        return json.loads((destination / "manifest.json").read_text(encoding="utf-8"))

    def test_seeded_generator_is_recursive_and_deterministic(self):
        with TemporaryDirectory() as first_directory, TemporaryDirectory() as second_directory:
            first_root, second_root = Path(first_directory), Path(second_directory)
            first, second = self.generate(first_root), self.generate(second_root)
            self.assertEqual(first, second)
            self.assertEqual(first["schema"], MANIFEST_SCHEMA)
            self.assertEqual(load_corpus_manifest(first_root), first)
            paths = {entry["path"] for entry in first["files"]}
            self.assertIn("corpus/fonts/full.woff2", paths)
            self.assertIn("corpus/js/source-mapped.js.map", paths)
            self.assertIn("assets/app.0123456789abcdef.css", paths)
            for entry in first["files"]:
                body = (first_root / entry["path"]).read_bytes()
                self.assertEqual(entry["bytes"], len(body))
                self.assertEqual(entry["sha256"], hashlib.sha256(body).hexdigest())

    def test_required_categories_and_size_boundaries(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = self.generate(root)
            self.assertEqual(set(manifest["categories"]), {"html", "css", "javascript", "images", "fonts", "edges"})
            expected_capabilities = {"normal", "webp", "avif", "save-data", "dpr-1x", "dpr-2x", "mobile", "tablet", "desktop"}
            self.assertTrue(expected_capabilities <= set(manifest["capabilities"]))
            self.assertEqual((root / "empty.bin").read_bytes(), b"")
            self.assertTrue((root / "image-thumbnail.jpg").stat().st_size >= 5 * 1024)
            self.assertTrue(9 * 1024 * 1024 <= (root / "image-processing-cap.png").stat().st_size <= 10 * 1024 * 1024)
            self.assertTrue((root / "corpus/fonts/subset.woff").read_bytes().startswith(b"wOFF"))
            self.assertTrue((root / "corpus/fonts/full.woff2").read_bytes().startswith(b"wOF2"))
            for font in manifest["categories"]["fonts"]:
                body = (root / font).read_bytes()
                self.assertIn(body[:4], {b"wOFF", b"wOF2"})
                self.assertEqual(int.from_bytes(body[8:12], "big"), len(body))
            self.assertIn("sourceMappingURL=source-mapped.js.map", (root / "corpus/js/source-mapped.js").read_text())


if __name__ == "__main__":
    unittest.main()
