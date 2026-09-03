# syntax=docker/dockerfile:1

# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}

ARG PACKAGE_FAMILY
ARG LAGHU_WITH_VIPS=ON
ARG LAGHU_WITH_CHROME=OFF
ARG LAGHU_CHROME_FOCUSED_TESTS=OFF
ARG LAGHU_PACKAGE_PROOF_ONLY=OFF
ARG LAGHU_SKIP_PACKAGE=OFF
ARG LAGHU_TEST_USER=laghu-test
ENV PACKAGE_FAMILY=${PACKAGE_FAMILY} LAGHU_WITH_VIPS=${LAGHU_WITH_VIPS} \
    LAGHU_WITH_CHROME=${LAGHU_WITH_CHROME} \
    LAGHU_CHROME_FOCUSED_TESTS=${LAGHU_CHROME_FOCUSED_TESTS} \
    LAGHU_PACKAGE_PROOF_ONLY=${LAGHU_PACKAGE_PROOF_ONLY} \
    LAGHU_SKIP_PACKAGE=${LAGHU_SKIP_PACKAGE} \
    LAGHU_TEST_USER=${LAGHU_TEST_USER} \
    DEBIAN_FRONTEND=noninteractive
COPY --from=rust:1.95-bookworm /usr/local/cargo /usr/local/cargo
COPY --from=rust:1.95-bookworm /usr/local/rustup /usr/local/rustup
ENV PATH=/usr/local/cargo/bin:${PATH} RUSTUP_HOME=/usr/local/rustup CARGO_HOME=/usr/local/cargo

WORKDIR /workspace
COPY packaging/test/install-dependencies /workspace/packaging/test/install-dependencies
RUN packaging/test/install-dependencies
ARG SERVER
ARG APACHE_MPM=
ENV SERVER=${SERVER} APACHE_MPM=${APACHE_MPM}
RUN if ! id -u "${LAGHU_TEST_USER}" >/dev/null 2>&1; then useradd --create-home --shell /bin/sh "${LAGHU_TEST_USER}"; fi
COPY . /workspace
RUN packaging/test/run
