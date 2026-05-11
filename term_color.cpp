#include "term_color.h"
#include <string.h>

// ============================================================================
// PALETTE DATA
// ============================================================================

const float PAL_GNOME_TERMINAL[16][3] = {
    // normal
    {0.090f, 0.078f, 0.129f},  // black   #171421
    {0.753f, 0.110f, 0.157f},  // red     #c01c28
    {0.149f, 0.635f, 0.412f},  // green   #26a269
    {0.635f, 0.451f, 0.298f},  // yellow  #a2734c
    {0.071f, 0.282f, 0.545f},  // blue    #12488b
    {0.639f, 0.278f, 0.729f},  // magenta #a347ba
    {0.165f, 0.631f, 0.702f},  // cyan    #2aa1b3
    {0.816f, 0.812f, 0.800f},  // white   #d0cfcc

    // bright
    {0.369f, 0.361f, 0.392f},  // bright black  #5e5c64
    {0.965f, 0.380f, 0.318f},  // bright red    #f66151
    {0.200f, 0.820f, 0.478f},  // bright green  #33d17a
    {0.914f, 0.678f, 0.047f},  // bright yellow #e9ad0c
    {0.165f, 0.482f, 0.871f},  // bright blue   #2a7bde
    {0.753f, 0.380f, 0.796f},  // bright magenta#c061cb
    {0.200f, 0.780f, 0.871f},  // bright cyan   #33c7de
    {1.000f, 1.000f, 1.000f},  // bright white  #ffffff
};

