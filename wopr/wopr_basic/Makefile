CC      = gcc
CFLAGS  = -O2 -Wall
LDFLAGS = -lgmp -lm

.PHONY: help

# --------------------------------------------------------------------
# HELP SYSTEM
# --------------------------------------------------------------------
# Any target with a comment like this:
#   target: ## description
# will show up in `make help`.
help: ## Show this help message
    @echo "Available targets:"
    @grep -E '^[a-zA-Z0-9_-]+:.*?## ' Makefile | \
        sed -e 's/:.*##/:/' | column -t -s ':'

# --------------------------------------------------------------------
# SDL2 autodetection
# --------------------------------------------------------------------
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs   sdl2 2>/dev/null)

ifneq ($(SDL_LIBS),)
    CFLAGS  += $(SDL_CFLAGS) -DHAVE_SDL
    LDFLAGS += $(SDL_LIBS)
    SOUND_OBJ = sound_sdl.o
    $(info SDL2 found — building with audio support)
else
    SOUND_OBJ = sound_null.o
    $(info SDL2 not found — building without audio support)
endif

# --------------------------------------------------------------------
# INLINE BASIC EMBEDDING
# --------------------------------------------------------------------
ifdef INLINEBASIC
	CFLAGS += -DINLINEBASIC
	BASIC_HEADER = inline_basic.h
	GENERATED_HEADERS = $(BASIC_HEADER)

$(BASIC_HEADER): $(INLINEBASIC) ## Convert BASIC file to inline header
	xxd -i $< > $@
	$(info Embedding BASIC program: $<)
endif

# --------------------------------------------------------------------
# SOURCES
# --------------------------------------------------------------------
SRCS = main.c vars.c expr.c program.c commands.c display_ansi.c
OBJS = $(SRCS:.c=.o) $(SOUND_OBJ)

# --------------------------------------------------------------------
# BUILD TARGETS
# --------------------------------------------------------------------
basic: $(GENERATED_HEADERS) $(OBJS) ## Build BASIC interpreter (optionally with INLINEBASIC=foo.bas)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

embed: ## Convenience wrapper: make embed FILE=foo.bas
	$(MAKE) basic INLINEBASIC=$(FILE)

# --------------------------------------------------------------------
# OBJECT RULES
# --------------------------------------------------------------------
%.o: %.c basic.h display.h sound.h $(GENERATED_HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

sound_null.o: sound_null.c sound.h
	$(CC) $(CFLAGS) -c -o $@ $<

sound_sdl.o: sound_sdl.c sound.h
	$(CC) $(CFLAGS) -c -o $@ $<

# --------------------------------------------------------------------
# CLEAN
# --------------------------------------------------------------------
clean: ## Remove all build artifacts
	rm -f $(OBJS) sound_null.o sound_sdl.o basic inline_basic.h

.PHONY: clean help embed

