---
title: Installation
parent: mod-laghu
nav_order: 1
permalink: /mod-laghu/install/
---

# Install mod-laghu

Build or install `mod_laghu` for the target Apache 2.4 module ABI and APR environment.

## Distribution Packages

The Debian package uses Apache module helpers to install and enable `mod-laghu`; the RPM installs `mod_laghu.so`, its load file, disabled configuration, workers, services, and runtime directories.
The packaging definitions are release inputs and do not imply that every distribution artifact has been published.

Validate after installation:

```bash
apache2ctl configtest
systemctl status laghu-libvips laghu-resource-fetch laghu-js-optimize
```

On RPM-family systems use the server-provided `httpd -t` command instead of `apache2ctl configtest` when appropriate.

## Homebrew

The Homebrew formula builds with the installed `apxs`, writes a disabled include file, and validates the resulting HTTP Server configuration.

## Container

```bash
docker build -f packaging/container/Dockerfile.apache -t mod-laghu .
docker run --rm mod-laghu apache2ctl configtest
```

## Windows

The Windows installer contains a matched Apache build and validates its manifest, architecture, and module before installation.

## Source Build

```bash
scripts/build-apache-module
```

The default output is `tmp/apache-module/.libs/mod_laghu.so`.
Load it with `LoadModule laghu_module ...`, configure `Laghu Off` initially, and run `apache2ctl configtest` or `httpd -t` before restart.
