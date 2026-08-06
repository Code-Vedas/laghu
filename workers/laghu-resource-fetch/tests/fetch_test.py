# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import http.server
import os
import pathlib
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time


CSS = (
    b"@font-face{font-family:Fixture;font-style:normal;font-weight:400;"
    b"src:url(https://assets.fixture.test/font.woff2) format('woff2')}"
)


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/css?family=Fixture":
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/css; charset=utf-8")
        self.send_header("Cache-Control", "public, max-age=3600")
        self.send_header("Content-Length", str(len(CSS)))
        self.end_headers()
        self.wfile.write(CSS)

    def log_message(self, _format, *_args):
        pass


def lock_queue(file):
    file.seek(0)
    import fcntl

    fcntl.flock(file.fileno(), fcntl.LOCK_EX)


def unlock_queue(file):
    file.seek(0)
    import fcntl

    fcntl.flock(file.fileno(), fcntl.LOCK_UN)


def main():
    worker = pathlib.Path(sys.argv[1])
    fixture = pathlib.Path(sys.argv[2])
    with tempfile.TemporaryDirectory(prefix="laghu-font-fetch-") as raw:
        root = pathlib.Path(raw)
        config = root / "font-providers.conf"
        queue = root / "fonts.queue"
        cache = root / "cache"
        key = root / "server.key"
        certificate = root / "server.crt"
        config.write_text(
            "provider fixture\n"
            "stylesheet fixture.test /css\n"
            "asset assets.fixture.test /\n"
            "max_css_bytes 262144\n"
            "ttl_seconds 604800\n"
            "end\n",
            encoding="ascii",
        )
        subprocess.run(
            [
                "openssl",
                "req",
                "-x509",
                "-newkey",
                "rsa:2048",
                "-nodes",
                "-config",
                os.devnull,
                "-subj",
                "/CN=fixture.test",
                "-addext",
                "subjectAltName=DNS:fixture.test",
                "-days",
                "1",
                "-keyout",
                key,
                "-out",
                certificate,
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        environment = os.environ.copy()
        environment["LAGHU_TEST_FETCH_ENDPOINT"] = (
            f"127.0.0.1:{server.server_address[1]}"
        )
        environment["LAGHU_TEST_FETCH_CA"] = str(certificate)
        absent = subprocess.Popen(
            [worker, "--serve", queue, cache, config], env=environment,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            time.sleep(0.2)
            if absent.poll() is not None:
                stdout, stderr = absent.communicate(timeout=5)
                raise AssertionError(
                    f"font fetch worker exited before queue initialization with "
                    f"{absent.returncode}\nworker stdout:\n{stdout}\n"
                    f"worker stderr:\n{stderr}"
                )
        finally:
            if absent.poll() is None:
                absent.terminate()
                absent.wait(timeout=5)
        subprocess.run([worker, "--init", queue, cache, config], check=True)
        with queue.open("r+b", buffering=0) as queue_lock:
            lock_queue(queue_lock)
            process = subprocess.Popen(
                [worker, "--serve", queue, cache, config], env=environment,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            try:
                # Keep the native whole-file lock held across several of the
                # worker's 100 ms unavailable intervals.  The same process
                # must remain alive and recover once the lock is released.
                time.sleep(0.35)
                if process.poll() is not None:
                    stdout, stderr = process.communicate(timeout=5)
                    raise AssertionError(
                        f"font fetch worker exited while queue was locked with "
                        f"{process.returncode}\nworker stdout:\n{stdout}\n"
                        f"worker stderr:\n{stderr}"
                    )
            finally:
                unlock_queue(queue_lock)
        try:
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                submitted = subprocess.run(
                    [
                        fixture,
                        "--font-job",
                        queue,
                        config,
                        "https://fixture.test/css?family=Fixture",
                    ],
                    check=False,
                )
                if submitted.returncode == 0:
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("font fetch queue did not accept a job")
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    stdout, stderr = process.communicate(timeout=5)
                    raise AssertionError(
                        f"font fetch worker exited with {process.returncode}\n"
                        f"worker stdout:\n{stdout}\nworker stderr:\n{stderr}"
                    )
                ready = subprocess.run(
                    [
                        fixture,
                        "--font-ready",
                        cache,
                        config,
                        "https://fixture.test/css?family=Fixture",
                    ],
                    check=False,
                )
                if ready.returncode == 0:
                    break
                time.sleep(0.05)
            else:
                process.terminate()
                stdout, stderr = process.communicate(timeout=5)
                raise AssertionError(
                    "font CSS was not fetched and published\n"
                    f"worker stdout:\n{stdout}\nworker stderr:\n{stderr}"
                )
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main()
