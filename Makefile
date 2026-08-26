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
	src/game.c \
	src/info_node.c \
	src/info_store.c \
	src/traversal.c \
	src/trainer.c \
	src/kuhn_poker.c

TEST_SOURCES := \
	tests/test_main.c \
	tests/test_public_headers.c \
	tests/test_game_contract.c \
	tests/test_info_node.c \
	tests/test_info_store.c \
	tests/test_traversal.c \
	tests/test_chance_trainer.c \
	tests/test_kuhn_poker.c \
	tests/support/test_allocator.c \
	tests/support/fake_game.c \
	tests/support/traversal_game.c \
	tests/support/chance_game.c

RELEASE_OBJECTS := $(patsubst %.c,$(RELEASE_DIR)/%.o,$(LIB_SOURCES))
DEBUG_OBJECTS := $(patsubst %.c,$(DEBUG_DIR)/%.o,$(LIB_SOURCES))
TEST_OBJECTS := $(patsubst %.c,$(DEBUG_DIR)/%.o,$(TEST_SOURCES))

RELEASE_LIBRARY := $(RELEASE_DIR)/libcfr.a
DEBUG_LIBRARY := $(DEBUG_DIR)/libcfr.a
TEST_BINARY := $(DEBUG_DIR)/cfr_tests

DEPENDENCY_FILES := \
	$(RELEASE_OBJECTS:.o=.d) \
	$(DEBUG_OBJECTS:.o=.d) \
	$(TEST_OBJECTS:.o=.d)

.PHONY: all test test-alloc test-sanitize debug clean

all: $(RELEASE_LIBRARY)

test: $(TEST_BINARY)
	$(TEST_ENV) ./$(TEST_BINARY)

test-alloc:
	$(MAKE) BUILD_DIR=$(BUILD_DIR)/test-alloc \
		CFLAGS='$(CFLAGS) -DCFR_TEST_WRAP_ALLOCATOR' \
		LDFLAGS='$(LDFLAGS) -Wl,--wrap=malloc -Wl,--wrap=realloc -Wl,--wrap=free' test

test-sanitize:
	$(MAKE) BUILD_DIR=$(BUILD_DIR)/test-sanitize \
		CFLAGS='$(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer' \
		LDFLAGS='$(LDFLAGS) -fsanitize=address,undefined' \
		TEST_ENV='$(SANITIZER_TEST_ENV)' test

debug: $(DEBUG_LIBRARY) $(TEST_BINARY)

$(RELEASE_LIBRARY): $(RELEASE_OBJECTS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(DEBUG_LIBRARY): $(DEBUG_OBJECTS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(TEST_BINARY): $(TEST_OBJECTS) $(DEBUG_LIBRARY)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(TEST_OBJECTS) $(DEBUG_LIBRARY) $(LDLIBS) -o $@

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
