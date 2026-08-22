# ngx_http_laghu_module

This directory contains Laghu native NGINX integration. It maps NGINX request and response state into the shared HTTP engine while NGINX retains chains, headers, and configuration inheritance.

Build it with an NGINX source tree using `--add-dynamic-module=/path/to/laghu/modules/ngx_http_laghu_module`. Operator configuration is in `docs/pages/03-ngx-laghu/`.
