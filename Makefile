CC      ?= cc
CPPFLAGS ?=
CPPFLAGS += -Iheader
CFLAGS  ?= -std=gnu11 -O2
CFLAGS  += -Wall -Wextra -Werror
LDFLAGS ?=
LDLIBS  ?=
PREFIX  ?= /usr/local

# Profile-guided optimization:
#   make PGO=generate [PROFDIR=...]
#   ... run every binary on a representative workload ...
#   make clean
#   make PGO=use [PROFDIR=...]
# With PGO=use and no profiles collected, this builds without PGO instead
# of erroring. A partial profile still errors under -Werror, so the
# workload must cover every source file (a --help run is not enough).
# Clang needs one extra step before the use build: make pgo-merge
# (requires llvm-profdata).
PGO     ?=
PROFDIR ?= $(CURDIR)/pgo-data
DIST_DIR ?= dist
DEPS_DIR ?= $(DIST_DIR)/deps
ifneq ($(PGO),)
# Heuristic; override on the command line if it guesses wrong.
PGO_IS_CLANG ?= $(findstring clang,$(shell $(CC) --version 2>/dev/null))
endif
ifeq ($(PGO),generate)
# Created now: the instrumented binaries run outside make.
$(shell mkdir -p "$(PROFDIR)")
ifeq ($(PGO_IS_CLANG),)
CFLAGS  += -fprofile-generate=$(PROFDIR)
LDFLAGS += -fprofile-generate=$(PROFDIR)
else
CFLAGS  += -fprofile-instr-generate=$(PROFDIR)/%m-%p.profraw
LDFLAGS += -fprofile-instr-generate=$(PROFDIR)/%m-%p.profraw
endif
else ifeq ($(PGO),use)
ifeq ($(PGO_IS_CLANG),)
# GCC mirrors the source path under PROFDIR, flat or nested by setup.
PGO_DATA  := $(shell find "$(PROFDIR)" -name '*.gcda' 2>/dev/null)
PGO_FLAGS := -fprofile-use=$(PROFDIR)
else
PGO_DATA  := $(wildcard $(PROFDIR)/merged.profdata)
PGO_FLAGS := -fprofile-instr-use=$(PROFDIR)/merged.profdata
endif
ifeq ($(PGO_DATA),)
$(info PGO=use: no profiles in $(PROFDIR); building without PGO)
else
CFLAGS  += $(PGO_FLAGS)
LDFLAGS += $(PGO_FLAGS)
endif
endif

TARGET  := rootlet
LIB     := $(DEPS_DIR)/io.o $(DEPS_DIR)/tty.o $(DEPS_DIR)/fwd.o
HDR     := header/io.h header/tty.h header/fwd.h

.PHONY: all rootlet sudo connect install clean distclean pgo-merge

all: rootlet

rootlet: $(DIST_DIR)/$(TARGET)

$(DIST_DIR)/$(TARGET): src/rootlet.c $(LIB) $(HDR) | $(DIST_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ src/rootlet.c $(LIB) $(LDLIBS)

sudo: $(DIST_DIR)/sudo

$(DIST_DIR)/sudo: src/sudo.c $(LIB) $(HDR) | $(DIST_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ src/sudo.c $(LIB) $(LDLIBS)

connect: $(DIST_DIR)/connect

$(DIST_DIR)/connect: src/connect.c $(LIB) $(HDR) | $(DIST_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ src/connect.c $(LIB) $(LDLIBS) -lpthread

$(DEPS_DIR)/%.o: src/lib/%.c $(HDR) | $(DEPS_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(DIST_DIR):
	mkdir -p $@

$(DEPS_DIR): | $(DIST_DIR)
	mkdir -p $@

install: rootlet
	install -D -m 0755 $(DIST_DIR)/$(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

# Merge clang raw profiles for the use build. Plain merge: -sparse drops
# records the use build needs, failing it under -Werror.
LLVM_PROFDATA ?= llvm-profdata
pgo-merge:
	@test -n "$(wildcard $(PROFDIR)/*.profraw)" || { echo "pgo-merge: no .profraw in $(PROFDIR)"; exit 1; }
	"$(LLVM_PROFDATA)" merge -o "$(PROFDIR)/merged.profdata" "$(PROFDIR)"/*.profraw

clean:
	rm -rf $(DIST_DIR)
	rm -f $(TARGET) sudo connect *.o *.gcno *.gcda

# clean plus any collected PGO profiles (kept by clean so a
# generate -> clean -> use cycle still finds them).
distclean: clean
	@if [ -n "$(PROFDIR)" ] && [ "$(PROFDIR)" != "/" ]; then rm -rf "$(PROFDIR)"; fi

.PHONY: install clean distclean pgo-merge
