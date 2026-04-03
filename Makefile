# Felix Terminal Makefile
# Cross-compile for Windows with: make windows
# Native Linux build with:        make linux
#
# SSH support (libssh2) — opt-in via SSH=1:
#   make linux   SSH=1
#   make windows SSH=1
#   make debug   SSH=1

# Compiler settings
CXX_LINUX = g++
CXX_WIN   = x86_64-w64-mingw32-g++
CC_LINUX  = gcc
CC_WIN    = x86_64-w64-mingw32-gcc

# Common flags
CXXFLAGS_COMMON = -Wall -Wextra -std=c++17
CFLAGS_COMMON   = -Wall -Wextra

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
# LIBSSH2  (only pulled in when SSH=1)
# ============================================================================
SSH2_CFLAGS_LINUX := $(shell $(PKG_CONFIG_LINUX) --cflags libssh2 2>/dev/null || echo "")
SSH2_LIBS_LINUX   := $(shell $(PKG_CONFIG_LINUX) --libs   libssh2 2>/dev/null || echo "-lssh2")

SSH2_CFLAGS_WIN   := $(shell $(PKG_CONFIG_WIN) --cflags libssh2 2>/dev/null || echo "")
SSH2_LIBS_WIN     := $(shell $(PKG_CONFIG_WIN) --libs   libssh2 2>/dev/null || echo "-lssh2")

  SSH_SRCS         = ssh_session.cpp port_forward.cpp pf_overlay.cpp
  SSH_DEFINE       = -DUSESSH
  SSH_CFLAGS_LINUX = $(SSH2_CFLAGS_LINUX)
  SSH_LIBS_LINUX   = $(SSH2_LIBS_LINUX) -lcrypto -lssl
  SSH_CFLAGS_WIN   = $(SSH2_CFLAGS_WIN)
  SSH_LIBS_WIN     = $(SSH2_LIBS_WIN) -lcrypto -lssl -lws2_32
  SSH_SUFFIX       = 

# ============================================================================
# LINUX FLAGS
# ============================================================================
CXXFLAGS_LINUX = $(CXXFLAGS_COMMON) \
                 $(SDL2_CFLAGS_LINUX) $(GLEW_CFLAGS_LINUX) $(FREETYPE_CFLAGS_LINUX) \
                 $(SSH_CFLAGS_LINUX) \
                 -Iwopr \
                 -DLINUX $(SSH_DEFINE) -O2 -ffunction-sections -fdata-sections -flto

LDFLAGS_LINUX  = $(SDL2_LIBS_LINUX) $(GLEW_LIBS_LINUX) $(FREETYPE_LIBS_LINUX) \
                 $(SSH_LIBS_LINUX) \
                 -lGL -lpng -lz -lm -pthread -lstdc++ -lSDL2_mixer -lgmp \
                 -s -Wl,--gc-sections -flto

CXXFLAGS_LINUX_DEBUG = $(CXXFLAGS_COMMON) \
                       $(SDL2_CFLAGS_LINUX) $(GLEW_CFLAGS_LINUX) $(FREETYPE_CFLAGS_LINUX) \
                       $(SSH_CFLAGS_LINUX) \
                       -Iwopr \
                       -DLINUX $(SSH_DEFINE) -DDEBUG -g -O0

LDFLAGS_LINUX_DEBUG  = $(SDL2_LIBS_LINUX) $(GLEW_LIBS_LINUX) $(FREETYPE_LIBS_LINUX) \
                       $(SSH_LIBS_LINUX) \
                       -lGL -lpng -lz -lm -pthread -lstdc++ -lSDL2_mixer -lgmp

# C flags for miniz .c files (no -std=c++17, no -Wextra pedantry on C)
CFLAGS_LINUX       = $(CFLAGS_COMMON) -DLINUX -O2
CFLAGS_LINUX_DEBUG = $(CFLAGS_COMMON) -DLINUX -DDEBUG -g -O0

