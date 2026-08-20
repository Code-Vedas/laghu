---
title: Installation
parent: Laghu Server
nav_order: 1
permalink: /laghu-server/install/
---

# Install the Laghu Server

## Distribution Packages

```bash
# Debian or Ubuntu
sudo apt install laghu

# Fedora, RHEL, Rocky, or AlmaLinux
sudo dnf install laghu
```

The package installs the server, native workers, provider configuration, service units, administrator CLI, health probes, runtime directories, and disabled example service configuration.

## Homebrew

```bash
brew install Code-Vedas/tap/laghu
brew services start laghu
```

### Optional Chrome analysis

Only pass `--chrome-analysis-queue` when browser analysis is wanted. Install Chromium and the separate worker, then start its user service:

```bash
brew install --cask chromium
brew install Code-Vedas/tap/laghu-chrome-analyze
brew services start laghu-chrome-analyze
```

Use the queue path shown by `brew info laghu-chrome-analyze` for `--chrome-analysis-queue`. Laghu publishes no browser work without that option; worker must remain running while it is set.

## Containers

```bash
docker pull ghcr.io/code-vedas/laghu:latest
docker run --rm ghcr.io/code-vedas/laghu:latest --version
```

Mount persistent cache and RUM directories and provide origin/TLS/Redis secrets through the orchestrator rather than baking them into the image.

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
cp servers/laghu/laghu.yaml.example /tmp/laghu.yaml
# Set origin and cache paths in /tmp/laghu.yaml.
tmp/build/servers/laghu/laghu --config /tmp/laghu.yaml
```

Use administrator-owned persistent paths instead of `/tmp` in production.
