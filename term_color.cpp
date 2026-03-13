#include "term_color.h"
#include <string.h>

// ============================================================================
// PALETTE DATA
// ============================================================================

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

const Theme THEMES[] = {
    { "Default",         0.04f,  0.04f,  0.08f,  PAL_DEFAULT   },
    { "Solarized Dark",  0.000f, 0.169f, 0.212f, PAL_SOLARIZED },
    { "Monokai",         0.117f, 0.117f, 0.117f, PAL_MONOKAI   },
    { "Nord",            0.180f, 0.204f, 0.251f, PAL_NORD      },
    { "Gruvbox",         0.157f, 0.157f, 0.157f, PAL_GRUVBOX   },
    { "Matrix",          0.f,    0.05f,  0.f,    nullptr       },
    { "Ocean",           0.047f, 0.082f, 0.133f, nullptr       },
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
            memcpy(g_palette16, PAL_DEFAULT, sizeof(g_palette16));
            g_palette16[2][0]=0.f; g_palette16[2][1]=1.f; g_palette16[2][2]=0.f;
            g_palette16[7][0]=0.f; g_palette16[7][1]=0.9f; g_palette16[7][2]=0.f;
        } else if (strcmp(th.name, "Ocean") == 0) {
            memcpy(g_palette16, PAL_DEFAULT, sizeof(g_palette16));
            g_palette16[4][0]=0.4f; g_palette16[4][1]=0.7f; g_palette16[4][2]=1.0f;
            g_palette16[6][0]=0.4f; g_palette16[6][1]=0.9f; g_palette16[6][2]=1.0f;
            g_palette16[7][0]=0.85f;g_palette16[7][1]=0.92f;g_palette16[7][2]=1.0f;
        } else {
            memcpy(g_palette16, PAL_DEFAULT, sizeof(g_palette16));
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
