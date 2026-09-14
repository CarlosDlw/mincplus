# Convenience wrapper around the CMake presets (`CMakePresets.json`) and the
# gates CI runs (`.github/workflows/ci.yml`).
#
# It is a shortcut, never a second source of truth: every target here calls the
# same `cmake --preset` / `ctest --preset` a developer would type, so a target
# and a bare command cannot drift apart. The presets own the flags; this file
# only owns the names.
#
# Portable by construction: `cmake`, `ctest`, and the shell built-ins are all it
# uses, and the two places that need `find` (the format gate) say so. It works in
# a POSIX shell, macOS, and Git Bash / MSYS on Windows.
#
#   make                # this help
#   make quick          # the inner loop: build, test, format check
#   make test           # configure + build + test the dev preset
#   make gates          # everything CI runs, in the order CI runs it
#   make examples       # every command, over every example

CMAKE ?= cmake
CTEST ?= ctest

# The preset that plain `make build` / `make test` use. `make BUILD=ci test`
# switches it without a second set of targets.
BUILD ?= dev

# Parallelism for the one tool that does not read the CMake presets: Ninja sizes
# itself, clang-tidy has to be told. `nproc` is in coreutils and Git Bash; macOS
# has only `sysctl`, and 4 is the last resort so the target still works.
JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Sources the format and tidy gates look at. Kept as one list so the two cannot
# disagree about what "the project's C++" means.
SOURCES := include src tests

.DEFAULT_GOAL := help
.PHONY: help configure build test quick gates \
        dev ci sanitize release \
        format format-check tidy examples \
        ccache ccache-tune clean distclean

help:
	@echo "minc+ - build, test, and check"
	@echo ""
	@echo "  make configure [BUILD=<preset>]  Configure (default preset: $(BUILD))"
	@echo "  make build     [BUILD=<preset>]  Build"
	@echo "  make test      [BUILD=<preset>]  Build and run the test suite"
	@echo "  make quick                       Build, test, format check: the loop"
	@echo "  make dev|ci|sanitize|release     Shorthand for the four presets"
	@echo "  make gates                       Everything CI runs: build, test,"
	@echo "                                   format check, clang-tidy, sanitizers"
	@echo ""
	@echo "  make format        Rewrite sources with clang-format"
	@echo "  make format-check  Verify formatting (CI's format job)"
	@echo "  make tidy          Run clang-tidy over src/ (CI's tidy job)"
	@echo "  make examples      lex + pp + parse + resolve every file in examples/"
	@echo ""
	@echo "  make ccache        Show the compiler cache's effectiveness"
	@echo "  make ccache-tune   Raise its size limit (default $(CCACHE_SIZE))"
	@echo ""
	@echo "  make clean         Remove build/$(BUILD)"
	@echo "  make distclean     Remove every build directory"

# --- the three steps, for one preset ----------------------------------------

configure:
	$(CMAKE) --preset $(BUILD)

build: configure
	$(CMAKE) --build --preset $(BUILD)

test: build
	$(CTEST) --preset $(BUILD)

# The presets, spelled as targets so `make ci` is shorter than the CMake line it
# replaces. `release` has no test preset of its own: it is the same tests with
# optimization on, so it builds and tests through `dev`'s runner.
dev:
	$(MAKE) BUILD=dev test

ci:
	$(MAKE) BUILD=ci test

sanitize:
	$(MAKE) BUILD=sanitize test

release:
	$(CMAKE) --preset release
	$(CMAKE) --build --preset release
	$(CTEST) --preset release

# The inner loop. One preset, one build, the tests, and the formatting gate:
# everything that is cheap and catches what an edit usually breaks. `gates` is
# the other end of the scale, and the difference is deliberate -- the sanitizer
# build and the static analysis are worth minutes before a commit, not on every
# save. Use `make BUILD=ci quick` to iterate with warnings as errors.
quick: format-check
	$(MAKE) BUILD=$(BUILD) test

# --- the gates --------------------------------------------------------------

# Everything CI runs, in the order CI runs it: a failure that would have been
# caught by the cheapest gate is reported by the cheapest gate.
gates: ci sanitize format-check tidy
	@echo "all gates passed"

