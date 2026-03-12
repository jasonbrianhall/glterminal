#include <GL/glew.h>
#include <GL/gl.h>
#include <SDL2/SDL.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <pty.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <vector>
#include <unordered_map>

// ============================================================================
// CONFIG
// ============================================================================

#define TERM_COLS_DEFAULT  80
#define TERM_ROWS_DEFAULT  24
#define TERM_MAX_COLS      512
#define TERM_MAX_ROWS      256
#define SCROLLBACK_LINES   5000
#define FONT_SIZE_DEFAULT  16
#define FONT_SIZE_MIN      6
#define FONT_SIZE_MAX     72
#define WIN_TITLE       "GL Terminal"
#define MAX_VERTS 400000

// ============================================================================
// TERMINAL DATA
// ============================================================================

#define ATTR_BOLD      (1<<0)
#define ATTR_UNDERLINE (1<<1)
#define ATTR_REVERSE   (1<<2)
#define ATTR_BLINK     (1<<3)

static int g_font_size = FONT_SIZE_DEFAULT;

// ============================================================================
// THEME + TRANSPARENCY
// ============================================================================

struct Theme {
    const char *name;
    float bg_r, bg_g, bg_b;        // terminal background
    // 16-colour palette overrides (NULL = use built-in defaults)
    const float (*palette)[3];      // [16][3], or nullptr
};

static const float PAL_DEFAULT[16][3] = {
    {0.f,    0.f,    0.f},
    {0.8f,   0.1f,   0.1f},
    {0.1f,   0.8f,   0.1f},
    {0.8f,   0.8f,   0.1f},
    {0.2f,   0.2f,   0.9f},
    {0.8f,   0.1f,   0.8f},
    {0.1f,   0.8f,   0.8f},
    {0.75f,  0.75f,  0.75f},
    {0.4f,   0.4f,   0.4f},
    {1.f,    0.3f,   0.3f},
    {0.3f,   1.f,    0.3f},
    {1.f,    1.f,    0.3f},
    {0.3f,   0.4f,   1.f},
    {1.f,    0.3f,   1.f},
    {0.3f,   1.f,    1.f},
    {1.f,    1.f,    1.f},
};
static const float PAL_SOLARIZED[16][3] = {
    {0.027f, 0.212f, 0.259f},  // base03  (black)
    {0.863f, 0.196f, 0.184f},  // red
    {0.522f, 0.600f, 0.000f},  // green
    {0.710f, 0.537f, 0.000f},  // yellow
    {0.149f, 0.545f, 0.824f},  // blue
    {0.827f, 0.212f, 0.510f},  // magenta
    {0.165f, 0.631f, 0.596f},  // cyan
    {0.933f, 0.910f, 0.835f},  // base2 (white)
    {0.000f, 0.169f, 0.212f},  // base02 (br black)
    {0.796f, 0.294f, 0.086f},  // orange (br red)
    {0.345f, 0.431f, 0.459f},  // base01 (br green)
    {0.396f, 0.482f, 0.514f},  // base00 (br yellow)
    {0.514f, 0.580f, 0.588f},  // base0  (br blue)
    {0.424f, 0.443f, 0.769f},  // violet (br magenta)
    {0.576f, 0.631f, 0.631f},  // base1  (br cyan)
    {0.992f, 0.965f, 0.890f},  // base3  (br white)
};
static const float PAL_MONOKAI[16][3] = {
    {0.117f, 0.117f, 0.117f},
    {0.980f, 0.145f, 0.227f},
    {0.639f, 0.878f, 0.176f},
    {0.902f, 0.682f, 0.188f},
    {0.396f, 0.675f, 1.000f},
    {0.678f, 0.506f, 1.000f},
    {0.396f, 0.835f, 0.969f},
    {0.925f, 0.925f, 0.925f},
    {0.498f, 0.498f, 0.498f},
    {0.980f, 0.145f, 0.227f},
    {0.639f, 0.878f, 0.176f},
    {0.902f, 0.682f, 0.188f},
    {0.396f, 0.675f, 1.000f},
    {0.678f, 0.506f, 1.000f},
    {0.396f, 0.835f, 0.969f},
    {1.000f, 1.000f, 1.000f},
};
static const float PAL_NORD[16][3] = {
    {0.180f, 0.204f, 0.251f},  // nord0
    {0.749f, 0.380f, 0.416f},  // nord11 red
    {0.639f, 0.745f, 0.549f},  // nord14 green
    {0.922f, 0.796f, 0.545f},  // nord13 yellow
    {0.506f, 0.631f, 0.757f},  // nord9  blue
    {0.706f, 0.557f, 0.678f},  // nord15 magenta
    {0.533f, 0.753f, 0.816f},  // nord8  cyan
    {0.898f, 0.914f, 0.941f},  // nord6  white
    {0.298f, 0.337f, 0.416f},  // nord3
    {0.749f, 0.380f, 0.416f},
    {0.639f, 0.745f, 0.549f},
    {0.922f, 0.796f, 0.545f},
    {0.506f, 0.631f, 0.757f},
    {0.706f, 0.557f, 0.678f},
    {0.533f, 0.753f, 0.816f},
    {0.925f, 0.937f, 0.957f},
};
static const float PAL_GRUVBOX[16][3] = {
    {0.157f, 0.157f, 0.157f},
    {0.800f, 0.141f, 0.114f},
    {0.596f, 0.592f, 0.102f},
    {0.843f, 0.600f, 0.129f},
    {0.271f, 0.522f, 0.533f},
    {0.694f, 0.384f, 0.525f},
    {0.408f, 0.616f, 0.416f},
    {0.922f, 0.859f, 0.698f},
    {0.573f, 0.514f, 0.451f},
    {0.984f, 0.286f, 0.204f},
    {0.722f, 0.733f, 0.149f},
    {0.980f, 0.741f, 0.184f},
    {0.514f, 0.647f, 0.596f},
    {0.827f, 0.525f, 0.608f},
    {0.557f, 0.753f, 0.486f},
    {0.922f, 0.859f, 0.698f},
};

