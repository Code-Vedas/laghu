---
title: Production Configuration
parent: Guides
grand_parent: ngx-laghu
nav_order: 1
permalink: /ngx-laghu/guides/production/
---

# ngx-laghu Production Configuration

This example enables the balanced policy, every asynchronous worker, persistent immutable storage, RUM instrumentation, and a durable remote Redis/Valkey backend over verified TLS.
Replace paths and endpoints with values managed by your deployment system.

## Filesystem and Services

The NGINX worker account needs read/write access to the immutable cache and read access to queue files and administrator-owned provider configuration.
The `laghu` service account owns the queue and cache directories.

```bash
systemctl enable --now laghu-libvips laghu-resource-fetch laghu-js-optimize
systemctl status laghu-libvips laghu-resource-fetch laghu-js-optimize
```

Persist `/var/cache/laghu/images` and `/var/lib/laghu/rum` across host or container replacement.

## Complete NGINX Example

```nginx
load_module modules/ngx_http_laghu_module.so;

events {
  worker_connections 4096;
}

http {
  # Process-wide RUM state. Keep secrets in the service environment.
  laghu rum_store "rediss://${LAGHU_REDIS_USERNAME}:${LAGHU_REDIS_PASSWORD}@rum.example.net:6380/0?prefix=laghu:&ca_file=/etc/laghu/redis-ca.pem";
  laghu rum_store_client_library /usr/lib/libhiredis.so;
  laghu rum_store_local_snapshot /var/lib/laghu/rum/rum.snapshot;
  laghu rum_store_timeout 100;
  laghu rum_store_ttl 604800;
  laghu rum_store_retry_limit 3;
  laghu rum_store_sync_interval 5;
  laghu rum_store_memory_limit 64m;
  laghu rum_store_pending_limit 16m;
  laghu rum_store_required off;

  # Shared worker and cache contracts.
  laghu worker_queue /run/laghu/jobs.queue;
  laghu font_fetch_queue /run/laghu/fonts.queue;
  laghu font_provider_config /etc/laghu/font-providers.conf;
  laghu javascript_queue /run/laghu/javascript.queue;
  laghu javascript_target "defaults and supports es6-module and not dead";
  laghu javascript_observation_config /etc/laghu/javascript-observation.conf;
  laghu file_cache_backend file:///var/cache/laghu/images;

  server {
    listen 443 ssl http2;
    server_name www.example.com;

    ssl_certificate /etc/nginx/tls/fullchain.pem;
    ssl_certificate_key /etc/nginx/tls/private.key;

    laghu on;
    laghu preset balanced;
    laghu allow_api off;
    laghu image_quality 82;
    laghu image_inline_limit 2048;
    laghu image_metadata_limit 10000;
    laghu image_metadata_ttl 7d;
    laghu css_inline_limit 2048;
    laghu css_outline_threshold 8192;
    laghu javascript_inline_limit 2048;
    laghu javascript_outline_threshold 8192;

    # Opt-in learning. Sampling occurs in the browser.
    laghu image_beacon on;
    laghu critical_css_beacon on;
    laghu instrumentation_beacon on;
    laghu instrumentation_sample_rate 10;

    location / {
      proxy_pass http://application;
    }

    location ~ ^/(api|graphql)(/|$) {
      laghu off;
      proxy_pass http://application;
    }
  }
}
```

## Redis TLS and Credentials

Use `rediss://` for every non-loopback backend.
The CA file must validate the Redis/Valkey service certificate, and the URI host must match its certificate identity.
Supply `LAGHU_REDIS_USERNAME` and `LAGHU_REDIS_PASSWORD` through the NGINX service environment or secret manager; never commit the expanded URI.
Laghu loads hiredis dynamically from `rum_store_client_library`, verifies TLS, and sends bounded idempotent Lua merge batches from the background synchronization thread only.

## Validation and Rollout

```bash
nginx -t
laghu doctor
laghu status
curl -sS -D- https://www.example.com/ -o /dev/null
```

Begin with one canary server or location, compare cold and warm responses, inspect `X-Laghu`, validators, cache metrics, worker health, and Core Web Vitals, then expand the rollout.
If a dependency fails, keep serving traffic and diagnose the affected optimization rather than disabling the entire server.
