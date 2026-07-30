# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

@{
  Nginx = @{
    Version = "1.31.3"
    Url = "https://nginx.org/download/nginx-1.31.3.tar.gz"
    Sha256 = "a7657c50811c2d92d9895395e8b873ef60398142c4db21eb647811c38f6dd525"
  }
  Pcre2 = @{
    Version = "10.47"
    Url = "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.47/pcre2-10.47.tar.bz2"
    Sha256 = "47fe8c99461250d42f89e6e8fdaeba9da057855d06eb7fc08d9ca03fd08d7bc7"
  }
  Zlib = @{
    Version = "1.3.2"
    Url = "https://zlib.net/zlib-1.3.2.tar.gz"
    Sha256 = "bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16"
  }
  OpenSsl = @{
    Version = "3.6.3"
    Url = "https://github.com/openssl/openssl/releases/download/openssl-3.6.3/openssl-3.6.3.tar.gz"
    Sha256 = "243a86649cf6f23eeb6a2ff2456e09e5d77dd9018a54d3d96b0c6bdd6ba6c7f1"
  }
  Apache = @{
    Version = "2.4.68"
    Url = "https://downloads.apache.org/httpd/httpd-2.4.68.tar.gz"
    Sha256 = "ed9a9d4500fb48bb28eaffb3ba71d06ccf86d498fa13ab9f781da010cc488498"
  }
  Apr = @{
    Version = "1.7.6"
    Url = "https://downloads.apache.org/apr/apr-1.7.6-win32-src.zip"
    Sha256 = "ded6d19a5f76e7d133d25c1c130bb34986701590245ecccceca0f2a682015e13"
  }
  AprUtil = @{
    Version = "1.6.3"
    Url = "https://downloads.apache.org/apr/apr-util-1.6.3-win32-src.zip"
    Sha256 = "0f19148479f43fa77c6c6b4daa64f753beee72d7ebc33ed5396af28a190d3c67"
  }
}
