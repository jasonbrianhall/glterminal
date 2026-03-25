#include "ft_font.h"
#include "gl_terminal.h"   // ATTR_BOLD, ATTR_ITALIC

// Embedded font headers
#include "DejaVuMonoBold.h"
#include "NotoEmoji.h"
#include "DejaVuMono.h"
#include "DejaVuMonoOblique.h"
#include "DejaVuMonoBoldOblique.h"

#include <SDL2/SDL.h>
#include <math.h>
#include <stdlib.h>

// ============================================================================
// GLOBALS
// ============================================================================

FT_Library s_ft_lib       = NULL;
FT_Face    s_ft_face      = NULL;
FT_Face    s_ft_face_reg  = NULL;
FT_Face    s_ft_face_obl  = NULL;
FT_Face    s_ft_face_bobl = NULL;
FT_Face    s_emoji_face   = NULL;

unsigned char *s_font_buf      = NULL;
unsigned char *s_font_buf_reg  = NULL;
unsigned char *s_font_buf_obl  = NULL;
unsigned char *s_font_buf_bobl = NULL;
static unsigned char *s_emoji_buf     = NULL;

// g_font_size is owned by app_globals.h; ft_font reads it via extern
extern int g_font_size;

// ============================================================================
// INIT
// ============================================================================

void ft_init(void) {
    FT_Init_FreeType(&s_ft_lib);

    size_t decoded_size = 0;
    int r = base64_decode(MONOSPACE_FONT_B64, MONOSPACE_FONT_B64_SIZE, &s_font_buf, &decoded_size);
    if (r != 0 || !s_font_buf) {
        //SDL_Log("[Font] base64 decode failed\n");
        return;
    }

    FT_New_Memory_Face(s_ft_lib, s_font_buf, (FT_Long)decoded_size, 0, &s_ft_face);
    SDL_Log("[Font] loaded: %s %s\n", s_ft_face->family_name, s_ft_face->style_name);

    auto load_variant = [&](const char *b64, size_t b64_size,
                            unsigned char **buf, FT_Face *face) {
        size_t sz = 0;
        if (base64_decode(b64, b64_size, buf, &sz) == 0 && *buf) {
            if (FT_New_Memory_Face(s_ft_lib, *buf, (FT_Long)sz, 0, face) == 0)
                SDL_Log("[Font] loaded: %s %s\n", (*face)->family_name, (*face)->style_name);
            else { free(*buf); *buf = nullptr; }
        }
    };
    load_variant(DEJAVU_REGULAR_FONT_B64,     DEJAVU_REGULAR_FONT_B64_SIZE,     &s_font_buf_reg,  &s_ft_face_reg);
    load_variant(DEJAVU_OBLIQUE_FONT_B64,     DEJAVU_OBLIQUE_FONT_B64_SIZE,     &s_font_buf_obl,  &s_ft_face_obl);
    load_variant(DEJAVU_BOLDOBLIQUE_FONT_B64, DEJAVU_BOLDOBLIQUE_FONT_B64_SIZE, &s_font_buf_bobl, &s_ft_face_bobl);

    size_t emoji_decoded_size = 0;
    int er = base64_decode(NOTOEMOJI_FONT_B64, NOTOEMOJI_FONT_B64_SIZE, &s_emoji_buf, &emoji_decoded_size);
    if (er == 0 && s_emoji_buf) {
        if (FT_New_Memory_Face(s_ft_lib, s_emoji_buf, (FT_Long)emoji_decoded_size, 0, &s_emoji_face) == 0)
            SDL_Log("[Font] emoji: %s %s\n", s_emoji_face->family_name, s_emoji_face->style_name);
        else {
            SDL_Log("[Font] emoji face load failed\n");
            free(s_emoji_buf); s_emoji_buf = nullptr;
        }
    } else {
        SDL_Log("[Font] emoji base64 decode failed\n");
    }
}

