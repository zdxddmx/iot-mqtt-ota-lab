# iot-mqtt-ota-lab —— 主机端编译与自检
CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -O2 -Iinclude
BUILD   := build
EXE     := .exe

ifeq ($(OS),Windows_NT)
RM    := cmd /c rmdir /s /q
MKDIR := cmd /c if not exist "$(BUILD)" mkdir "$(BUILD)"
else
RM    := rm -rf
MKDIR := mkdir -p $(BUILD)
endif

LIB_SRC := $(wildcard src/*.c)
LIB_OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(LIB_SRC))
TESTS   := $(BUILD)/iot_tests$(EXE)

.PHONY: all test clean
all: $(TESTS)

$(BUILD):
	@$(MKDIR)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTS): $(LIB_OBJ) $(BUILD)/test_iot.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_iot.o: tests/test_iot.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TESTS)
	@./$(TESTS)

clean:
	-@$(RM) $(BUILD)
