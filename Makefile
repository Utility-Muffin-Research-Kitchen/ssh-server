SHELL := /bin/bash

CC ?= cc
CSTD := -std=c11
CWARN := -Wall -Wextra -Wpedantic -Wno-unused-parameter
CDEBUG ?= -g -O0
BUILD ?= build
PLATFORM ?= mac
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
CJSON_HEADER := $(wildcard $(CJSON_DIR)/cJSON.h)

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
APP_HDRS := $(wildcard internal/*.h)
CAT_HEADERS := $(wildcard $(CATASTROPHE_INCLUDE)/*.h)
APP_DEPS := $(APP_SRCS) $(APP_HDRS) $(CAT_HEADERS) $(CJSON_HEADER)

APP_BIN := $(BUILD)/bin/ssh-server
PACKAGE_ROOT := $(BUILD)/package
PACKAGE_DIR := $(PACKAGE_ROOT)/SSHServer.pak
MLP1_BUILD ?= build/mlp1
MLP1_APP_BIN := $(MLP1_BUILD)/bin/ssh-server

.PHONY: all native run-native package package-build package-platform package-mlp1 mlp adb-stage-pak adb-stage-pak-mlp1 clean check-catastrophe check-sdl

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

$(APP_BIN): $(APP_DEPS) | $(BUILD)/bin check-catastrophe check-sdl
	$(CC) $(CFLAGS_COMMON) -o "$@" $(APP_SRCS) $(LDLIBS_COMMON)

run-native: $(APP_BIN)
	CAT_WINDOW_WIDTH=640 CAT_WINDOW_HEIGHT=480 \
	CAT_FONTS_DIR="$(CATASTROPHE_RES)" \
	CAT_THEMES_DIR="$(CATASTROPHE_RES)/themes" \
	UMRK_SSH_APP_ROOT="$(CURDIR)" \
	UMRK_SSH_DESKTOP_FONT="$(CATASTROPHE_RES)/font.ttf" \
	"$(APP_BIN)"

package: $(APP_BIN)
	@$(MAKE) BUILD="$(BUILD)" PLATFORM="$(PLATFORM)" package-build

package-build:
	@rm -rf "$(PACKAGE_ROOT)"
	@mkdir -p "$(PACKAGE_DIR)/bin" "$(PACKAGE_DIR)/res" "$(PACKAGE_DIR)/runtime/bin"
	@cp -f "$(APP_BIN)" "$(PACKAGE_DIR)/bin/ssh-server"
	@cp -f "pak/launch.sh" "$(PACKAGE_DIR)/launch.sh"
	@printf '{ "name": "SSH Server", "icon": "res/icon.png", "platform": "$(PLATFORM)", "pak_version": "0.1.0", "min_jawaka_version": "0.0.1" }\n' > "$(PACKAGE_DIR)/pak.json"
	@if [ -f "$(CATASTROPHE_RES)/font.ttf" ]; then cp -f "$(CATASTROPHE_RES)/font.ttf" "$(PACKAGE_DIR)/res/font.ttf"; fi
	@if [ -d "pak/res" ]; then cp -Rf pak/res/. "$(PACKAGE_DIR)/res/"; fi
	@chmod 755 "$(PACKAGE_DIR)/launch.sh" "$(PACKAGE_DIR)/bin/ssh-server"
	@if [ -f "$(BUILD)/runtime/bin/dropbear" ]; then cp -f "$(BUILD)/runtime/bin/dropbear" "$(PACKAGE_DIR)/runtime/bin/dropbear"; chmod 755 "$(PACKAGE_DIR)/runtime/bin/dropbear"; fi
	@if [ -f "$(BUILD)/runtime/bin/dropbearkey" ]; then cp -f "$(BUILD)/runtime/bin/dropbearkey" "$(PACKAGE_DIR)/runtime/bin/dropbearkey"; chmod 755 "$(PACKAGE_DIR)/runtime/bin/dropbearkey"; fi
	@find "$(PACKAGE_DIR)" -maxdepth 3 -type f -print | sort

package-mlp1: mlp dropbear-mlp1
	@$(MAKE) BUILD="$(MLP1_BUILD)" PLATFORM=mlp1 package-build

package-platform:
	@test -n "$(PLATFORM)" || { echo "usage: make package-platform PLATFORM=<platform>" >&2; exit 1; }
	@case "$(PLATFORM)" in \
		mlp1) $(MAKE) package-mlp1 ;; \
		mac) $(MAKE) PLATFORM=mac package ;; \
		*) echo "unsupported ssh-server package platform: $(PLATFORM)" >&2; exit 1 ;; \
	esac

mlp: $(MLP1_APP_BIN)

ifneq ($(APP_BIN),$(MLP1_APP_BIN))
$(MLP1_APP_BIN): $(APP_DEPS) Makefile ports/mlp1/Makefile
	@docker run --rm \
		-v "$(WORKSPACE_ROOT)":/workspace \
		-w /workspace/ssh-server \
		"$(MLP1_TOOLCHAIN_IMAGE)" \
		make -f ports/mlp1/Makefile NATIVE_MAKE_FLAGS=-B all
endif

dropbear-mlp1:
	@BUILD_DIR="$(MLP1_BUILD)" \
		DROPBEAR_VERSION="$(DROPBEAR_VERSION)" \
		DROPBEAR_FORCE_REBUILD="$(DROPBEAR_FORCE_REBUILD)" \
		DROPBEAR_VERBOSE="$(DROPBEAR_VERBOSE)" \
		MLP1_TOOLCHAIN_IMAGE="$(MLP1_TOOLCHAIN_IMAGE)" \
		scripts/build-dropbear-mlp1.sh

adb-stage-pak: package
	@scripts/adb-stage-pak.sh

adb-stage-pak-mlp1: package-mlp1
	@BUILD_DIR="$(MLP1_BUILD)" scripts/adb-stage-pak.sh

clean:
	rm -rf "$(BUILD)"
