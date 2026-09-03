# typed: strict
# frozen_string_literal: true

# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Homebrew package for the optional Laghu headless Chrome analysis worker.
class LaghuChromeAnalyze < Formula
  desc "Optional headless Chromium analysis worker for Laghu"
  homepage "https://laghu.codevedas.com"
  url "https://github.com/Code-Vedas/laghu.git", tag: "v0.1.0"
  license "MIT"

  depends_on "cmake" => :build

  def install
    system "cmake", "-S", ".", "-B", "build",
           "-DLAGHU_WITH_VIPS=OFF", "-DLAGHU_BUILD_TESTS=OFF",
           "-DCMAKE_BUILD_TYPE=Release", *std_cmake_args
    system "cmake", "--build", "build", "--target",
           "laghu-chrome-analyze", "--parallel"
    bin.install "build/workers/laghu-chrome-analyze/laghu-chrome-analyze"
  end

  def caveats
    <<~EOS
      Chrome analysis is unsupported on macOS. Laghu requires a Linux user,
      mount, and network namespace plus seccomp boundary before it will run
      captured HTML, so this formula intentionally provides no service.

      Keep ChromeAnalysisQueue disabled on macOS. Use the supported Linux
      package service with Bubblewrap and a distribution Chromium binary.
    EOS
  end

  test do
    system bin/"laghu-chrome-analyze", "--init", testpath/"chrome.queue",
           testpath
  end
end
