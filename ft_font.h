#pragma once
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdint.h>
#include <vector>
#include "gl_renderer.h"  // Vertex

// ============================================================================
// FREETYPE STATE
// ============================================================================

extern FT_Library s_ft_lib;
extern FT_Face    s_ft_face;      // Bold
extern FT_Face    s_ft_face_reg;  // Regular
extern FT_Face    s_ft_face_obl;  // Oblique
extern FT_Face    s_ft_face_bobl; // BoldOblique
extern FT_Face    s_emoji_face;

// ============================================================================
// API
// ============================================================================

void    ft_init(void);
void    ft_shutdown(void);

// UTF-8 / codepoint utilities
uint32_t next_codepoint(const unsigned char **p);
bool     is_emoji_codepoint(uint32_t cp);
int      cp_to_utf8(uint32_t cp, char *buf);  // buf must be >= 5 bytes

// Glyph / text rendering
FT_Face face_for_attrs(uint8_t attrs);
float   draw_glyph(FT_Face face, uint32_t cp, float cx, float baseline_y,
                   int font_px, int emoji_px, float r, float g, float b, float a,
                   std::vector<Vertex> &verts);
float   draw_text(const char *text, float x, float y, int font_px, int emoji_px,
                  float r, float g, float b, float a, uint8_t attrs = 0);
float   measure_text(const char *text, int font_px);
