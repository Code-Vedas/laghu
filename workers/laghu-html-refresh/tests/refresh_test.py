# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import http.server
import os
import pathlib
import ssl
import subprocess
import sys
import tempfile
import threading


BODY = b"<html><body>refreshed</body></html>"
ORIGIN = "https://fixture.test"


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/page?lang=en":
            self.send_error(404)
            return
        if self.headers.get("If-None-Match"):
            self.send_response(304)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("ETag", '"fixture-v1"')
        self.send_header("Content-Length", str(len(BODY)))
        self.end_headers()
        self.wfile.write(BODY)

    def log_message(self, _format, *_args):
        pass


def main():
    worker = pathlib.Path(sys.argv[1])
    fixture = pathlib.Path(sys.argv[2])
    with tempfile.TemporaryDirectory(prefix="laghu-html-refresh-") as raw:
        root = pathlib.Path(raw)
        queue = root / "html.queue"
        cache = root / "cache"
        key = root / "server.key"
        certificate = root / "server.crt"
        subprocess.run(
            [
                "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-config", os.devnull, "-subj", "/CN=fixture.test",
                "-addext", "subjectAltName=DNS:fixture.test", "-days", "1",
                "-keyout", key, "-out", certificate,
            ],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        environment = os.environ.copy()
        environment["LAGHU_TEST_HTML_REFRESH_ENDPOINT"] = (
            f"127.0.0.1:{server.server_address[1]}"
        )
        environment["LAGHU_TEST_HTML_REFRESH_CA"] = str(certificate)
        try:
            subprocess.run([worker, "--init", queue, cache, ORIGIN], check=True)
            subprocess.run(
                [fixture, "--job", queue, ORIGIN, "/page?lang=en", ""], check=True
            )
            subprocess.run(
                [worker, "--once", queue, cache, ORIGIN], check=True, env=environment
            )
            subprocess.run(
                [fixture, "--cached", cache, ORIGIN, "/page?lang=en", BODY.decode()],
                check=True,
            )
            subprocess.run(
                [fixture, "--job", queue, ORIGIN, "/page?lang=en", '"fixture-v1"'],
                check=True,
            )
            subprocess.run(
                [worker, "--once", queue, cache, ORIGIN], check=True, env=environment
            )
            subprocess.run(
                [fixture, "--cached", cache, ORIGIN, "/page?lang=en", BODY.decode()],
                check=True,
            )
        finally:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main()
