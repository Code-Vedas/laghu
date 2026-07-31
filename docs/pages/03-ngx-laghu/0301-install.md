---
title: Installation
parent: ngx-laghu
nav_order: 1
permalink: /ngx-laghu/install/
---

# Install ngx-laghu

An NGINX module is architecture- and ABI-bound.
Load it only into the NGINX build used to compile or package it.

## Distribution Packages

After configuring the Codevedas package repository for the distribution, install the adapter and shared runtime:

```bash
# Debian or Ubuntu
sudo apt install ngx-laghu laghu-libvips

# Fedora, RHEL, Rocky, or AlmaLinux
sudo dnf install ngx-laghu laghu-libvips
```

Packages install the matched module, three worker executables, services, provider files, runtime directories, and disabled-by-default NGINX configuration.

After installation, validate before reload:

```bash
nginx -t
systemctl status laghu-libvips laghu-resource-fetch laghu-js-optimize
```

## Homebrew

```bash
brew install Code-Vedas/tap/ngx-laghu
```

The formula builds against the supported Homebrew NGINX, installs a disabled configuration, starts worker services, and validates `nginx -t`.

## Container

The production image contains matched NGINX, the adapter, workers, providers, entrypoint, health probes, and persistent cache/RUM mount points.

```bash
docker pull ghcr.io/code-vedas/ngx-laghu:latest
docker run --rm ghcr.io/code-vedas/ngx-laghu:latest nginx -t
```

## Windows

```powershell
winget install CodeVedas.NgxLaghu
```

The installer includes matched NGINX and native workers, configures their services and ACLs, and validates the signed build manifest.

## Source Build

```bash
NGINX_VERSION=1.31.3 scripts/build-nginx-module
```

The module is written beneath `tmp/nginx/1.31.3/build/`.
Load the generated `ngx_http_laghu_module.so` at NGINX main scope, include the Laghu configuration inside `http`, and run `nginx -t` before reload.