# The formatter version matters: a newer one can reflow code the older one
# accepted, so CI pins its own and this target uses whatever is on PATH. `find`
# is the one non-POSIX tool here; it is present in Git Bash on Windows, which is
# what the Windows instructions assume.
format:
	find $(SOURCES) \( -name '*.h' -o -name '*.cc' \) -print0 | xargs -0 clang-format -i

format-check:
	find $(SOURCES) \( -name '*.h' -o -name '*.cc' \) -print0 \
	  | xargs -0 clang-format --dry-run --Werror
	@echo "format clean"

# clang-tidy cannot consume a GNU compile database (GCC emits flags it does not
# understand), so this configures a clang build of its own -- and does not build
# it, which is why the target is cheap and why it can run before the sanitizer
# job has finished.
#
# The analysis is the expensive half: the path-sensitive checks cost seconds per
# file, and one clang-tidy invocation walks its files one at a time. That is
# minutes of wall clock on a machine with idle cores. `run-clang-tidy` is the
# same tool driven in parallel -- same checks, same `--warnings-as-errors`, same
# non-zero exit on a finding -- so the only thing that changes is how long the
# gate takes. It is a separate binary, so where it is missing the sequential
# invocation is the fallback rather than a build error.
tidy:
	$(CMAKE) --preset ci -DCMAKE_CXX_COMPILER=clang++ -B build/tidy
	@if command -v run-clang-tidy >/dev/null 2>&1; then \
	  run-clang-tidy -p build/tidy -j $(JOBS) -quiet \
	    -warnings-as-errors '*' 'src/.*\.cc'; \
	else \
	  echo "minc+: run-clang-tidy not found, using one clang-tidy at a time"; \
	  clang-tidy -p build/tidy --warnings-as-errors='*' $$(find src -name '*.cc'); \
	fi

# --- the corpus -------------------------------------------------------------

# The examples are the language's de facto specification, so this is the check a
# reader can run without knowing anything about the test suite. `examples/pp`
# needs the include path; the five top-level files do not, and `-I` is harmless
# for them.
BIN := build/$(BUILD)/src/driver/mincc
INCLUDE_DIR := examples/pp/include

examples: build
	@set -e; \
	for file in examples/*.mx; do \
	  echo "== $$file"; \
	  $(BIN) lex "$$file" > /dev/null; \
	  $(BIN) parse --no-trivia "$$file" > /dev/null; \
	  $(BIN) resolve "$$file" > /dev/null; \
	  $(BIN) check "$$file" > /dev/null; \
	  $(BIN) ir "$$file" > /dev/null; \
	done; \
	for file in examples/pp/*.mx; do \
	  echo "== $$file"; \
	  $(BIN) pp -I $(INCLUDE_DIR) "$$file" > /dev/null; \
	  $(BIN) parse -I $(INCLUDE_DIR) "$$file" > /dev/null; \
	  $(BIN) resolve -I $(INCLUDE_DIR) "$$file" > /dev/null; \
	  $(BIN) check -I $(INCLUDE_DIR) "$$file" > /dev/null; \
	done; \
	echo "examples ok"

# --- the compiler cache -----------------------------------------------------

# ccache, when it has the room, is the difference between a cold rebuild and a
# no-op: the same clean build takes ~106 s with an empty cache and ~4 s with a
# warm one. But the 5 GiB default is small for four presets, and an evicted entry
# is a compile. These two targets report and raise the limit; neither is needed
# for a correct build, which is why `gates` does not call them.
ccache:
	@ccache --version 2>/dev/null | head -1 || echo "ccache is not installed"
	@ccache -s 2>/dev/null || true

CCACHE_SIZE ?= 10G

ccache-tune:
	@command -v ccache >/dev/null 2>&1 || { \
	  echo "minc+: ccache is not installed; nothing to raise"; exit 1; \
	}
	ccache -M $(CCACHE_SIZE)
	@echo "minc+: ccache limit is now $(CCACHE_SIZE); override with CCACHE_SIZE=..."

# --- cleaning ---------------------------------------------------------------

clean:
	rm -rf build/$(BUILD)

distclean:
	rm -rf build
