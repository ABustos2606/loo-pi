# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 Alejandro Bustos
PREFIX ?= /usr/local
LV2DIR ?= $(PREFIX)/lib/lv2
DESTDIR ?=
OPT ?= -O2
SAN ?= 0
PYTHON ?= python3
PKG_CONFIG ?= pkg-config
LV2LINT ?= lv2lint
BUILD_DIR ?= build
ifeq ($(SAN),1)
override BUILD_DIR := $(BUILD_DIR)/san
SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g
TEST_ENV := UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
endif

BUNDLE := $(BUILD_DIR)/loo-pi.lv2
OBJECTS := $(BUILD_DIR)/obj/lv2_entry.o $(BUILD_DIR)/obj/loop_set.o $(BUILD_DIR)/obj/stereo_loop.o $(BUILD_DIR)/obj/plugin.o
METADATA := manifest.ttl loo-pi.ttl
TTL_TARGETS := $(addprefix $(BUNDLE)/,$(METADATA))
LV2_CFLAGS = $(shell $(PKG_CONFIG) --cflags lv2)
COMPILE_FLAGS = $(CPPFLAGS) $(LV2_CFLAGS) $(CXXFLAGS) $(OPT) -std=c++17 -fPIC \
	-fvisibility=hidden -Wall -Wextra -fno-fast-math -fno-math-errno -fno-trapping-math $(SAN_FLAGS)

.PHONY: all install test test-core test-timing test-hosts lint metadata clean
all: $(BUNDLE)/loo-pi.so $(TTL_TARGETS)

$(BUILD_DIR)/obj/%.o: lv2/%.cpp
	@mkdir -p "$(@D)"
	$(CXX) $(COMPILE_FLAGS) -MMD -MP -c $< -o $@

# Upstream warnings stay local to the old core. No application dependencies.
$(BUILD_DIR)/obj/plugin.o: src/plugin.cc
	@mkdir -p "$(@D)"
	$(CXX) $(COMPILE_FLAGS) -Wno-unused-parameter -Wno-unused-variable \
		-Wno-unused-but-set-variable -Wno-sign-compare -Wno-implicit-fallthrough -MMD -MP -c $< -o $@

$(BUNDLE)/loo-pi.so: $(OBJECTS) lv2/exports.map
	@mkdir -p "$(@D)"
	$(CXX) $(OBJECTS) $(LDFLAGS) $(SAN_FLAGS) -shared -Wl,--no-undefined \
		-Wl,--version-script=lv2/exports.map -o $@

$(BUNDLE)/%.ttl: loo-pi.lv2/%.ttl
	@mkdir -p "$(@D)"
	install -m 644 $< $@

metadata:
	$(PYTHON) tools/gen_ttl.py

$(BUILD_DIR)/test_scaffold: tests/test_scaffold.cpp tests/host.hpp lv2/ports.hpp
	@mkdir -p "$(@D)"
	$(CXX) $(COMPILE_FLAGS) $< $(LDFLAGS) $(SAN_FLAGS) -ldl -pthread -o $@

$(BUILD_DIR)/test_plugin: tests/test_plugin.cpp tests/host.hpp lv2/ports.hpp
	@mkdir -p "$(@D)"
	$(CXX) $(COMPILE_FLAGS) $< $(LDFLAGS) $(SAN_FLAGS) -ldl -pthread -o $@

$(BUILD_DIR)/test_core: tests/test_core.cpp tests/rt_guard.cpp tests/rt_guard.hpp lv2/stereo_loop.hpp $(BUILD_DIR)/obj/stereo_loop.o $(BUILD_DIR)/obj/plugin.o
	@mkdir -p "$(@D)"
	$(CXX) $(COMPILE_FLAGS) tests/test_core.cpp tests/rt_guard.cpp $(BUILD_DIR)/obj/stereo_loop.o $(BUILD_DIR)/obj/plugin.o \
		$(LDFLAGS) $(SAN_FLAGS) -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free -o $@

$(BUILD_DIR)/test_multiloop: tests/test_multiloop.cpp tests/host.hpp tests/rt_guard.cpp tests/rt_guard.hpp $(OBJECTS)
	@mkdir -p "$(@D)"
	$(CXX) $(COMPILE_FLAGS) tests/test_multiloop.cpp tests/rt_guard.cpp $(OBJECTS) $(LDFLAGS) $(SAN_FLAGS) \
		-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free -o $@

test-core: $(BUILD_DIR)/test_core
	$(TEST_ENV) $(BUILD_DIR)/test_core

test-timing: $(BUILD_DIR)/test_core $(BUILD_DIR)/test_multiloop
	$(TEST_ENV) $(BUILD_DIR)/test_core --timing
	$(TEST_ENV) $(BUILD_DIR)/test_multiloop --timing

test: all $(BUILD_DIR)/test_scaffold $(BUILD_DIR)/test_core $(BUILD_DIR)/test_plugin $(BUILD_DIR)/test_multiloop
	$(PYTHON) tools/gen_ttl.py --check
	$(PYTHON) tests/ttl_check.py $(BUNDLE)
	$(TEST_ENV) $(BUILD_DIR)/test_scaffold $(BUNDLE)/loo-pi.so
	$(TEST_ENV) $(BUILD_DIR)/test_core
	$(TEST_ENV) $(BUILD_DIR)/test_plugin $(BUNDLE)/loo-pi.so
	$(TEST_ENV) $(BUILD_DIR)/test_multiloop

test-hosts: all
	$(PYTHON) tests/host_smoke.py $(BUNDLE) $(HOST_ARGS)

lint: all
	$(PYTHON) tests/ttl_check.py $(BUNDLE)
	@install -d "$(BUILD_DIR)/lint-lv2/loo-pi.lv2"
	@install -m 755 $(BUNDLE)/loo-pi.so "$(BUILD_DIR)/lint-lv2/loo-pi.lv2/"
	@install -m 644 $(TTL_TARGETS) "$(BUILD_DIR)/lint-lv2/loo-pi.lv2/"
	LV2_PATH="$(abspath $(BUILD_DIR))/lint-lv2:$${LV2_PATH:-/usr/local/lib/lv2:/usr/lib/lv2}" $(LV2LINT) 'https://github.com/ABustos2606/loo-pi#loopi'

install: all
	install -d "$(DESTDIR)$(LV2DIR)/loo-pi.lv2"
	install -m 755 $(BUNDLE)/loo-pi.so "$(DESTDIR)$(LV2DIR)/loo-pi.lv2/"
	install -m 644 $(TTL_TARGETS) COPYING "$(DESTDIR)$(LV2DIR)/loo-pi.lv2/"

clean:
	$(RM) $(OBJECTS) $(OBJECTS:.o=.d) $(BUNDLE)/loo-pi.so $(TTL_TARGETS) $(BUILD_DIR)/test_scaffold $(BUILD_DIR)/test_core $(BUILD_DIR)/test_plugin $(BUILD_DIR)/test_multiloop

-include $(OBJECTS:.o=.d)
