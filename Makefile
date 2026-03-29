CXX := g++
CC := gcc

COMMONFLAGS := -O2 -Wall -Wextra -I/root/src/ -I/root/speedometer
CXXFLAGS := -std=c++17 $(COMMONFLAGS)
CFLAGS := -std=gnu11 $(COMMONFLAGS)
LDLIBS := -lpthread -lm

TARGET := hello_rpi
SRC := main.cpp
LVGL_DIR := /root/src/lvgl
BUILD_DIR := build

LVGL_SRC := $(shell find $(LVGL_DIR)/src -name '*.c')
MAIN_OBJ := $(BUILD_DIR)/main.o
LVGL_OBJ := $(patsubst $(LVGL_DIR)/src/%.c,$(BUILD_DIR)/lvgl/%.o,$(LVGL_SRC))
OBJ := $(MAIN_OBJ) $(LVGL_OBJ)

.PHONY: all clean run

all: $(TARGET)

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

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
