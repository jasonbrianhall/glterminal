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
             fight_mode.cpp      \
             crt_audio.cpp

OBJS     := $(SRCS:.cpp=.o)
DEPS     := $(SRCS:.cpp=.d)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# -MMD -MP: generate .d files tracking all #included headers automatically.
# The .d file for foo.cpp becomes foo.d and is included below.
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPS)

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)
