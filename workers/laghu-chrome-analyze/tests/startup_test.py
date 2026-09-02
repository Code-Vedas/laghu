# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import pathlib
import subprocess
import sys
import tempfile


def run(command, **kwargs):
    return subprocess.run(command, check=True, text=True, **kwargs)


def main():
    worker, root_worker, source, service = map(pathlib.Path, sys.argv[1:5])
    source_text = source.read_text()
    service_text = service.read_text()
    assert "--no-sandbox" not in source_text
    assert "User=laghu" in service_text
    assert "RestartPreventExitStatus=77" in service_text

    with tempfile.TemporaryDirectory(prefix="laghu-chrome-startup-") as raw:
        root = pathlib.Path(raw)
        queue, output = root / "analysis.queue", root / "output"
        root_result = subprocess.run(
            [root_worker, "--init", queue, output], text=True, capture_output=True
        )
        assert root_result.returncode == 77
        assert "refusing to start as root" in root_result.stderr
        assert "Chrome sandbox must remain enabled" in root_result.stderr
        assert not queue.exists()

        run([worker, "--init", queue, output])
        assert queue.exists()


if __name__ == "__main__":
    main()
