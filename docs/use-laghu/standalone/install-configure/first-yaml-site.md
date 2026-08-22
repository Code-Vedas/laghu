---
title: First YAML site
parent: Install and configure
grand_parent: Standalone
ancestor: Use Laghu
---
# First YAML site

Set `runtime.listen`, `runtime.origin`, and one `sites` entry with `host` and `document_root`. Start with `preset: safe` and one explicit route.

~~~yaml
runtime:
  listen: 0.0.0.0:8080
  origin: http://127.0.0.1:8000
  cache: /var/cache/laghu/images
  worker_queue: /run/laghu/jobs.queue
sites:
  - host: shop.example.test
    document_root: /srv/www/shop
    laghu:
      preset: safe
      compression: gzip
~~~

The default startup path is `/etc/laghu/laghu.yaml`; add site fragments under `/etc/laghu/conf.d`.

## Check

Follow the next page only after this step has completed without an installation or configuration error.