void ft_shutdown(void) {
    if (s_emoji_face)    FT_Done_Face(s_emoji_face);
    if (s_ft_face_bobl)  FT_Done_Face(s_ft_face_bobl);
    if (s_ft_face_obl)   FT_Done_Face(s_ft_face_obl);
    if (s_ft_face_reg)   FT_Done_Face(s_ft_face_reg);
    if (s_ft_face)       FT_Done_Face(s_ft_face);
    if (s_ft_lib)        FT_Done_FreeType(s_ft_lib);
    if (s_emoji_buf)     free(s_emoji_buf);
    if (s_font_buf_bobl) free(s_font_buf_bobl);
    if (s_font_buf_obl)  free(s_font_buf_obl);
    if (s_font_buf_reg)  free(s_font_buf_reg);
    if (s_font_buf)      free(s_font_buf);
}

// Creates a standalone DejaVu Regular face for fixed-size UI rendering.
// Decodes the font into its own buffer so it is unaffected by font switching.
FT_Face ft_make_menu_face(int pixel_size) {
    if (!s_ft_lib) return nullptr;
    unsigned char *buf = nullptr;
    size_t sz = 0;
    if (base64_decode(DEJAVU_REGULAR_FONT_B64, DEJAVU_REGULAR_FONT_B64_SIZE, &buf, &sz) != 0 || !buf)
        return nullptr;
    FT_Face face = nullptr;
    if (FT_New_Memory_Face(s_ft_lib, buf, (FT_Long)sz, 0, &face) != 0) {
        free(buf);
        return nullptr;
    }
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)pixel_size);
    // Store the buffer pointer in the face's generic field so we can free it later.
    // Caller frees via:  free(face->generic.data); FT_Done_Face(face);
    face->generic.data      = buf;
    face->generic.finalizer = [](void *obj) {
        FT_Face f = (FT_Face)obj;
        if (f->generic.data) { free(f->generic.data); f->generic.data = nullptr; }
    };
    return face;
}

void ft_reload_embedded(void) {
    auto free_face = [](FT_Face *face, unsigned char **buf) {
        if (*face) { FT_Done_Face(*face); *face = nullptr; }
        if (*buf)  { free(*buf); *buf = nullptr; }
    };
    free_face(&s_ft_face,      &s_font_buf);
    free_face(&s_ft_face_reg,  &s_font_buf_reg);
    free_face(&s_ft_face_obl,  &s_font_buf_obl);
    free_face(&s_ft_face_bobl, &s_font_buf_bobl);

    size_t decoded_size = 0;
    if (base64_decode(MONOSPACE_FONT_B64, MONOSPACE_FONT_B64_SIZE,
                      &s_font_buf, &decoded_size) == 0 && s_font_buf)
        FT_New_Memory_Face(s_ft_lib, s_font_buf, (FT_Long)decoded_size, 0, &s_ft_face);

    auto reload = [&](const char *b64, size_t b64_size,
                      unsigned char **buf, FT_Face *face) {
        size_t sz = 0;
        if (base64_decode(b64, b64_size, buf, &sz) == 0 && *buf)
            if (FT_New_Memory_Face(s_ft_lib, *buf, (FT_Long)sz, 0, face) != 0)
                { free(*buf); *buf = nullptr; }
    };
    reload(DEJAVU_REGULAR_FONT_B64,     DEJAVU_REGULAR_FONT_B64_SIZE,     &s_font_buf_reg,  &s_ft_face_reg);
    reload(DEJAVU_OBLIQUE_FONT_B64,     DEJAVU_OBLIQUE_FONT_B64_SIZE,     &s_font_buf_obl,  &s_ft_face_obl);
    reload(DEJAVU_BOLDOBLIQUE_FONT_B64, DEJAVU_BOLDOBLIQUE_FONT_B64_SIZE, &s_font_buf_bobl, &s_ft_face_bobl);
}

// ============================================================================
// FACE SELECTION
// ============================================================================

FT_Face face_for_attrs(uint8_t attrs) {
    bool bold = (attrs & ATTR_BOLD)   != 0;
    bool ital = (attrs & ATTR_ITALIC) != 0;
    if (bold && ital) return s_ft_face_bobl ? s_ft_face_bobl : s_ft_face;
    if (bold)         return s_ft_face;
    if (ital)         return s_ft_face_obl  ? s_ft_face_obl  : s_ft_face_reg;
    return s_ft_face_reg ? s_ft_face_reg : s_ft_face;
}

// ============================================================================
// UTF-8 UTILITIES
// ============================================================================

