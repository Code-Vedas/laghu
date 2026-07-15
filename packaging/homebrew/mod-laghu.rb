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

  def install
    ENV["APXS"] = formula_opt_bin("httpd")/"apxs"
    ENV["APACHE_BUILD_DIR"] = buildpath/"apache-module"
    system "scripts/build-apache-module"
    libexec.install "apache-module/.libs/mod_laghu.so"
    (share/"laghu/mod-laghu.conf").write <<~EOS
      LoadModule laghu_module #{opt_libexec}/mod_laghu.so
      Laghu Off
      Laghu Preset balanced
      Laghu WorkerQueue #{var}/run/laghu/jobs.queue
      Laghu ImageCache #{var}/cache/laghu/images
    EOS
  end

  def post_install
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
