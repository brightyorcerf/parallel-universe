# Builds two binaries per problem:
#   bin/<prog>       OpenMP
#   bin/<prog>_seq   compiled with no -fopenmp at all, so it is genuinely
#                    single-threaded. This is the sequential reference the
#                    assignment requires results to be verified against; the
#                    programs fall back to omp_* stubs in common.h.

BIN   := bin
SRC   := src
PROGS := p1_matsq p2_xpow15 p3_sort p4_primes p5_dotprod

UNAME := $(shell uname -s)

ifeq ($(UNAME),Darwin)
  CC          ?= clang
  # Override for a non-Homebrew or not-yet-linked install:
  #   make OMP_PREFIX=/usr/local/Cellar/libomp/23.1.0
  OMP_PREFIX  ?= $(shell brew --prefix libomp 2>/dev/null)
  OMP_CFLAGS  := -Xpreprocessor -fopenmp -I$(OMP_PREFIX)/include
  # Homebrew's libomp has install name @rpath/libomp.dylib, so an -rpath entry
  # is required or the binary links but aborts at startup with
  # "Symbol not found: ___kmpc_end_master".
  OMP_LDFLAGS := -L$(OMP_PREFIX)/lib -Wl,-rpath,$(OMP_PREFIX)/lib -lomp
else
  CC          ?= gcc
  OMP_CFLAGS  := -fopenmp
  OMP_LDFLAGS := -fopenmp
endif

# -march=native unlocks AVX2/FMA and is worth a large multiple on these kernels,
# but is rejected by some compiler/target combinations. Probe once, degrade
# quietly if unavailable.
ARCH := $(shell $(CC) -march=native -E -x c /dev/null >/dev/null 2>&1 && echo -march=native)

# gnu11, not c11: strict ISO mode hides the POSIX declaration of gettimeofday().
CFLAGS := -O3 $(ARCH) -std=gnu11 -Wall -Wextra -Wno-unknown-pragmas
LDLIBS := -lm

OMP_BINS := $(addprefix $(BIN)/,$(PROGS))
SEQ_BINS := $(addsuffix _seq,$(OMP_BINS))

.PHONY: all check clean info
all: $(OMP_BINS) $(SEQ_BINS)

$(BIN):
	@mkdir -p $(BIN)

$(BIN)/%_seq: $(SRC)/%.c $(SRC)/common.c $(SRC)/common.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $(SRC)/$*.c $(SRC)/common.c $(LDLIBS)

$(BIN)/%: $(SRC)/%.c $(SRC)/common.c $(SRC)/common.h | $(BIN)
	$(CC) $(CFLAGS) $(OMP_CFLAGS) -o $@ $(SRC)/$*.c $(SRC)/common.c $(OMP_LDFLAGS) $(LDLIBS)

# Every correctness gate in one command. Run this before trusting any timing.
check: all
	@fail=0; for p in $(PROGS); do \
	   $(BIN)/$$p --selftest || fail=1; \
	 done; \
	 [ $$fail -eq 0 ] && echo "ALL SELFTESTS PASSED" || { echo "SELFTESTS FAILED"; exit 1; }

info:
	@echo "CC      = $(CC)"
	@echo "CFLAGS  = $(CFLAGS)"
	@echo "OpenMP  = $(OMP_CFLAGS) $(OMP_LDFLAGS)"

clean:
	rm -rf $(BIN)
