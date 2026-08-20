# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import http.server
import json
import os
import pathlib
import re
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse


BODY = b"<!doctype html>\n<html>  <head><!-- remove --></head>  <body>hello</body></html>"


def start_process(arguments, **options):
    process_arguments, config_path = yaml_runtime_arguments(arguments)
    process = subprocess.Popen(process_arguments, **options)
    process.laghu_config_path = config_path
    return process


def request_shutdown(process):
    process.terminate()


def wait_for_shutdown(process, timeout=5):
    status = process.wait(timeout=timeout)
    config_path = getattr(process, "laghu_config_path", None)
    if config_path:
        os.unlink(config_path)
    assert status == 0
    return status


def yaml_runtime_arguments(arguments):
    implicit_flags = {
        "--allow-api",
        "--image-beacon",
        "--critical-css-beacon",
        "--instrumentation-beacon",
        "--include-js-source-maps",
        "--rum-store-required",
    }
    pair_settings = {"--map-rewrite-domain", "--map-proxy-domain", "--shard-domain"}
    if len(arguments) < 2 or pathlib.Path(arguments[0]).name != "laghu":
        return arguments, None
    runtime = {}
    index = 1
    while index < len(arguments):
        option = arguments[index]
        assert option.startswith("--"), option
        key = option[2:].replace("-", "_")
        if option in implicit_flags:
            value = "true"
            index += 1
        elif option in pair_settings:
            assert index + 2 < len(arguments), option
            value = [arguments[index + 1], arguments[index + 2]]
            index += 3
        else:
            assert index + 1 < len(arguments), option
            value = arguments[index + 1]
            index += 2
        if option in pair_settings:
            if key not in runtime:
                runtime[key] = []
            runtime[key].append(value)
        elif key in runtime:
            if not isinstance(runtime[key], list):
                runtime[key] = [runtime[key]]
            runtime[key].append(value)
        else:
            runtime[key] = value
    lines = ["schema: 1", "runtime:"]
    for key, value in runtime.items():
        if isinstance(value, list):
            lines.append(f"  {key}:")
            for item in value:
                if isinstance(item, list):
                    lines.append("    - [" + ", ".join(json.dumps(part) for part in item) + "]")
                else:
                    lines.append(f"    - {json.dumps(item)}")
        else:
            lines.append(f"  {key}: {json.dumps(value)}")
    config = tempfile.NamedTemporaryFile(prefix="laghu-runtime-", suffix=".yaml", mode="w", delete=False)
    config.write("\n".join(lines) + "\n")
    config.close()
    return [arguments[0], "--config", config.name], config.name


