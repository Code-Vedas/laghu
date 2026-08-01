---
layout: default
title: CDN asset offload
parent: Developer guide
nav_order: 8
---

# CDN asset offload

Laghu can publish eligible origin assets to immutable S3-compatible storage without making provider I/O part of a request. Cold responses retain origin URLs. The `laghu-asset-upload` worker uploads or verifies queued objects, and later HTML, CSS, and JavaScript responses rewrite only catalog records in the verified `ready` state.

Copy `packaging/asset-offload.conf.example` to an administrator-owned location and adjust its source, public CDN, MIME, path, storage, and environment-variable settings. Credential values belong only in the named environment variables. Direct credentials, HTTP endpoints, loopback/IP origins, malformed mappings, unsupported providers, and incomplete upload settings fail configuration validation.

Configure both values on the selected surface:

```nginx
laghu asset_offload_config /etc/laghu/asset-offload.conf;
laghu asset_upload_queue /run/laghu/assets;
```

```apache
Laghu AssetOffloadConfig /etc/laghu/asset-offload.conf
Laghu AssetUploadQueue /run/laghu/assets
```

```console
laghu ... --asset-offload-config /etc/laghu/asset-offload.conf --asset-upload-queue /run/laghu/assets
```

`rewrite_only` verifies pre-existing immutable objects; `upload_and_rewrite` uploads and then verifies them. Both modes preserve the original URL for pending, failed, stale, corrupt, oversized, denied, or unavailable assets. Query strings and fragments are preserved by default but never determine the immutable content key.

Provider health is represented by catalog transitions. Retryable failures use bounded exponential backoff; permanent failures remain fail-open. Deleting a failed catalog record allows an administrator to retry after correcting configuration. Mutable overwrites, proxy-domain routing, direct-file loading, and vendor-specific providers are outside this feature.
