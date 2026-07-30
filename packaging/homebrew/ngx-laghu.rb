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

  depends_on "laghu-libvips"
  depends_on "laghu-resource-fetch"
  depends_on "nginx"

  resource "nginx" do
    url "https://nginx.org/download/nginx-1.31.3.tar.gz"
    sha256 "a7657c50811c2d92d9895395e8b873ef60398142c4db21eb647811c38f6dd525"
  end

  def install
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
      laghu image_cache #{var}/cache/laghu/images;
    EOS
  end

  def post_install
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
