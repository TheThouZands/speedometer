CXX := g++
CC := gcc

DRM_CFLAGS := $(shell pkg-config --cflags libdrm)
DRM_LIBS := $(shell pkg-config --libs libdrm)
LVGL_DIR ?= ../src/lvgl
LVGL_DIR := $(abspath $(LVGL_DIR))
LVGL_PARENT := $(abspath $(LVGL_DIR)/..)

COMMONFLAGS := -O2 -Wall -Wextra -I$(LVGL_PARENT) -I$(CURDIR) $(DRM_CFLAGS)
CXXFLAGS := -std=c++17 $(COMMONFLAGS)
CFLAGS := -std=gnu11 $(COMMONFLAGS)
LDLIBS := -lpthread -lm $(DRM_LIBS)

TARGET := hello_rpi
SRC := main.cpp
LVGL_DIR := /root/src/lvgl
BUILD_DIR := build

LVGL_SRC := $(shell find $(LVGL_DIR)/src -name '*.c')
MAIN_OBJ := $(BUILD_DIR)/main.o
LVGL_OBJ := $(patsubst $(LVGL_DIR)/src/%.c,$(BUILD_DIR)/lvgl/%.o,$(LVGL_SRC))
OBJ := $(MAIN_OBJ) $(LVGL_OBJ)

.PHONY: all clean run compdb

all: check-lvgl $(TARGET)

check-lvgl:
	test -f "$(LVGL_DIR)/lvgl.h"
	test -d "$(LVGL_DIR)/src"

$(TARGET): $(OBJ)
	$(CXX) -o $@ $(OBJ) $(LDLIBS)

$(MAIN_OBJ): $(SRC)
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/lvgl/%.o: $(LVGL_DIR)/src/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

compile_commands.json: Makefile check-lvgl
	printf '[\n' > $@
	first=1; \
	main_cmd='$(CXX) $(CXXFLAGS) -c $(SRC) -o $(MAIN_OBJ)'; \
	printf '  {"directory":"%s","file":"%s","command":"%s"}' "$(CURDIR)" "$(CURDIR)/$(SRC)" "$$main_cmd" >> $@; \
	for src in $(LVGL_SRC); do \
		rel_path=$${src#$(LVGL_DIR)/src/}; \
		obj_path="$(BUILD_DIR)/lvgl/$${rel_path%.c}.o"; \
		cmd='$(CC) $(CFLAGS) -c '"$$src"' -o '"$$obj_path"; \
		printf ',\n  {"directory":"%s","file":"%s","command":"%s"}' "$(CURDIR)" "$$src" "$$cmd" >> $@; \
	done; \
	printf '\n]\n' >> $@

compdb: compile_commands.json

clean:
	rm -rf $(BUILD_DIR) $(TARGET) compile_commands.json
