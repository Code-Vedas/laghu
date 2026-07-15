---
title: Installation
nav_order: 2
permalink: /installation/
---

# Installation

Laghu currently ships as source. The module must be built against a compatible
NGINX source release and loaded as a dynamic module.

## Prerequisites

- a C11 compiler and `make`
- CMake 3.20 or newer for the core library tests
- `curl` and `tar` for the repository build helper
- NGINX build dependencies for the selected release

On Debian or Ubuntu:

```bash
sudo apt-get install build-essential cmake curl libpcre2-dev libssl-dev zlib1g-dev
```

## Build With the Repository Helper

From the repository root:

```bash
scripts/run-module-build-all
```

Set `NGINX_VERSION` to select another release:

```bash
NGINX_VERSION=1.31.2 scripts/run-module-build-all
```

The artifact is written under `tmp/nginx/<version>/build/`.

## Build From an Existing NGINX Source Tree

```bash
cd /path/to/nginx-source
./configure \
  --with-compat \
  --add-dynamic-module=/path/to/laghu/modules/ngx_http_laghu_module
make modules
```

Copy `objs/ngx_http_laghu_module.so` into the module directory used by the
target NGINX installation. Dynamic modules must be built for the target NGINX
version and compatible build options.

## Load the Module

Add the module before the `events` block:

```nginx
load_module modules/ngx_http_laghu_module.so;
```

Then enable the pass-through skeleton in an `http`, `server`, or `location`
context:

```nginx
http {
  server {
    listen 8080;
    laghu on;
    laghu preset balanced;
  }
}
```

Validate the configuration before reloading:

```bash
nginx -t
```

