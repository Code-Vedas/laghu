#!/usr/bin/env python3
# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Compute a structural-similarity score for two delivered images."""

from __future__ import annotations

import argparse
import json

import numpy
from PIL import Image
from skimage.metrics import structural_similarity


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("reference")
    parser.add_argument("candidate")
    args = parser.parse_args()
    reference = Image.open(args.reference).convert("RGB")
    candidate = Image.open(args.candidate).convert("RGB")
    resized = candidate.size != reference.size
    if resized:
        candidate = candidate.resize(reference.size, Image.Resampling.LANCZOS)
    reference_array = numpy.asarray(reference)
    candidate_array = numpy.asarray(candidate)
    mse = float(numpy.mean((reference_array.astype(numpy.float64) - candidate_array.astype(numpy.float64)) ** 2))
    print(json.dumps({"metric": "ssim", "ssim": structural_similarity(reference_array, candidate_array, channel_axis=2, data_range=255), "mse": mse, "reference_dimensions": reference.size, "candidate_dimensions": candidate.size, "resized_for_metric": resized}, sort_keys=True))


if __name__ == "__main__":
    main()
