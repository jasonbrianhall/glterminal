#pragma once
#include <stdint.h>
#include <string.h>

// ============================================================================
// COLOR VALUE
// High byte 0x00 = palette index in low byte
// High byte 0x01 = 24-bit RGB in low 3 bytes
// ============================================================================

typedef uint32_t TermColorVal;
#define TCOLOR_PALETTE(idx)    ((TermColorVal)(idx))
#define TCOLOR_RGB(r,g,b)      ((TermColorVal)(0x01000000u | ((r)<<16) | ((g)<<8) | (b)))
#define TCOLOR_IS_RGB(c)       (((c) & 0xFF000000u) == 0x01000000u)
#define TCOLOR_R(c)            (((c)>>16)&0xFF)
#define TCOLOR_G(c)            (((c)>>8)&0xFF)
#define TCOLOR_B(c)            ((c)&0xFF)
#define TCOLOR_IDX(c)          ((c)&0xFF)

struct TermColor { float r, g, b; };

// Active resolved palette — written by apply_theme(), read by tcolor_resolve()
extern float g_palette16[16][3];

TermColor tcolor_resolve(TermColorVal c);

// ============================================================================
// THEME
// ============================================================================

struct Theme {
    const char *name;
    float bg_r, bg_g, bg_b;
    const float (*palette)[3];  // [16][3], or nullptr = use PAL_DEFAULT
};

extern const float PAL_DEFAULT[16][3];
extern const float PAL_SOLARIZED[16][3];
extern const float PAL_MONOKAI[16][3];
extern const float PAL_NORD[16][3];
extern const float PAL_GRUVBOX[16][3];

extern const Theme THEMES[];
extern const int   THEME_COUNT;

extern int g_theme_idx;

void apply_theme(int idx);