static const Theme THEMES[] = {
    { "Default",         0.04f,  0.04f,  0.08f,  PAL_DEFAULT   },
    { "Solarized Dark",  0.000f, 0.169f, 0.212f, PAL_SOLARIZED },
    { "Monokai",         0.117f, 0.117f, 0.117f, PAL_MONOKAI   },
    { "Nord",            0.180f, 0.204f, 0.251f, PAL_NORD      },
    { "Gruvbox",         0.157f, 0.157f, 0.157f, PAL_GRUVBOX   },
    { "Matrix",          0.f,    0.05f,  0.f,    nullptr       },
    { "Ocean",           0.047f, 0.082f, 0.133f, nullptr       },
};
static const int THEME_COUNT = (int)(sizeof(THEMES)/sizeof(THEMES[0]));

// ============================================================================
// VERTEX / GL STATE  (same pattern as cometbuster_render_gl.cpp)
// ============================================================================

typedef struct { float x, y, r, g, b, a; } Vertex;

typedef struct {
    float m[16];
} Mat4;

static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 pos;\n"
    "layout(location=1) in vec4 col;\n"
    "uniform mat4 proj;\n"
    "out vec4 vCol;\n"
    "void main(){gl_Position=proj*vec4(pos,0,1);vCol=col;}\n";

static const char *FS =
    "#version 330 core\n"
    "in vec4 vCol;\n"
    "out vec4 frag;\n"
    "void main(){frag=vCol;}\n";

struct GLState {
    GLuint prog, vao, vbo;
    GLint  proj_loc;
    Mat4   proj;
    float  cr, cg, cb, ca;   // current colour
};
static GLState G = {};

