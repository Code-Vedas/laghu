# typed: strict
# frozen_string_literal: true

# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Homebrew package for the shared Laghu image service.
class LaghuLibvips < Formula
  desc "Shared asynchronous libvips service for Laghu"
  homepage "https://laghu.codevedas.com"
  url "https://github.com/Code-Vedas/laghu.git", tag: "v0.1.0"
  license "MIT"

  depends_on "cmake" => :build
  depends_on "pkgconf" => :build
  depends_on "vips"

  def install
    system "cmake", "-S", ".", "-B", "build",
           "-DLAGHU_WITH_VIPS=ON", "-DLAGHU_BUILD_TESTS=OFF",
           "-DCMAKE_BUILD_TYPE=Release", *std_cmake_args
    system "cmake", "--build", "build", "--target", "laghu-libvips", "--parallel"
    bin.install "build/workers/laghu-libvips/laghu-libvips"
    (var/"run/laghu").mkpath
    (var/"cache/laghu/images").mkpath
  end

  service do
    run [opt_bin/"laghu-libvips", "--init-and-serve",
         var/"run/laghu/jobs.queue", var/"cache/laghu/images"]
    keep_alive crashed: true
    working_dir var/"cache/laghu"
    log_path var/"log/laghu-libvips.log"
    error_log_path var/"log/laghu-libvips.log"
  end

  test do
    assert_match "backend=", shell_output("#{bin}/laghu-libvips --probe")
  end
end
