# ==============================================================================
#  Makefile — Real-Time Elevator Control System
#
#  Supports two build modes:
#    make              → Build for the current host (gcc/cc)
#    make TARGET=qnx   → Cross-compile for QNX Neutrino (qcc)
#
#  The output is a single executable: elevator_sim
# ==============================================================================

# ── Build target selection ────────────────────────────────────────────────────
#  Set TARGET=qnx on the command line to use the QNX cross-compiler.
#  By default, the native host compiler (gcc) is used.
TARGET ?= host

ifeq ($(TARGET),qnx)
    CC      = qcc
    CFLAGS  = -Wall -Wextra -g -Vgcc_ntoaarch64le
    LDFLAGS = -lm
else
    CC      = gcc
    CFLAGS  = -Wall -Wextra -g -pthread
    LDFLAGS = -pthread -lm
endif

# ── Project layout ────────────────────────────────────────────────────────────
SRCDIR   = src
INCDIR   = include
OBJDIR   = obj
_dummy  := $(shell mkdir -p $(OBJDIR) 2>/dev/null)
BINARY   = elevator_sim

# ── Source files (single-executable build) ────────────────────────────────────
SRCS = $(SRCDIR)/main.c              \
       $(SRCDIR)/elevator.c          \
       $(SRCDIR)/request_manager.c   \
       $(SRCDIR)/display.c           \
       $(SRCDIR)/logger.c

OBJS = $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(SRCS))

# ── Default target ────────────────────────────────────────────────────────────
all: $(BINARY)
	@echo ""
	@echo "Build complete: ./$(BINARY)"
	@echo "  Target platform: $(TARGET)"
	@echo ""

# ── Link ──────────────────────────────────────────────────────────────────────
$(BINARY): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ── Compile ───────────────────────────────────────────────────────────────────
$(OBJDIR)/%.o: $(SRCDIR)/%.c $(INCDIR)/elevator.h | $(OBJDIR)
	$(CC) $(CFLAGS) -I$(INCDIR) -c -o $@ $<

# ── Create object directory ───────────────────────────────────────────────────
$(OBJDIR):
	@-mkdir -p $(OBJDIR) 2>/dev/null || true

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(OBJDIR) $(BINARY) elevator_log.txt

# ── Convenience: rebuild from scratch ─────────────────────────────────────────
rebuild: clean all

# ── Run (host only) ──────────────────────────────────────────────────────────
run: $(BINARY)
	./$(BINARY)

.PHONY: all clean rebuild run
