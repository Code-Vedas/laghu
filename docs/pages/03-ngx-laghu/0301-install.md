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

The repository contains Debian and RPM packaging for `ngx-laghu`, the three worker executables, service units, provider files, runtime directories, and disabled-by-default server configuration.
These definitions are build inputs; consult a release before assuming a package has been published for a distribution.

After installation, validate before reload:

```bash
nginx -t
systemctl status laghu-libvips laghu-resource-fetch laghu-js-optimize
```

## Homebrew

The Homebrew formula builds a module against the exact pinned Homebrew NGINX version and refuses a mismatch.
It installs a disabled configuration under the Homebrew prefix and validates `nginx -t`.

## Container

`packaging/container/Dockerfile` builds an NGINX image containing the module, workers, provider configuration, entrypoint, and persistent cache/RUM directories.
Build it from the repository root:

```bash
docker build -f packaging/container/Dockerfile -t ngx-laghu .
docker run --rm ngx-laghu nginx -t
```

## Windows

The Windows installer contains a matched NGINX build because an arbitrary prebuilt `nginx.exe` cannot safely load the adapter.
Installer construction validates the server machine type, build manifest, revision, and module hashes.

## Source Build

```bash
NGINX_VERSION=1.31.3 scripts/build-nginx-module
```

The module is written beneath `tmp/nginx/1.31.3/build/`.
Load the generated `ngx_http_laghu_module.so` at NGINX main scope, include the Laghu configuration inside `http`, and run `nginx -t` before reload.
