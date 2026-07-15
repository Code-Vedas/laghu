.PHONY: all build test lint module module-smoke docs docker clean

all:
	scripts/run-all

build:
	scripts/run-build-all

test:
	scripts/run-test-all

lint:
	scripts/run-lint-all

module:
	scripts/run-module-build-all

module-smoke:
	scripts/run-module-smoke-all

docs:
	scripts/run-docs-build-all

docker:
	scripts/run-in-docker scripts/run-all

clean:
	cmake -E remove_directory tmp