# ============================================================================
# WINDOWS FLAGS
# ============================================================================
CXXFLAGS_WIN = $(CXXFLAGS_COMMON) \
               $(SDL2_CFLAGS_WIN) $(GLEW_CFLAGS_WIN) $(FREETYPE_CFLAGS_WIN) \
               $(SSH_CFLAGS_WIN) \
               -Iwopr \
               -DWIN32 -D_WIN32 -D_WIN32_WINNT=0x0A00 \
               $(SSH_DEFINE) -O2 -ffunction-sections -fdata-sections -flto

# -mwindows: no console window behind the GL window
# term_pty_win.cpp replaces term_pty.cpp for ConPTY
LDFLAGS_WIN  = $(SDL2_LIBS_WIN) $(GLEW_LIBS_WIN) $(FREETYPE_LIBS_WIN) \
               $(SSH_LIBS_WIN) \
               -lopengl32 -lpng -lz -lwinmm -mwindows -lSDL2_mixer -lshlwapi \
               -s -Wl,--gc-sections -flto

CXXFLAGS_WIN_DEBUG = $(CXXFLAGS_COMMON) \
                     $(SDL2_CFLAGS_WIN) $(GLEW_CFLAGS_WIN) $(FREETYPE_CFLAGS_WIN) \
                     $(SSH_CFLAGS_WIN) \
                     -Iwopr \
                     -DWIN32 -D_WIN32 -D_WIN32_WINNT=0x0A00 \
                     $(SSH_DEFINE) -DDEBUG -g -O0

CFLAGS_WIN         = $(CFLAGS_COMMON) -DWIN32 -D_WIN32 -O2
CFLAGS_WIN_DEBUG   = $(CFLAGS_COMMON) -DWIN32 -D_WIN32 -DDEBUG -g -O0

# ============================================================================
# SOURCES
# ============================================================================
SRCS_COMMON = gl_terminal_main.cpp  gl_renderer.cpp       \
              sdl_renderer.cpp      ft_font.cpp           \
              term_color.cpp        terminal.cpp          \
              term_ui.cpp           gl_bouncingcircle.cpp \
              fight_mode.cpp        crt_audio.cpp         \
              felix_settings.cpp    kitty_graphics.cpp    \
              font_manager.cpp      sftp_overlay.cpp      \
              sftp_console.cpp      image_viewer.cpp      \
              cdg.cpp               $(SSH_SRCS)

SRCS_MINIZ = miniz.c miniz_tdef.c miniz_tinfl.c miniz_zip.c \
            wopr/zork/actors.c \
            wopr/zork/ballop.c \
            wopr/zork/clockr.c \
            wopr/zork/demons.c \
            wopr/zork/dgame.c \
            wopr/zork/dinit.c  \
	    wopr/zork_modified/dmain.c \
	    wopr/zork/dso1.c \
	    wopr/zork/dso2.c \
	    wopr/zork_modified/dso3.c \
	    wopr/zork/dso4.c \
	    wopr/zork/dso5.c \
	    wopr/zork/dso6.c \
	    wopr/zork/dso7.c \
	    wopr/zork_modified/dsub.c \
	    wopr/zork/dverb1.c\
	    wopr/zork/dverb2.c \
	    wopr/zork_modified/gdt.c \
	    wopr/zork/lightp.c \
	    wopr/zork/local.c \
	    wopr/zork/nobjs.c \
	    wopr/zork_modified/np.c \
	    wopr/zork/np1.c \
	    wopr/zork/np2.c \
	    wopr/zork/np3.c \
	    wopr/zork/nrooms.c \
	    wopr/zork/objcts.c \
	    wopr/zork/rooms.c \
	    wopr/zork/sobjs.c \
	    wopr/zork_modified/supp.c \
	    wopr/zork/sverbs.c \
	    wopr/zork/verbs.c \
	    wopr/zork/villns.c \
	    wopr/wopr_basic/commands.c \
            wopr/wopr_basic/display_ansi.c \
            wopr/wopr_basic/expr.c \
            wopr/wopr_basic/program.c \
            wopr/wopr_basic/sound_null.c \
            wopr/wopr_basic/sound_sdl.c \
            wopr/wopr_basic/vars.c \
            wopr/wopr_basic/basic_shim.c \
            wopr/wopr_basic/wopr_main.c


