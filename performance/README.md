# Performance rail

This folder contains the local AMD64 benchmark rail for standalone, NGINX, and Apache. It creates a controlled corpus and preserves raw results under `tmp/benchmarks/`.

Run `LAGHU_BENCH_FULL=1 scripts/run-benchmarks-all` on a native AMD64 Linux builder. This command is replaced by direct targets during the script-removal goal.
