CC      = gcc
CFLAGS  = -O2 -Wall
LDFLAGS = -lgmp -lm

# Auto-detect SDL2.  If pkg-config finds it, compile the real audio
# backend and pass -DHAVE_SDL; otherwise use the silent null backend.
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

SRCS = main.c vars.c expr.c program.c commands.c display_ansi.c
OBJS = $(SRCS:.c=.o) $(SOUND_OBJ)

basic: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c basic.h display.h sound.h
	$(CC) $(CFLAGS) -c -o $@ $<

sound_null.o: sound_null.c sound.h
	$(CC) $(CFLAGS) -c -o $@ $<

sound_sdl.o: sound_sdl.c sound.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) sound_null.o sound_sdl.o basic

.PHONY: clean
