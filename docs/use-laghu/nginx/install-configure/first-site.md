---
title: First site
parent: Install and configure
grand_parent: NGINX
ancestor: Use Laghu
---
# First site

Enable `laghu on;` in one server block only. Set `laghu worker_queue` and `laghu file_cache_backend` to writable runtime paths.

~~~nginx
server {
  listen 80;
  server_name shop.example.test;
  root /srv/www/shop;
  laghu on;
  laghu preset safe;
  laghu worker_queue /run/laghu/jobs.queue;
  laghu file_cache_backend file:///var/cache/laghu/images;
}
~~~

Create the directories with the NGINX worker account ownership before testing.

## Check

Follow the next page only after this step has completed without an installation or configuration error.
