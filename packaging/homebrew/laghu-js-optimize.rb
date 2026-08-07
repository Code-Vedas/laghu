# typed: strict
# frozen_string_literal: true

# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Homebrew package for the Laghu JavaScript optimization worker.
class LaghuJsOptimize < Formula
  desc "Native SWC JavaScript optimization worker for Laghu"
  homepage "https://laghu.codevedas.com"
  url "https://github.com/Code-Vedas/laghu.git", tag: "v0.1.0"
  license "MIT"

  depends_on "rust" => :build

  def install
    cd "workers/laghu-js-optimize" do
      system "cargo", "install", *std_cargo_args(path: ".")
    end
  end

  service do
    run [opt_bin/"laghu-js-optimize", "--init-and-serve",
         var/"run/laghu/javascript.queue", var/"cache/laghu/images"]
    keep_alive true
    working_dir var/"cache/laghu"
    log_path var/"log/laghu-js-optimize.log"
    error_log_path var/"log/laghu-js-optimize.log"
  end

  test do
    assert_match "available=yes", shell_output("#{bin}/laghu-js-optimize --probe")
  end
end
