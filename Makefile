# Felix Terminal Makefile
# Cross-compile for Windows with: make windows
# Native Linux build with:        make linux

# Compiler settings
CXX_LINUX = g++
CXX_WIN   = x86_64-w64-mingw32-g++

# Common flags
CXXFLAGS_COMMON = -Wall -Wextra -std=c++17

# Package config
PKG_CONFIG_LINUX = pkg-config
PKG_CONFIG_WIN   = mingw64-pkg-config

# ============================================================================
# FREETYPE
# ============================================================================
FREETYPE_CFLAGS_LINUX := $(shell $(PKG_CONFIG_LINUX) --cflags freetype2 2>/dev/null || echo "-I/usr/include/freetype2")
FREETYPE_LIBS_LINUX   := $(shell $(PKG_CONFIG_LINUX) --libs   freetype2 2>/dev/null || echo "-lfreetype")

FREETYPE_CFLAGS_WIN   := $(shell $(PKG_CONFIG_WIN) --cflags freetype2 2>/dev/null || echo "")
FREETYPE_LIBS_WIN     := $(shell $(PKG_CONFIG_WIN) --libs   freetype2 2>/dev/null || echo "-lfreetype")

# ============================================================================
# SDL2
# ============================================================================
SDL2_CFLAGS_LINUX := $(shell pkg-config --cflags sdl2 2>/dev/null || echo "")
SDL2_LIBS_LINUX   := $(shell pkg-config --libs   sdl2 2>/dev/null || echo "-lSDL2")

SDL2_CFLAGS_WIN   := $(shell $(PKG_CONFIG_WIN) --cflags sdl2 2>/dev/null || echo "")
SDL2_LIBS_WIN     := $(shell $(PKG_CONFIG_WIN) --libs   sdl2 2>/dev/null || echo "-lSDL2")

# ============================================================================
# GLEW
# ============================================================================
GLEW_CFLAGS_LINUX := $(shell pkg-config --cflags glew 2>/dev/null || echo "")
GLEW_LIBS_LINUX   := $(shell pkg-config --libs   glew 2>/dev/null || echo "-lGLEW")

GLEW_CFLAGS_WIN   := $(shell $(PKG_CONFIG_WIN) --cflags glew 2>/dev/null || echo "")
GLEW_LIBS_WIN     := $(shell $(PKG_CONFIG_WIN) --libs   glew 2>/dev/null || echo "-lglew32")

# ============================================================================
# LINUX FLAGS
# ============================================================================
CXXFLAGS_LINUX = $(CXXFLAGS_COMMON) \
                 $(SDL2_CFLAGS_LINUX) $(GLEW_CFLAGS_LINUX) $(FREETYPE_CFLAGS_LINUX) \
                 -DLINUX -O2 -ffunction-sections -fdata-sections -flto

LDFLAGS_LINUX  = $(SDL2_LIBS_LINUX) $(GLEW_LIBS_LINUX) $(FREETYPE_LIBS_LINUX) \
                 -lGL -lm -pthread -lstdc++ \
                 -s -Wl,--gc-sections -flto

CXXFLAGS_LINUX_DEBUG = $(CXXFLAGS_COMMON) \
                       $(SDL2_CFLAGS_LINUX) $(GLEW_CFLAGS_LINUX) $(FREETYPE_CFLAGS_LINUX) \
                       -DLINUX -DDEBUG -g -O0

LDFLAGS_LINUX_DEBUG  = $(SDL2_LIBS_LINUX) $(GLEW_LIBS_LINUX) $(FREETYPE_LIBS_LINUX) \
                       -lGL -lm -pthread -lstdc++

# ============================================================================
# WINDOWS FLAGS
# ============================================================================
CXXFLAGS_WIN = $(CXXFLAGS_COMMON) \
               $(SDL2_CFLAGS_WIN) $(GLEW_CFLAGS_WIN) $(FREETYPE_CFLAGS_WIN) \
               -DWIN32 -D_WIN32 -D_WIN32_WINNT=0x0A00 \
               -O2 -ffunction-sections -fdata-sections -flto

# -mwindows: no console window behind the GL window
# term_pty_win.cpp replaces term_pty.cpp for ConPTY
LDFLAGS_WIN  = $(SDL2_LIBS_WIN) $(GLEW_LIBS_WIN) $(FREETYPE_LIBS_WIN) \
               -lopengl32 -lwinmm -mwindows \
               -s -Wl,--gc-sections -flto

CXXFLAGS_WIN_DEBUG = $(CXXFLAGS_COMMON) \
                     $(SDL2_CFLAGS_WIN) $(GLEW_CFLAGS_WIN) $(FREETYPE_CFLAGS_WIN) \
                     -DWIN32 -D_WIN32 -D_WIN32_WINNT=0x0A00 -DDEBUG -g -O0

