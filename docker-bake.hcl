# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

group "default" {
  targets = ["ubuntu-nginx"]
}

target "base" {
  context = "."
  dockerfile = "packaging/test/Dockerfile"
}

target "debian-nginx" {
  inherits = ["base"]
  args = { BASE_IMAGE = "debian:13", PACKAGE_FAMILY = "apt", SERVER = "nginx" }
}
target "debian-apache" {
  inherits = ["base"]
  args = { BASE_IMAGE = "debian:13", PACKAGE_FAMILY = "apt", SERVER = "apache" }
}
target "ubuntu-nginx" {
  inherits = ["base"]
  args = { BASE_IMAGE = "ubuntu:24.04", PACKAGE_FAMILY = "apt", SERVER = "nginx" }
}
target "ubuntu-apache" {
  inherits = ["base"]
  args = { BASE_IMAGE = "ubuntu:24.04", PACKAGE_FAMILY = "apt", SERVER = "apache" }
}
target "ubuntu-nginx-no-libvips" {
  inherits = ["base"]
  args = {
    BASE_IMAGE = "ubuntu:24.04"
    PACKAGE_FAMILY = "apt"
    SERVER = "nginx"
    LAGHU_SKIP_PACKAGE = "ON"
  }
}
target "debian-nginx-chrome-analysis" {
  inherits = ["base"]
  args = {
    BASE_IMAGE = "debian:13"
    PACKAGE_FAMILY = "apt"
    SERVER = "nginx"
    LAGHU_WITH_VIPS = "OFF"
    LAGHU_WITH_CHROME = "ON"
  }
}
target "ubuntu-apache-event" {
  inherits = ["base"]
  args = {
    BASE_IMAGE = "ubuntu:24.04"
    PACKAGE_FAMILY = "apt"
    SERVER = "apache"
    APACHE_MPM = "event"
  }
}
target "ubuntu-apache-worker" {
  inherits = ["base"]
  args = {
    BASE_IMAGE = "ubuntu:24.04"
    PACKAGE_FAMILY = "apt"
    SERVER = "apache"
    APACHE_MPM = "worker"
  }
}
target "ubuntu-apache-prefork" {
  inherits = ["base"]
  args = {
    BASE_IMAGE = "ubuntu:24.04"
    PACKAGE_FAMILY = "apt"
    SERVER = "apache"
    APACHE_MPM = "prefork"
  }
}
target "fedora-nginx" {
  inherits = ["base"]
  args = { BASE_IMAGE = "fedora:44", PACKAGE_FAMILY = "dnf", SERVER = "nginx" }
}
target "fedora-apache" {
  inherits = ["base"]
  args = { BASE_IMAGE = "fedora:44", PACKAGE_FAMILY = "dnf", SERVER = "apache" }
}
target "rocky-nginx" {
  inherits = ["base"]
  args = { BASE_IMAGE = "rockylinux:9", PACKAGE_FAMILY = "yum", SERVER = "nginx" }
}
target "rocky-apache" {
  inherits = ["base"]
  args = { BASE_IMAGE = "rockylinux:9", PACKAGE_FAMILY = "yum", SERVER = "apache" }
}
target "alma-nginx" {
  inherits = ["base"]
  args = { BASE_IMAGE = "almalinux:9", PACKAGE_FAMILY = "yum", SERVER = "nginx" }
}
target "alma-apache" {
  inherits = ["base"]
  args = { BASE_IMAGE = "almalinux:9", PACKAGE_FAMILY = "yum", SERVER = "apache" }
}
