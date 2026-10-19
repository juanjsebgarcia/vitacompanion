# Build vitacompanion in the VitaSDK Docker image, so nothing has to be
# installed on the host. `make native` builds with a local VitaSDK instead.
#
#   make            build build/vitacompanion.suprx and build/vitacompanion_kernel.skprx
#   make native     build with the VitaSDK at $VITASDK, without Docker
#   make test       run the host-side tests in tests/
#   make shell      open a shell in the VitaSDK image
#   make clean      remove the build directory
#
# Extra CMake options go in CMAKE_ARGS. For example, a side-by-side test build
# that leaves the canonical 1337/1338 service alone:
#
#   make BUILD_DIR=build-test CMAKE_ARGS="-DVITACOMPANION_FTP_PORT=1340 \
#     -DVITACOMPANION_CMD_PORT=1341 -DVITACOMPANION_MODULE_NAME=vitacompanion_test"

# Pinned so builds are reproducible. This is the VitaSDK 2026.08 image, a
# multi-arch index (linux/arm64 and linux/amd64). Update it deliberately.
VITASDK_IMAGE ?= vitasdk/vitasdk@sha256:8227da1b596d981f4513cd9b36bda1e43857f6dae5bcf4312c1cd40167f7d9bb
BUILD_DIR ?= build
CMAKE_ARGS ?=
DOCKER ?= docker
PYTHON ?= python3

JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Run as the host user so build output isn't owned by root.
DOCKER_RUN = $(DOCKER) run --rm --user $$(id -u):$$(id -g) -e HOME=/tmp \
	-v "$(CURDIR)":/src -w /src

.PHONY: all build native test shell clean

all: build

build:
	$(DOCKER_RUN) $(VITASDK_IMAGE) \
		make native BUILD_DIR='$(BUILD_DIR)' CMAKE_ARGS='$(CMAKE_ARGS)' JOBS='$(JOBS)'

native:
	cmake -S . -B '$(BUILD_DIR)' $(CMAKE_ARGS)
	$(MAKE) -C '$(BUILD_DIR)' -j$(JOBS)

test:
	$(PYTHON) -m unittest discover -s tests -v

shell:
	$(DOCKER_RUN) -it $(VITASDK_IMAGE) bash

clean:
	rm -rf '$(BUILD_DIR)'
