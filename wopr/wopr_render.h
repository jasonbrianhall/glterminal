#pragma once
// wopr_render.h — thin wrappers around the Felix Terminal rendering API.

#include "../gl_renderer.h"   // draw_rect()
#include "../ft_font.h"       // draw_text()

extern int g_font_size;       // defined in gl_terminal_main.cpp

static inline void gl_draw_rect(float x, float y, float w, float h,
                                float r, float g, float b, float a)
{
    draw_rect(x, y, w, h, r, g, b, a);
}

static inline void gl_draw_text(const char *text, float x, float y,
                                float r, float g, float b, float a, float /*scale*/)
{
    draw_text(text, x, y, g_font_size, g_font_size, r, g, b, a, 0);
}

static inline float gl_text_width(const char *text, float /*scale*/)
{
    return draw_text(text, -99999.f, -99999.f,
                     g_font_size, g_font_size,
                     0.f, 0.f, 0.f, 0.f, 0);
}

static inline float gl_text_height(float /*scale*/)
{
    return (float)g_font_size;
}

