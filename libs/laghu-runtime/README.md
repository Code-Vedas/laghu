# Laghu Runtime

This library owns bounded queues, cache publication, catalogs, runtime state, and worker coordination. NGINX, Apache, standalone, and workers use it; it contains no server adapter code.

Build from the repository root and run `ctest --test-dir build -R laghu_runtime` for its tests.