uint32_t next_codepoint(const unsigned char **p) {
    uint32_t cp;
    unsigned char c = **p;
    if (c < 0x80)       { cp = c;                      *p += 1; }
    else if (c < 0xE0)  { cp = (c&0x1F)<<6  | ((*p)[1]&0x3F);             *p += 2; }
    else if (c < 0xF0)  { cp = (c&0x0F)<<12 | ((*p)[1]&0x3F)<<6 | ((*p)[2]&0x3F); *p += 3; }
    else                { cp = (c&0x07)<<18 | ((*p)[1]&0x3F)<<12| ((*p)[2]&0x3F)<<6|((*p)[3]&0x3F); *p += 4; }
    return cp;
}

bool is_emoji_codepoint(uint32_t cp) {
    return (cp >= 0x2600  && cp <= 0x27BF)
        || (cp >= 0x1F300 && cp <= 0x1FAFF)
        || (cp >= 0x1F000 && cp <= 0x1F02F)
        || (cp >= 0xFE00  && cp <= 0xFE0F);
}

int cp_to_utf8(uint32_t cp, char *buf) {
    if (cp < 0x80)      { buf[0]=(char)cp; return 1; }
    if (cp < 0x800)     { buf[0]=(char)(0xC0|(cp>>6)); buf[1]=(char)(0x80|(cp&0x3F)); return 2; }
    if (cp < 0x10000)   { buf[0]=(char)(0xE0|(cp>>12)); buf[1]=(char)(0x80|((cp>>6)&0x3F)); buf[2]=(char)(0x80|(cp&0x3F)); return 3; }
    buf[0]=(char)(0xF0|(cp>>18)); buf[1]=(char)(0x80|((cp>>12)&0x3F)); buf[2]=(char)(0x80|((cp>>6)&0x3F)); buf[3]=(char)(0x80|(cp&0x3F)); return 4;
}

// ============================================================================
// GLYPH / TEXT RENDERING
// ============================================================================

float draw_glyph(FT_Face face, uint32_t cp, float cx, float baseline_y,
                 int font_px, int emoji_px, float r, float g, float b, float a,
                 std::vector<Vertex> &verts) {
    float glyph_scale = 1.0f;
    if (face->num_fixed_sizes > 0) {
        int best = 0;
        int best_diff = abs(face->available_sizes[0].height - emoji_px);
        for (int si = 1; si < face->num_fixed_sizes; si++) {
            int diff = abs(face->available_sizes[si].height - emoji_px);
            if (diff < best_diff) { best_diff = diff; best = si; }
        }
        FT_Select_Size(face, best);
        glyph_scale = (float)emoji_px / (float)face->available_sizes[best].height;
    } else {
        int req_px = (face == s_emoji_face) ? emoji_px : font_px;
        FT_Set_Pixel_Sizes(face, 0, (FT_UInt)req_px);
    }

    int load_flags = FT_LOAD_RENDER;
    if (face->num_fixed_sizes > 0 || face == s_emoji_face) load_flags |= FT_LOAD_COLOR;

    FT_UInt gi = FT_Get_Char_Index(face, cp);
    if (!gi) {
        if (cp > 0x7F) SDL_Log("[Glyph] cp=U+%04X not in face '%s'\n", cp, face->family_name);
        return 0.f;
    }
    int load_err = FT_Load_Glyph(face, gi, load_flags);
    if (load_err) {
        return 0.f;
    }

    FT_GlyphSlot slot = face->glyph;
    FT_Bitmap *bm = &slot->bitmap;

    if (bm->width == 0 || bm->rows == 0) return 0.f;

    float render_scale = glyph_scale;
    float drawn_w = bm->width * render_scale;
    float drawn_h = bm->rows  * render_scale;
    float gx, gy;
    if (face->num_fixed_sizes > 0) {
        gx = cx + (font_px - drawn_w) * 0.5f;
        gy = baseline_y - drawn_h;
    } else {
        gx = cx + slot->bitmap_left;
        gy = baseline_y - slot->bitmap_top;
    }

    if (bm->pixel_mode == FT_PIXEL_MODE_BGRA) {
        for (int row = 0; row < (int)bm->rows; row++) {
            for (int col = 0; col < (int)bm->width; col++) {
                const unsigned char *px = bm->buffer + row * bm->pitch + col * 4;
                float pb = px[0]/255.f, pg = px[1]/255.f, pr = px[2]/255.f, pa = px[3]/255.f * a;
                if (pa < 0.01f) continue;
                float ex = gx + col * render_scale;
                float ey = gy + row * render_scale;
                float ps = render_scale < 1.f ? 1.f : render_scale;
                verts.push_back({ex,      ey,      pr,pg,pb,pa});
                verts.push_back({ex+ps,   ey,      pr,pg,pb,pa});
                verts.push_back({ex+ps,   ey+ps,   pr,pg,pb,pa});
                verts.push_back({ex,      ey,      pr,pg,pb,pa});
                verts.push_back({ex+ps,   ey+ps,   pr,pg,pb,pa});
                verts.push_back({ex,      ey+ps,   pr,pg,pb,pa});
            }
        }
    } else {
        for (int row = 0; row < (int)bm->rows; row++) {
            for (int col = 0; col < (int)bm->width; col++) {
                unsigned char pv = bm->buffer[row * bm->pitch + col];
                if (pv < 1) continue;
                float fa = a * sqrtf(pv / 255.f);
                float ex = gx + col * render_scale;
                float ey = gy + row * render_scale;
                float ps = render_scale < 1.f ? 1.f : render_scale;
                verts.push_back({ex,     ey,     r,g,b,fa});
                verts.push_back({ex+ps,  ey,     r,g,b,fa});
                verts.push_back({ex+ps,  ey+ps,  r,g,b,fa});
                verts.push_back({ex,     ey,     r,g,b,fa});
                verts.push_back({ex+ps,  ey+ps,  r,g,b,fa});
                verts.push_back({ex,     ey+ps,  r,g,b,fa});
            }
        }
    }
    return (float)(slot->advance.x >> 6) * render_scale;
}

