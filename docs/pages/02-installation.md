---
title: Installation
nav_order: 2
permalink: /installation/
---

# Installation

Choose the adapter for the web server already in use. Each command installs
the adapter and its internal `laghu-libvips` dependency, loads the adapter,
validates the server configuration, and leaves optimization off.

## NGINX

```bash
sudo apt install ngx-laghu
sudo dnf install ngx-laghu
sudo yum install ngx-laghu
brew install code-vedas/tap/ngx-laghu
winget install CodeVedas.NgxLaghu
```

Linux packages install an ABI-matched `ngx_http_laghu_module.so` on top of the
distribution NGINX. The Windows installer includes a matched NGINX build
because a separately compiled module cannot be treated as universally binary
compatible. Validate and then enable Laghu explicitly:

```nginx
laghu on;
laghu preset balanced;
```

```bash
sudo systemctl enable --now laghu-libvips
sudo nginx -t
sudo systemctl reload nginx
```

## Apache HTTP Server

```bash
sudo apt install mod-laghu
sudo dnf install mod-laghu
sudo yum install mod-laghu
brew install code-vedas/tap/mod-laghu
winget install CodeVedas.ModLaghu
```

Linux and Homebrew packages build `mod_laghu` against the selected Apache 2.4
development ABI. The Windows installer includes a matched Apache build. Enable
optimization only after validation:

```apache
Laghu On
Laghu Preset balanced
```

```bash
sudo systemctl enable --now laghu-libvips
sudo apache2ctl configtest  # Debian and Ubuntu
sudo httpd -t               # RPM systems
```

## Containers

The published images contain one matched server adapter and the shared service:

```bash
docker run --rm -p 8080:8080 ghcr.io/code-vedas/ngx-laghu:latest
docker run --rm -p 8080:80 ghcr.io/code-vedas/mod-laghu:latest
```

Persist `/var/cache/laghu/images`; keep the queue local to the container.

## Build From Source

Build the shared libraries and service:

```bash
cmake -S . -B tmp/build -DLAGHU_WITH_VIPS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build tmp/build --parallel
```

Build either adapter:

```bash
scripts/run-module-build-all
scripts/build-apache-module
```

NGINX output is under `tmp/nginx/<version>/build/`. Apache output is
`tmp/apache-module/.libs/mod_laghu.so`. Both adapters communicate with:

```bash
laghu-libvips --init-and-serve /run/laghu/jobs.queue \
  /var/cache/laghu/images
```

`LAGHU_WITH_VIPS=AUTO` degrades safely when libvips is unavailable, `ON`
requires libvips 8.15 or newer, and `OFF` deliberately tests the no-codec path.

## Local Docker Tests

Run one or every supported Linux package/server case locally:

```bash
scripts/run-in-docker
scripts/run-in-docker --list
scripts/run-in-docker --target debian-apache
scripts/run-in-docker --all
scripts/run-in-docker --all --architectures amd64,arm64
```

Cross-architecture Docker runs use Buildx/QEMU and provide developer feedback;
native CI remains required for release evidence.
