#!/usr/bin/env python3
# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import json
import pathlib
import subprocess
import sys
import tempfile

def main():
    root = pathlib.Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as directory:
        output = pathlib.Path(directory) / "evidence"
        subprocess.run([sys.executable, str(root / "execution_rail.py"), "--contract", str(root / "parity-contract.json"), "--output", str(output), "--architecture", "native", "--laghu-command", "printf laghu", "--nginx-command", "printf nginx", "--apache-command", "printf apache"], check=True)
        evidence = json.loads((output / "evidence.json").read_text())
        assert evidence["architecture"] == "native"
        assert len(evidence["cases"]) == 15
        assert [item["surface"] for item in evidence["surfaces"]] == ["laghu", "nginx", "apache"]
        assert all(item["passed"] and (output / item["raw"]).is_file() for item in evidence["surfaces"])
        failed = pathlib.Path(directory) / "failed"
        result = subprocess.run([sys.executable, str(root / "execution_rail.py"), "--contract", str(root / "parity-contract.json"), "--output", str(failed), "--architecture", "native", "--laghu-command", "false", "--nginx-command", "true", "--apache-command", "true"])
        assert result.returncode == 1
        failed_evidence = json.loads((failed / "evidence.json").read_text())
        assert not failed_evidence["surfaces"][0]["passed"]
        assert (failed / failed_evidence["surfaces"][0]["raw"]).is_file()

if __name__ == "__main__":
    main()
