# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

.PHONY: all build test lint nginx nginx-smoke apache apache-smoke docs docker clean

all:
	scripts/run-all

build:
	scripts/run-build-all

test:
	scripts/run-test-all

lint:
	scripts/run-lint-all

nginx:
	scripts/run-module-build-all

nginx-smoke:
	scripts/run-module-smoke-all

apache:
	scripts/build-apache-module

apache-smoke:
	scripts/run-apache-module-smoke-all

docs:
	scripts/run-docs-build-all

docker:
	scripts/run-in-docker scripts/run-all

clean:
	cmake -E remove_directory tmp
