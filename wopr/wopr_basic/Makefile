# --------------------------------------------------------------------
# Toolchain selection
#   make              → native Linux   (output: build/linux/basic)
#   make windows      → Windows cross  (output: build/windows/basic.exe)
# --------------------------------------------------------------------
ifeq ($(TARGET),windows)
    CC         = x86_64-w64-mingw32-gcc
    PKG_CONFIG = mingw64-pkg-config
    BUILDDIR   = build/windows
    EXE        = $(BUILDDIR)/basic.exe
    CFLAGS     = -O2 -Wall -std=c11 -DWIN32 -D_WIN32
    LDFLAGS    = -lgmp -lm -mconsole
else
    CC         = gcc
    PKG_CONFIG = pkg-config
    BUILDDIR   = build/linux
    EXE        = $(BUILDDIR)/basic
    CFLAGS     = -O2 -Wall -std=c11
    LDFLAGS    = -lgmp -lm
endif

# --------------------------------------------------------------------
# SDL2 autodetection
# --------------------------------------------------------------------
SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell $(PKG_CONFIG) --libs   sdl2 2>/dev/null)

ifneq ($(SDL_LIBS),)
    CFLAGS    += $(SDL_CFLAGS) -DHAVE_SDL
    LDFLAGS   += $(SDL_LIBS)
    SOUND_SRC  = sound_sdl.c
    GFX_SRC    = gfx_sdl.c
    $(info SDL2 found — building with audio + SCREEN 1-9 graphics support)
else
    SOUND_SRC  = sound_null.c
    GFX_SRC    = gfx_null.c
    $(info SDL2 not found — building text-only \(no audio or SCREEN 1-9\))
endif

# --------------------------------------------------------------------
# INLINE BASIC EMBEDDING
# --------------------------------------------------------------------
ifdef INLINEBASIC
    CFLAGS += -DINLINEBASIC
    BASIC_HEADER = inline_basic.h
    GENERATED_HEADERS = $(BASIC_HEADER)

$(BASIC_HEADER): $(INLINEBASIC)
	xxd -n wopr_basic_program -i $< > $@
	$(info Embedding BASIC program: $<)
endif

# --------------------------------------------------------------------
# SOURCES → per-target object paths
# --------------------------------------------------------------------
SRCS = main.c vars.c expr.c program.c commands.c type.c display_ansi.c $(GFX_SRC) basic_print.c $(SOUND_SRC)
OBJS = $(patsubst %.c,$(BUILDDIR)/%.o,$(SRCS))

# --------------------------------------------------------------------
# BUILD TARGETS
# --------------------------------------------------------------------
.PHONY: all linux windows clean embed

all: linux

linux:
	$(MAKE) build/linux/basic TARGET=linux

windows:
	$(MAKE) build/windows/basic.exe TARGET=windows

$(EXE): $(GENERATED_HEADERS) $(OBJS)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

embed:
	$(MAKE) $(EXE) INLINEBASIC=$(FILE)

# --------------------------------------------------------------------
# OBJECT RULES
# --------------------------------------------------------------------
$(BUILDDIR)/%.o: %.c basic.h display.h sound.h $(GENERATED_HEADERS)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# --------------------------------------------------------------------
# CLEAN
# --------------------------------------------------------------------
clean:
	rm -rf build
	rm -f inline_basic.h
