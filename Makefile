CC ?= cc
AR ?= ar

CPPFLAGS += -Iinclude
C_STANDARD := -std=c17
WARNINGS := -Wall -Wextra -Wpedantic -Werror
RELEASE_FLAGS := -O2
DEBUG_FLAGS := -O0 -g3
DEPENDENCY_FLAGS := -MMD -MP

BUILD_DIR := build
RELEASE_DIR := $(BUILD_DIR)/release
DEBUG_DIR := $(BUILD_DIR)/debug

LIB_SOURCES := \
	src/game.c \
	src/info_node.c

TEST_SOURCES := \
	tests/test_main.c \
	tests/test_public_headers.c \
	tests/test_game_contract.c \
	tests/test_info_node.c \
	tests/support/fake_game.c

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

.PHONY: all test debug clean

all: $(RELEASE_LIBRARY)

test: $(TEST_BINARY)
	./$(TEST_BINARY)

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
