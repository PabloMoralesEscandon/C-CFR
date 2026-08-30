CC ?= cc
AR ?= ar

CPPFLAGS += -Iinclude
C_STANDARD := -std=c17
WARNINGS := -Wall -Wextra -Wpedantic -Wvla -Werror
RELEASE_FLAGS := -O2
DEBUG_FLAGS := -O0 -g3
DEPENDENCY_FLAGS := -MMD -MP
SANITIZER_TEST_ENV ?= ASAN_OPTIONS=detect_leaks=0

BUILD_DIR := build
RELEASE_DIR := $(BUILD_DIR)/release
DEBUG_DIR := $(BUILD_DIR)/debug

LIB_SOURCES := \
	src/blackjack.c \
	src/evaluation.c \
	src/game.c \
	src/info_node.c \
	src/info_store.c \
	src/kuhn_poker.c \
	src/trainer.c \
	src/traversal.c

APP_SOURCE := app/cfr_cli.c
BLACKJACK_APP_SOURCE := app/blackjack_cli.c
CLI_TEST_SCRIPT := tests/test_cli.sh
BLACKJACK_CLI_TEST_SCRIPT := tests/test_blackjack_cli.sh

TEST_SOURCES := \
	tests/test_main.c \
	tests/test_public_headers.c \
	tests/test_blackjack.c \
	tests/test_game_contract.c \
	tests/test_info_node.c \
	tests/test_info_store.c \
	tests/test_traversal.c \
	tests/test_chance_trainer.c \
	tests/test_kuhn_poker.c \
	tests/test_evaluation.c \
	tests/support/test_allocator.c \
	tests/support/fake_game.c \
	tests/support/traversal_game.c \
	tests/support/chance_game.c

RELEASE_OBJECTS := $(patsubst %.c,$(RELEASE_DIR)/%.o,$(LIB_SOURCES))
DEBUG_OBJECTS := $(patsubst %.c,$(DEBUG_DIR)/%.o,$(LIB_SOURCES))
TEST_OBJECTS := $(patsubst %.c,$(DEBUG_DIR)/%.o,$(TEST_SOURCES))
RELEASE_APP_OBJECT := $(patsubst %.c,$(RELEASE_DIR)/%.o,$(APP_SOURCE))
DEBUG_APP_OBJECT := $(patsubst %.c,$(DEBUG_DIR)/%.o,$(APP_SOURCE))
RELEASE_BLACKJACK_APP_OBJECT := \
	$(patsubst %.c,$(RELEASE_DIR)/%.o,$(BLACKJACK_APP_SOURCE))
DEBUG_BLACKJACK_APP_OBJECT := \
	$(patsubst %.c,$(DEBUG_DIR)/%.o,$(BLACKJACK_APP_SOURCE))

RELEASE_LIBRARY := $(RELEASE_DIR)/libcfr.a
DEBUG_LIBRARY := $(DEBUG_DIR)/libcfr.a
TEST_BINARY := $(DEBUG_DIR)/cfr_tests
BLACKJACK_TEST_BINARY := $(DEBUG_DIR)/cfr_blackjack_tests
BLACKJACK_TEST_OBJECT := $(DEBUG_DIR)/tests/test_blackjack_standalone.o
RELEASE_BINARY := $(RELEASE_DIR)/cfr-kuhn
DEBUG_BINARY := $(DEBUG_DIR)/cfr-kuhn
RELEASE_BLACKJACK_BINARY := $(RELEASE_DIR)/cfr-blackjack
DEBUG_BLACKJACK_BINARY := $(DEBUG_DIR)/cfr-blackjack

DEPENDENCY_FILES := \
	$(RELEASE_OBJECTS:.o=.d) \
	$(DEBUG_OBJECTS:.o=.d) \
	$(TEST_OBJECTS:.o=.d) \
	$(BLACKJACK_TEST_OBJECT:.o=.d) \
	$(RELEASE_APP_OBJECT:.o=.d) \
	$(DEBUG_APP_OBJECT:.o=.d) \
	$(RELEASE_BLACKJACK_APP_OBJECT:.o=.d) \
	$(DEBUG_BLACKJACK_APP_OBJECT:.o=.d)

.PHONY: all blackjack test test-blackjack test-blackjack-cli test-alloc \
	test-alloc-run test-asan test-ubsan test-sanitize debug clean

all: $(RELEASE_LIBRARY) $(RELEASE_BINARY) $(RELEASE_BLACKJACK_BINARY)

blackjack: $(RELEASE_BLACKJACK_BINARY)

test: $(TEST_BINARY) $(DEBUG_BINARY) $(DEBUG_BLACKJACK_BINARY)
	$(TEST_ENV) ./$(TEST_BINARY)
	$(TEST_ENV) ./$(CLI_TEST_SCRIPT) ./$(DEBUG_BINARY)
	$(TEST_ENV) ./$(BLACKJACK_CLI_TEST_SCRIPT) ./$(DEBUG_BLACKJACK_BINARY)