SRCS_WOPR = wopr/wopr.cpp                \
            wopr/wopr_audio.cpp          \
            wopr/wopr_tictactoe.cpp      \
            wopr/wopr_chess.cpp          \
            wopr/wopr_mines.cpp          \
            wopr/wopr_maze.cpp           \
            wopr/wopr_war.cpp            \
            wopr/wopr_zork.cpp           \
            wopr/wopr_basic.cpp          \
            wopr/beatchess.cpp           \
            wopr/chess_ai_move.cpp       \
            wopr/minesweeper_game.cpp    \
            wopr/highscores.cpp            



SRCS_LINUX = $(SRCS_COMMON) $(SRCS_WOPR) term_pty.cpp
SRCS_WIN   = $(SRCS_COMMON) $(SRCS_WOPR) term_pty_win.cpp

# ============================================================================
# OBJECTS
# ============================================================================
OBJECTS_LINUX       = $(addprefix $(BUILD_DIR_LINUX)/,       $(SRCS_LINUX:.cpp=.o)) \
                      $(addprefix $(BUILD_DIR_LINUX)/,        $(SRCS_MINIZ:.c=.o))
OBJECTS_LINUX_DEBUG = $(addprefix $(BUILD_DIR_LINUX_DEBUG)/,  $(SRCS_LINUX:.cpp=.debug.o)) \
                      $(addprefix $(BUILD_DIR_LINUX_DEBUG)/,  $(SRCS_MINIZ:.c=.debug.o))
OBJECTS_WIN         = $(addprefix $(BUILD_DIR_WIN)/,          $(SRCS_WIN:.cpp=.win.o)) \
                      $(addprefix $(BUILD_DIR_WIN)/,           $(SRCS_MINIZ:.c=.win.o))
OBJECTS_WIN_DEBUG   = $(addprefix $(BUILD_DIR_WIN_DEBUG)/,    $(SRCS_WIN:.cpp=.win.debug.o)) \
                      $(addprefix $(BUILD_DIR_WIN_DEBUG)/,    $(SRCS_MINIZ:.c=.win.debug.o))

EXECUTABLE_LINUX       = flt$(SSH_SUFFIX)
EXECUTABLE_LINUX_DEBUG = flt_debug$(SSH_SUFFIX)
EXECUTABLE_WIN         = flt$(SSH_SUFFIX).exe
EXECUTABLE_WIN_DEBUG   = flt_debug$(SSH_SUFFIX).exe

BUILD_DIR             = build
BUILD_DIR_LINUX       = $(BUILD_DIR)/linux
BUILD_DIR_LINUX_DEBUG = $(BUILD_DIR)/linux_debug
BUILD_DIR_WIN         = $(BUILD_DIR)/windows
BUILD_DIR_WIN_DEBUG   = $(BUILD_DIR)/windows_debug

DLL_SOURCE_DIR = /usr/x86_64-w64-mingw32/sys-root/mingw/bin

$(shell mkdir -p \
    $(BUILD_DIR_LINUX) $(BUILD_DIR_LINUX_DEBUG) \
    $(BUILD_DIR_WIN)   $(BUILD_DIR_WIN_DEBUG) \
    $(BUILD_DIR_LINUX)/wopr/zork         $(BUILD_DIR_LINUX)/wopr/zork_modified \
    $(BUILD_DIR_LINUX_DEBUG)/wopr/zork   $(BUILD_DIR_LINUX_DEBUG)/wopr/zork_modified \
    $(BUILD_DIR_WIN)/wopr/zork           $(BUILD_DIR_WIN)/wopr/zork_modified \
    $(BUILD_DIR_WIN_DEBUG)/wopr/zork     $(BUILD_DIR_WIN_DEBUG)/wopr/zork_modified)

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

$(BUILD_DIR_LINUX)/wopr/%.o: wopr/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling (Linux): $<"
	$(CXX_LINUX) $(CXXFLAGS_LINUX) -MMD -MP -c $< -o $@

