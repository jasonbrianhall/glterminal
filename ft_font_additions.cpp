// ============================================================================
// ADDITIONS TO ft_font.cpp
// ============================================================================

// 1. Make the four font buffers non-static so font_manager.cpp can extern them.
//    Change these four lines near the top of ft_font.cpp:
//
//    BEFORE:
//        static unsigned char *s_font_buf      = NULL;
//        static unsigned char *s_font_buf_reg  = NULL;
//        static unsigned char *s_font_buf_obl  = NULL;
//        static unsigned char *s_font_buf_bobl = NULL;
//
//    AFTER (remove 'static'):
//        unsigned char *s_font_buf      = NULL;
//        unsigned char *s_font_buf_reg  = NULL;
//        unsigned char *s_font_buf_obl  = NULL;
//        unsigned char *s_font_buf_bobl = NULL;


// 2. Add ft_reload_embedded() after ft_shutdown() in ft_font.cpp.
//    This restores all four faces from the compiled-in base64 headers.

void ft_reload_embedded(void) {
    // Free any currently loaded faces and buffers
    auto free_face = [](FT_Face *face, unsigned char **buf) {
        if (*face) { FT_Done_Face(*face); *face = nullptr; }
        if (*buf)  { free(*buf); *buf = nullptr; }
    };
    free_face(&s_ft_face,      &s_font_buf);
    free_face(&s_ft_face_reg,  &s_font_buf_reg);
    free_face(&s_ft_face_obl,  &s_font_buf_obl);
    free_face(&s_ft_face_bobl, &s_font_buf_bobl);

    // Reload from embedded base64 headers (same logic as ft_init)
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
// ADDITIONS TO ft_font.h
// ============================================================================

// Add this declaration alongside ft_init / ft_shutdown:

void ft_reload_embedded(void);
