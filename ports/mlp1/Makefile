SHELL := /bin/bash

SSH_SERVER_DIR ?= /workspace/ssh-server
BUILD ?= build/mlp1
CATASTROPHE_DIR ?= /workspace/Catastrophe
NATIVE_MAKE_FLAGS ?=
MLP1_BUILD_PROFILE ?= release

.PHONY: all clean

all:
	$(MAKE) $(NATIVE_MAKE_FLAGS) -C "$(SSH_SERVER_DIR)" \
		BUILD="$(BUILD)" \
		CSTD="-std=gnu11" \
		CFLAGS_PLATFORM="-DPLATFORM_MLP1" \
		PLATFORM=mlp1 \
		MLP1_BUILD_PROFILE="$(MLP1_BUILD_PROFILE)" \
		CATASTROPHE_DIR="$(CATASTROPHE_DIR)" \
		native

clean:
	$(MAKE) -C "$(SSH_SERVER_DIR)" BUILD="$(BUILD)" clean
