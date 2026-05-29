SHELL := /bin/bash

CC ?= cc
CSTD := -std=c11
CWARN := -Wall -Wextra -Wpedantic -Wno-unused-parameter
CDEBUG ?= -g -O0
BUILD ?= build
WORKSPACE_ROOT ?= $(abspath ..)
MLP1_TOOLCHAIN_IMAGE ?= ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:local
DROPBEAR_VERSION ?= 2025.88

DEFAULT_CATASTROPHE_DIR := $(if $(wildcard ../Catastrophe/include/catastrophe.h),$(abspath ../Catastrophe),third_party/catastrophe)
CATASTROPHE_DIR ?= $(DEFAULT_CATASTROPHE_DIR)
CATASTROPHE_INCLUDE := $(CATASTROPHE_DIR)/include
CATASTROPHE_HEADER := $(CATASTROPHE_INCLUDE)/catastrophe.h
CATASTROPHE_RES := $(CATASTROPHE_DIR)/res
DEFAULT_CJSON_DIR := $(if $(wildcard ../Jawaka/third_party/cjson/cJSON.c),$(abspath ../Jawaka/third_party/cjson),third_party/cjson)
CJSON_DIR ?= $(DEFAULT_CJSON_DIR)
CJSON_SRC := $(CJSON_DIR)/cJSON.c

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_ttf SDL2_image)
SDL_LDFLAGS := $(shell pkg-config --libs sdl2 SDL2_ttf SDL2_image)

CFLAGS_COMMON := $(CSTD) $(CWARN) $(CDEBUG) $(CFLAGS_PLATFORM) -I. -Iinternal -I$(CATASTROPHE_INCLUDE) -I$(CJSON_DIR) $(SDL_CFLAGS)
LDLIBS_COMMON := $(SDL_LDFLAGS) -lm -lpthread
ifeq ($(shell uname -s),Darwin)
LDLIBS_COMMON += -lobjc
else
LDLIBS_COMMON += -lcrypt
endif

APP_SRCS := \
	cmd/ssh-server/main.c \
	internal/config.c \
	internal/account.c \
	internal/runtime.c \
	$(CJSON_SRC)

APP_BIN := $(BUILD)/bin/ssh-server
PACKAGE_ROOT := $(BUILD)/package
PACKAGE_DIR := $(PACKAGE_ROOT)/SSHServer.pak

.PHONY: all native run-native package package-mlp1 mlp adb-stage-pak adb-stage-pak-mlp1 clean check-catastrophe check-sdl

all: native

native: $(APP_BIN)

$(BUILD)/bin:
	@mkdir -p "$(BUILD)/bin"

check-catastrophe:
	@test -f "$(CATASTROPHE_HEADER)" || \
		( echo "Catastrophe headers not found. Set CATASTROPHE_DIR=/path/to/Catastrophe and retry." && exit 1 )

check-sdl:
	@pkg-config --exists sdl2 SDL2_ttf SDL2_image 2>/dev/null || \
		( echo "SDL2 libraries not found. Install with: brew install sdl2 sdl2_ttf sdl2_image" && exit 1 )

$(APP_BIN): $(APP_SRCS) $(CATASTROPHE_HEADER) | $(BUILD)/bin check-catastrophe check-sdl
	$(CC) $(CFLAGS_COMMON) -o "$@" $(APP_SRCS) $(LDLIBS_COMMON)

run-native: $(APP_BIN)
	CAT_WINDOW_WIDTH=640 CAT_WINDOW_HEIGHT=480 \
	CAT_FONTS_DIR="$(CATASTROPHE_RES)" \
	CAT_THEMES_DIR="$(CATASTROPHE_RES)/themes" \
	UMRK_SSH_APP_ROOT="$(CURDIR)" \
	UMRK_SSH_DESKTOP_FONT="$(CATASTROPHE_RES)/font.ttf" \
	"$(APP_BIN)"

package: $(APP_BIN)
	@rm -rf "$(PACKAGE_ROOT)"
	@mkdir -p "$(PACKAGE_DIR)/bin" "$(PACKAGE_DIR)/res" "$(PACKAGE_DIR)/runtime/bin"
	@cp -f "$(APP_BIN)" "$(PACKAGE_DIR)/bin/ssh-server"
	@cp -f "pak/launch.sh" "$(PACKAGE_DIR)/launch.sh"
	@cp -f "pak/pak.json" "$(PACKAGE_DIR)/pak.json"
	@if [ -f "$(CATASTROPHE_RES)/font.ttf" ]; then cp -f "$(CATASTROPHE_RES)/font.ttf" "$(PACKAGE_DIR)/res/font.ttf"; fi
	@chmod 755 "$(PACKAGE_DIR)/launch.sh" "$(PACKAGE_DIR)/bin/ssh-server"
	@if [ -f "$(BUILD)/runtime/bin/dropbear" ]; then cp -f "$(BUILD)/runtime/bin/dropbear" "$(PACKAGE_DIR)/runtime/bin/dropbear"; chmod 755 "$(PACKAGE_DIR)/runtime/bin/dropbear"; fi
	@if [ -f "$(BUILD)/runtime/bin/dropbearkey" ]; then cp -f "$(BUILD)/runtime/bin/dropbearkey" "$(PACKAGE_DIR)/runtime/bin/dropbearkey"; chmod 755 "$(PACKAGE_DIR)/runtime/bin/dropbearkey"; fi
	@find "$(PACKAGE_DIR)" -maxdepth 3 -type f -print | sort

package-mlp1: mlp dropbear-mlp1
	$(MAKE) BUILD=build/mlp1 package

mlp:
	docker run --rm \
		-v "$(WORKSPACE_ROOT)":/workspace \
		-w /workspace/ssh-server \
		"$(MLP1_TOOLCHAIN_IMAGE)" \
		make -f ports/mlp1/Makefile all

dropbear-mlp1:
	BUILD_DIR=build/mlp1 DROPBEAR_VERSION="$(DROPBEAR_VERSION)" MLP1_TOOLCHAIN_IMAGE="$(MLP1_TOOLCHAIN_IMAGE)" scripts/build-dropbear-mlp1.sh

adb-stage-pak: package
	scripts/adb-stage-pak.sh

adb-stage-pak-mlp1: package-mlp1
	BUILD_DIR=build/mlp1 scripts/adb-stage-pak.sh

clean:
	rm -rf "$(BUILD)"
