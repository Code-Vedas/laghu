# Local AMD64 performance rail

Run only on an AMD64 Linux host. This rail is intentionally local-only: it is never called by CI.

`scripts/run-benchmarks-all` builds a deterministic corpus and runs the rail against all native NGINX,
native Apache, and standalone targets. The NGINX plain/PageSpeed/Laghu triad uses the same corpus,
static Brotli/Gzip negotiation, cache policy, WebP capability request, load profile, and resource
collection. Apache pins `mpm_event` to 1,024 request workers: rail peak, not Debian's 150-worker
default. NGINX targets pin 4,096 connections and an 8,192 descriptor ceiling. Each load lane runs
only its target and required origin; unrelated transform workers are
stopped to prevent cross-target CPU/queue contamination. Standalone pins 32 workers, a 4,096
connection queue, and 8,192 descriptors. It writes JSON and HTML artifacts under
`tmp/benchmarks/`. Set `LAGHU_BENCH_FULL=1` to
run the complete 1, 10, 50, 100, 500, and 1000 VU sweep; the default smoke rail runs 1 and 10 VUs.

Every target runs warm HTML, excluded/private/no-store, negotiated image, mixed-page, same-URL
cache-storm, and soak cells. A failed HTTP check or correctness assertion fails the rail; the JSON
artifact records each target's origin and optimization mode.

The JSON includes a joined `nginx_comparison` table and reproducibility metadata (corpus/config
digests, image IDs, NGINX versions, k6 image, host profile). The report records any unavailable
optional measurement as `unavailable`; it never substitutes an estimate for image quality or CWV data.
