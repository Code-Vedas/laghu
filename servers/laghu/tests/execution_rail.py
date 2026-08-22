#!/usr/bin/env python3
# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Run parity surfaces and preserve stable, normalized raw evidence."""

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys

SURFACES = ("laghu", "nginx", "apache")

def load_contract(path):
    contract = json.loads(path.read_text())
    if (contract.get("contract"), contract.get("surfaces")) != ("laghu-execution-rail", list(SURFACES)):
        raise ValueError("invalid laghu execution rail contract")
    return contract

def run_surface(surface, command, output):
    result = subprocess.run(command, shell=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
    raw = result.stdout.encode()
    raw_path = output / "raw" / f"{surface}.log"
    raw_path.parent.mkdir(parents=True, exist_ok=True)
    raw_path.write_bytes(raw)
    return {"surface": surface, "exit_code": result.returncode, "raw": str(raw_path.relative_to(output)), "sha256": hashlib.sha256(raw).hexdigest(), "passed": result.returncode == 0}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--contract", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--architecture", choices=("native", "linux/amd64"), required=True)
    for surface in SURFACES:
        parser.add_argument(f"--{surface}-command", required=True)
    args = parser.parse_args()
    contract = load_contract(args.contract)
    args.output.mkdir(parents=True, exist_ok=True)
    results = [run_surface(surface, getattr(args, f"{surface}_command"), args.output) for surface in SURFACES]
    evidence = {
        "contract": contract["contract"],
        "architecture": args.architecture,
        "cases": contract["cases"],
        "surfaces": results,
    }
    (args.output / "evidence.json").write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n")
    return 0 if all(result["passed"] for result in results) else 1

if __name__ == "__main__":
    sys.exit(main())
