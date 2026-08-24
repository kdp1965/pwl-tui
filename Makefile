PROJECT_NAME ?= pwl_tui

# runtime.c is the LOCAL (ttsky25a) version of the SDK runtime: it
# carries the host-filesystem syscall glue (weak __tinyqv_fs_* hooks +
# fd>=3 routing in _open/_read/_write) that upstream tinyQV-sdk lacks.
# Linking it here keeps the submodule pristine: our runtime.o resolves
# __runtime_init and the syscalls first, so the archive's runtime.o
# (whose _open is a hardwired -1) is never pulled.
PROJECT_SOURCES ?= main.c song_paradise.c song_paradise_v.c song_axelf.c \
                   song_axelf_a.c song_gmlast.c song_willie.c \
                   song_skybells.c cxxrt.cpp \
                   strsafe.c pwl_synth.c tqv_fs.c runtime.c

# Ported ncurses TUI stack (see tui/, copied from prism-test): termcurses
# VT100 backend over the UART, pdcurses 3.4 core, bare-metal platform
# layer, and the CTui/CPwlSynth application classes.  Sources build with
# tui/include compat headers standing in for the NuttX ones.
TUI_SOURCES = $(wildcard tui/*.c) \
              $(wildcard tui/termcurses/*.c) \
              $(wildcard tui/pdcurses/*.c) \
              $(wildcard tui/platform/*.c) \
              $(wildcard tui/*.cxx)
PROJECT_SOURCES += $(TUI_SOURCES)

RISCV_TOOLCHAIN ?= /opt/tinyQV

CC = $(RISCV_TOOLCHAIN)/bin/riscv32-unknown-elf-gcc
CXX = $(RISCV_TOOLCHAIN)/bin/riscv32-unknown-elf-g++
LD = $(RISCV_TOOLCHAIN)/bin/riscv32-unknown-elf-ld
OBJCOPY = $(RISCV_TOOLCHAIN)/bin/riscv32-unknown-elf-objcopy

TINYQV_SDK ?= tinyQV-sdk

OBJDIR ?= obj
PROJECT_OBJS = $(addprefix $(OBJDIR)/,$(patsubst %.cxx,%.o,$(patsubst %.cpp,%.o,$(PROJECT_SOURCES:.c=.o))))

CFLAGS = -O2 -I$(TINYQV_SDK) -I. -march=rv32ec_zicsr_zcb_zicond_zilsd -mabi=ilp32e -mno-strict-align -nostdlib -nostartfiles -ffreestanding -ffunction-sections -fdata-sections -Wall -Werror -MMD -MP

# Embedded C++ subset: no exceptions/RTTI (no unwinder or type info in
# flash), no thread-safe static guards (single core), no atexit dtors
# (firmware never exits).  Global constructors DO run - the SDK's
# __runtime_init walks .init_array before main.  operator new/delete
# forward to newlib malloc (cxxrt.cpp) over the ram_a heap span in the
# local memmap; -fcheck-new makes new-expressions NULL-safe since new
# can't throw.
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -fcheck-new

# TUI library sources: NuttX compat headers from tui/include, and the
# imported code keeps its original style so a few benign warnings are
# tolerated (the project's own files stay -Werror).
# -include nuttx/config.h mirrors how the NuttX build force-feeds the
# config into every translation unit (headers guard on CONFIG_ symbols
# without including it themselves).
TUI_INC = -Itui/include -Itui/platform -Itui/termcurses -include nuttx/config.h -D_GNU_SOURCE
TUI_CFLAGS = $(CFLAGS) $(TUI_INC) -Wno-error
TUI_CXXFLAGS = $(CXXFLAGS) $(TUI_INC) -Wno-error

all: $(PROJECT_NAME).bin

clean:
	@rm -rf $(OBJDIR) $(PROJECT_NAME).elf $(PROJECT_NAME).bin $(PROJECT_NAME).map

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(OBJDIR)/tui/%.o: tui/%.c | $(OBJDIR)
	@mkdir -p $(@D)
	@echo "Compiling $(notdir $<)..."
	@$(CC) $(TUI_CFLAGS) -c $< -o $@

$(OBJDIR)/tui/%.o: tui/%.cxx | $(OBJDIR)
	@mkdir -p $(@D)
	@echo "Compiling $(notdir $<)..."
	@$(CXX) $(TUI_CXXFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	@echo "Compiling $(notdir $<)..."
	@$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.cpp | $(OBJDIR)
	@echo "Compiling $(notdir $<)..."
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# The pwl_synth driver builds from the SDK source tree (compiled here
# rather than into tinyQV.a so this app stays self-contained)
#$(OBJDIR)/pwl_synth.o: $(TINYQV_SDK)/pwl_synth.c $(TINYQV_SDK)/pwl_synth.h | $(OBJDIR)
#	@echo "Compiling $(notdir $<)..."
#	@$(CC) $(CFLAGS) -c $< -o $@

# Host filesystem client.  Linking it also pulls its strong
# __tinyqv_fs_* definitions over runtime.c's weak stubs, which is what
# turns fopen/fgets/fprintf into real file access over the console.
#$(OBJDIR)/tqv_fs.o: $(TINYQV_SDK)/tqv_fs.c $(TINYQV_SDK)/tqv_fs.h | $(OBJDIR)
#	@echo "Compiling $(notdir $<)..."
#	@$(CC) $(CFLAGS) -c $< -o $@

# The SDK submodule builds on demand, so a fresh checkout links without
# a manual 'cd tinyQV-sdk && make' first.  Only the two artifacts this
# app links are requested (the SDK's default target also builds sim and
# asteroids libraries).  Both fall out of one recursive make; if make
# fires the recipe once per missing file, the second run is a no-op.
# A just-cloned superproject has an empty submodule dir - populate it.
$(TINYQV_SDK)/start.o $(TINYQV_SDK)/tinyQV.a:
	@test -f $(TINYQV_SDK)/Makefile || git submodule update --init $(TINYQV_SDK)
	@echo "Building $(TINYQV_SDK)..."
	@$(MAKE) -C $(TINYQV_SDK) tinyQV.a start.o

$(PROJECT_NAME).elf: $(PROJECT_OBJS) $(TINYQV_SDK)/start.o $(TINYQV_SDK)/tinyQV.a memmap
	@echo "Linking $@..."
	@$(LD) $(PROJECT_OBJS) $(TINYQV_SDK)/start.o $(TINYQV_SDK)/tinyQV.a $(RISCV_TOOLCHAIN)/riscv32-unknown-elf/lib/libc.a $(RISCV_TOOLCHAIN)/lib/gcc/riscv32-unknown-elf/*/libgcc.a -T memmap --gc-sections -Map=$(PROJECT_NAME).map -o $@

$(PROJECT_NAME).bin: $(PROJECT_NAME).elf
	@echo "Creating $@..."
	@$(OBJCOPY) $< -O binary $@

-include $(wildcard $(OBJDIR)/*.d $(OBJDIR)/*/*.d $(OBJDIR)/*/*/*.d)