# ============================================================================
# SOURCES
# ============================================================================
# Shared across platforms (term_pty is platform-specific, excluded here)
SRCS_COMMON = gl_terminal_main.cpp  gl_renderer.cpp       \
              ft_font.cpp           term_color.cpp        \
              terminal.cpp          term_ui.cpp           \
              gl_bouncingcircle.cpp fight_mode.cpp        \
              crt_audio.cpp         felix_settings.cpp    \
              kitty_graphics.cpp

SRCS_LINUX = $(SRCS_COMMON) term_pty.cpp
SRCS_WIN   = $(SRCS_COMMON) term_pty_win.cpp

# ============================================================================
# OBJECTS
# ============================================================================
OBJECTS_LINUX       = $(addprefix $(BUILD_DIR_LINUX)/,      $(SRCS_LINUX:.cpp=.o))
OBJECTS_LINUX_DEBUG = $(addprefix $(BUILD_DIR_LINUX_DEBUG)/, $(SRCS_LINUX:.cpp=.debug.o))
OBJECTS_WIN         = $(addprefix $(BUILD_DIR_WIN)/,         $(SRCS_WIN:.cpp=.win.o))
OBJECTS_WIN_DEBUG   = $(addprefix $(BUILD_DIR_WIN_DEBUG)/,   $(SRCS_WIN:.cpp=.win.debug.o))

# ============================================================================
# TARGETS / DIRS
# ============================================================================
EXECUTABLE_LINUX       = flt
EXECUTABLE_LINUX_DEBUG = flt_debug
EXECUTABLE_WIN         = flt.exe
EXECUTABLE_WIN_DEBUG   = flt_debug.exe

BUILD_DIR             = build
BUILD_DIR_LINUX       = $(BUILD_DIR)/linux
BUILD_DIR_LINUX_DEBUG = $(BUILD_DIR)/linux_debug
BUILD_DIR_WIN         = $(BUILD_DIR)/windows
BUILD_DIR_WIN_DEBUG   = $(BUILD_DIR)/windows_debug

DLL_SOURCE_DIR = /usr/x86_64-w64-mingw32/sys-root/mingw/bin

$(shell mkdir -p $(BUILD_DIR_LINUX) $(BUILD_DIR_LINUX_DEBUG) $(BUILD_DIR_WIN) $(BUILD_DIR_WIN_DEBUG))

# ============================================================================
# TOP-LEVEL TARGETS
# ============================================================================
.PHONY: all linux windows debug clean clean-all help check-deps

all: linux

linux: flt-linux

windows: flt-windows flt-collect-dlls

debug: flt-linux-debug flt-windows-debug

# ============================================================================
# LINUX BUILD
# ============================================================================
.PHONY: flt-linux
flt-linux: $(BUILD_DIR_LINUX)/$(EXECUTABLE_LINUX)

$(BUILD_DIR_LINUX)/$(EXECUTABLE_LINUX): $(OBJECTS_LINUX)
	@echo "Linking Linux: $@"
	$(CXX_LINUX) $(CXXFLAGS_LINUX) $^ -o $@ $(LDFLAGS_LINUX)
	@echo "✓ $@"

$(BUILD_DIR_LINUX)/%.o: %.cpp
	@echo "Compiling (Linux): $<"
	$(CXX_LINUX) $(CXXFLAGS_LINUX) -MMD -MP -c $< -o $@

# ============================================================================
# LINUX DEBUG BUILD
# ============================================================================
.PHONY: flt-linux-debug
flt-linux-debug: $(BUILD_DIR_LINUX_DEBUG)/$(EXECUTABLE_LINUX_DEBUG)

$(BUILD_DIR_LINUX_DEBUG)/$(EXECUTABLE_LINUX_DEBUG): $(OBJECTS_LINUX_DEBUG)
	@echo "Linking Linux debug: $@"
	$(CXX_LINUX) $(CXXFLAGS_LINUX_DEBUG) $^ -o $@ $(LDFLAGS_LINUX_DEBUG)
	@echo "✓ $@"

$(BUILD_DIR_LINUX_DEBUG)/%.debug.o: %.cpp
	@echo "Compiling (Linux Debug): $<"
	$(CXX_LINUX) $(CXXFLAGS_LINUX_DEBUG) -MMD -MP -c $< -o $@

# ============================================================================
# WINDOWS BUILD
# ============================================================================
.PHONY: flt-windows
flt-windows: $(BUILD_DIR_WIN)/$(EXECUTABLE_WIN)

$(BUILD_DIR_WIN)/$(EXECUTABLE_WIN): $(OBJECTS_WIN)
	@echo "Linking Windows: $@"
	$(CXX_WIN) $(CXXFLAGS_WIN) $^ -o $@ $(LDFLAGS_WIN)
	@echo "✓ $@"

