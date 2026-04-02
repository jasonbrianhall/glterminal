CC      = gcc
CFLAGS  = -O2 -Wall
LDFLAGS = -lgmp -lm

SRCS = main.c vars.c expr.c program.c commands.c display_ansi.c
OBJS = $(SRCS:.c=.o)

basic: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c basic.h display.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) basic

.PHONY: clean
