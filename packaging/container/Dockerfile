# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

ARG NGINX_VERSION=1.30.4
ARG LAGHU_BUILD_REVISION=0.1.0

FROM ubuntu:24.04 AS build

ARG NGINX_VERSION

RUN apt-get update && apt-get install --no-install-recommends -y \
      build-essential ca-certificates cmake curl libpcre2-dev libssl-dev \
      libvips-dev pkgconf zlib1g-dev && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
COPY --from=rust:1.95-bookworm /usr/local/cargo /usr/local/cargo
COPY --from=rust:1.95-bookworm /usr/local/rustup /usr/local/rustup
ENV PATH=/usr/local/cargo/bin:${PATH} RUSTUP_HOME=/usr/local/rustup CARGO_HOME=/usr/local/cargo
RUN cmake -S . -B build -DLAGHU_WITH_VIPS=ON -DLAGHU_BUILD_TESTS=OFF \
      -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --target laghu-libvips laghu-resource-fetch laghu-html-refresh laghu-asset-upload laghu-js-optimize laghu-chrome-analyze --parallel

RUN curl --fail --location --retry 3 \
      --output /tmp/nginx.tar.gz \
      "https://nginx.org/download/nginx-${NGINX_VERSION}.tar.gz" && \
    mkdir -p /tmp/nginx && \
    tar -xzf /tmp/nginx.tar.gz --strip-components=1 -C /tmp/nginx && \
    cd /tmp/nginx && \
    ./configure \
      --prefix=/etc/nginx \
      --sbin-path=/usr/sbin/nginx \
      --modules-path=/usr/lib/nginx/modules \
      --conf-path=/etc/nginx/nginx.conf \
      --pid-path=/run/laghu/nginx.pid \
      --error-log-path=/dev/stderr \
      --http-log-path=/dev/stdout \
      --http-client-body-temp-path=/var/cache/nginx/client_temp \
      --http-proxy-temp-path=/var/cache/nginx/proxy_temp \
      --http-fastcgi-temp-path=/var/cache/nginx/fastcgi_temp \
      --http-uwsgi-temp-path=/var/cache/nginx/uwsgi_temp \
      --http-scgi-temp-path=/var/cache/nginx/scgi_temp \
      --with-compat \
      --with-threads \
      --with-http_ssl_module \
      --with-http_v2_module \
      --with-http_stub_status_module \
      --with-http_gzip_static_module \
      --with-pcre-jit \
      --with-cc-opt='-D_GNU_SOURCE' \
      --add-dynamic-module=/src/modules/ngx_http_laghu_module && \
    make -j"$(nproc)" && \
    make install DESTDIR=/out && \
    install -D -m 0644 LICENSE /out/usr/share/licenses/nginx/LICENSE

FROM ubuntu:24.04

ARG NGINX_VERSION
ARG LAGHU_BUILD_REVISION

LABEL org.opencontainers.image.title="ngx-laghu" \
      org.opencontainers.image.description="NGINX with the native ngx-laghu adapter" \
      org.opencontainers.image.url="https://laghu.codevedas.com" \
      org.opencontainers.image.source="https://github.com/Code-Vedas/laghu" \
      org.opencontainers.image.version="${LAGHU_BUILD_REVISION}" \
      org.opencontainers.image.licenses="MIT" \
      org.opencontainers.image.base.name="docker.io/library/ubuntu:24.04" \
      org.codevedas.laghu.nginx.version="${NGINX_VERSION}"

RUN apt-get update && apt-get install --no-install-recommends -y \
      ca-certificates curl ffmpeg libheif-plugin-aomenc libjxl0.7 libpcre2-8-0 libssl3t64 libvips42t64 tini zlib1g && \
    rm -rf /var/lib/apt/lists/* && \
    groupadd --system laghu && useradd --system --gid laghu laghu && \
    install -d -o laghu -g laghu -m 0750 \
      /run/laghu /var/cache/laghu /var/cache/laghu/images /var/cache/laghu/chrome-analysis /var/cache/nginx/client_temp \
      /var/cache/nginx/proxy_temp /var/cache/nginx/fastcgi_temp \
      /var/cache/nginx/uwsgi_temp /var/cache/nginx/scgi_temp \
      /usr/share/nginx/html && \
    install -d -o laghu -g laghu -m 0770 /var/lib/laghu/rum
COPY --from=build /out/usr/sbin/nginx /usr/sbin/nginx
COPY --from=build /out/usr/lib/nginx/modules/ngx_http_laghu_module.so \
  /usr/lib/nginx/modules/ngx_http_laghu_module.so
COPY --from=build /out/etc/nginx/mime.types /etc/nginx/mime.types
COPY --from=build /out/usr/share/licenses/nginx /usr/share/licenses/nginx
COPY --from=build /src/build/workers/laghu-libvips/laghu-libvips \
  /usr/bin/laghu-libvips
COPY --from=build /src/build/workers/laghu-resource-fetch/laghu-resource-fetch \
  /usr/bin/laghu-resource-fetch
COPY --from=build /src/build/workers/laghu-html-refresh/laghu-html-refresh \
  /usr/bin/laghu-html-refresh
COPY --from=build /src/build/workers/laghu-asset-upload/laghu-asset-upload \
  /usr/bin/laghu-asset-upload
COPY --from=build /src/build/workers/laghu-js-optimize/cargo/release/laghu-js-optimize \
  /usr/bin/laghu-js-optimize
COPY --from=build /src/build/workers/laghu-chrome-analyze/laghu-chrome-analyze \
  /usr/bin/laghu-chrome-analyze
COPY LICENSE packaging/NOTICE-libvips.md /usr/share/licenses/laghu/
COPY packaging/font-providers.conf /etc/laghu/font-providers.conf
COPY packaging/javascript-observation.conf /etc/laghu/javascript-observation.conf
COPY packaging/javascript-defer.conf /etc/laghu/javascript-defer.conf
COPY packaging/container/nginx.conf /etc/nginx/nginx.conf
COPY packaging/container/entrypoint /usr/bin/laghu-container-entrypoint
RUN printf '%s\n' '<!doctype html><title>Laghu</title><h1>Laghu is running</h1>' \
      >/usr/share/nginx/html/index.html && \
    chown -R laghu:laghu /etc/nginx /usr/share/nginx/html && \
    chmod 0755 /usr/bin/laghu-container-entrypoint

USER laghu
EXPOSE 8080
STOPSIGNAL SIGQUIT
HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
  CMD curl --fail --silent http://127.0.0.1:8080/ >/dev/null || exit 1
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/bin/laghu-container-entrypoint"]