$(BUILD_DIR_WIN)/%.win.o: %.cpp
	@echo "Compiling (Windows): $<"
	$(CXX_WIN) $(CXXFLAGS_WIN) -MMD -MP -c $< -o $@

# ============================================================================
# WINDOWS DEBUG BUILD
# ============================================================================
.PHONY: flt-windows-debug
flt-windows-debug: $(BUILD_DIR_WIN_DEBUG)/$(EXECUTABLE_WIN_DEBUG)

$(BUILD_DIR_WIN_DEBUG)/$(EXECUTABLE_WIN_DEBUG): $(OBJECTS_WIN_DEBUG)
	@echo "Linking Windows debug: $@"
	$(CXX_WIN) $(CXXFLAGS_WIN_DEBUG) $^ -o $@ $(LDFLAGS_WIN)
	@echo "✓ $@"

$(BUILD_DIR_WIN_DEBUG)/%.win.debug.o: %.cpp
	@echo "Compiling (Windows Debug): $<"
	$(CXX_WIN) $(CXXFLAGS_WIN_DEBUG) -MMD -MP -c $< -o $@

# ============================================================================
# DLL COLLECTION
# ============================================================================
.PHONY: flt-collect-dlls
flt-collect-dlls: $(BUILD_DIR_WIN)/$(EXECUTABLE_WIN)
	@echo "Collecting DLLs..."
	@if [ -f collect_dlls.sh ]; then \
		./collect_dlls.sh $(BUILD_DIR_WIN)/$(EXECUTABLE_WIN) $(DLL_SOURCE_DIR) $(BUILD_DIR_WIN); \
	else \
		echo "Tip: write collect_dlls.sh or copy manually:"; \
		echo "  SDL2.dll, glew32.dll, freetype.dll, libwinpthread-1.dll,"; \
		echo "  libgcc_s_seh-1.dll, libstdc++-6.dll"; \
	fi

# ============================================================================
# DEPENDENCY FILES
# ============================================================================
-include $(OBJECTS_LINUX:.o=.d)
-include $(OBJECTS_LINUX_DEBUG:.o=.d)
-include $(OBJECTS_WIN:.o=.d)
-include $(OBJECTS_WIN_DEBUG:.o=.d)

# ============================================================================
# UTILITY
# ============================================================================
.PHONY: check-deps
check-deps:
	@echo "=== Linux ==="
	@$(PKG_CONFIG_LINUX) --exists freetype2 && echo "✓ freetype2" || echo "✗ freetype2"
	@$(PKG_CONFIG_LINUX) --exists sdl2      && echo "✓ sdl2"      || echo "✗ sdl2"
	@$(PKG_CONFIG_LINUX) --exists glew      && echo "✓ glew"      || echo "✗ glew"
	@echo "=== Windows (mingw64) ==="
	@$(PKG_CONFIG_WIN) --exists freetype2 && echo "✓ freetype2" || echo "✗ freetype2"
	@$(PKG_CONFIG_WIN) --exists sdl2      && echo "✓ sdl2"      || echo "✗ sdl2"
	@$(PKG_CONFIG_WIN) --exists glew      && echo "✓ glew"      || echo "✗ glew"

clean:
	find $(BUILD_DIR) -type f \( -name "*.o" -o -name "*.d" \) -delete 2>/dev/null || true
	rm -f $(BUILD_DIR_LINUX)/$(EXECUTABLE_LINUX)
	rm -f $(BUILD_DIR_LINUX_DEBUG)/$(EXECUTABLE_LINUX_DEBUG)
	rm -f $(BUILD_DIR_WIN)/$(EXECUTABLE_WIN)
	rm -f $(BUILD_DIR_WIN_DEBUG)/$(EXECUTABLE_WIN_DEBUG)

clean-all:
	rm -rf $(BUILD_DIR)

help:
	@echo "Felix Terminal build targets:"
	@echo "  make              - Linux build (default)"
	@echo "  make linux        - Linux build"
	@echo "  make windows      - Windows cross-compile (mingw)"
	@echo "  make debug        - Debug builds for both platforms"
	@echo "  make check-deps   - Verify all dependencies are present"
	@echo "  make clean        - Remove object files and binaries"
	@echo "  make clean-all    - Remove entire build/ directory"
	@echo ""
	@echo "Windows cross-compile requires:"
	@echo "  x86_64-w64-mingw32-g++  (mingw-w64)"
	@echo "  mingw64-pkg-config"
	@echo "  mingw64 packages: sdl2, glew, freetype2"
	@echo "  On Fedora: sudo dnf install mingw64-SDL2 mingw64-glew mingw64-freetype"
	@echo "  On Ubuntu: sudo apt install mingw-w64 mingw-w64-tools"
	@echo "             (then build deps from source or use mxe.cc)"
