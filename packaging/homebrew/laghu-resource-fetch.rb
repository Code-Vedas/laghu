# typed: strict
# frozen_string_literal: true

# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

class LaghuResourceFetch < Formula
  desc "Allowlisted external font stylesheet service for Laghu"
  homepage "https://laghu.codevedas.com"
  url "https://github.com/Code-Vedas/laghu.git", tag: "v0.1.0"
  license "MIT"

  depends_on "cmake" => :build
  depends_on "openssl@3"

  def install
    system "cmake", "-S", ".", "-B", "build",
           "-DLAGHU_WITH_VIPS=OFF", "-DLAGHU_BUILD_TESTS=OFF",
           "-DCMAKE_BUILD_TYPE=Release", *std_cmake_args
    system "cmake", "--build", "build", "--target",
           "laghu-resource-fetch", "--parallel"
    bin.install "build/workers/laghu-resource-fetch/laghu-resource-fetch"
    share.install "packaging/font-providers.conf"
    (var/"run/laghu").mkpath
    (var/"cache/laghu/images").mkpath
  end

  def post_install
    config = etc/"laghu/font-providers.conf"
    config.dirname.mkpath
    config.write((share/"font-providers.conf").read) unless config.exist?
  end

  service do
    run [opt_bin/"laghu-resource-fetch", "--init-and-serve",
         var/"run/laghu/fonts.queue", var/"cache/laghu/images",
         etc/"laghu/font-providers.conf"]
    keep_alive crashed: true
    working_dir var/"cache/laghu"
    log_path var/"log/laghu-resource-fetch.log"
    error_log_path var/"log/laghu-resource-fetch.log"
  end

  test do
    system bin/"laghu-resource-fetch", "--init", testpath/"fonts.queue",
           testpath, share/"font-providers.conf"
  end
end
