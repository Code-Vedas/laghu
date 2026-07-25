# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import http.server
import json
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading
import time


BODY = b"<!doctype html>\n<html>  <head><!-- remove --></head>  <body>hello</body></html>"


class Origin(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        if self.path == "/api/data":
            body = b'{"ok":true}'
            content_type = "application/json"
        elif self.path == "/site.css":
            body = b"body { color: red; }"
            content_type = "text/css"
        elif self.path == "/private":
            body = BODY
            content_type = "text/html"
        elif self.path == "/encoded":
            body = b"encoded-origin"
            content_type = "text/html"
        elif self.path == "/image.png":
            body = b"not-decoded-without-worker"
            content_type = "image/png"
        elif self.path == "/partial":
            body = b"partial"
            content_type = "text/html"
        elif self.path == "/truncated":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", "100")
            self.end_headers()
            self.wfile.write(b"short")
            self.close_connection = True
            return
        elif self.path in ("/chunked", "/bad-chunk"):
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            if self.path == "/chunked":
                self.wfile.write(f"{len(BODY):x}\r\n".encode() + BODY + b"\r\n0\r\n\r\n")
            else:
                self.wfile.write(b"4\r\nno")
            self.close_connection = True
            return
        elif self.path == "/slow":
            time.sleep(2)
            body = BODY
            content_type = "text/html"
        elif self.path == "/drain":
            time.sleep(0.4)
            body = BODY
            content_type = "text/html"
        elif self.path == "/hang":
            time.sleep(10)
            body = BODY
            content_type = "text/html"
        else:
            body = BODY
            content_type = "text/html"
        self.send_response(206 if self.path == "/partial" else 200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("ETag", '"origin-v1"')
        if self.path == "/private":
            self.send_header("Cache-Control", "private")
        if self.path == "/encoded":
            self.send_header("Content-Encoding", "gzip")
        if self.path == "/partial":
            self.send_header("Content-Range", "bytes 0-6/20")
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


class QuietThreadingHTTPServer(http.server.ThreadingHTTPServer):
    def handle_error(self, _request, _client_address):
        pass


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def request(port, path, method="GET", headers=None, body=b""):
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    extra = ""
    for name, value in (headers or {}).items():
        extra += f"{name}: {value}\r\n"
    if body:
        extra += f"Content-Length: {len(body)}\r\n"
    sock.sendall(
        f"{method} {path} HTTP/1.1\r\nHost: example.test\r\n{extra}Connection: close\r\n\r\n".encode()
        + body
    )
    chunks = []
    while True:
        try:
            chunk = sock.recv(65536)
        except ConnectionResetError:
            if chunks:
                break
            raise
        if not chunk:
            break
        chunks.append(chunk)
    sock.close()
    head, body = b"".join(chunks).split(b"\r\n\r\n", 1)
    return head.lower(), body


def raw_request(port, payload):
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.sendall(payload)
    chunks = []
    while True:
        try:
            chunk = sock.recv(65536)
        except ConnectionResetError:
            if chunks:
                break
            raise
        if not chunk:
            break
        chunks.append(chunk)
    sock.close()
    return b"".join(chunks).lower()


def read_open_socket(sock):
    chunks = []
    while True:
        try:
            chunk = sock.recv(65536)
        except (ConnectionResetError, OSError):
            break
        if not chunk:
            break
        chunks.append(chunk)
    sock.close()
    return b"".join(chunks).lower()


def main():
    executable = pathlib.Path(sys.argv[1]).resolve()
    cache_fixture = pathlib.Path(sys.argv[2]).resolve()
    origin_port = free_port()
    proxy_port = free_port()
    origin = QuietThreadingHTTPServer(("127.0.0.1", origin_port), Origin)
    thread = threading.Thread(target=origin.serve_forever, daemon=True)
    thread.start()
    with tempfile.TemporaryDirectory(prefix="laghu-proxy-") as directory:
        root = pathlib.Path(directory)
        (root / "cache").mkdir()
        asset_key = "a" * 64
        subprocess.run(
            [str(cache_fixture), str(root / "cache"), asset_key], check=True
        )
        invalid_port = free_port()
        invalid = subprocess.run(
            [
                str(executable),
                "--listen",
                f"127.0.0.1:{invalid_port}",
                "--origin",
                f"http://127.0.0.1:{origin_port}",
                "--cache",
                str(root / "missing-cache"),
                "--worker-queue",
                str(root / "missing.queue"),
            ],
            capture_output=True,
            timeout=5,
        )
        assert invalid.returncode == 1
        invalid_log = invalid.stderr.decode().strip()
        assert json.loads(invalid_log)["event"] == "startup_failure"
        process = subprocess.Popen(
            [
                str(executable),
                "--listen",
                f"127.0.0.1:{proxy_port}",
                "--origin",
                f"http://127.0.0.1:{origin_port}",
                "--cache",
                str(root / "cache"),
                "--worker-queue",
                str(root / "missing.queue"),
                "--io-timeout",
                "1",
                "--workers",
                "1",
                "--connection-queue",
                "1",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        try:
            for _ in range(50):
                try:
                    first_head, first_body = request(proxy_port, "/index.html")
                    break
                except OSError:
                    time.sleep(0.05)
            else:
                raise AssertionError("proxy did not start")
            assert first_body == BODY, (first_head, first_body)
            assert b"x-laghu: pass" in first_head, first_head
            health_head, health_body = request(proxy_port, "/.laghu/health")
            assert b" 200 " in health_head.split(b"\r\n", 1)[0]
            assert health_body == b'{"status":"ok","state":"running"}'
            ready_head, ready_body = request(proxy_port, "/.laghu/ready")
            assert b" 200 " in ready_head.split(b"\r\n", 1)[0]
            assert b'"optimizer":"degraded"' in ready_body
            subprocess.run(
                [
                    str(cache_fixture),
                    "--queue",
                    str(root / "missing.queue"),
                    str(int(time.time())),
                ],
                check=True,
            )
            ready_head, ready_body = request(proxy_port, "/.laghu/ready")
            assert b" 200 " in ready_head.split(b"\r\n", 1)[0]
            assert b'"optimizer":"ready"' in ready_body
            ready_head, ready_body = request(
                proxy_port, "/.laghu/ready", method="HEAD"
            )
            assert b" 200 " in ready_head.split(b"\r\n", 1)[0]
            assert ready_body == b""
            (root / "cache").chmod(0o500)
            unavailable_head, unavailable_body = request(
                proxy_port, "/.laghu/ready"
            )
            (root / "cache").chmod(0o700)
            assert b" 503 " in unavailable_head.split(b"\r\n", 1)[0]
            assert b'"cache":"unavailable"' in unavailable_body
            second_head, second_body = request(proxy_port, "/index.html")
            assert len(second_body) < len(BODY)
            assert b"etag: \"laghu-html-" in second_head
            css_cold_head, css_cold_body = request(proxy_port, "/site.css")
            assert css_cold_body == b"body { color: red; }"
            assert b"x-laghu: pass" in css_cold_head
            css_warm_head, css_warm_body = request(proxy_port, "/site.css")
            assert len(css_warm_body) < len(css_cold_body)
            assert b"etag: \"laghu-css-" in css_warm_head
            api_head, api_body = request(proxy_port, "/api/data")
            assert api_body == b'{"ok":true}', (api_head, api_body)
            assert b"x-laghu: bypass-api" in api_head
            private_head, private_body = request(proxy_port, "/private")
            assert private_body == BODY
            assert b"x-laghu: bypass-private" in private_head
            auth_head, auth_body = request(
                proxy_port,
                "/index.html?query-secret=hidden",
                headers={"Authorization": "Bearer private-token", "Cookie": "private-cookie"},
            )
            assert auth_body == BODY
            assert b"x-laghu: bypass-authorized" in auth_head
            encoded_head, encoded_body = request(proxy_port, "/encoded")
            assert encoded_body == b"encoded-origin"
            assert b"x-laghu: bypass-encoded" in encoded_head
            image_head, image_body = request(proxy_port, "/image.png")
            assert image_body == b"not-decoded-without-worker"
            assert b"x-laghu: bypass-image-backend" in image_head
            partial_head, partial_body = request(proxy_port, "/partial")
            assert partial_body == b"partial"
            assert b"x-laghu: bypass-status" in partial_head
            post_head, post_body = request(
                proxy_port, "/echo", method="POST", body=b"request-body"
            )
            assert post_body == b"request-body"
            assert b"x-laghu: bypass-content-type" in post_head
            truncated_head, truncated_body = request(proxy_port, "/truncated")
            assert b" 502 " in truncated_head.split(b"\r\n", 1)[0]
            assert truncated_body == b""
            chunked_head, chunked_body = request(proxy_port, "/chunked")
            assert len(chunked_body) <= len(BODY)
            assert b"x-laghu: pass" in chunked_head
            bad_chunk_head, bad_chunk_body = request(proxy_port, "/bad-chunk")
            assert b" 502 " in bad_chunk_head.split(b"\r\n", 1)[0]
            assert bad_chunk_body == b""
            slow_head, slow_body = request(proxy_port, "/slow")
            assert b" 502 " in slow_head.split(b"\r\n", 1)[0]
            assert slow_body == b""
            active = socket.create_connection(("127.0.0.1", proxy_port), timeout=5)
            active.sendall(
                b"GET /slow HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n"
            )
            time.sleep(0.1)
            queued = socket.create_connection(("127.0.0.1", proxy_port), timeout=5)
            queued.sendall(
                b"GET /slow HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n"
            )
            time.sleep(0.1)
            saturated = raw_request(
                proxy_port,
                b"GET /api/data HTTP/1.1\r\nHost: example.test\r\n\r\n",
            )
            assert saturated.startswith(b"http/1.1 503 "), saturated
            active.close()
            queued.close()
            time.sleep(1.1)
            duplicate_host = raw_request(
                proxy_port,
                b"GET / HTTP/1.1\r\nHost: one\r\nHost: two\r\n\r\n",
            )
            assert duplicate_host.startswith(b"http/1.1 400 ")
            expect = raw_request(
                proxy_port,
                b"POST /echo HTTP/1.1\r\nHost: example.test\r\nExpect: 100-continue\r\nContent-Length: 1\r\n\r\n",
            )
            assert expect.startswith(b"http/1.1 417 ")
            chunked = raw_request(
                proxy_port,
                b"POST /echo HTTP/1.1\r\nHost: example.test\r\nTransfer-Encoding: chunked\r\n\r\n",
            )
            assert chunked.startswith(b"http/1.1 501 ")
            abandoned = socket.create_connection(("127.0.0.1", proxy_port), timeout=5)
            abandoned.sendall(b"GET /index.html HTTP/1.1\r\nHost:")
            abandoned.close()
            recovered_head, recovered_body = request(proxy_port, "/api/data")
            assert recovered_body == b'{"ok":true}'
            assert b"x-laghu: bypass-api" in recovered_head
            missing_head, _ = request(proxy_port, "/.laghu/image/not-a-hash")
            assert b" 404 " in missing_head.split(b"\r\n", 1)[0]
            asset_head, asset_body = request(
                proxy_port, f"/.laghu/image/{asset_key}"
            )
            assert asset_body == b"immutable-fixture"
            assert b"cache-control: public, max-age=31536000, immutable" in asset_head
            assert f'etag: "{asset_key}"'.encode() in asset_head
            for cached in (root / "cache").rglob("*"):
                if cached.is_file():
                    cached.write_bytes(b"corrupt")
            corrupt_head, corrupt_body = request(proxy_port, "/index.html")
            assert corrupt_body == BODY
            assert (
                b"x-laghu: pass" in corrupt_head
                or b"x-laghu: bypass-error" in corrupt_head
            )
            draining = socket.create_connection(("127.0.0.1", proxy_port), timeout=5)
            draining.sendall(
                b"GET /drain HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n"
            )
            time.sleep(0.1)
            queued_for_shutdown = socket.create_connection(
                ("127.0.0.1", proxy_port), timeout=5
            )
            queued_for_shutdown.sendall(
                b"GET /api/data HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n"
            )
            time.sleep(0.1)
            process.terminate()
            queued_response = read_open_socket(queued_for_shutdown)
            active_response = read_open_socket(draining)
            assert queued_response.startswith(b"http/1.1 503 "), queued_response
            assert active_response.startswith(b"http/1.1 200 "), active_response
            assert b"hello" in active_response, active_response
            assert process.wait(timeout=5) == 0
            logs = process.stderr.read().decode()
            assert '"event":"startup"' in logs
            assert '"event":"transaction"' in logs
            assert '"event":"readiness"' in logs
            assert '"state":"draining"' in logs
            assert '"state":"stopped"' in logs
            assert '"failure":"origin_timeout"' in logs
            assert '"failure":"queue_saturated"' in logs
            assert '"failure":"shutdown"' in logs
            assert "query-secret" not in logs
            assert "private-token" not in logs
            assert "private-cookie" not in logs
            for line in logs.splitlines():
                assert line.startswith("{") and line.endswith("}"), line
                json.loads(line)
            process.stderr.close()
            force_port = free_port()
            process = subprocess.Popen(
                [
                    str(executable),
                    "--listen",
                    f"127.0.0.1:{force_port}",
                    "--origin",
                    f"http://127.0.0.1:{origin_port}",
                    "--cache",
                    str(root / "cache"),
                    "--worker-queue",
                    str(root / "missing.queue"),
                    "--io-timeout",
                    "30",
                    "--drain-timeout",
                    "30",
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )
            for _ in range(50):
                try:
                    forced = socket.create_connection(
                        ("127.0.0.1", force_port), timeout=5
                    )
                    break
                except OSError:
                    time.sleep(0.05)
            else:
                raise AssertionError("forced-drain proxy did not start")
            forced.sendall(
                b"GET /hang HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n"
            )
            time.sleep(0.2)
            process.terminate()
            time.sleep(0.3)
            process.terminate()
            started = time.monotonic()
            assert process.wait(timeout=5) == 0
            assert time.monotonic() - started < 5
            read_open_socket(forced)
            forced_logs = process.stderr.read().decode()
            assert '"state":"forcing"' in forced_logs
            assert '"failure":"shutdown"' in forced_logs
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
            if process.stderr is not None:
                process.stderr.close()
            origin.shutdown()
    print("laghu proxy smoke passed")


if __name__ == "__main__":
    main()