class Origin(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    connections = 0
    connections_lock = threading.Lock()

    def setup(self):
        super().setup()
        with self.connections_lock:
            type(self).connections += 1

    def do_GET(self):
        request_path = urllib.parse.urlsplit(self.path).path
        if request_path == "/headers":
            body = json.dumps(
                {
                    name.lower(): self.headers.get_all(name)
                    for name in (
                        "Forwarded",
                        "X-Forwarded-For",
                        "X-Forwarded-Proto",
                        "X-Forwarded-Host",
                    )
                    if self.headers.get_all(name)
                },
                sort_keys=True,
            ).encode()
            content_type = "application/json"
        elif request_path == "/api/data":
            body = b'{"ok":true}'
            content_type = "application/json"
        elif request_path == "/lcp.html":
            body = (
                b'<html><body><nav><img src="/logo.png" width="40" '
                b'height="40"></nav><img src="/image.png" width="320" '
                b'height="240"></body></html>'
            )
            content_type = "text/html"
        elif request_path == "/hints.html":
            body = (
                b'<html><head><link rel="preconnect" '
                b'href="https://reserved.example.test"></head><body>'
                b'<script src="https://cdn.example.test/app.js?token=secret#part">'
                b"</script></body></html>"
            )
            content_type = "text/html"
        elif request_path == "/site.css":
            body = b"body { color: red; }"
            content_type = "text/css"
        elif request_path == "/app.js":
            body = b"function publicName(longLocal) { return longLocal + 1; }"
            content_type = "application/javascript"
        elif request_path == "/combine-one.js":
            body = (
                b" " * 160
                + b"console.log('combine one value'); console.log('combine one value again');"
            )
            content_type = "application/javascript"
        elif request_path == "/combine-two.js":
            body = (
                b" " * 160
                + b"console.log('combine two value'); console.log('combine two value again');"
            )
            content_type = "application/javascript"
        elif request_path == "/javascript-inline.html":
            body = b"<html><body><script>function inlinePublic(longLocal) { return longLocal + 1; }</script></body></html>"
            content_type = "text/html"
        elif request_path == "/javascript-csp-hash.html":
            body = (
                b'<html><head><meta http-equiv="Content-Security-Policy" '
                b'content="script-src \'sha256-YWJjZA==\'"></head><body>'
                b'<script>function cspProtected(longLocal) { return longLocal + 1; }</script>'
                b'</body></html>'
            )
            content_type = "text/html"
        elif request_path == "/javascript-external.html":
            body = (
                b'<html><body><script src="/app.js"></script><script>'
                b"function externalPageHelper(veryLongLocalArgument) { return "
                + b" + ".join([b"veryLongLocalArgument"] * 16)
                + b"; }</script></body></html>"
            )
            content_type = "text/html"
        elif request_path == "/javascript-combine.html":
            body = (
                b'<html><body><script src="/combine-one.js" data-laghu-combine="application-main"></script>\n'
                b'<script src="/combine-two.js" data-laghu-combine="application-main"></script></body></html>'
            )
            content_type = "text/html"
        elif request_path == "/javascript-outline.html":
            body = (
                b"<html><body><script>function outlinePublic(veryLongLocalArgument) { return "
                + b" + ".join([b"veryLongLocalArgument"] * 500)
                + b"; }</script></body></html>"
            )
            content_type = "text/html"
        elif request_path == "/trim-urls.html":
            body = (
                b'<html><body><audio src="http://example.test/asset.mp3?q=1#hero">'
                b'</audio><a href="/next">next</a></body></html>'
            )
            content_type = "text/html"
        elif request_path == "/private":
            body = BODY
            content_type = "text/html"
        elif request_path == "/encoded":
            body = b"encoded-origin"
            content_type = "text/html"
        elif request_path == "/image.png":
            body = b"not-decoded-without-worker"
            content_type = "image/png"
        elif request_path == "/partial":
            body = b"partial"
            content_type = "text/html"
        elif request_path == "/truncated":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", "100")
            self.end_headers()
            self.wfile.write(b"short")
            self.close_connection = True
            return
        elif request_path in ("/chunked", "/bad-chunk"):
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            if request_path == "/chunked":
                self.wfile.write(f"{len(BODY):x}\r\n".encode() + BODY + b"\r\n0\r\n\r\n")
            else:
                self.wfile.write(b"4\r\nno")
            self.wfile.flush()
            self.close_connection = True
            return
        elif request_path == "/slow":
            time.sleep(2)
            body = BODY
            content_type = "text/html"
        elif request_path == "/drain":
            time.sleep(0.4)
            body = BODY
            content_type = "text/html"
        elif request_path == "/hang":
            time.sleep(10)
            body = BODY
            content_type = "text/html"
        else:
            body = BODY
            content_type = "text/html"
        self.send_response(206 if request_path == "/partial" else 200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header(
            "ETag",
            '"trim-v1"' if request_path == "/trim-urls.html" else '"origin-v1"',
        )
        if request_path != "/pooled":
            self.send_header("Connection", "close")
        if request_path == "/hints.html":
            self.send_header("Link", "<https://origin.example.test>; rel=preload")
        if request_path == "/private":
            self.send_header("Cache-Control", "private")
        if request_path == "/encoded":
            self.send_header("Content-Encoding", "gzip")
        if request_path == "/partial":
            self.send_header("Content-Range", "bytes 0-6/20")
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()
        self.close_connection = request_path != "/pooled"

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()
        self.close_connection = True

    def log_message(self, *_args):
        pass


class QuietThreadingHTTPServer(http.server.ThreadingHTTPServer):
    request_queue_size = 128
    daemon_threads = True

    def handle_error(self, _request, _client_address):
        pass


def one_shot_tcp_server(payload=None, hold=0, connections=1):
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(connections)
    port = listener.getsockname()[1]

    def serve():
        accepted = []
        try:
            for _ in range(connections):
                connection, _ = listener.accept()
                accepted.append(connection)
                if payload is not None:
                    connection.sendall(payload)
            if hold:
                time.sleep(hold)
        finally:
            for connection in accepted:
                connection.close()
            listener.close()

    threading.Thread(target=serve, daemon=True).start()
    return port


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


class StatusOrigin(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    paths = []
    ready_status = 200
    stats_status = 200
    ready_body = (
        b'{"status":"ready","runtime":"ready","cache":"ready",'
        b'"workers":"ready","budgets":"ready","policy":"degraded",'
        b'"configured_workers":1,"healthy_workers":1}'
    )
    stats_body = (
        b'{"schema":"laghu-cache-stats-v1","backend":"file",'
        b'"capacity":{"bytes":10,"files":1},"usage":{"bytes":2,"files":1},'
        b'"requests":{"hits":1,"misses":2,"hit_ratio_ppm":333333},'
        b'"publications":0,"rejected_writes":0,"evictions":0,'
        b'"purges":{"url":0,"full":0,"artifacts":0,"bytes":0,'
        b'"generation":0,"last":0},"corrupt_removals":0,'
        b'"cleaner_active":false,"rebuilding":false,"last_maintenance":0}'
    )

    def do_GET(self):
        assert self.headers.get("X-Laghu-Purge-Token") == "0123456789abcdef"
        self.__class__.paths.append(self.path)
        if self.path == "/.laghu/ready":
            status, body = self.ready_status, self.ready_body
        elif self.path == "/.laghu/stats":
            status, body = self.stats_status, self.stats_body
        elif self.path.startswith("/.laghu/explain"):
            parsed = urllib.parse.urlparse(self.path)
            query = urllib.parse.parse_qs(parsed.query)
            target = query.get("path", ("/",))[0]
            if target is None:
                self.send_error(400)
                return
            body = json.dumps({
                "schema": "laghu-explain-v1",
                "target": target,
                "status": "ready",
                "source_hash": "abcd1234",
                "readiness": {
                    "runtime": "ready",
                    "cache": "ready",
                    "workers": "ready",
                },
                "hit_ratio_ppm": 333333,
                "recommendation": "ready for request",
            }, separators=(",", ":")).encode()
            status, body = 200, body
        else:
            self.send_error(404)
            return
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def log_message(self, *_args):
        pass


class PurgeOrigin(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    requests = []
    response_status = 202
    response_body = b'{"status":"accepted","matched_artifacts":7}'
    content_type = "application/json"

    def do_PURGE(self):
        assert self.headers.get("X-Laghu-Purge-Token") == "0123456789abcdef"
        self.__class__.requests.append((self.path, self.headers.get("Host")))
        self.send_response(self.response_status)
        self.send_header("Content-Type", self.content_type)
        self.send_header("Content-Length", str(len(self.response_body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(self.response_body)
        self.close_connection = True

    def log_message(self, *_args):
        pass


class BenchOrigin(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    requests = []
    status_for_path = {
        "/": (200, b"ok", "text/plain"),
        "/error": (500, b"error", "text/plain"),
    }
    chunked_paths = set()

    def do_GET(self):
        status, body, content_type = self.__class__.status_for_path.get(
            self.path, (200, b"ok", "text/plain")
        )
        self.__class__.requests.append(self.path)
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        if self.path in self.__class__.chunked_paths:
            self.send_header("Transfer-Encoding", "chunked")
        else:
            self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if self.path in self.__class__.chunked_paths:
            self.wfile.write(f"{len(body):x}\r\n".encode() + body + b"\r\n0\r\n\r\n")
        else:
            self.wfile.write(body)
        self.close_connection = True

    def log_message(self, *_args):
        pass


def status_smoke(executable, root):
    token = root / "status.token"
    token.write_text("0123456789abcdef\n")
    token.chmod(0o600)
    def run(port, *options, scheme="http"):
        return subprocess.run(
            [str(executable), "status", f"{scheme}://127.0.0.1:{port}",
             "--token-file", str(token), *options],
            capture_output=True, text=True,
        )

    def serve(handler):
        server = QuietThreadingHTTPServer(("127.0.0.1", 0), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        return server

    server = serve(StatusOrigin)
    try:
        result = run(server.server_port, "--json")
        assert result.returncode == 0
        payload = json.loads(result.stdout)
        assert payload["schema"] == "laghu-status-v1"
        assert payload["ready"]["status"] == "ready"
        assert payload["stats"]["requests"] == {
            "hits": 1, "misses": 2, "hit_ratio_ppm": 333333,
        }
        assert "0123456789abcdef" not in result.stdout + result.stderr
    finally:
        server.shutdown()
        server.server_close()

    server = serve(StatusOrigin)
    try:
        result = run(server.server_port)
        assert result.returncode == 0
        assert result.stdout == "ready: ready\ncache: hits=1 misses=2 usage=2 capacity=10\n"
    finally:
        server.shutdown()
        server.server_close()

    for code in (403, 503, 418):
        handler = type("StatusFailure", (StatusOrigin,), {
            "ready_status": code, "stats_status": code,
            "ready_body": b"{}", "stats_body": b"{}",
            "paths": [],
        })
        server = serve(handler)
        try:
            result = run(server.server_port, "--json")
            assert result.returncode == {403: 3, 503: 6, 418: 7}[code]
            if code == 503:
                assert handler.paths == ["/.laghu/ready", "/.laghu/stats"]
            assert "0123456789abcdef" not in result.stdout + result.stderr
        finally:
            server.shutdown()
            server.server_close()

    handler = type("MalformedStatus", (StatusOrigin,), {
        "ready_body": b'{"status":"ready","status":"bad"}',
    })
    server = serve(handler)
    try:
        result = run(server.server_port, "--json")
        assert result.returncode == 5
    finally:
        server.shutdown()
        server.server_close()

    result = run(free_port(), "--json")
    assert result.returncode == 4
    timeout_port = one_shot_tcp_server(hold=2, connections=2)
    result = run(timeout_port, "--timeout", "1", "--json")
    assert result.returncode == 4
    token.chmod(0o644)
    result = run(free_port(), "--json")
    assert result.returncode == 2
    token.chmod(0o600)
    token.write_text("a" * 256 + "\n")
    result = run(free_port(), "--json")
    assert result.returncode == 4
    token.write_text("0123456789abcdef\n")
    result = subprocess.run(
        [str(executable), "status", "http://127.0.0.1/path", "--token-file", str(token)],
        capture_output=True, text=True,
    )
    assert result.returncode == 2
    token_link = root / "status-link"
    token_link.symlink_to(token)
    result = subprocess.run(
        [str(executable), "status", "http://127.0.0.1", "--token-file", str(token_link)],
        capture_output=True, text=True,
    )
    assert result.returncode == 2
    result = subprocess.run(
        [str(executable), "status", "http://[::1]:9", "--token-file", str(token)],
        capture_output=True, text=True,
    )
    assert result.returncode == 4
    ca_file, server_file, server_key = create_tls_certificate(root)
    server = serve(StatusOrigin)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(server_file, server_key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    try:
        result = run(server.server_port, "--ca-file", str(ca_file), "--json", scheme="https")
        assert result.returncode == 0
    finally:
        server.shutdown()
        server.server_close()


def doctor_smoke(executable, root):
    token = root / "doctor.token"
    token.write_text("0123456789abcdef\n")
    token.chmod(0o600)

    def run(port, *options):
        return subprocess.run(
            [str(executable), "doctor", f"http://127.0.0.1:{port}",
             "--token-file", str(token), *options],
            capture_output=True, text=True,
        )

    server = QuietThreadingHTTPServer(("127.0.0.1", 0), StatusOrigin)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        result = run(server.server_port, "--json")
        assert result.returncode == 0
        payload = json.loads(result.stdout)
        assert payload["schema"] == "laghu-doctor-v1"
        assert payload["runtime"] == "ready"
        assert payload["cache"] == "ready"
        assert payload["workers"] == "ready"
        assert payload["budgets"] == "ready"
        assert payload["policy"] == "degraded"
        assert payload["stats"]["requests"]["hits"] == 1
        assert "0123456789abcdef" not in result.stdout + result.stderr
        result = run(server.server_port)
        assert result.returncode == 0
        assert result.stdout == (
            "runtime: ready\ncache: ready\nworkers: ready\nbudgets: ready\n"
            "policy: degraded\ncache_stats: hits=1 misses=2 usage=2 capacity=10\n"
        )
    finally:
        server.shutdown()
        server.server_close()

    result = run(free_port(), "--json")
    assert result.returncode == 4


def purge_smoke(executable, root):
    token = root / "purge-client.token"
    token.write_text("0123456789abcdef\n")
    token.chmod(0o600)

    def serve(handler):
        server = QuietThreadingHTTPServer(("127.0.0.1", 0), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        return server

    def run(port, url="/site.css?tenant=blue", *options, scheme="http"):
        return subprocess.run(
            [str(executable), "purge", f"{scheme}://127.0.0.1:{port}{url}",
             "--token-file", str(token), *options],
            capture_output=True, text=True,
        )

    server = serve(PurgeOrigin)
    try:
        result = run(server.server_port)
        assert result.returncode == 0
        assert result.stdout == "purge: accepted matched_artifacts=7\n"
        assert PurgeOrigin.requests == [
            ("/site.css?tenant=blue", f"127.0.0.1:{server.server_port}"),
        ]
        assert "0123456789abcdef" not in result.stdout + result.stderr
    finally:
        server.shutdown()
        server.server_close()

    handler = type("PurgeJson", (PurgeOrigin,), {"requests": []})
    server = serve(handler)
    try:
        result = run(server.server_port, "/", "--json")
        assert result.returncode == 0
        assert json.loads(result.stdout) == {
            "schema": "laghu-purge-v1", "status": "accepted",
            "matched_artifacts": 7,
        }
        assert handler.requests == [("/", f"127.0.0.1:{server.server_port}")]
    finally:
        server.shutdown()
        server.server_close()

    for code, expected in ((400, 2), (401, 3), (403, 3), (404, 7),
                           (405, 7), (429, 8), (503, 6), (418, 7)):
        handler = type("PurgeFailure", (PurgeOrigin,), {
            "requests": [], "response_status": code,
            "response_body": b"not JSON", "content_type": "text/plain",
        })
        server = serve(handler)
        try:
            result = run(server.server_port, "/site.css", "--json")
            assert result.returncode == expected
            assert "0123456789abcdef" not in result.stdout + result.stderr
        finally:
            server.shutdown()
            server.server_close()

    handler = type("MalformedPurge", (PurgeOrigin,), {
        "requests": [],
        "response_body": b'{"status":"accepted","status":"bad",'
                         b'"matched_artifacts":7}',
    })
    server = serve(handler)
    try:
        result = run(server.server_port, "/site.css", "--json")
        assert result.returncode == 5
        assert "0123456789abcdef" not in result.stdout + result.stderr
    finally:
        server.shutdown()
        server.server_close()

    for url in ("http://127.0.0.1/path#fragment",
                "http://token@127.0.0.1/path",
                "http://127.0.0.1//path",
                "http://127.0.0.1/path?laghu=purge",
                "http://127.0.0.1/path\r\nHost: attacker",
                "http://127.0.0.1/" + "x" * 1024):
        result = subprocess.run(
            [str(executable), "purge", url, "--token-file", str(token)],
            capture_output=True, text=True,
        )
        assert result.returncode == 2
        assert "0123456789abcdef" not in result.stdout + result.stderr

    ca_file, server_file, server_key = create_tls_certificate(root)
    handler = type("PurgeTls", (PurgeOrigin,), {"requests": []})
    server = serve(handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(server_file, server_key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    try:
        result = run(server.server_port, "/site.css", "--ca-file", str(ca_file),
                     "--json", scheme="https")
        assert result.returncode == 0
    finally:
        server.shutdown()
        server.server_close()


def migrate_smoke(executable, root):
    source = root / "legacy.conf"
    source.write_text(
        "\n".join(
            [
                "# legacy mod_pagespeed sample",
                "pagespeed on;",
                "ModPagespeed Off;",
                "pagespeed RewriteLevel CoreFilters;",
                "pagespeed EnableFilters rewrite_images,combine_css,Image;",
                "pagespeed Disallow \"/private/*\";",
                "pagespeed FileCachePath file:///tmp/legacy-cache;",
                "pagespeed AllowResources \"/*\";",
                "modpagespeed inplaceresourceoptimization On;",
                "modpagespeed maprewritedomain https://public.test https://origin.test;",
                "modpagespeed mapproxyDomain https://cdn.public.test https://cdn.origin.test;",
                "modpagespeed shardDomain https://shard.test https://shard1.test,https://shard2.test;",
                "modpagespeed unknownlegacy on;",
                "mod_pagespeed InPlaceOptimizeForBrowser Off;",
                "mod_pagespeed imagerecompressquality 81;",
                "Location /pagespeed_admin {",
                '    ProxyPass "/pagespeed_statistics" "http://127.0.0.1/status"',
                "}",
                "Location /pagespeed_stats {",
                '    ProxyPass "/pagespeed_statistics" "http://127.0.0.1/status"',
                "}",
                "PageSpeedFilters=\"RewriteImages,inlinecss\";",
            ]
        ),
        encoding="utf-8",
    )
    expected = "\n".join(
        [
            "# legacy mod_pagespeed sample",
            "laghu on;",
            "laghu off;",
            "laghu preset balanced;",
            "laghu enable image_lossless,image_metadata,image_dimensions,image_responsive,image_lazyload,resource_combine,image_modern;",
            "laghu disallow /private/*;",
            "laghu file_cache_backend file:///tmp/legacy-cache/laghu;",
            "laghu allow_resources /*;",
            "laghu enable image_modern;",
            "laghu map_rewrite_domain https://public.test https://origin.test;",
            "laghu map_proxy_domain https://cdn.public.test https://cdn.origin.test;",
            "laghu shard_domain https://shard.test https://shard1.test,https://shard2.test;",
            "# unsupported legacy directive omitted by laghu migrate; manual review required",
            "laghu disable image_modern;",
            "laghu image_quality 81;",
            "Location /.laghu/console {",
            '    ProxyPass "/.laghu/stats" "http://127.0.0.1/status"',
            "}",
            "Location /.laghu/stats {",
            '    ProxyPass "/.laghu/stats" "http://127.0.0.1/status"',
            "}",
            "laghu query_filter_overrides on;",
            "laghuFilters=+image_lossless,+image_metadata,+image_dimensions,+image_responsive,+image_lazyload,+resource_inline;",
        ]
    )
    result = subprocess.run(
        [str(executable), "migrate", str(source)],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"laghu migrate returned {result.returncode}", file=sys.stderr)
        print(f"stdout: {result.stdout}", file=sys.stderr)
        print(f"stderr: {result.stderr}", file=sys.stderr)
    assert result.returncode == 0
    assert result.stdout.splitlines() == expected.splitlines()

    result = subprocess.run(
        [str(executable), "migrate", str(root / "missing-legacy.conf")],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 4

    result = subprocess.run(
        [str(executable), "migrate"],
        input=(
            "pagespeed on;\n"
            "location /pagespeed_console { return 200; }\n"
            "location /pagespeed_stats { return 200; }\n"
            "PageSpeedFilters=\"RemoveComments\";\n"
        ),
        text=True,
        capture_output=True,
    )
    assert result.returncode == 0
    assert result.stdout == (
        "laghu on;\n"
        "location /.laghu/metrics { return 200; }\n"
        "location /.laghu/stats { return 200; }\n"
        "laghu query_filter_overrides on;\n"
        "laghuFilters=+html_minify;\n"
    )


def explain_smoke(executable, root):
    token = root / "explain.token"
    token.write_text("0123456789abcdef\n")
    token.chmod(0o600)

    def run(port, *options, path="/index.html", scheme="http"):
        return subprocess.run(
            [str(executable), "explain", f"{scheme}://127.0.0.1:{port}{path}",
             "--token-file", str(token), *options],
            capture_output=True, text=True,
        )

    def serve(handler):
        server = QuietThreadingHTTPServer(("127.0.0.1", 0), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        return server

    server = serve(StatusOrigin)
    try:
        result = run(server.server_port, "--json")
        assert result.returncode == 0
        payload = json.loads(result.stdout)
        assert payload["schema"] == "laghu-explain-v1"
        assert payload["target"] == "/index.html"
        assert payload["status"] == "ready"
        assert payload["source_hash"] == "abcd1234"
        assert payload["readiness"] == {
            "runtime": "ready",
            "cache": "ready",
            "workers": "ready",
        }
        assert payload["hit_ratio_ppm"] == 333333
        assert payload["recommendation"] == "ready for request"
        assert "0123456789abcdef" not in result.stdout + result.stderr

        result = run(server.server_port, path="/index.html?tenant=blue")
        assert result.returncode == 0
        assert "target: /index.html?tenant=blue" in result.stdout
        assert "runtime: ready" in result.stdout
        assert "recommendation: ready for request" in result.stdout
        assert "0123456789abcdef" not in result.stdout + result.stderr

        result = run(server.server_port, path="/index.html?tenant=blue&tenant=red")
        assert result.returncode == 0
        assert "target: /index.html?tenant=blue&tenant=red" in result.stdout

        for url in ("http://127.0.0.1/path#fragment",
                    "http://token@127.0.0.1/path",
                    "http://127.0.0.1/" + "x" * 1024,
                    "http://127.0.0.1//path"):
            result = subprocess.run(
                [str(executable), "explain", url, "--token-file", str(token)],
                capture_output=True, text=True,
            )
            assert result.returncode == 2
            assert "0123456789abcdef" not in result.stdout + result.stderr

        ca_file, server_file, server_key = create_tls_certificate(root)
        tls_server = QuietThreadingHTTPServer(("127.0.0.1", 0), StatusOrigin)
        tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls_context.load_cert_chain(server_file, server_key)
        tls_server.socket = tls_context.wrap_socket(
            tls_server.socket, server_side=True
        )
        thread = threading.Thread(
            target=tls_server.serve_forever, daemon=True
        )
        thread.start()
        try:
            result = run(tls_server.server_port, "--json", scheme="https")
            assert result.returncode == 4
            result = run(
                tls_server.server_port, "--ca-file", str(ca_file), "--json",
                path="/index.html", scheme="https"
            )
            assert result.returncode == 0
            payload = json.loads(result.stdout)
            assert payload["schema"] == "laghu-explain-v1"
        finally:
            tls_server.shutdown()
            tls_server.server_close()
    finally:
        server.shutdown()
        server.server_close()


def bench_smoke(executable, root):
    def run(port, *options, path="/", scheme="http"):
        return subprocess.run(
            [str(executable), "bench", f"{scheme}://127.0.0.1:{port}{path}",
             *options],
            capture_output=True, text=True,
        )

    def serve(handler):
        server = QuietThreadingHTTPServer(("127.0.0.1", 0), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        return server

    server = serve(BenchOrigin)
    try:
        result = run(server.server_port, "--requests", "3")
        assert result.returncode == 0
        assert "bench: target=/" in result.stdout
        assert "success=3" in result.stdout
        assert "failures=0" in result.stdout

        result = run(
            server.server_port, "--requests", "2", "--json", path="/error",
        )
        assert result.returncode == 7
        payload = json.loads(result.stdout)
        assert payload["schema"] == "laghu-bench-v1"
        assert payload["target"] == "/error"
        assert payload["requests"] == 2
        assert payload["success"] == 0
        assert payload["failures"] == 2
        assert payload["status_5xx"] == 2

        handler = type("MalformedBench", (BenchOrigin,), {
            "requests": [],
            "status_for_path": {
                "/": (200, b"ok", "text/plain"),
                "/chunked": (200, b"partial", "text/plain"),
            },
            "chunked_paths": {"/chunked"},
        })
        server.shutdown()
        server.server_close()
        server = serve(handler)
        result = run(server.server_port, "--requests", "1", path="/chunked")
        assert result.returncode == 5

        for url in ("http://127.0.0.1/path#fragment",
                    "http://127.0.0.1//path",
                    "http://127.0.0.1/" + "x" * 1024):
            result = subprocess.run(
                [str(executable), "bench", url, "--requests", "1"],
                capture_output=True, text=True,
            )
            assert result.returncode == 2

        BenchOrigin.status_for_path["/timeout"] = (200, b"ok", "text/plain")
        result = subprocess.run(
            [str(executable), "bench", f"http://127.0.0.1:{server.server_port}/timeout",
             "--requests", "1", "--timeout", "1"],
            capture_output=True, text=True,
        )
        assert result.returncode == 0
    finally:
        server.shutdown()
        server.server_close()


def create_tls_certificate(root):
    ca_key = root / "ca.key"
    ca_file = root / "ca.pem"
    server_key = root / "server.key"
    request_file = root / "server.csr"
    server_file = root / "server.pem"
    openssl = shutil.which("openssl")
    if openssl is None:
        raise AssertionError("OpenSSL executable is required for TLS fixtures")
    command_environment = os.environ.copy()
    quiet = {
        "stdout": subprocess.DEVNULL,
        "stderr": subprocess.DEVNULL,
        "env": command_environment,
    }
    subprocess.run(
        [
            openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(ca_key), "-out", str(ca_file), "-days", "1",
            "-subj", "/CN=Laghu Test CA", "-addext",
            "basicConstraints=critical,CA:TRUE", "-addext",
            "keyUsage=critical,keyCertSign,cRLSign",
        ],
        check=True,
        **quiet,
    )
    subprocess.run(
        [
            openssl, "req", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(server_key), "-out", str(request_file),
            "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1",
        ],
        check=True,
        **quiet,
    )
    subprocess.run(
        [
            openssl, "x509", "-req", "-in", str(request_file),
            "-CA", str(ca_file), "-CAkey", str(ca_key), "-CAcreateserial",
            "-out", str(server_file), "-days", "1", "-copy_extensions", "copy",
        ],
        check=True,
        **quiet,
    )
    return ca_file, server_file, server_key


def create_dns_tls_certificate(root, name, prefix):
    ca_file = root / "ca.pem"
    ca_key = root / "ca.key"
    server_key = root / f"{prefix}.key"
    request_file = root / f"{prefix}.csr"
    server_file = root / f"{prefix}.pem"
    openssl = shutil.which("openssl")
    if openssl is None:
        raise AssertionError("OpenSSL executable is required for TLS fixtures")
    quiet = {"stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL}
    subprocess.run(
        [
            openssl, "req", "-newkey", "rsa:2048", "-nodes", "-keyout",
            str(server_key), "-out", str(request_file), "-subj", f"/CN={name}",
            "-addext", f"subjectAltName=DNS:{name}",
        ], check=True, **quiet
    )
    subprocess.run(
        [
            openssl, "x509", "-req", "-in", str(request_file), "-CA",
            str(ca_file), "-CAkey", str(ca_key), "-CAcreateserial", "-out",
            str(server_file), "-days", "1", "-copy_extensions", "copy",
        ], check=True, **quiet
    )
    return server_file, server_key


def request(port, path, method="GET", headers=None, body=b"", timeout=10,
            include_interim=False, tls_context=None, server_hostname="127.0.0.1",
            host="example.test"):
    sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    if tls_context is not None:
        sock = tls_context.wrap_socket(sock, server_hostname=server_hostname)
    extra = ""
    for name, value in (headers or {}).items():
        extra += f"{name}: {value}\r\n"
    if body:
        extra += f"Content-Length: {len(body)}\r\n"
    sock.sendall(
        f"{method} {path} HTTP/1.1\r\nHost: {host}\r\n{extra}Connection: close\r\n\r\n".encode()
        + body
    )
    if tls_context is None:
        sock.shutdown(socket.SHUT_WR)
    chunks = []
    expected_length = None
    while True:
        try:
            chunk = sock.recv(65536)
        except TimeoutError as error:
            response = b"".join(chunks)
            raise AssertionError(
                f"response timeout after {len(response)} bytes: {response!r}"
            ) from error
        except OSError:
            if chunks:
                break
            raise
        if not chunk:
            break
        chunks.append(chunk)
        response = b"".join(chunks)
        if expected_length is None and b"\r\n\r\n" in response:
            response_head, response_body = response.split(b"\r\n\r\n", 1)
            for line in response_head.split(b"\r\n")[1:]:
                name, separator, value = line.partition(b":")
                if separator and name.lower() == b"content-length":
                    expected_length = int(value.strip())
                    break
        else:
            response_body = (
                response.split(b"\r\n\r\n", 1)[1]
                if b"\r\n\r\n" in response
                else b""
            )
        if expected_length is not None and len(response_body) >= expected_length:
            break
    sock.close()
    response = b"".join(chunks)
    if b"\r\n\r\n" not in response:
        raise OSError("incomplete HTTP response")
    interim = []
    while True:
        head, body = response.split(b"\r\n\r\n", 1)
        status = head.split(b"\r\n", 1)[0]
        if not status.startswith(b"HTTP/") or not status[9:12].startswith(b"1"):
            break
        interim.append(head.lower())
        if b"\r\n\r\n" not in body:
            raise OSError("missing final HTTP response")
        response = body
    if include_interim:
        return head.lower(), body, interim
    return head.lower(), body


def raw_request(port, payload):
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.sendall(payload)
    sock.shutdown(socket.SHUT_WR)
    chunks = []
    while True:
        try:
            chunk = sock.recv(65536)
        except OSError:
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
    javascript_worker = pathlib.Path(sys.argv[3]).resolve()
    warm_attempts = 200 if os.name == "nt" else 50
    origin_port = free_port()
    origin = QuietThreadingHTTPServer(("127.0.0.1", origin_port), Origin)
    thread = threading.Thread(target=origin.serve_forever, daemon=True)
    thread.start()
    with tempfile.TemporaryDirectory(prefix="laghu-proxy-") as directory:
        root = pathlib.Path(directory)
        status_smoke(executable, root)
        doctor_smoke(executable, root)
        purge_smoke(executable, root)
        migrate_smoke(executable, root)
        explain_smoke(executable, root)
        bench_smoke(executable, root)
        (root / "cache").mkdir()
        subprocess.run(
            [str(javascript_worker), "--init", str(root / "javascript.queue"),
             str(root / "cache")], check=True
        )
        javascript_process = start_process(
            [str(javascript_worker), "--serve", str(root / "javascript.queue"),
             str(root / "cache")], stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        asset_key = "a" * 64
        subprocess.run(
            [str(cache_fixture), str(root / "cache"), asset_key], check=True
        )
        ca_file, server_file, server_key = create_tls_certificate(root)
        tls_origin_port = free_port()
        tls_origin = QuietThreadingHTTPServer(("127.0.0.1", tls_origin_port), Origin)
        tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls_context.load_cert_chain(server_file, server_key)
        tls_origin.socket = tls_context.wrap_socket(tls_origin.socket, server_side=True)
        tls_thread = threading.Thread(target=tls_origin.serve_forever, daemon=True)
        tls_thread.start()
        tls_proxy_port = free_port()
        tls_process = start_process(
            [
                str(executable), "--listen", f"127.0.0.1:{tls_proxy_port}",
                "--origin", f"https://127.0.0.1:{tls_origin_port}",
                "--origin-ca-file", str(ca_file), "--cache", str(root / "cache"),
                "--worker-queue", str(root / "missing.queue"),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        tls_started = False
        tls_error = None
        try:
            for _ in range(warm_attempts):
                try:
                    tls_head, tls_body = request(tls_proxy_port, "/api/data")
                    tls_started = True
                    break
                except OSError as error:
                    tls_error = error
                    time.sleep(0.05)
        finally:
            request_shutdown(tls_process)
            tls_process.wait(timeout=5)
            tls_logs = tls_process.stderr.read().decode()
            tls_process.stderr.close()
        if not tls_started:
            raise AssertionError(f"TLS proxy did not start ({tls_process.returncode}): {tls_error}: {tls_logs}")
        assert b" 200 " in tls_head.split(b"\r\n", 1)[0], (tls_head, tls_logs)
        assert tls_body == b'{"ok":true}'
        downstream_tls_port = free_port()
        downstream_tls_process = start_process(
            [
                str(executable), "--listen", f"127.0.0.1:{downstream_tls_port}",
                "--origin", f"http://127.0.0.1:{origin_port}", "--cache",
                str(root / "cache"), "--worker-queue", str(root / "missing.queue"),
                "--tls-certificate", str(server_file), "--tls-private-key",
                str(server_key), "--forwarded-headers", "both",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        downstream_client_context = ssl.create_default_context(cafile=ca_file)
        try:
            downstream_error = None
            for _ in range(warm_attempts):
                try:
                    downstream_head, downstream_body = request(
                        downstream_tls_port, "/api/data", tls_context=downstream_client_context
                    )
                    break
                except OSError as error:
                    downstream_error = error
                    time.sleep(0.05)
            else:
                raise AssertionError(f"downstream TLS proxy did not start: {downstream_error}")
            assert b" 200 " in downstream_head.split(b"\r\n", 1)[0]
            assert downstream_body == b'{"ok":true}'
            health_head, health_body = request(
                downstream_tls_port, "/.laghu/health", tls_context=downstream_client_context
            )
            assert b" 200 " in health_head.split(b"\r\n", 1)[0]
            assert b'"status":"ok"' in health_body
            _, forwarded_body = request(
                downstream_tls_port, "/headers", tls_context=downstream_client_context
            )
            forwarded = json.loads(forwarded_body)
            assert forwarded["x-forwarded-proto"] == ["https"]
            assert "proto=https" in forwarded["forwarded"][0]
            try:
                request(downstream_tls_port, "/api/data", tls_context=ssl.create_default_context())
                raise AssertionError("downstream TLS unexpectedly trusted an unknown CA")
            except ssl.SSLCertVerificationError:
                pass
        finally:
            request_shutdown(downstream_tls_process)
            downstream_tls_process.wait(timeout=5)
            downstream_tls_logs = downstream_tls_process.stderr.read().decode()
            downstream_tls_process.stderr.close()
        assert downstream_tls_process.returncode == 0, downstream_tls_logs
        one_certificate, one_key = create_dns_tls_certificate(root, "one.example.test", "one")
        two_certificate, two_key = create_dns_tls_certificate(root, "two.example.test", "two")
        one_root = root / "one-site"
        two_root = root / "two-site"
        one_root.mkdir()
        two_root.mkdir()
        (one_root / "index.html").write_text("one site")
        (two_root / "index.html").write_text("two site")
        sni_port = free_port()
        sni_config = root / "sni-reload.yaml"
        sni_pid = root / "sni-reload.pid"

        def write_sni_config(redirect):
            sni_config.write_text(
                "schema: 1\n"
                "runtime:\n"
                f"  listen: 127.0.0.1:{sni_port}\n"
                f"  origin: http://127.0.0.1:{origin_port}\n"
                f"  cache: {root / 'cache'}\n"
                f"  worker_queue: {root / 'missing.queue'}\n"
                f"  pid_file: {sni_pid}\n"
                "sites:\n"
                "  - host: one.example.test\n"
                f"    document_root: {one_root}\n"
                f"    tls_certificate: {one_certificate}\n"
                f"    tls_private_key: {one_key}\n"
                "    laghu:\n"
                "      preset: safe\n"
                "    service:\n"
                "      javascript_target: defaults\n"
                "    routes:\n"
                "      - match: exact\n"
                "        pattern: /route\n"
                f"        redirect: {redirect}\n"
                "        laghu:\n"
                "          mode: off\n"
                "  - host: two.example.test\n"
                f"    document_root: {two_root}\n"
                f"    tls_certificate: {two_certificate}\n"
                f"    tls_private_key: {two_key}\n"
            )

        write_sni_config("/before")
        sni_process = subprocess.Popen(
            [str(executable), "--config", str(sni_config)], stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        sni_context = ssl.create_default_context(cafile=ca_file)
        try:
            for _ in range(warm_attempts):
                try:
                    one_head, one_body = request(
                        sni_port, "/", tls_context=sni_context,
                        server_hostname="one.example.test", host="one.example.test"
                    )
                    two_head, two_body = request(
                        sni_port, "/", tls_context=sni_context,
                        server_hostname="two.example.test", host="two.example.test"
                    )
                    break
                except OSError:
                    time.sleep(0.05)
            else:
                raise AssertionError("SNI virtual hosts did not start")
            assert b" 200 " in one_head.split(b"\r\n", 1)[0] and one_body == b"one site"
            assert b" 200 " in two_head.split(b"\r\n", 1)[0] and two_body == b"two site"
            route_head, _ = request(
                sni_port, "/route", tls_context=sni_context,
                server_hostname="one.example.test", host="one.example.test"
            )
            assert b"location: /before" in route_head
            write_sni_config("/after")
            reload_result = subprocess.run(
                [str(executable), "reload", "--config", str(sni_config)],
                capture_output=True, text=True,
            )
            assert reload_result.returncode == 0, reload_result.stderr
            for _ in range(warm_attempts):
                route_head, _ = request(
                    sni_port, "/route", tls_context=sni_context,
                    server_hostname="one.example.test", host="one.example.test"
                )
                if b"location: /after" in route_head:
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("reload did not atomically publish the replacement")
            sni_config.write_text("schema: 1\nruntime: invalid\n")
            invalid_reload = subprocess.run(
                [str(executable), "reload", "--config", str(sni_config)],
                capture_output=True, text=True,
            )
            assert invalid_reload.returncode != 0
            route_head, _ = request(
                sni_port, "/route", tls_context=sni_context,
                server_hostname="one.example.test", host="one.example.test"
            )
            assert b"location: /after" in route_head
            os.kill(sni_process.pid, signal.SIGHUP)
            time.sleep(0.1)
            route_head, _ = request(
                sni_port, "/route", tls_context=sni_context,
                server_hostname="one.example.test", host="one.example.test"
            )
            assert b"location: /after" in route_head
        finally:
            request_shutdown(sni_process)
            sni_process.wait(timeout=5)
            sni_logs = sni_process.stderr.read().decode()
            sni_process.stderr.close()
        assert sni_process.returncode == 0, sni_logs
        assert '"state":"applied"' in sni_logs
        assert '"state":"retained"' in sni_logs
        invalid_arguments, invalid_config = yaml_runtime_arguments(
            [
                str(executable), "--listen", f"127.0.0.1:{free_port()}",
                "--origin", f"http://127.0.0.1:{origin_port}", "--cache", str(root / "cache"),
                "--worker-queue", str(root / "missing.queue"), "--tls-certificate", str(server_file),
                "--tls-private-key", str(root / "ca.key"),
            ]
        )
        try:
            invalid_downstream_tls = subprocess.run(invalid_arguments, capture_output=True, text=True)
        finally:
            if invalid_config:
                os.unlink(invalid_config)
        assert invalid_downstream_tls.returncode != 0
        assert '"failure":"downstream_tls"' in invalid_downstream_tls.stderr
        unknown_ca_port = free_port()
        unknown_ca_process = start_process(
            [
                str(executable), "--listen", f"127.0.0.1:{unknown_ca_port}",
                "--origin", f"https://127.0.0.1:{tls_origin_port}",
                "--cache", str(root / "cache"), "--worker-queue",
                str(root / "missing.queue"),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        try:
            for _ in range(warm_attempts):
                try:
                    unknown_head, unknown_body = request(
                        unknown_ca_port, "/api/data"
                    )
                    break
                except OSError:
                    time.sleep(0.05)
            else:
                raise AssertionError("unknown-CA proxy did not start")
            assert b" 502 " in unknown_head.split(b"\r\n", 1)[0]
            assert unknown_body == b""
        finally:
            request_shutdown(unknown_ca_process)
            unknown_ca_process.wait(timeout=5)
            unknown_logs = unknown_ca_process.stderr.read().decode()
            assert '"failure":"origin_tls"' in unknown_logs
            unknown_ca_process.stderr.close()
        mismatch_port = free_port()
        mismatch_process = start_process(
            [
                str(executable), "--listen", f"127.0.0.1:{mismatch_port}",
                "--origin", f"https://localhost:{tls_origin_port}",
                "--origin-ca-file", str(ca_file), "--cache", str(root / "cache"),
                "--worker-queue", str(root / "missing.queue"),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        try:
            for _ in range(warm_attempts):
                try:
                    mismatch_head, mismatch_body = request(mismatch_port, "/api/data")
                    break
                except OSError:
                    time.sleep(0.05)
            else:
                raise AssertionError("hostname-mismatch proxy did not start")
            assert b" 502 " in mismatch_head.split(b"\r\n", 1)[0]
            assert mismatch_body == b""
        finally:
            request_shutdown(mismatch_process)
            mismatch_process.wait(timeout=5)
            mismatch_logs = mismatch_process.stderr.read().decode()
            assert '"failure":"origin_tls"' in mismatch_logs
            assert "127.0.0.1" not in mismatch_logs
            mismatch_process.stderr.close()
        for broken_port, expected_failure in (
            (one_shot_tcp_server(hold=2), "origin_timeout"),
            (one_shot_tcp_server(payload=b"not tls\r\n"), "origin_tls"),
        ):
            broken_proxy_port = free_port()
            broken_process = start_process(
                [
                    str(executable), "--listen",
                    f"127.0.0.1:{broken_proxy_port}", "--origin",
                    f"https://127.0.0.1:{broken_port}", "--origin-ca-file",
                    str(ca_file), "--cache", str(root / "cache"),
                    "--worker-queue", str(root / "missing.queue"),
                    "--connect-timeout", "1",
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )
            try:
                for _ in range(warm_attempts):
                    try:
                        broken_head, broken_body = request(
                            broken_proxy_port, "/api/data"
                        )
                        break
                    except OSError:
                        time.sleep(0.05)
                else:
                    raise AssertionError("broken-TLS proxy did not start")
                assert b" 502 " in broken_head.split(b"\r\n", 1)[0]
                assert broken_body == b""
            finally:
                request_shutdown(broken_process)
                broken_process.wait(timeout=5)
                broken_logs = broken_process.stderr.read().decode()
                assert f'"failure":"{expected_failure}"' in broken_logs
                broken_process.stderr.close()
        tls_origin.shutdown()
        invalid_port = free_port()
        invalid_arguments, invalid_config = yaml_runtime_arguments(
            [
                str(executable), "--listen", f"127.0.0.1:{invalid_port}", "--origin",
                f"http://127.0.0.1:{origin_port}", "--cache", str(root / "missing-cache"),
                "--worker-queue", str(root / "missing.queue"),
            ]
        )
        try:
            invalid = subprocess.run(invalid_arguments, capture_output=True, timeout=5)
        finally:
            if invalid_config:
                os.unlink(invalid_config)
        assert invalid.returncode == 1
        invalid_log = invalid.stderr.decode().strip()
        invalid_record = json.loads(invalid_log)
        assert invalid_record["schema"] == "laghu-log-v1"
        assert invalid_record["event"] == "lifecycle"
        assert invalid_record["state"] == "failed"
        proxy_port = free_port()
        purge_token = root / "purge.token"
        purge_token.write_text("standalone-purge-token-0123456789\n")
        purge_token.chmod(0o600)
        flush_file = root / "cache.flush"
        asset_catalog = root / "asset-catalog"
        asset_queue = root / "asset.queue"
        asset_catalog.mkdir()
        asset_queue.mkdir()
        asset_config = root / "asset-offload.conf"
        asset_config.write_text(
            "\n".join(
                [
                    "version=1",
                    "source_domain=https://origin.example.test",
                    "public_domain=https://cdn.example.test",
                    "source_prefix=/assets",
                    "public_prefix=/immutable",
                    "mime_types=text/css,image/png",
                    "allow_paths=/",
                    "mode=upload_and_rewrite",
                    "preserve_query=on",
                    "trusted_origin_fallback=on",
                    "max_body_bytes=67108864",
                    "retry_limit=3",
                    "timeout_seconds=10",
                    "stale_ttl_seconds=86400",
                    f"catalog_path={asset_catalog}",
                    f"queue_path={asset_queue}",
                    "provider=s3",
                    "endpoint=https://s3.example.test",
                    "region=us-east-1",
                    "bucket=laghu-assets",
                    "object_prefix=production",
                    "access_key_env=LAGHU_TEST_S3_ACCESS_KEY",
                    "secret_key_env=LAGHU_TEST_S3_SECRET_KEY",
                    "",
                ]
            )
        )
        main_log = (root / "main-proxy.log").open("w+b")
        process = start_process(
            [
                str(executable),
                "--listen",
                f"127.0.0.1:{proxy_port}",
                "--origin",
                f"http://127.0.0.1:{origin_port}",
                "--cache",
                str(root / "cache"),
                "--rum-store",
                "memory:",
                "--worker-queue",
                str(root / "missing.queue"),
                "--javascript-queue",
                str(root / "javascript.queue"),
                "--asset-offload-config",
                str(asset_config),
                "--asset-upload-queue",
                str(asset_queue),
                "--load-from-file",
                "mapped",
                "--file-source-map",
                f"https://origin.example.test/assets/={root}",
                "--javascript-target",
                "last 2 chrome versions",
                "--javascript-inline-limit",
                "2048",
                "--javascript-outline-threshold",
                "8192",
                "--rewrite-level",
                "all",
                "--transform-deadline-ms",
                "1000",
                "--allow-resources",
                "/*",
                "--disallow",
                "/never-optimized/*",
                "--respect-vary",
                "on",
                "--query-filter-overrides",
                "on",
                "--respect-x-forwarded-proto",
                "on",
                "--trusted-proxy",
                "127.0.0.1/32",
                "--critical-css-beacon",
                "--instrumentation-beacon",
                "--instrumentation-sample-rate",
                "100",
                "--io-timeout",
                "1",
                "--workers",
                "1",
                "--connection-queue",
                "1",
                "--origin-pool-size",
                "1",
                "--origin-idle-timeout",
                "1",
                "--purge-method",
                "PURGE",
                "--purge-query",
                "on",
                "--purge-token-file",
                str(purge_token),
                "--purge-allow",
                "127.0.0.1/32",
                "--cache-flush-file",
                str(flush_file),
                "--statistics",
                "on",
                "--metrics",
                "on",
                "--readiness",
                "on",
                "--readiness-policy",
                "degraded",
            ],
            stdout=subprocess.DEVNULL,
            stderr=main_log,
        )
        try:
            for _ in range(warm_attempts):
                try:
                    first_head, first_body = request(
                        proxy_port, "/index.html", timeout=0.25
                    )
                    break
                except OSError:
                    if process.poll() is not None:
                        main_log.flush()
                        main_log.seek(0)
                        raise AssertionError(
                            f"proxy failed startup ({process.returncode}): "
                            + main_log.read().decode(errors="replace")
                        )
                    time.sleep(0.05)
            else:
                raise AssertionError("proxy did not start")
            assert b"<body>hello" in first_body, (first_head, first_body)
            assert pathlib.Path(str(asset_queue) + ".sources").is_file()
            assert b"/.laghu/beacon/instrumentation.js" in first_body
            assert b'data-laghu-sample="100"' in first_body
            assert b"x-laghu: pass" in first_head, first_head
            with Origin.connections_lock:
                pooled_before = Origin.connections
            _, pooled_one = request(proxy_port, "/pooled")
            _, pooled_two = request(proxy_port, "/pooled")
            assert b"<body>hello" in pooled_one and b"<body>hello" in pooled_two
            with Origin.connections_lock:
                assert Origin.connections == pooled_before + 1, Origin.connections
            _, closed_origin = request(proxy_port, "/api/data")
            assert closed_origin == b'{"ok":true}'
            _, pooled_after_close = request(proxy_port, "/pooled")
            assert b"<body>hello" in pooled_after_close
            with Origin.connections_lock:
                assert Origin.connections == pooled_before + 2, Origin.connections
                idle_before = Origin.connections
            time.sleep(1.05)
            _, pooled_after_idle = request(proxy_port, "/pooled")
            assert b"<body>hello" in pooled_after_idle
            with Origin.connections_lock:
                assert Origin.connections == idle_before + 1, Origin.connections
            lcp_head, lcp_body, lcp_interim = request(
                proxy_port, "/lcp.html", include_interim=True
            )
            assert b'fetchpriority="high"' in lcp_body
            assert b'</image.png>; rel=preload; as=image' in lcp_head
            assert any(
                b"103 early hints" in header
                and b'</image.png>; rel=preload; as=image' in header
                for header in lcp_interim
            )
            assert b'/logo.png>; rel=preload' not in lcp_head
            hints_head, hints_cold = request(proxy_port, "/hints.html")
            assert b'<https://origin.example.test>; rel=preload' in hints_head
            assert b'<https://cdn.example.test>; rel=' not in hints_head
            assert b'token=secret' in hints_cold
            for _ in range(warm_attempts):
                hints_head, hints_warm = request(proxy_port, "/hints.html")
                if (
                    b'<https://cdn.example.test>; rel=preconnect' in hints_head
                    and b'<https://cdn.example.test>; rel=dns-prefetch' in hints_head
                ):
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("standalone resource hints did not become warm")
            assert b"reserved.example.test" in hints_cold
            assert b"reserved.example.test" in hints_warm
            assert b"token=secret" in hints_warm
            assert b'<https://origin.example.test>; rel=preload' in hints_head
            assert b'token=secret' not in hints_head
            assert b'#part' not in hints_head
            assert b'etag: "laghu-html-' in hints_head
            preview_head, preview_body = request(
                proxy_port, "/trim-urls.html?laghu=preview"
            )
            assert preview_body == (
                b'<html><body><audio src="http://example.test/asset.mp3?q=1#hero">'
                b'</audio><a href="/next">next</a></body></html>'
            )
            assert b'x-laghu: bypass-query-preview' in preview_head
            assert b'x-laghu-preview:' in preview_head
            for _ in range(warm_attempts):
                trim_head, trim_body = request(proxy_port, "/trim-urls.html")
                if (
                    b"asset.mp3?q=1#hero" in trim_body
                    and b"http://example.test/" not in trim_body
                ):
                    break
                time.sleep(0.05)
            else:
                raise AssertionError(
                    f"standalone resource URLs did not trim: {trim_body!r}"
                )
            assert b'href="/next"' in trim_body or b"href=/next" in trim_body
            assert b'etag: "laghu-html-' in trim_head
            javascript_head, javascript_cold = request(proxy_port, "/app.js")
            assert javascript_cold == b"function publicName(longLocal) { return longLocal + 1; }"
            for _ in range(warm_attempts):
                javascript_head, javascript_warm = request(proxy_port, "/app.js")
                if b'etag: "laghu-js-' in javascript_head:
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("standalone JavaScript did not become warm")
            assert len(javascript_warm) < len(javascript_cold)
            for _ in range(warm_attempts):
                _, external_warm = request(proxy_port, "/javascript-external.html")
                if b'src="/app.js"' not in external_warm and b"publicName" in external_warm:
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("standalone external JavaScript did not become warm")
            assert b"publicName(n)" in external_warm
            for asset in ("/combine-one.js", "/combine-two.js"):
                for _ in range(warm_attempts):
                    asset_head, _ = request(proxy_port, asset)
                    if b'etag: "laghu-js-' in asset_head:
                        break
                    time.sleep(0.05)
                else:
                    worker_status = javascript_process.poll()
                    worker_error = b""
                    if worker_status is not None and javascript_process.stderr is not None:
                        worker_error = javascript_process.stderr.read()
                    raise AssertionError(
                        f"standalone dependency {asset} did not become warm; "
                        f"worker_status={worker_status}; worker_error={worker_error!r}"
                    )
            for _ in range(warm_attempts):
                _, combine_warm = request(proxy_port, "/javascript-combine.html")
                combine_match = re.search(
                    rb'src="(/\.laghu/js/[0-9a-f]{64})"', combine_warm
                )
                if combine_match:
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("standalone JavaScript combine did not become warm")
            immutable_head, immutable_body = request(
                proxy_port, combine_match.group(1).decode()
            )
            assert b"combine one value" in immutable_body
            assert b"combine two value" in immutable_body
            assert b"cache-control: public, max-age=31536000, immutable" in immutable_head
            _, inline_cold = request(proxy_port, "/javascript-inline.html")
            assert b"inlinePublic(longLocal)" in inline_cold
            for _ in range(warm_attempts):
                inline_head, inline_warm = request(proxy_port, "/javascript-inline.html")
                if b"inlinePublic(n){return n+1;}" in inline_warm:
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("standalone inline JavaScript did not become warm")
            assert b"inlinePublic(n){return n+1;}" in inline_warm
            for _ in range(10):
                _, csp_hash_body = request(proxy_port, "/javascript-csp-hash.html")
                time.sleep(0.05)
            assert b"cspProtected(longLocal)" in csp_hash_body
            assert b"cspProtected(n)" not in csp_hash_body
            _, outline_cold = request(proxy_port, "/javascript-outline.html")
            assert b"outlinePublic" in outline_cold
            for _ in range(warm_attempts):
                _, outline_warm = request(proxy_port, "/javascript-outline.html")
                outline_match = re.search(
                    rb'src="(/\.laghu/js/[0-9a-f]{64})"', outline_warm
                )
                if outline_match:
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("standalone JavaScript outline did not become warm")
            _, outline_asset = request(proxy_port, outline_match.group(1).decode())
            assert b"outlinePublic" in outline_asset
            critical_head, critical_body = request(
                proxy_port, "/.laghu/beacon/critical-css.js"
            )
            assert b" 200 " in critical_head.split(b"\r\n", 1)[0]
            assert b"data-laghu-critical" in critical_body
            rum_head, rum_body = request(
                proxy_port, "/.laghu/beacon/instrumentation.js"
            )
            assert b" 200 " in rum_head.split(b"\r\n", 1)[0]
            assert b"largest-contentful-paint" in rum_body
            template_key = re.search(
                rb'data-laghu-template="([0-9a-f]{64})"', first_body
            ).group(1).decode()
            rum_post_head, rum_post_body = request(
                proxy_port,
                "/.laghu/beacon/instrumentation",
                method="POST",
                headers={
                    "Content-Type": "application/json",
                    "Sec-Fetch-Site": "same-origin",
                },
                body=json.dumps({
                    "version": 1, "template": template_key, "bucket": 0,
                    "lcp_ms": 1200, "inp_ms": 100, "cls_milli": 50,
                    "dcl_ms": 500, "load_ms": 700, "errors": 0,
                    "rejections": 0, "candidates": [],
                }).encode(),
            )
            assert b" 204 " in rum_post_head.split(b"\r\n", 1)[0]
            assert rum_post_body == b""
            health_head, health_body = request(proxy_port, "/.laghu/health")
            assert b" 200 " in health_head.split(b"\r\n", 1)[0]
            assert health_body == b'{"status":"ok","state":"running"}'
            forbidden_head, _ = request(proxy_port, "/.laghu/stats")
            assert b" 403 " in forbidden_head.split(b"\r\n", 1)[0]
            admin_headers = {"X-Laghu-Purge-Token":
                             "standalone-purge-token-0123456789"}
            ready_head, ready_body = request(
                proxy_port, "/.laghu/ready", headers=admin_headers
            )
            assert b" 200 " in ready_head.split(b"\r\n", 1)[0]
            assert b'"status":"ready"' in ready_body
            metrics_head, metrics_body = request(
                proxy_port, "/.laghu/metrics", headers=admin_headers
            )
            assert b" 200 " in metrics_head.split(b"\r\n", 1)[0]
            assert b"text/plain; version=0.0.4" in metrics_head
            assert b"laghu_requests_total" in metrics_body
            stats_head, stats_body = request(
                proxy_port, "/.laghu/stats", headers=admin_headers
            )
            assert b" 200 " in stats_head.split(b"\r\n", 1)[0]
            assert b"laghu-cache-stats-v1" in stats_body
            console_head, console_body = request(
                proxy_port, "/.laghu/console", headers=admin_headers
            )
            assert b" 200 " in console_head.split(b"\r\n", 1)[0]
            assert b"text/html; charset=utf-8" in console_head
            assert b"<h1>Laghu console</h1>" in console_body
            admin_head, admin_body = request(
                proxy_port, "/pagespeed_admin", headers=admin_headers
            )
            assert b" 200 " in admin_head.split(b"\r\n", 1)[0]
            assert b"text/html; charset=utf-8" in admin_head
            assert b"<h1>Laghu console</h1>" in admin_body
            admin_json_head, admin_json_body = request(
                proxy_port, "/pagespeed_admin?format=json", headers=admin_headers
            )
            assert b" 200 " in admin_json_head.split(b"\r\n", 1)[0]
            assert b"\"schema\":\"laghu-console-v1\"" in admin_json_body
            history_head, history_body = request(
                proxy_port, "/.laghu/history", headers=admin_headers
            )
            assert b" 200 " in history_head.split(b"\r\n", 1)[0]
            assert b"content-type: text/html; charset=utf-8" in history_head
            assert b"<h1>Laghu console</h1>" in history_body
            assert b"<h2>Laghu history</h2>" in history_body
            history_json_head, history_json_body = request(
                proxy_port, "/.laghu/history?format=json&limit=2", headers=admin_headers
            )
            assert b" 200 " in history_json_head.split(b"\r\n", 1)[0]
            assert b"\"schema\":\"laghu-history-v1\"" in history_json_body
            stats_legacy_head, stats_legacy_body = request(
                proxy_port, "/pagespeed_statistics", headers=admin_headers
            )
            assert b" 200 " in stats_legacy_head.split(b"\r\n", 1)[0]
            assert b"laghu-cache-stats-v1" in stats_legacy_body
            stats_legacy_json_head, _ = request(
                proxy_port, "/pagespeed_statistics?format=json", headers=admin_headers
            )
            assert b" 200 " in stats_legacy_json_head.split(b"\r\n", 1)[0]
            explain_head, explain_body = request(
                proxy_port, "/.laghu/explain?path=/index.html", headers=admin_headers
            )
            assert b" 200 " in explain_head.split(b"\r\n", 1)[0]
            assert b"content-type: text/html; charset=utf-8" in explain_head
            assert b"<h1>Laghu console</h1>" in explain_body
            assert b"<h2>Laghu explain</h2>" in explain_body
            assert b"Target: /index.html" in explain_body
            explain_json_head, explain_json_body = request(
                proxy_port,
                "/.laghu/explain?path=/index.html&format=json",
                headers=admin_headers,
            )
            assert b" 200 " in explain_json_head.split(b"\r\n", 1)[0]
            assert b"\"schema\":\"laghu-explain-v1\"" in explain_json_body
            purge_form_head, purge_form_body = request(
                proxy_port, "/.laghu/purge?path=/site.css", headers=admin_headers
            )
            assert b" 202 " in purge_form_head.split(b"\r\n", 1)[0]
            assert b"cache-control: no-store" in purge_form_head
            assert b'"status":"accepted"' in purge_form_body
            metrics_legacy_head, metrics_legacy_body = request(
                proxy_port, "/pagespeed_console", headers=admin_headers
            )
            assert b" 200 " in metrics_legacy_head.split(b"\r\n", 1)[0]
            assert b"laghu_requests_total" in metrics_legacy_body
            purge_head, purge_body = request(
                proxy_port, "/site.css", method="PURGE", headers=admin_headers
            )
            assert b" 202 " in purge_head.split(b"\r\n", 1)[0]
            assert b"cache-control: no-store" in purge_head
            assert b'"status":"accepted"' in purge_body
            cli_purge = subprocess.run(
                [
                    str(executable), "purge", f"http://127.0.0.1:{proxy_port}/site.css",
                    "--token-file", str(purge_token), "--json",
                ],
                capture_output=True,
                text=True,
                check=True,
            )
            assert json.loads(cli_purge.stdout)["status"] == "accepted"
            assert "standalone-purge-token-0123456789" not in (
                cli_purge.stdout + cli_purge.stderr
            )
            query_head, _ = request(
                proxy_port, "/site.css?laghu=purge", headers=admin_headers
            )
            assert b" 202 " in query_head.split(b"\r\n", 1)[0]
            subprocess.run(
                [
                    str(cache_fixture),
                    "--queue",
                    str(root / "missing.queue"),
                    str(int(time.time())),
                ],
                check=True,
            )
            ready_head, ready_body = request(
                proxy_port, "/.laghu/ready", headers=admin_headers
            )
            assert b" 200 " in ready_head.split(b"\r\n", 1)[0]
            assert b'"status":"ready"' in ready_body
            ready_head, ready_body = request(
                proxy_port, "/.laghu/ready", method="HEAD",
                headers=admin_headers
            )
            assert b" 200 " in ready_head.split(b"\r\n", 1)[0]
            assert ready_body == b""
            cache_path = root / "cache"
            unavailable_cache_path = root / "cache-unavailable"
            cache_path.rename(unavailable_cache_path)
            cache_path.touch()
            try:
                unavailable_head, unavailable_body = request(
                    proxy_port, "/.laghu/ready", headers=admin_headers
                )
            finally:
                cache_path.unlink()
                unavailable_cache_path.rename(cache_path)
            assert b" 503 " in unavailable_head.split(b"\r\n", 1)[0]
            assert b'"cache":"not_ready"' in unavailable_body
            second_head, second_body = request(proxy_port, "/index.html")
            assert b"<!-- remove -->" not in second_body
            assert b"/.laghu/beacon/instrumentation.js" in second_body
            assert b"etag: \"laghu-html-" in second_head
            css_cold_head, css_cold_body = request(proxy_port, "/site.css")
            assert css_cold_body == b"body { color: red; }"
            assert b"x-laghu: pass" in css_cold_head
            assert b"x-laghu-cache: miss" in css_cold_head
            assert b"x-laghu-transform: queued" in css_cold_head
            css_warm_head, css_warm_body = request(proxy_port, "/site.css")
            assert len(css_warm_body) < len(css_cold_body)
            assert b"etag: \"laghu-css-" in css_warm_head
            api_head, api_body = request(proxy_port, "/api/data")
            assert api_body == b'{"ok":true}', (api_head, api_body)
            assert b"x-laghu: bypass-api" in api_head
            _, stripped_body = request(
                proxy_port,
                "/headers",
                headers={
                    "Forwarded": "for=spoofed",
                    "X-Forwarded-For": "spoofed",
                    "X-Forwarded-Proto": "https",
                    "X-Forwarded-Host": "spoofed.test",
                },
            )
            assert json.loads(stripped_body) == {}
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
            for _ in range(warm_attempts):
                chunked_head, chunked_body = request(proxy_port, "/chunked")
                if b"<!-- remove -->" not in chunked_body:
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("chunked HTML did not become warm")
            assert b"<!-- remove -->" not in chunked_body
            assert b"/.laghu/beacon/instrumentation.js" in chunked_body
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
            for _ in range(warm_attempts):
                try:
                    recovered_head, recovered_body = request(
                        proxy_port, "/api/data", timeout=0.25
                    )
                    if (
                        recovered_body == b'{"ok":true}'
                        and b"x-laghu: bypass-api" in recovered_head
                    ):
                        break
                except (AssertionError, OSError):
                    pass
                time.sleep(0.05)
            else:
                raise AssertionError("proxy did not recover from saturation")
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
            for _ in range(warm_attempts):
                try:
                    recovered_head, recovered_body = request(
                        proxy_port, "/api/data", timeout=0.25
                    )
                    if (
                        recovered_body == b'{"ok":true}'
                        and b"x-laghu: bypass-api" in recovered_head
                    ):
                        break
                except (AssertionError, OSError):
                    pass
                time.sleep(0.05)
            else:
                raise AssertionError("proxy did not recover after abandoned client")
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
            assert b"<body>hello" in corrupt_body
            assert b"/.laghu/beacon/instrumentation.js" in corrupt_body
            assert (
                b"x-laghu: pass" in corrupt_head
                or b"x-laghu: bypass-error" in corrupt_head
            )
            flush_file.write_text("laghu-cache-flush-v1 9\n")
            flush_file.chmod(0o600)
            _, flushed_stats = request(
                proxy_port, "/.laghu/stats", headers=admin_headers
            )
            assert b'"generation":9' in flushed_stats
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
            request_shutdown(process)
            queued_response = read_open_socket(queued_for_shutdown)
            active_response = read_open_socket(draining)
            assert queued_response.startswith(b"http/1.1 503 "), queued_response
            assert active_response.startswith(b"http/1.1 200 "), active_response
            assert b"hello" in active_response, active_response
            wait_for_shutdown(process)
            main_log.flush()
            main_log.seek(0)
            logs = main_log.read().decode()
            assert '"schema":"laghu-log-v1"' in logs
            assert '"event":"transaction"' in logs
            assert '"path":"/.laghu/ready"' in logs
            assert '"event":"lifecycle"' in logs
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
            main_log.close()
            forwarded_port = free_port()
            process = start_process(
                [
                    str(executable),
                    "--listen",
                    f"127.0.0.1:{forwarded_port}",
                    "--origin",
                    f"http://127.0.0.1:{origin_port}",
                    "--cache",
                    str(root / "cache"),
                    "--worker-queue",
                    str(root / "missing.queue"),
                    "--forwarded-headers",
                    "both",
                    "--trusted-proxy",
                    "127.0.0.1/32",
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )
            for _ in range(warm_attempts):
                try:
                    _, forwarded_body = request(
                        forwarded_port,
                        "/headers",
                        headers={
                            "Forwarded": "for=192.0.2.10;proto=https",
                            "X-Forwarded-For": "192.0.2.10",
                        },
                    )
                    break
                except OSError:
                    time.sleep(0.05)
            else:
                raise AssertionError("forwarding proxy did not start")
            forwarded = json.loads(forwarded_body)
            assert "192.0.2.10" in forwarded["forwarded"][0]
            assert "for=127.0.0.1;proto=http" in forwarded["forwarded"][0]
            assert forwarded["x-forwarded-for"] == ["192.0.2.10, 127.0.0.1"]
            assert forwarded["x-forwarded-proto"] == ["http"]
            assert forwarded["x-forwarded-host"] == ["example.test"]
            _, replaced_body = request(
                forwarded_port,
                "/headers",
                headers={
                    "Forwarded": "spoofed-without-parameter",
                    "X-Forwarded-For": "not-an-address",
                },
            )
            replaced = json.loads(replaced_body)
            assert replaced["forwarded"] == [
                'for=127.0.0.1;proto=http;host="example.test"'
            ]
            assert replaced["x-forwarded-for"] == ["127.0.0.1"]
            request_shutdown(process)
            wait_for_shutdown(process)
            forwarded_logs = process.stderr.read().decode()
            assert "192.0.2.10" not in forwarded_logs
            assert "127.0.0.1" not in forwarded_logs
            process.stderr.close()
            force_port = free_port()
            process = start_process(
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
            for _ in range(warm_attempts):
                try:
                    forced = socket.create_connection(("127.0.0.1", force_port), timeout=5)
                    break
                except OSError:
                    time.sleep(0.05)
            else:
                raise AssertionError("forced-drain proxy did not start")
            forced.sendall(
                b"GET /hang HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n"
            )
            time.sleep(0.2)
            request_shutdown(process)
            time.sleep(0.3)
            request_shutdown(process)
            started = time.monotonic()
            wait_for_shutdown(process)
            assert time.monotonic() - started < 5
            read_open_socket(forced)
            forced_logs = process.stderr.read().decode()
            assert '"state":"forcing"' in forced_logs
            assert '"failure":"shutdown"' in forced_logs
        except Exception:
            if process.poll() is None:
                request_shutdown(process)
                wait_for_shutdown(process)
            if not main_log.closed:
                main_log.flush()
                main_log.seek(0)
                print(main_log.read().decode(), file=sys.stderr)
            raise
        finally:
            if process.poll() is None:
                request_shutdown(process)
                wait_for_shutdown(process)
            if process.stderr is not None:
                process.stderr.close()
            if not main_log.closed:
                main_log.close()
            origin.shutdown()
            request_shutdown(javascript_process)
            javascript_process.wait(timeout=5)
            javascript_process.stderr.close()
    print("laghu proxy smoke passed")


if __name__ == "__main__":
    main()
