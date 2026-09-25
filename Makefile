CC      ?= cc
CFLAGS  ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes
CPPFLAGS?= -Iinclude
LDFLAGS ?=
LDLIBS  ?= -lcrypto

BUILD_DIR := build
TARGET := $(BUILD_DIR)/pux

SRC := \
    src/main.c \
    src/cli.c \
    src/package.c \
    src/container.c \
    src/builder.c \
    src/extract.c \
    src/db.c \
    src/resolver.c \
    src/transaction.c \
    src/repo.c \
    src/sha256.c \
    src/signature.c \
    src/trust.c \
    src/transport.c \
    src/update.c \
    src/config.c
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
	./tests/test_sha256.sh ./$(TARGET)
	./tests/test_signature.sh ./$(TARGET)
	./tests/test_trust.sh ./$(TARGET)
	./tests/test_package.sh ./$(TARGET)
	./tests/test_build.sh ./$(TARGET)
	./tests/test_extract.sh ./$(TARGET)
	./tests/test_symlink.sh ./$(TARGET)
	./tests/test_db.sh ./$(TARGET)
	./tests/test_resolver.sh ./$(TARGET)
	./tests/test_install.sh ./$(TARGET)
	./tests/test_repo_install.sh ./$(TARGET)
	./tests/test_remote_install.sh ./$(TARGET)
	./tests/test_remove.sh ./$(TARGET)
	./tests/test_upgrade.sh ./$(TARGET)
	./tests/test_repo.sh ./$(TARGET)
	./tests/test_update.sh ./$(TARGET)
	./tests/test_config.sh ./$(TARGET)
	./tests/test_configured_install.sh ./$(TARGET)

run: $(TARGET)
	./$(TARGET) help

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/bin/pux

clean:
	rm -rf $(BUILD_DIR)
