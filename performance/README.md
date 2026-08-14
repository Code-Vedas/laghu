# Local AMD64 performance rail

Run only on an AMD64 Linux host. This rail is intentionally local-only: it is never called by CI.

`scripts/run-benchmarks-all` builds a deterministic corpus and runs the §5.3 rail against native NGINX, native Apache, and standalone Laghu with NGINX and Apache origins. It writes JSON and HTML artifacts under `tmp/benchmarks/`. Set `LAGHU_BENCH_FULL=1` to run the complete 1, 10, 50, 100, 500, and 1000 VU sweep; the default smoke rail runs 1 and 10 VUs.

Every target runs warm HTML, excluded/private/no-store, negotiated image, mixed-page, same-URL cache-storm, and soak cells. A failed HTTP check or correctness assertion fails the rail; the JSON artifact records each target's origin and optimization mode.

The report records any unavailable optional measurement as `unavailable`; it never substitutes an estimate for image quality or CWV data.
