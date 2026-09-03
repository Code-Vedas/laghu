# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import os
import pathlib
import subprocess
import sys
import tempfile


def main():
    worker = pathlib.Path(sys.argv[1])
    if os.geteuid() == 0:
        print("run this test as an unprivileged user")
        return 77
    with tempfile.TemporaryDirectory(prefix="laghu-chrome-bwrap-version-") as raw:
        root = pathlib.Path(raw)
        result = subprocess.run(
            [worker, "--once", root / "analysis.queue", root / "output"],
            text=True,
            capture_output=True,
        )
    assert result.returncode == 78, result.stderr
    assert "No insecure fallback exists" in result.stderr
    return 0


if __name__ == "__main__":
    sys.exit(main())
