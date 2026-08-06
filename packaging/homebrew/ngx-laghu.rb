# typed: strict
# frozen_string_literal: true

# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require "open3"
require "shellwords"

# Homebrew package for the Laghu NGINX adapter.
class NgxLaghu < Formula
  desc "Native Laghu HTTP optimization module for NGINX"
  homepage "https://laghu.codevedas.com"
  url "https://github.com/Code-Vedas/laghu.git", tag: "v0.1.0"
  license "MIT"

  depends_on "laghu-js-optimize"
  depends_on "laghu-libvips"
  depends_on "laghu-resource-fetch"
  depends_on "nginx"

  resource "nginx" do
    url "https://nginx.org/download/nginx-1.31.3.tar.gz"
    sha256 "a7657c50811c2d92d9895395e8b873ef60398142c4db21eb647811c38f6dd525"
  end

  def install
    (share/"laghu").install "packaging/javascript-observation.conf", "packaging/javascript-defer.conf"
    nginx_version = Formula["nginx"].version
    odie "ngx-laghu must be updated for nginx #{nginx_version}" if nginx_version != Version.new("1.31.3")
    _stdout, nginx_build, status = Open3.capture3(formula_opt_bin("nginx")/"nginx", "-V")
    odie "cannot inspect the Homebrew nginx build" unless status.success?
    configure_line = nginx_build[/configure arguments: (.*)$/, 1]
    odie "Homebrew nginx did not report configure arguments" if configure_line.nil?
    configure_args = Shellwords.split(configure_line)
    configure_args << "--with-compat" unless configure_args.include?("--with-compat")
    configure_args << "--add-dynamic-module=#{buildpath}/modules/ngx_http_laghu_module"
    resource("nginx").stage do
      system "./configure", *configure_args
      system "make", "modules"
      libexec.install "objs/ngx_http_laghu_module.so"
    end
    (share/"laghu/ngx-laghu.conf").write <<~EOS
      laghu off;
      laghu preset balanced;
      laghu worker_queue #{var}/run/laghu/jobs.queue;
      laghu font_fetch_queue #{var}/run/laghu/fonts.queue;
      laghu font_provider_config #{etc}/laghu/font-providers.conf;
      laghu javascript_queue #{var}/run/laghu/javascript.queue;
      laghu javascript_observation_config #{etc}/laghu/javascript-observation.conf;
      laghu javascript_defer_config #{etc}/laghu/javascript-defer.conf;
      laghu file_cache_backend file://#{var}/cache/laghu/images;
      laghu rum_store local:;
      laghu rum_store_local_snapshot #{var}/lib/laghu/rum/rum.snapshot;
      # For Redis, set rum_store_client_library to the installed hiredis 1.x library.
      laghu rum_store_timeout 100;
      laghu rum_store_ttl 604800;
      laghu rum_store_retry_limit 3;
      laghu rum_store_sync_interval 5;
      laghu rum_store_memory_limit 8m;
      laghu rum_store_pending_limit 1m;
      laghu rum_store_required off;
    EOS
  end

  def post_install
    (var/"lib/laghu/rum").mkpath
    observation_config = etc/"laghu/javascript-observation.conf"
    observation_config.dirname.mkpath
    observation_config.write((share/"laghu/javascript-observation.conf").read) unless observation_config.exist?
    defer_config = etc/"laghu/javascript-defer.conf"
    defer_config.write((share/"laghu/javascript-defer.conf").read) unless defer_config.exist?
    config = etc/"nginx/nginx.conf"
    loader = "load_module #{opt_libexec}/ngx_http_laghu_module.so;"
    inreplace(config) { |s| s.sub!(/\A/, "#{loader}\n") } unless config.read.include?(loader)
    laghu_config = etc/"nginx/conf.d/ngx-laghu.conf"
    laghu_config.dirname.mkpath
    laghu_config.write((share/"laghu/ngx-laghu.conf").read) unless laghu_config.exist?
    system formula_opt_bin("nginx")/"nginx", "-t", "-c", config
  end

  def caveats
    "ngx-laghu is loaded but disabled; include #{etc}/nginx/conf.d/ngx-laghu.conf inside http before enabling it."
  end

  test do
    assert_path_exists libexec/"ngx_http_laghu_module.so"
  end
end
