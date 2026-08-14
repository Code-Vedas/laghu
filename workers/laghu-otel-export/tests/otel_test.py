# Copyright Codevedas Inc. 2026-present
import http.server
import os
import pathlib
import ssl
import subprocess
import sys
import tempfile
import threading
import time

class Collector(http.server.BaseHTTPRequestHandler):
    received = None
    status = 200
    def do_POST(self):
        Collector.received = (self.path, self.headers.get("Authorization"), self.rfile.read(int(self.headers["Content-Length"])))
        self.send_response(Collector.status)
        self.end_headers()
    def log_message(self, _format, *_args): pass

def main():
    worker, fixture = map(pathlib.Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="laghu-otel-") as raw:
        root = pathlib.Path(raw); key = root / "key.pem"; certificate = root / "cert.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-config", os.devnull, "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost", "-days", "1", "-keyout", key, "-out", certificate], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Collector); context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key); server.socket = context.wrap_socket(server.socket, server_side=True)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        queue, cache = root / "trace.queue", root / "cache"; subprocess.run([fixture, queue], check=True)
        env = os.environ | {"LAGHU_OTEL_CACHE_PATH": str(cache), "LAGHU_OTEL_AUTHORIZATION": "Bearer fixture", "LAGHU_TEST_OTEL_ENDPOINT": "https://localhost/v1/traces", "LAGHU_TEST_OTEL_PORT": str(server.server_address[1])}
        process = subprocess.Popen([worker, "--queue", queue, "--endpoint", "https://example.com/v1/traces", "--ca-file", certificate], env=env, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 10
            while Collector.received is None and time.monotonic() < deadline: time.sleep(.05)
            assert Collector.received is not None and Collector.received[0] == "/v1/traces"
            assert Collector.received[1] == "Bearer fixture" and b'"traceId"' in Collector.received[2]
            Collector.received = None; Collector.status = 503; subprocess.run([fixture, "--publish", queue], check=True)
            deadline = time.monotonic() + 10
            while Collector.received is None and time.monotonic() < deadline: time.sleep(.05)
            assert Collector.received is not None and process.poll() is None
        finally:
            process.terminate(); _, stderr = process.communicate(timeout=5); assert '"outcome":"failed"' in stderr
            server.shutdown(); server.server_close()

if __name__ == "__main__": main()
