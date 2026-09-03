# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import json
import os
import pathlib
import subprocess
import sys
import tempfile


HTML = b'''<!doctype html><html><head><style>
.hero { color: red; } .unused { color: blue; }
</style></head><body><img class="hero" src="data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==" width="320" height="180"></body></html>'''


def main():
    if os.geteuid() == 0:
        print("run this Chrome sandbox test as an unprivileged user")
        return 77
    worker, fixture, chrome = map(pathlib.Path, sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix="laghu-chrome-analyze-") as raw:
        root = pathlib.Path(raw)
        queue, output, source = root / "analysis.queue", root / "output", root / "source.html"
        source.write_bytes(HTML)
        subprocess.run([worker, "--init", queue, output, chrome], check=True)
        subprocess.run([fixture, "--publish", queue, source, "5000"], check=True)
        result = subprocess.run([worker, "--once", queue, output, chrome], text=True, capture_output=True)
        if result.returncode == 78:
            print(result.stderr.strip())
            return 77
        result.check_returncode()
        report = json.loads((output / ("a" * 64 + ".json")).read_text())
        assert report["version"] == 1
        assert report["viewport"]["width"] >= 1000
        assert report["viewport"]["height"] >= 500
        assert report["images"] == [{"ordinal": 0, "width": 320, "height": 180}]
        assert ".hero" in report["critical_css"]
        assert ".unused" not in report["critical_css"]
        assert report["network_requests_blocked"] == 0


if __name__ == "__main__":
    sys.exit(main())
