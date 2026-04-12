#pragma once
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdint.h>
#include <vector>
#include "gl_renderer.h"  // Vertex, GlyphVertex

// ============================================================================
// FREETYPE STATE
// ============================================================================

extern FT_Library s_ft_lib;
extern FT_Face    s_ft_face;      // Bold
extern FT_Face    s_ft_face_reg;  // Regular
extern FT_Face    s_ft_face_obl;  // Oblique
extern FT_Face    s_ft_face_bobl; // BoldOblique
extern FT_Face    s_emoji_face;
extern FT_Face    s_symbols_face;

// ============================================================================
// API
// ============================================================================

void    ft_init(void);
void    ft_shutdown(void);
void    ft_reload_embedded(void);   // reloads faces AND clears glyph cache

// Create a standalone DejaVu Regular face at a fixed pixel size for UI use.
// Caller must free(face->generic.data) and FT_Done_Face(face) on shutdown.
FT_Face ft_make_menu_face(int pixel_size);

// Call when font size changes so the atlas is repopulated at the new size.
void    ft_invalidate_glyph_cache(void);

// Font buffer pointers — non-static so font_manager can free/replace them
extern unsigned char *s_font_buf;
extern unsigned char *s_font_buf_reg;
extern unsigned char *s_font_buf_obl;
extern unsigned char *s_font_buf_bobl;

// UTF-8 / codepoint utilities
uint32_t next_codepoint(const unsigned char **p);
bool     is_emoji_codepoint(uint32_t cp);
int      cp_to_utf8(uint32_t cp, char *buf);  // buf must be >= 5 bytes

// Glyph / text rendering
FT_Face face_for_attrs(uint8_t attrs);

// draw_glyph: atlas-backed; legacy `verts` param accepted but ignored.
// Prefer draw_text for bulk rendering.
float   draw_glyph(FT_Face face, uint32_t cp, float cx, float baseline_y,
                   int font_px, int emoji_px, float r, float g, float b, float a,
                   std::vector<Vertex> &verts);

float   draw_text(const char *text, float x, float y, int font_px, int emoji_px,
                  float r, float g, float b, float a, uint8_t attrs = 0);
float   measure_text(const char *text, int font_px);
