# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import pathlib
import socket
import subprocess
import sys
import tempfile
import time


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def request(port):
    with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
        connection.sendall(b"GET / HTTP/1.1\r\nHost: passthrough.example.test\r\nConnection: close\r\n\r\n")
        response = b""
        while True:
            chunk = connection.recv(4096)
            if not chunk:
                return response
            response += chunk


def wait_for(port, expected_body):
    last_response = b""
    for _ in range(100):
        try:
            response = request(port)
            last_response = response
            if response.startswith(b"HTTP/1.1 200") and (expected_body is None or response.endswith(expected_body)):
                return response
        except OSError:
            pass
        time.sleep(0.05)
    raise AssertionError(f"standalone passthrough did not become ready: {last_response[:200]!r}")


def queue_is_mapped(process, queue):
    maps = pathlib.Path(f"/proc/{process.pid}/maps")
    if not maps.exists():
        return None
    return str(queue) in maps.read_text()


def wait_for_mapping(process, queue, expected):
    for _ in range(100):
        mapped = queue_is_mapped(process, queue)
        if mapped is expected or mapped is None:
            return mapped
        time.sleep(0.05)
    raise AssertionError(f"queue mapping did not become {expected}")


def config(port, cache, document_root, pid_file, policy, queue=None):
    queue_line = "" if queue is None else f"  worker_queue: {queue}\n"
    return (
        "runtime:\n"
        f"  listen: 127.0.0.1:{port}\n"
        "  origin: http://127.0.0.1:9\n"
        f"  file_cache_backend: file://{cache}\n"
        f"  pid_file: {pid_file}\n"
        f"  {policy}\n"
        f"{queue_line}"
        "sites:\n"
        "  - host: passthrough.example.test\n"
        f"    document_root: {document_root}\n"
    )


def main():
    laghu = pathlib.Path(sys.argv[1]).resolve()
    libvips = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="laghu-passthrough-queue-") as directory:
        root = pathlib.Path(directory)
        cache = root / "cache"
        document_root = root / "site"
        queue = root / "jobs.queue"
        pid_file = root / "laghu.pid"
        active = root / "active.yaml"
        passthrough = root / "passthrough.yaml"
        port = free_port()
        cache.mkdir()
        document_root.mkdir()
        (document_root / "index.html").write_bytes(b"exact passthrough body")
        subprocess.run([str(libvips), "--init", str(queue), str(cache)], check=True, capture_output=True)
        active.write_text(config(port, cache, document_root, pid_file, "preset: balanced", queue))
        passthrough.write_text(config(port, cache, document_root, pid_file, "rewrite_level: passthrough"))
        active.chmod(0o600)
        passthrough.chmod(0o600)
        process = subprocess.Popen([str(laghu), "--config", str(active)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        failure = None
        try:
            wait_for(port, None)
            before = queue_is_mapped(process, queue)
            assert before is not False
            reload_result = subprocess.run([str(laghu), "reload", "--config", str(passthrough)], capture_output=True, text=True)
            assert reload_result.returncode == 0, reload_result.stderr
            after_detach = wait_for_mapping(process, queue, False)
            wait_for(port, b"exact passthrough body")
            assert after_detach is not True
            reload_result = subprocess.run([str(laghu), "reload", "--config", str(active)], capture_output=True, text=True)
            assert reload_result.returncode == 0, reload_result.stderr
            after_attach = wait_for_mapping(process, queue, True)
            wait_for(port, None)
            assert after_attach is not False
        except Exception as error:
            failure = error
            raise
        finally:
            process.terminate()
            process.wait(timeout=5)
            logs = process.stderr.read().decode()
            process.stderr.close()
            if failure is not None:
                print(logs, file=sys.stderr)
        assert process.returncode == 0, logs


if __name__ == "__main__":
    main()
