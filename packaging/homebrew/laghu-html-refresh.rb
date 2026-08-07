# typed: strict
# frozen_string_literal: true

# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Homebrew package for the Laghu HTTPS HTML cache refresh worker.
class LaghuHtmlRefresh < Formula
  desc "HTTPS stale-while-revalidate worker for Laghu HTML caches"
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
           "laghu-html-refresh", "--parallel"
    bin.install "build/workers/laghu-html-refresh/laghu-html-refresh"
    share.install "packaging/html-refresh.conf.example"
    (var/"run/laghu").mkpath
    (var/"cache/laghu/images").mkpath
  end

  def caveats
    <<~EOS
      Configure a specific HTTPS origin before running this worker:
        #{bin}/laghu-html-refresh --init-and-serve \\
          #{var}/run/laghu/html-refresh-site.queue \\
          #{var}/cache/laghu/images https://origin.example.com

      Use the same queue, cache, and origin in Laghu's HTML cache settings.
      This worker has no Homebrew service because each origin needs its own
      explicit queue and lifecycle.
    EOS
  end

  test do
    system bin/"laghu-html-refresh", "--init", testpath/"html-refresh.queue",
           testpath, "https://origin.example.com"
  end
end
