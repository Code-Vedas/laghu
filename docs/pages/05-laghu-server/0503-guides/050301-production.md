---
title: Production Configuration
parent: Guides
grand_parent: Laghu Server
nav_order: 1
permalink: /laghu-server/guides/production/
---

# Laghu Server Production Configuration

The standalone server is installed as a managed service and can terminate downstream TLS, speak modern HTTP to clients, maintain bounded persistent origin connections, and expose the same optimization and administration surfaces as the native modules.

## Environment File

Store credentials in a root-readable service environment file rather than a unit or repository:

```bash
LAGHU_REDIS_USERNAME=laghu
LAGHU_REDIS_PASSWORD=replace-from-secret-manager
```

## Complete Service Command

```bash
laghu \
  --listen 0.0.0.0:8443 \
  --origin https://application.internal:443 \
  --origin-ca-file /etc/laghu/origin-ca.pem \
  --file-cache-backend file:///var/cache/laghu/images \
  --worker-queue /run/laghu/jobs.queue \
  --font-fetch-queue /run/laghu/fonts.queue \
  --font-provider-config /etc/laghu/font-providers.conf \
  --javascript-queue /run/laghu/javascript.queue \
  --javascript-target "defaults and supports es6-module and not dead" \
  --javascript-observation-config /etc/laghu/javascript-observation.conf \
  --javascript-inline-limit 2048 \
  --javascript-outline-threshold 8192 \
  --preset balanced \
  --image-quality 82 \
  --image-beacon \
  --critical-css-beacon \
  --instrumentation-beacon \
  --instrumentation-sample-rate 10 \
  --workers 8 \
  --connection-queue 1024 \
  --connect-timeout 5 \
  --io-timeout 30 \
  --drain-timeout 30 \
  --forwarded-headers both \
  --trusted-proxy 10.20.0.0/16 \
  --rum-store 'rediss://${LAGHU_REDIS_USERNAME}:${LAGHU_REDIS_PASSWORD}@rum.example.net:6380/0?prefix=laghu:&ca_file=/etc/laghu/redis-ca.pem' \
  --rum-store-client-library /usr/lib/libhiredis.so \
  --rum-store-local-snapshot /var/lib/laghu/rum/rum.snapshot \
  --rum-store-timeout 100 \
  --rum-store-ttl 604800 \
  --rum-store-retry-limit 3 \
  --rum-store-sync-interval 5 \
  --rum-store-memory-limit 64m \
  --rum-store-pending-limit 16m
```

Add `--rum-store-required` only when the service must refuse startup without the selected backend.
The default fail-open mode restores the local snapshot, keeps request decisions in memory, and retries backend synchronization.

## Service Lifecycle

Run `laghu-libvips`, `laghu-resource-fetch`, and `laghu-js-optimize` as independently supervised services.
Configure graceful termination to allow at least the selected drain timeout and expose readiness only after listener, origin trust, cache, and required dependencies validate.

## Validation

```bash
curl -sS -D- https://edge.example.com/ -o /dev/null
curl -sS -H "X-Laghu-Purge-Token: $LAGHU_OPERATIONS_TOKEN" https://edge.example.com/.laghu/ready
```

Warm representative traffic, compare application behavior and byte counts, then expand one bounded deployment scope at a time.
