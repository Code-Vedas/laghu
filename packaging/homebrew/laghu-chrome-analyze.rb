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
    (var/"run/laghu").mkpath
    (var/"cache/laghu/chrome-analysis").mkpath
  end

  service do
    run [opt_bin/"laghu-chrome-analyze", "--init-and-serve",
         var/"run/laghu/chrome-analysis.queue",
         var/"cache/laghu/chrome-analysis", "chromium"]
    keep_alive crashed: true
    environment_variables PATH: std_service_path_env
    working_dir var/"cache/laghu"
    log_path var/"log/laghu-chrome-analyze.log"
    error_log_path var/"log/laghu-chrome-analyze.log"
  end

  def caveats
    <<~EOS
      This optional worker does not install a browser. Install Chromium first:
        brew install --cask chromium

      Configure the matching Laghu analysis queue:
        #{var}/run/laghu/chrome-analysis.queue

      Then start the worker:
        brew services start laghu-chrome-analyze

      Run `brew info laghu-chrome-analyze` to see these matching paths.
      The service finds the Chromium cask's `chromium` command through
      Homebrew's standard service PATH. To use another browser executable,
      run `laghu-chrome-analyze --init-and-serve QUEUE OUTPUT BROWSER` under
      your preferred supervisor instead of this default service.
    EOS
  end

  test do
    system bin/"laghu-chrome-analyze", "--init", testpath/"chrome.queue",
           testpath
  end
end