test-blackjack-cli: $(DEBUG_BLACKJACK_BINARY)
	$(TEST_ENV) ./$(BLACKJACK_CLI_TEST_SCRIPT) ./$(DEBUG_BLACKJACK_BINARY)

test-blackjack: $(BLACKJACK_TEST_BINARY) $(DEBUG_BLACKJACK_BINARY)
	$(TEST_ENV) ./$(BLACKJACK_TEST_BINARY)
	$(TEST_ENV) ./$(BLACKJACK_CLI_TEST_SCRIPT) ./$(DEBUG_BLACKJACK_BINARY)

test-alloc:
	$(MAKE) BUILD_DIR=$(BUILD_DIR)/test-alloc \
		CFLAGS='$(CFLAGS) -DCFR_TEST_WRAP_ALLOCATOR' \
		LDFLAGS='$(LDFLAGS) -Wl,--wrap=malloc -Wl,--wrap=realloc -Wl,--wrap=free' \
		test-alloc-run

# El asignador envuelto pertenece a la suite C y no se enlaza con la CLI.
test-alloc-run: $(TEST_BINARY)
	$(TEST_ENV) ./$(TEST_BINARY)

test-asan:
	$(MAKE) BUILD_DIR=$(BUILD_DIR)/test-asan \
		CFLAGS='$(CFLAGS) -fsanitize=address -fno-omit-frame-pointer' \
		LDFLAGS='$(LDFLAGS) -fsanitize=address' \
		TEST_ENV='$(SANITIZER_TEST_ENV)' test

test-ubsan:
	$(MAKE) BUILD_DIR=$(BUILD_DIR)/test-ubsan \
		CFLAGS='$(CFLAGS) -fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' \
		LDFLAGS='$(LDFLAGS) -fsanitize=undefined' \
		test

test-sanitize:
	$(MAKE) BUILD_DIR=$(BUILD_DIR)/test-sanitize \
		CFLAGS='$(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' \
		LDFLAGS='$(LDFLAGS) -fsanitize=address,undefined' \
		TEST_ENV='$(SANITIZER_TEST_ENV)' test

debug: $(DEBUG_LIBRARY) $(TEST_BINARY) $(DEBUG_BINARY) \
	$(DEBUG_BLACKJACK_BINARY)

$(RELEASE_LIBRARY): $(RELEASE_OBJECTS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(DEBUG_LIBRARY): $(DEBUG_OBJECTS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(TEST_BINARY): $(TEST_OBJECTS) $(DEBUG_LIBRARY)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(TEST_OBJECTS) $(DEBUG_LIBRARY) $(LDLIBS) -o $@

$(BLACKJACK_TEST_BINARY): $(BLACKJACK_TEST_OBJECT) $(DEBUG_LIBRARY)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(BLACKJACK_TEST_OBJECT) $(DEBUG_LIBRARY) $(LDLIBS) -o $@

$(BLACKJACK_TEST_OBJECT): tests/test_blackjack.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(C_STANDARD) $(WARNINGS) \
		$(DEBUG_FLAGS) $(DEPENDENCY_FLAGS) \
		-DCFR_TEST_BLACKJACK_STANDALONE -c $< -o $@

$(RELEASE_BINARY): $(RELEASE_APP_OBJECT) $(RELEASE_LIBRARY)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(RELEASE_APP_OBJECT) $(RELEASE_LIBRARY) $(LDLIBS) -o $@

$(DEBUG_BINARY): $(DEBUG_APP_OBJECT) $(DEBUG_LIBRARY)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(DEBUG_APP_OBJECT) $(DEBUG_LIBRARY) $(LDLIBS) -o $@

$(RELEASE_BLACKJACK_BINARY): $(RELEASE_BLACKJACK_APP_OBJECT) $(RELEASE_LIBRARY)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(RELEASE_BLACKJACK_APP_OBJECT) $(RELEASE_LIBRARY) \
		$(LDLIBS) -o $@

$(DEBUG_BLACKJACK_BINARY): $(DEBUG_BLACKJACK_APP_OBJECT) $(DEBUG_LIBRARY)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(DEBUG_BLACKJACK_APP_OBJECT) $(DEBUG_LIBRARY) \
		$(LDLIBS) -o $@

$(RELEASE_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(C_STANDARD) $(WARNINGS) \
		$(RELEASE_FLAGS) $(DEPENDENCY_FLAGS) -c $< -o $@

$(DEBUG_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(C_STANDARD) $(WARNINGS) \
		$(DEBUG_FLAGS) $(DEPENDENCY_FLAGS) -c $< -o $@

clean:
	$(RM) -r -- $(BUILD_DIR)

-include $(DEPENDENCY_FILES)