const float PAL_LEGACY[16][3] = {
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

const float PAL_DEFAULT[16][3] = {
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

const float PAL_SOLARIZED[16][3] = {
    {0.027f, 0.212f, 0.259f},
    {0.863f, 0.196f, 0.184f},
    {0.522f, 0.600f, 0.000f},
    {0.710f, 0.537f, 0.000f},
    {0.149f, 0.545f, 0.824f},
    {0.827f, 0.212f, 0.510f},
    {0.165f, 0.631f, 0.596f},
    {0.933f, 0.910f, 0.835f},
    {0.000f, 0.169f, 0.212f},
    {0.796f, 0.294f, 0.086f},
    {0.345f, 0.431f, 0.459f},
    {0.396f, 0.482f, 0.514f},
    {0.514f, 0.580f, 0.588f},
    {0.424f, 0.443f, 0.769f},
    {0.576f, 0.631f, 0.631f},
    {0.992f, 0.965f, 0.890f},
};

const float PAL_MONOKAI[16][3] = {
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

const float PAL_NORD[16][3] = {
    {0.180f, 0.204f, 0.251f},
    {0.749f, 0.380f, 0.416f},
    {0.639f, 0.745f, 0.549f},
    {0.922f, 0.796f, 0.545f},
    {0.506f, 0.631f, 0.757f},
    {0.706f, 0.557f, 0.678f},
    {0.533f, 0.753f, 0.816f},
    {0.898f, 0.914f, 0.941f},
    {0.298f, 0.337f, 0.416f},
    {0.749f, 0.380f, 0.416f},
    {0.639f, 0.745f, 0.549f},
    {0.922f, 0.796f, 0.545f},
    {0.506f, 0.631f, 0.757f},
    {0.706f, 0.557f, 0.678f},
    {0.533f, 0.753f, 0.816f},
    {0.925f, 0.937f, 0.957f},
};

const float PAL_GRUVBOX[16][3] = {
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

const float PAL_DRACULA[16][3] = {
    {0.098f, 0.098f, 0.149f},  // black
    {1.000f, 0.286f, 0.408f},  // red
    {0.529f, 0.949f, 0.451f},  // green
    {1.000f, 0.949f, 0.369f},  // yellow
    {0.451f, 0.682f, 0.996f},  // blue
    {1.000f, 0.537f, 0.804f},  // magenta
    {0.404f, 0.996f, 0.918f},  // cyan
    {0.961f, 0.961f, 0.949f},  // white
    {0.294f, 0.306f, 0.388f},  // bright black
    {1.000f, 0.286f, 0.408f},  // bright red
    {0.529f, 0.949f, 0.451f},  // bright green
    {1.000f, 0.949f, 0.369f},  // bright yellow
    {0.451f, 0.682f, 0.996f},  // bright blue
    {1.000f, 0.537f, 0.804f},  // bright magenta
    {0.404f, 0.996f, 0.918f},  // bright cyan
    {1.000f, 1.000f, 1.000f},  // bright white
};

const float PAL_ONE_DARK[16][3] = {
    {0.098f, 0.102f, 0.122f},  // black
    {0.859f, 0.290f, 0.310f},  // red
    {0.514f, 0.733f, 0.357f},  // green
    {0.949f, 0.733f, 0.302f},  // yellow
    {0.341f, 0.565f, 0.914f},  // blue
    {0.812f, 0.459f, 0.761f},  // magenta
    {0.365f, 0.820f, 0.769f},  // cyan
    {0.929f, 0.929f, 0.929f},  // white
    {0.267f, 0.275f, 0.302f},  // bright black
    {0.918f, 0.439f, 0.459f},  // bright red
    {0.608f, 0.816f, 0.451f},  // bright green
    {0.980f, 0.816f, 0.400f},  // bright yellow
    {0.467f, 0.651f, 0.957f},  // bright blue
    {0.871f, 0.565f, 0.820f},  // bright magenta
    {0.502f, 0.886f, 0.839f},  // bright cyan
    {1.000f, 1.000f, 1.000f},  // bright white
};

const float PAL_CATPPUCCIN[16][3] = {
    {0.161f, 0.145f, 0.204f},  // black
    {0.969f, 0.235f, 0.341f},  // red
    {0.482f, 0.839f, 0.373f},  // green
    {0.965f, 0.745f, 0.235f},  // yellow
    {0.400f, 0.596f, 1.000f},  // blue
    {0.855f, 0.408f, 0.839f},  // magenta
    {0.373f, 0.906f, 0.906f},  // cyan
    {0.925f, 0.925f, 0.925f},  // white
    {0.369f, 0.345f, 0.447f},  // bright black
    {1.000f, 0.400f, 0.486f},  // bright red
    {0.608f, 0.894f, 0.486f},  // bright green
    {1.000f, 0.855f, 0.388f},  // bright yellow
    {0.565f, 0.710f, 1.000f},  // bright blue
    {0.914f, 0.553f, 0.902f},  // bright magenta
    {0.529f, 0.949f, 0.949f},  // bright cyan
    {1.000f, 1.000f, 1.000f},  // bright white
};

const float PAL_TOKYO_NIGHT[16][3] = {
    {0.090f, 0.086f, 0.129f},  // black
    {0.949f, 0.337f, 0.384f},  // red
    {0.533f, 0.859f, 0.431f},  // green
    {0.992f, 0.761f, 0.298f},  // yellow
    {0.380f, 0.631f, 0.969f},  // blue
    {0.820f, 0.478f, 0.898f},  // magenta
    {0.341f, 0.871f, 0.902f},  // cyan
    {0.922f, 0.929f, 0.957f},  // white
    {0.278f, 0.278f, 0.306f},  // bright black
    {1.000f, 0.486f, 0.518f},  // bright red
    {0.667f, 0.914f, 0.565f},  // bright green
    {1.000f, 0.859f, 0.439f},  // bright yellow
    {0.533f, 0.745f, 0.992f},  // bright blue
    {0.914f, 0.616f, 0.941f},  // bright magenta
    {0.494f, 0.922f, 0.941f},  // bright cyan
    {1.000f, 1.000f, 1.000f},  // bright white
};

const float PAL_EVERFOREST[16][3] = {
    {0.178f, 0.192f, 0.161f},  // black
    {0.831f, 0.376f, 0.357f},  // red
    {0.549f, 0.659f, 0.439f},  // green
    {0.957f, 0.714f, 0.439f},  // yellow
    {0.400f, 0.596f, 0.514f},  // blue
    {0.741f, 0.604f, 0.741f},  // magenta
    {0.424f, 0.749f, 0.647f},  // cyan
    {0.918f, 0.914f, 0.871f},  // white
    {0.373f, 0.392f, 0.357f},  // bright black
    {0.957f, 0.514f, 0.486f},  // bright red
    {0.682f, 0.761f, 0.573f},  // bright green
    {1.000f, 0.824f, 0.565f},  // bright yellow
    {0.561f, 0.710f, 0.647f},  // bright blue
    {0.847f, 0.725f, 0.847f},  // bright magenta
    {0.565f, 0.855f, 0.765f},  // bright cyan
    {1.000f, 1.000f, 1.000f},  // bright white
};

const float PAL_CYBERPUNK[16][3] = {
    {0.039f, 0.039f, 0.039f},  // black
    {1.000f, 0.149f, 0.522f},  // red (neon pink)
    {0.000f, 1.000f, 0.757f},  // green (neon cyan)
    {1.000f, 0.843f, 0.000f},  // yellow (neon yellow)
    {0.000f, 0.760f, 1.000f},  // blue (neon blue)
    {1.000f, 0.149f, 0.522f},  // magenta (neon pink)
    {0.000f, 1.000f, 0.757f},  // cyan (neon cyan)
    {0.922f, 0.922f, 0.922f},  // white
    {0.216f, 0.216f, 0.216f},  // bright black
    {1.000f, 0.314f, 0.671f},  // bright red
    {0.314f, 1.000f, 0.878f},  // bright green
    {1.000f, 0.922f, 0.216f},  // bright yellow
    {0.314f, 0.886f, 1.000f},  // bright blue
    {1.000f, 0.400f, 0.671f},  // bright magenta
    {0.314f, 1.000f, 0.878f},  // bright cyan
    {1.000f, 1.000f, 1.000f},  // bright white
};

const float PAL_AYU_MIRAGE[16][3] = {
    {0.125f, 0.149f, 0.188f},  // black
    {0.937f, 0.376f, 0.388f},  // red
    {0.576f, 0.812f, 0.522f},  // green
    {0.969f, 0.776f, 0.290f},  // yellow
    {0.369f, 0.659f, 0.945f},  // blue
    {0.839f, 0.514f, 0.894f},  // magenta
    {0.376f, 0.859f, 0.839f},  // cyan
    {0.933f, 0.929f, 0.918f},  // white
    {0.298f, 0.329f, 0.384f},  // bright black
    {1.000f, 0.498f, 0.510f},  // bright red
    {0.706f, 0.914f, 0.651f},  // bright green
    {1.000f, 0.878f, 0.431f},  // bright yellow
    {0.510f, 0.776f, 0.973f},  // bright blue
    {0.933f, 0.639f, 0.945f},  // bright magenta
    {0.510f, 0.945f, 0.933f},  // bright cyan
    {1.000f, 1.000f, 1.000f},  // bright white
};

const float PAL_ROSE_PINE[16][3] = {
    {0.110f, 0.102f, 0.129f},  // black
    {0.922f, 0.404f, 0.459f},  // red
    {0.545f, 0.816f, 0.525f},  // green
    {0.969f, 0.788f, 0.478f},  // yellow
    {0.396f, 0.608f, 0.890f},  // blue
    {0.804f, 0.518f, 0.835f},  // magenta
    {0.341f, 0.827f, 0.820f},  // cyan
    {0.925f, 0.922f, 0.914f},  // white
    {0.341f, 0.318f, 0.373f},  // bright black
    {1.000f, 0.533f, 0.588f},  // bright red
    {0.671f, 0.902f, 0.655f},  // bright green
    {1.000f, 0.890f, 0.608f},  // bright yellow
    {0.529f, 0.729f, 0.929f},  // bright blue
    {0.902f, 0.647f, 0.918f},  // bright magenta
    {0.471f, 0.918f, 0.910f},  // bright cyan
    {1.000f, 1.000f, 1.000f},  // bright white
};

const Theme THEMES[] = {
    { "Default",         0.078f, 0.078f, 0.118f, PAL_GNOME_TERMINAL },
    { "Classic",         0.0f,   0.0f,   0.0f,   PAL_LEGACY },
    { "Solarized Dark",  0.000f, 0.169f, 0.212f, PAL_SOLARIZED },
    { "Monokai",         0.117f, 0.117f, 0.117f, PAL_MONOKAI },
    { "Nord",            0.180f, 0.204f, 0.251f, PAL_NORD },
    { "Gruvbox",         0.157f, 0.157f, 0.157f, PAL_GRUVBOX },
    { "Dracula",         0.098f, 0.098f, 0.149f, PAL_DRACULA },
    { "One Dark",        0.098f, 0.102f, 0.122f, PAL_ONE_DARK },
    { "Catppuccin",      0.161f, 0.145f, 0.204f, PAL_CATPPUCCIN },
    { "Tokyo Night",     0.090f, 0.086f, 0.129f, PAL_TOKYO_NIGHT },
    { "Everforest",      0.178f, 0.192f, 0.161f, PAL_EVERFOREST },
    { "Cyberpunk",       0.039f, 0.039f, 0.039f, PAL_CYBERPUNK },
    { "Ayu Mirage",      0.125f, 0.149f, 0.188f, PAL_AYU_MIRAGE },
    { "Rose Pine",       0.110f, 0.102f, 0.129f, PAL_ROSE_PINE },
    { "Matrix",          0.f,    0.05f,  0.f,    nullptr },
    { "Ocean",           0.047f, 0.082f, 0.133f, nullptr },
};

const int THEME_COUNT = (int)(sizeof(THEMES)/sizeof(THEMES[0]));

// ============================================================================
// GLOBALS
// ============================================================================

int   g_theme_idx  = 0;
float g_palette16[16][3];

// ============================================================================
// APPLY THEME
// ============================================================================

void apply_theme(int idx) {
    if (idx < 0 || idx >= THEME_COUNT) return;
    g_theme_idx = idx;
    const Theme &th = THEMES[idx];
    if (!th.palette) {
        if (strcmp(th.name, "Matrix") == 0) {
            memcpy(g_palette16, PAL_GNOME_TERMINAL, sizeof(g_palette16));
            g_palette16[2][0]=0.f; g_palette16[2][1]=1.f; g_palette16[2][2]=0.f;
            g_palette16[7][0]=0.f; g_palette16[7][1]=0.9f; g_palette16[7][2]=0.f;
        } else if (strcmp(th.name, "Ocean") == 0) {
            memcpy(g_palette16, PAL_GNOME_TERMINAL, sizeof(g_palette16));
            g_palette16[4][0]=0.4f; g_palette16[4][1]=0.7f; g_palette16[4][2]=1.0f;
            g_palette16[6][0]=0.4f; g_palette16[6][1]=0.9f; g_palette16[6][2]=1.0f;
            g_palette16[7][0]=0.85f;g_palette16[7][1]=0.92f;g_palette16[7][2]=1.0f;
        } else {
            memcpy(g_palette16, PAL_GNOME_TERMINAL, sizeof(g_palette16));
        }
    } else {
        memcpy(g_palette16, th.palette, sizeof(g_palette16));
    }
}

// ============================================================================
// COLOR RESOLUTION
// ============================================================================

TermColor tcolor_resolve(TermColorVal c) {
    if (TCOLOR_IS_RGB(c))
        return { TCOLOR_R(c)/255.f, TCOLOR_G(c)/255.f, TCOLOR_B(c)/255.f };
    int idx = (int)TCOLOR_IDX(c);
    if (idx < 16) return { g_palette16[idx][0], g_palette16[idx][1], g_palette16[idx][2] };
    if (idx < 232) {
        int i = idx - 16;
        int b = i % 6, g = (i/6) % 6, r = i/36;
        auto cv = [](int v) { return v ? (55 + v*40)/255.f : 0.f; };
        return { cv(r), cv(g), cv(b) };
    }
    float v = (8 + (idx-232)*10) / 255.f;
    return { v, v, v };
}
