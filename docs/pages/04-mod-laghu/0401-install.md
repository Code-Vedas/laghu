---
title: Installation
parent: mod-laghu
nav_order: 1
permalink: /mod-laghu/install/
---

# Install mod-laghu

Build or install `mod_laghu` for the target Apache 2.4 module ABI and APR environment.

## Distribution Packages

After configuring the Codevedas package repository, install:

```bash
# Debian or Ubuntu
sudo apt install mod-laghu laghu-libvips

# Fedora, RHEL, Rocky, or AlmaLinux
sudo dnf install mod-laghu laghu-libvips
```

Packages install and load the matched module, leave optimization disabled, and install workers, provider files, services, and runtime directories.

Validate after installation:

```bash
apache2ctl configtest
systemctl status laghu-libvips laghu-resource-fetch laghu-js-optimize
```

On RPM-family systems use the server-provided `httpd -t` command instead of `apache2ctl configtest` when appropriate.

## Homebrew

```bash
brew install Code-Vedas/tap/mod-laghu
```

The formula builds with the supported Homebrew `apxs`, installs a disabled include, starts workers, and validates HTTP Server configuration.

## Container

```bash
docker pull ghcr.io/code-vedas/mod-laghu:latest
docker run --rm ghcr.io/code-vedas/mod-laghu:latest apache2ctl configtest
```

## Windows

```powershell
winget install CodeVedas.ModLaghu
```

The installer contains matched Apache and workers, configures services and ACLs, and validates the signed build manifest.

## Source Build

```bash
scripts/build-apache-module
```

The default output is `tmp/apache-module/.libs/mod_laghu.so`.
Load it with `LoadModule laghu_module ...`, configure `Laghu Off` initially, and run `apache2ctl configtest` or `httpd -t` before restart.
