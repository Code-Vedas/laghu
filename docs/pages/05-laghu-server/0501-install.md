---
title: Installation
parent: Laghu Server
nav_order: 1
permalink: /laghu-server/install/
---

# Install the Laghu Server

The standalone executable is implemented and validated from the source tree.
The current Debian, RPM, and Homebrew definitions package the native adapters and workers but do not yet publish a standalone `laghu` package.
The production NGINX and Apache Dockerfiles likewise do not represent a published standalone image.

## Source Build

```bash
cmake -S . -B tmp/build -DLAGHU_WITH_VIPS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build tmp/build --parallel
```

The executable is built at `tmp/build/servers/laghu/laghu` with the worker executables under `tmp/build/workers/`.

## Development Run

Initialize and start the worker queue before starting the proxy:

```bash
tmp/build/workers/laghu-libvips/laghu-libvips --init /tmp/laghu/jobs.queue /tmp/laghu/cache
tmp/build/workers/laghu-libvips/laghu-libvips --serve /tmp/laghu/jobs.queue /tmp/laghu/cache
tmp/build/servers/laghu/laghu --listen 127.0.0.1:8080 --origin http://127.0.0.1:8000 --cache /tmp/laghu/cache --worker-queue /tmp/laghu/jobs.queue
```

Use administrator-owned persistent paths instead of `/tmp` in production.
Windows builds also support service mode; public installer publication remains a roadmap item rather than current package availability.
