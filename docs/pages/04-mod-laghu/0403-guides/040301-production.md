---
title: Production Configuration
parent: Guides
grand_parent: mod-laghu
nav_order: 1
permalink: /mod-laghu/guides/production/
---

# mod-laghu Production Configuration

This example configures process-wide RUM, all workers, a persistent cache, one optimized virtual host, and verified remote Redis/Valkey synchronization.

## Services and Permissions

```bash
systemctl enable --now laghu-libvips laghu-resource-fetch laghu-js-optimize
systemctl status laghu-libvips laghu-resource-fetch laghu-js-optimize
```

Grant the Apache worker account access through the `laghu` group and persist `/var/cache/laghu/images` and `/var/lib/laghu/rum`.

## Complete Apache Example

```apache
LoadModule laghu_module modules/mod_laghu.so

# Main-server-only RUM configuration.
Laghu RumStore "rediss://${LAGHU_REDIS_USERNAME}:${LAGHU_REDIS_PASSWORD}@rum.example.net:6380/0?prefix=laghu:&ca_file=/etc/laghu/redis-ca.pem"
Laghu RumStoreClientLibrary /usr/lib/libhiredis.so
Laghu RumStoreLocalSnapshot /var/lib/laghu/rum/rum.snapshot
Laghu RumStoreTimeout 100
Laghu RumStoreTtl 604800
Laghu RumStoreRetryLimit 3
Laghu RumStoreSyncInterval 5
Laghu RumStoreMemoryLimit 64m
Laghu RumStorePendingLimit 16m
Laghu RumStoreRequired Off

# Shared worker and cache contracts.
Laghu WorkerQueue /run/laghu/jobs.queue
Laghu FontFetchQueue /run/laghu/fonts.queue
Laghu FontProviderConfig /etc/laghu/font-providers.conf
Laghu JavaScriptQueue /run/laghu/javascript.queue
Laghu JavaScriptTarget "defaults and supports es6-module and not dead"
Laghu JavaScriptObservationConfig /etc/laghu/javascript-observation.conf
Laghu ImageCache /var/cache/laghu/images

<VirtualHost *:443>
  ServerName www.example.com
  SSLEngine On
  SSLCertificateFile /etc/httpd/tls/fullchain.pem
  SSLCertificateKeyFile /etc/httpd/tls/private.key

  Laghu On
  Laghu Preset balanced
  Laghu AllowApi Off
  Laghu ImageQuality 82
  Laghu ImageInlineLimit 2048
  Laghu ImageMetadataLimit 10000
  Laghu ImageMetadataTtl 7d
  Laghu CssInlineLimit 2048
  Laghu CssOutlineThreshold 8192
  Laghu JavaScriptInlineLimit 2048
  Laghu JavaScriptOutlineThreshold 8192
  Laghu ImageBeacon On
  Laghu CriticalCssBeacon On
  Laghu InstrumentationBeacon On
  Laghu InstrumentationSampleRate 10

  ProxyPass / http://application/
  ProxyPassReverse / http://application/

  <LocationMatch "^/(api|graphql)(/|$)">
    Laghu Off
  </LocationMatch>
</VirtualHost>
```

Apache environment interpolation and secret injection differ by service manager and distribution.
Ensure the final `RumStore` value reaches Laghu through an administrator-controlled mechanism without writing credentials into source control or logs.

## Validate and Roll Out

```bash
apache2ctl configtest
curl -sS -D- https://www.example.com/ -o /dev/null
curl -sS -H "X-Laghu-Purge-Token: $LAGHU_OPERATIONS_TOKEN" https://www.example.com/.laghu/ready
```

On RPM-family systems use `httpd -t` when that is the packaged command.
Start with one virtual host, verify cold/warm behavior and application semantics, then expand after metrics and RUM stabilize.
