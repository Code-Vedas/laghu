# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import pathlib
import signal
import socket
import subprocess
import sys
import tempfile
import time


NESTING_LIMIT = 1000


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def nested_config(depth):
    return "runtime: " + "[" * depth + "]" * depth + "\n"


def write_secure_yaml(path, contents):
    path.write_text(contents)
    path.chmod(0o600)


def run(command):
    return subprocess.run(command, text=True, capture_output=True, timeout=5)


def main():
    executable = pathlib.Path(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="laghu-yaml-nesting-") as raw:
        root = pathlib.Path(raw)
        deep = root / "too-deep.yaml"
        write_secure_yaml(deep, nested_config(NESTING_LIMIT + 1))
        startup = run([executable, "--config", deep])
        assert startup.returncode != 0
        assert "exceeded maximum nesting depth" in startup.stderr

        document_root = root / "site"
        document_root.mkdir()
        (document_root / "index.html").write_text("yaml nesting reload")
        good = root / "good.yaml"
        pid_file = root / "laghu.pid"
        write_secure_yaml(
            good,
            "runtime:\n"
            f"  listen: 127.0.0.1:{free_port()}\n"
            "  rewrite_level: passthrough\n"
            "  access_log: off\n"
            f"  pid_file: {pid_file}\n"
            "sites:\n"
            "  - host: yaml-nesting.test\n"
            f"    document_root: {document_root}\n"
        )
        process = subprocess.Popen([executable, "--config", good], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        try:
            for _ in range(100):
                if pid_file.exists():
                    break
                if process.poll() is not None:
                    raise AssertionError(process.stderr.read())
                time.sleep(0.02)
            else:
                raise AssertionError("Laghu did not become ready")
            reload = run([executable, "reload", "--config", deep])
            assert reload.returncode != 0
            assert "exceeded maximum nesting depth" in reload.stderr
            assert process.poll() is None
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
            process.wait(timeout=5)
            process.stderr.close()


if __name__ == "__main__":
    main()