$(BUILD_DIR_LINUX)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "Compiling C (Linux): $<"
	$(CC_LINUX) $(CFLAGS_LINUX) -MMD -MP -c $< -o $@

$(BUILD_DIR_LINUX)/wopr/%.o: wopr/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling C (Linux): $<"
	$(CC_LINUX) $(CFLAGS_LINUX) -MMD -MP -c $< -o $@

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

$(BUILD_DIR_LINUX_DEBUG)/wopr/%.debug.o: wopr/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling (Linux Debug): $<"
	$(CXX_LINUX) $(CXXFLAGS_LINUX_DEBUG) -MMD -MP -c $< -o $@

$(BUILD_DIR_LINUX_DEBUG)/%.debug.o: %.c
	@mkdir -p $(dir $@)
	@echo "Compiling C (Linux Debug): $<"
	$(CC_LINUX) $(CFLAGS_LINUX_DEBUG) -MMD -MP -c $< -o $@

$(BUILD_DIR_LINUX_DEBUG)/wopr/%.debug.o: wopr/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling C (Linux Debug): $<"
	$(CC_LINUX) $(CFLAGS_LINUX_DEBUG) -MMD -MP -c $< -o $@

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

$(BUILD_DIR_WIN)/wopr/%.win.o: wopr/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling (Windows): $<"
	$(CXX_WIN) $(CXXFLAGS_WIN) -MMD -MP -c $< -o $@

$(BUILD_DIR_WIN)/%.win.o: %.c
	@mkdir -p $(dir $@)
	@echo "Compiling C (Windows): $<"
	$(CC_WIN) $(CFLAGS_WIN) -MMD -MP -c $< -o $@

$(BUILD_DIR_WIN)/wopr/%.win.o: wopr/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling C (Windows): $<"
	$(CC_WIN) $(CFLAGS_WIN) -MMD -MP -c $< -o $@

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

$(BUILD_DIR_WIN_DEBUG)/wopr/%.win.debug.o: wopr/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling (Windows Debug): $<"
	$(CXX_WIN) $(CXXFLAGS_WIN_DEBUG) -MMD -MP -c $< -o $@

$(BUILD_DIR_WIN_DEBUG)/%.win.debug.o: %.c
	@mkdir -p $(dir $@)
	@echo "Compiling C (Windows Debug): $<"
	$(CC_WIN) $(CFLAGS_WIN_DEBUG) -MMD -MP -c $< -o $@

$(BUILD_DIR_WIN_DEBUG)/wopr/%.win.debug.o: wopr/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling C (Windows Debug): $<"
	$(CC_WIN) $(CFLAGS_WIN_DEBUG) -MMD -MP -c $< -o $@

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
		if [ "$(SSH)" = "1" ]; then \
			echo "  (SSH build) libssh2.dll, libssl-*.dll, libcrypto-*.dll"; \
		fi \
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
	@$(PKG_CONFIG_LINUX) --exists libssh2   && echo "✓ libssh2"   || echo "✗ libssh2 (optional, needed for SSH=1)"
	@echo "=== Windows (mingw64) ==="
	@$(PKG_CONFIG_WIN) --exists freetype2 && echo "✓ freetype2" || echo "✗ freetype2"
	@$(PKG_CONFIG_WIN) --exists sdl2      && echo "✓ sdl2"      || echo "✗ sdl2"
	@$(PKG_CONFIG_WIN) --exists glew      && echo "✓ glew"      || echo "✗ glew"
	@$(PKG_CONFIG_WIN) --exists libssh2   && echo "✓ libssh2"   || echo "✗ libssh2 (optional, needed for SSH=1)"

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
	@echo ""
	@echo "SSH Linux deps:   sudo apt install libssh2-1-dev libssl-dev"
	@echo "                  sudo dnf install libssh2-devel openssl-devel"
	@echo "SSH Windows deps: mingw64-libssh2 mingw64-openssl (Fedora)"
	@echo "                  or build from source via mxe.cc"
