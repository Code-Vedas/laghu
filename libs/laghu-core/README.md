# Laghu Core

This library owns server-independent optimization policy, configuration inheritance, response eligibility, and candidate selection. NGINX, Apache, standalone, and workers link it for shared behavior.

Build the project from the repository root, then run `ctest --test-dir build -R laghu_core`.

Its public API is in `include/laghu/core.h`.
