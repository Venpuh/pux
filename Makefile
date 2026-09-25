CC      ?= cc
CFLAGS  ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes
CPPFLAGS?= -Iinclude
LDFLAGS ?=
LDLIBS  ?=

BUILD_DIR := build
TARGET := $(BUILD_DIR)/pux

SRC := \
    src/main.c \
    src/cli.c \
    src/package.c \
    src/container.c \
    src/builder.c

OBJ := $(SRC:src/%.c=$(BUILD_DIR)/%.o)

.PHONY: all clean test run install

all: $(TARGET)

$(TARGET): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

test: $(TARGET)
	./tests/test_cli.sh ./$(TARGET)
	./tests/test_package.sh ./$(TARGET)
	./tests/test_build.sh ./$(TARGET)

run: $(TARGET)
	./$(TARGET) help

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/bin/pux

clean:
	rm -rf $(BUILD_DIR)
