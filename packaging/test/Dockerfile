# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}

ARG PACKAGE_FAMILY
ARG LAGHU_WITH_VIPS=ON
ENV PACKAGE_FAMILY=${PACKAGE_FAMILY} LAGHU_WITH_VIPS=${LAGHU_WITH_VIPS} \
    DEBIAN_FRONTEND=noninteractive

WORKDIR /workspace
COPY packaging/test/install-dependencies /workspace/packaging/test/install-dependencies
RUN packaging/test/install-dependencies
ARG SERVER
ARG APACHE_MPM=
ENV SERVER=${SERVER} APACHE_MPM=${APACHE_MPM}
COPY . /workspace
RUN packaging/test/run
