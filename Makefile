CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra \
            $(shell pkg-config --cflags freetype2 sdl2 glew)
LDFLAGS  := $(shell pkg-config --libs freetype2 sdl2 glew) -lGL

TARGET   := gl_terminal

SRCS     := gl_terminal_main.cpp \
             gl_renderer.cpp     \
             ft_font.cpp         \
             term_color.cpp      \
             terminal.cpp        \
             term_pty.cpp        \
             term_ui.cpp         \
             gl_bouncingcircle.cpp \
             fight_mode.cpp

OBJS     := $(SRCS:.cpp=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Header dependencies (explicit — avoids makedepend)
gl_terminal_main.o: gl_terminal_main.cpp gl_terminal.h gl_renderer.h ft_font.h \
                    term_color.h terminal.h term_pty.h term_ui.h gl_bouncingcircle.h
gl_renderer.o:      gl_renderer.cpp gl_renderer.h gl_terminal.h
ft_font.o:          ft_font.cpp ft_font.h gl_renderer.h gl_terminal.h \
                    DejaVuMonoBold.h DejaVuMono.h DejaVuMonoOblique.h \
                    DejaVuMonoBoldOblique.h NotoEmoji.h
term_color.o:       term_color.cpp term_color.h
terminal.o:         terminal.cpp terminal.h ft_font.h gl_terminal.h
term_pty.o:         term_pty.cpp term_pty.h terminal.h
term_ui.o:          term_ui.cpp term_ui.h term_pty.h ft_font.h \
                    gl_renderer.h term_color.h gl_terminal.h gl_bouncingcircle.h \
                    fight_mode.h
gl_bouncingcircle.o: gl_bouncingcircle.cpp gl_bouncingcircle.h gl_renderer.h ft_font.h
fight_mode.o:       fight_mode.cpp fight_mode.h gl_renderer.h

clean:
	rm -f $(OBJS) $(TARGET)