float draw_text(const char *text, float x, float y, int font_px, int emoji_px,
                float r, float g, float b, float a, uint8_t attrs) {
    if (!s_ft_face || !text || !*text) return x;

    static std::vector<Vertex> verts;
    verts.clear();

    FT_Face base_face = face_for_attrs(attrs);

    float cx = x;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t cp = next_codepoint(&p);
        if (!cp) continue;
        if (cp == 0xFE0F) continue;

        FT_Face face = base_face;
        if (s_emoji_face && is_emoji_codepoint(cp)) {
            if (FT_Get_Char_Index(s_emoji_face, cp)) face = s_emoji_face;
        }

        bool is_grayscale_emoji = (face == s_emoji_face && face->num_fixed_sizes == 0);
        float er = is_grayscale_emoji ? 1.f : r;
        float eg = is_grayscale_emoji ? 1.f : g;
        float eb = is_grayscale_emoji ? 1.f : b;

        float adv = draw_glyph(face, cp, cx, y, font_px, emoji_px, er, eg, eb, a, verts);
        if (adv == 0.f) {
            if (face != s_emoji_face && s_emoji_face)
                adv = draw_glyph(s_emoji_face, cp, cx, y, font_px, emoji_px, 1.f, 1.f, 1.f, a, verts);
        }
        if (adv == 0.f) {
            FT_Set_Pixel_Sizes(base_face, 0, (FT_UInt)font_px);
            FT_UInt gi = FT_Get_Char_Index(base_face, ' ');
            if (!FT_Load_Glyph(base_face, gi, FT_LOAD_DEFAULT))
                adv = (float)(base_face->glyph->advance.x >> 6);
        }
        cx += adv;
    }

    if (!verts.empty())
        draw_verts(verts.data(), (int)verts.size(), GL_TRIANGLES);

    return cx;
}

float measure_text(const char *text, int font_px) {
    if (!s_ft_face || !text) return 0;
    FT_Set_Pixel_Sizes(s_ft_face, 0, (FT_UInt)font_px);
    float w = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t cp = next_codepoint(&p);
        FT_UInt gi = FT_Get_Char_Index(s_ft_face, cp);
        if (!FT_Load_Glyph(s_ft_face, gi, FT_LOAD_DEFAULT))
            w += (float)(s_ft_face->glyph->advance.x >> 6);
    }
    return w;
}
