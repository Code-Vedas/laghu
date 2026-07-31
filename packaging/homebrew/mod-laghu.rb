# typed: strict
# frozen_string_literal: true

# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Homebrew package for the Laghu Apache HTTP Server adapter.
class ModLaghu < Formula
  desc "Native Laghu output-filter module for Apache HTTP Server"
  homepage "https://laghu.codevedas.com"
  url "https://github.com/Code-Vedas/laghu.git", tag: "v0.1.0"
  license "MIT"

  depends_on "httpd"
  depends_on "laghu-libvips"
  depends_on "laghu-resource-fetch"
  depends_on "laghu-js-optimize"

  def install
    (share/"laghu").install "packaging/javascript-observation.conf", "packaging/javascript-defer.conf"
    ENV["APXS"] = formula_opt_bin("httpd")/"apxs"
    ENV["APACHE_BUILD_DIR"] = buildpath/"apache-module"
    system "scripts/build-apache-module"
    libexec.install "apache-module/.libs/mod_laghu.so"
    (share/"laghu/mod-laghu.conf").write <<~EOS
      LoadModule laghu_module #{opt_libexec}/mod_laghu.so
      Laghu Off
      Laghu Preset balanced
      Laghu WorkerQueue #{var}/run/laghu/jobs.queue
      Laghu FontFetchQueue #{var}/run/laghu/fonts.queue
      Laghu FontProviderConfig #{etc}/laghu/font-providers.conf
      Laghu JavaScriptObservationConfig #{etc}/laghu/javascript-observation.conf
      Laghu JavaScriptDeferConfig #{etc}/laghu/javascript-defer.conf
      Laghu ImageCache #{var}/cache/laghu/images
      Laghu RumStore local:
      Laghu RumStoreLocalSnapshot #{var}/lib/laghu/rum/rum.snapshot
      # For Redis, set RumStoreClientLibrary to the installed hiredis 1.x library.
      Laghu RumStoreTimeout 100
      Laghu RumStoreTtl 604800
      Laghu RumStoreRetryLimit 3
      Laghu RumStoreSyncInterval 5
      Laghu RumStoreMemoryLimit 8m
      Laghu RumStorePendingLimit 1m
      Laghu RumStoreRequired Off
    EOS
  end

  def post_install
    (var/"lib/laghu/rum").mkpath
    observation_config = etc/"laghu/javascript-observation.conf"
    observation_config.dirname.mkpath
    observation_config.write((share/"laghu/javascript-observation.conf").read) unless observation_config.exist?
    defer_config = etc/"laghu/javascript-defer.conf"
    defer_config.write((share/"laghu/javascript-defer.conf").read) unless defer_config.exist?
    config = etc/"httpd/httpd.conf"
    laghu_config = etc/"httpd/extra/mod-laghu.conf"
    laghu_config.dirname.mkpath
    laghu_config.write((share/"laghu/mod-laghu.conf").read) unless laghu_config.exist?
    include_line = "Include #{etc}/httpd/extra/mod-laghu.conf"
    inreplace(config) { |s| s.sub!(/\z/, "\n#{include_line}\n") } unless config.read.include?(include_line)
    system formula_opt_bin("httpd")/"httpd", "-t", "-f", config
  end

  def caveats
    "mod-laghu is loaded but disabled; set Laghu On explicitly to enable it."
  end

  test do
    assert_path_exists libexec/"mod_laghu.so"
  end
end
