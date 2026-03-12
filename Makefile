CXX     = g++
CXXFLAGS = -std=c++17 -O2 -Wall \
           $(shell pkg-config --cflags sdl2 freetype2 glew)

LIBS    = $(shell pkg-config --libs sdl2 freetype2 glew) \
          -lGL -lutil

TARGET  = gl_terminal
SRC     = gl_terminal_main.cpp

# Monospace.h must be in the same directory (copy from your project)
# If it's elsewhere: make MONOSPACE_DIR=/path/to/dir

MONOSPACE_DIR ?= .

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -I$(MONOSPACE_DIR) -o $@ $< $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: clean
