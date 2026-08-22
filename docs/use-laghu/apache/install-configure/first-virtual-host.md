---
title: First virtual host
parent: Install and configure
grand_parent: Apache
ancestor: Use Laghu
---
# First virtual host

Set `Laghu On` in one virtual host, then point `Laghu WorkerQueue` and `Laghu ImageCache` at writable locations.

~~~apache
<VirtualHost *:80>
  ServerName shop.example.test
  DocumentRoot /srv/www/shop
  Laghu On
  Laghu Preset safe
  Laghu WorkerQueue /run/laghu/jobs.queue
  Laghu ImageCache /var/cache/laghu/images
</VirtualHost>
~~~

Create the directories with the Apache service account ownership before testing.

## Check

Follow the next page only after this step has completed without an installation or configuration error.
