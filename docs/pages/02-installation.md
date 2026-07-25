---
title: Installation
nav_order: 2
permalink: /installation/
---

# Installation

Build Laghu from source against the web server ABI used by the target host.

## Shared Libraries and Service

Build the shared libraries and service:

```bash
cmake -S . -B tmp/build -DLAGHU_WITH_VIPS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build tmp/build --parallel
```

## NGINX Module

```bash
scripts/run-module-build-all
```

The module is written to `tmp/nginx/<version>/build/ngx_http_laghu_module.so`. Load it only into the NGINX build whose ABI was used during compilation, then validate the configuration with `nginx -t`.

## Apache Module

```bash
scripts/build-apache-module
```

The module is written to `tmp/apache-module/.libs/mod_laghu.so`. Load it into Apache HTTP Server 2.4 and validate the configuration with `apache2ctl configtest` or `httpd -t`.

Both modules communicate with:

```bash
laghu-libvips --init-and-serve /run/laghu/jobs.queue \
  /var/cache/laghu/images
```

`LAGHU_WITH_VIPS=AUTO` degrades safely when libvips is unavailable, `ON` requires libvips 8.15 or newer, and `OFF` deliberately tests the no-codec path.

## Standalone Proxy

The same build produces `tmp/build/servers/laghu/laghu`. Run it with one explicit plaintext origin:

```bash
tmp/build/servers/laghu/laghu \
  --listen 127.0.0.1:8080 \
  --origin http://127.0.0.1:8000 \
  --cache /var/cache/laghu \
  --worker-queue /run/laghu/jobs.queue
```

This source-build interface does not install a service or production proxy package. See [Standalone Proxy](/standalone-proxy/) for its implemented HTTP limits.

## Local Docker Tests

Run one or every supported Linux package/server case locally:

```bash
scripts/run-in-docker
scripts/run-in-docker --list
scripts/run-in-docker --target debian-apache
scripts/run-in-docker --all
scripts/run-in-docker --all --architectures amd64,arm64
```

Cross-architecture Docker runs use Buildx/QEMU and provide developer feedback; native CI remains required for release evidence.
