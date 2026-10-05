SHELL := /bin/bash
LAB_DIR := $(abspath .)
KEYSTONE_DIR := $(LAB_DIR)/build/keystone
KEYSTONE_REV := 73aeab79b97f4cea3e418897841e5534fc978f4b
JOBS ?= 2
CXX ?= c++
BUILD_PATH := $(LAB_DIR)/build/host-deps/usr/bin:/usr/local/bin:/usr/bin:/bin
BUILD_LIB_PATH := $(LAB_DIR)/build/host-deps/usr/lib/x86_64-linux-gnu
BUILD_PERL_PATH := $(LAB_DIR)/build/host-deps/usr/share/texinfo:$(LAB_DIR)/build/host-deps/usr/share/texinfo/lib/libintl-perl/lib:$(LAB_DIR)/build/host-deps/usr/share/texinfo/lib/Unicode-EastAsianWidth/lib:$(LAB_DIR)/build/host-deps/usr/share/perl5

.PHONY: toolcheck bootstrap build smoke unit test inspect clean

toolcheck:
	@for tool in git make cmake $(CXX) python3 ssh timeout rg setsid; do command -v $$tool >/dev/null || { echo "missing: $$tool" >&2; exit 1; }; done
	@printf 'Keystone revision: %s\n' '$(KEYSTONE_REV)'
	@for tool in gperf jq makeinfo makeself; do \
	  PATH='$(BUILD_PATH)' command -v $$tool >/dev/null || { echo "Buildroot dependency missing: $$tool" >&2; exit 1; }; \
	done
	@PATH='$(BUILD_PATH)' LD_LIBRARY_PATH='$(BUILD_LIB_PATH)' jq --version >/dev/null
	@PATH='$(BUILD_PATH)' PERL5LIB='$(BUILD_PERL_PATH)' makeinfo --version >/dev/null

bootstrap: toolcheck
	LAB_DIR='$(LAB_DIR)' KEYSTONE_REV='$(KEYSTONE_REV)' ./scripts/bootstrap.sh

build: bootstrap
	LAB_DIR='$(LAB_DIR)' ./scripts/prepare-overlay.sh
	PATH='$(BUILD_PATH)' LD_LIBRARY_PATH='$(BUILD_LIB_PATH)' \
	  PERL5LIB='$(BUILD_PERL_PATH)' \
	  $(MAKE) -C '$(KEYSTONE_DIR)' -j$(JOBS) KEYSTONE_PLATFORM=generic KEYSTONE_BITS=64

smoke: build
	LAB_DIR='$(LAB_DIR)' ./scripts/guest-test.sh smoke

unit:
	mkdir -p build/unit
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O2 -Iexercises tests/policy_test.cpp -o build/unit/policy_test
	./build/unit/policy_test

test: unit build
	LAB_DIR='$(LAB_DIR)' ./scripts/guest-test.sh test

inspect: bootstrap
	LAB_DIR='$(LAB_DIR)' ./scripts/inspect.sh

clean:
	@echo 'build/ contains the pinned Keystone checkout and Buildroot output.'
	@echo 'Remove it manually when those downloaded artifacts are no longer needed.'
