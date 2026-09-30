# GoalGraph — root Makefile
#
# Targets:
#   make / make debug   Build the debug configuration   (build/debug/)
#   make release        Build the release configuration (build/release/)
#   make test           Build and run every tests/test_*.c (debug by default;
#                       use `make test BUILD=release` for release)
#   make memcheck       Run every test under Valgrind (must report zero leaks)
#   make clean          Remove all build output
#
# Options:
#   BUILD=debug|release   Configuration used by `test`/`memcheck` (default: debug)
#   SANITIZE=1            Add -fsanitize=address,undefined (not with memcheck)
#   EXTRA_CFLAGS=...      Extra compiler flags

CC := gcc

# ---------------------------------------------------------------- flags ----

WARN_FLAGS    := -std=c17 -Wall -Wextra -Werror
DEBUG_FLAGS   := -g -O0 -DDEBUG
RELEASE_FLAGS := -O2 -DNDEBUG

BUILD ?= debug
ifeq ($(BUILD),debug)
  MODE_FLAGS := $(DEBUG_FLAGS)
else ifeq ($(BUILD),release)
  MODE_FLAGS := $(RELEASE_FLAGS)
else
  $(error BUILD must be 'debug' or 'release' (got '$(BUILD)'))
endif

ifeq ($(SANITIZE),1)
  SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
endif

CPPFLAGS := -Iinclude -Isrc -MMD -MP
CFLAGS   := $(WARN_FLAGS) $(MODE_FLAGS) $(SAN_FLAGS) $(EXTRA_CFLAGS)
LDFLAGS  := $(SAN_FLAGS)
LDLIBS   :=

# gcc on Windows appends .exe to outputs; match it so make sees targets exist.
ifeq ($(OS),Windows_NT)
  EXE := .exe
endif

# ---------------------------------------------------------------- files ----

# Recursive wildcard: $(call rwildcard,dir,pattern)
rwildcard = $(foreach d,$(wildcard $(1:=/*)),$(call rwildcard,$d,$2) $(filter $(subst *,%,$2),$d))

MAIN_SRC  := src/cli/main.c
SRCS      := $(call rwildcard,src,*.c)
LIB_SRCS  := $(filter-out $(MAIN_SRC),$(SRCS))
TEST_SRCS := $(wildcard tests/test_*.c)

BUILD_DIR := build/$(BUILD)
OBJ_DIR   := $(BUILD_DIR)/obj

LIB_OBJS  := $(LIB_SRCS:%.c=$(OBJ_DIR)/%.o)
MAIN_OBJ  := $(MAIN_SRC:%.c=$(OBJ_DIR)/%.o)
TEST_OBJS := $(TEST_SRCS:%.c=$(OBJ_DIR)/%.o)
DEPS      := $(LIB_OBJS:.o=.d) $(MAIN_OBJ:.o=.d) $(TEST_OBJS:.o=.d)

BIN       := $(BUILD_DIR)/goalgraph$(EXE)
TEST_BINS := $(TEST_SRCS:tests/%.c=$(BUILD_DIR)/tests/%$(EXE))

# The executable is only built once src/cli/main.c exists.
BUILD_GOALS := $(LIB_OBJS) $(if $(wildcard $(MAIN_SRC)),$(BIN))

# -------------------------------------------------------------- targets ----

.PHONY: all debug release build test memcheck clean
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

all: debug

debug:
	@$(MAKE) --no-print-directory BUILD=debug build

release:
	@$(MAKE) --no-print-directory BUILD=release build

build: $(BUILD_GOALS)
	@echo "[$(BUILD)] build complete"

$(BIN): $(MAIN_OBJ) $(LIB_OBJS)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

# Each tests/test_<name>.c is its own executable, linked against all library
# objects (everything in src/ except main.c).
$(BUILD_DIR)/tests/%$(EXE): $(OBJ_DIR)/tests/%.o $(LIB_OBJS)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Tests run from the project root so they can open data/... relative paths.
test: $(TEST_BINS)
ifeq ($(strip $(TEST_BINS)),)
	@echo "No tests found (expected tests/test_*.c)"
else
	@pass=0; fail=0; \
	for t in $(TEST_BINS); do \
	  if ./$$t; then echo "PASS  $$t"; pass=$$((pass+1)); \
	  else echo "FAIL  $$t"; fail=$$((fail+1)); fi; \
	done; \
	echo "[$(BUILD)] $$pass passed, $$fail failed"; \
	[ $$fail -eq 0 ]
endif

VALGRIND := valgrind --leak-check=full --show-leak-kinds=all \
            --errors-for-leak-kinds=all --error-exitcode=1 --quiet

memcheck: $(TEST_BINS)
ifeq ($(SANITIZE),1)
	$(error memcheck cannot be combined with SANITIZE=1)
endif
ifeq ($(strip $(TEST_BINS)),)
	@echo "No tests found (expected tests/test_*.c)"
else
	@fail=0; \
	for t in $(TEST_BINS); do \
	  if $(VALGRIND) ./$$t; then echo "CLEAN $$t"; \
	  else echo "LEAK/ERROR $$t"; fail=1; fi; \
	done; \
	[ $$fail -eq 0 ]
endif

clean:
	rm -rf build

-include $(DEPS)
