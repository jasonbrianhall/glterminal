/*
 * gfx_null.c — No-op graphics stubs, compiled when HAVE_SDL is not defined.
 */
#ifndef HAVE_SDL
#include "gfx.h"
void gfx_open(int m)                            { (void)m; }
void gfx_close(void)                            { }
int  gfx_is_open(void)                          { return 0; }
int  gfx_get_mode(void)                         { return 0; }
int  gfx_get_cols(void)                         { return 80; }
void gfx_palette(int i,int r,int g,int b)       { (void)i;(void)r;(void)g;(void)b; }
void gfx_cls(int c)                             { (void)c; }
void gfx_pset(int x,int y,int c)               { (void)x;(void)y;(void)c; }
int  gfx_point(int x,int y)                    { (void)x;(void)y; return 0; }
void gfx_line(int a,int b,int c,int d,int e,int f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}
void gfx_circle(int a,int b,int c,int d,double e,double f,double g){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;}
void gfx_paint(int x,int y,int p,int b)        { (void)x;(void)y;(void)p;(void)b; }
void gfx_draw(const char *s)                    { (void)s; }
void gfx_get_pen(int *x,int *y)                { *x=0;*y=0; }
void gfx_set_pen(int x,int y,int c)            { (void)x;(void)y;(void)c; }
void gfx_print_char(unsigned char c,int f,int b){ (void)c;(void)f;(void)b; }
void gfx_locate(int r,int c)                   { (void)r;(void)c; }
void gfx_color(int f,int b)                    { (void)f;(void)b; }
void gfx_cursor(int v)                         { (void)v; }
void gfx_flush(void)                           { }
int  gfx_inkey(void)                           { return 0; }
int  gfx_getchar(void)                         { return 0; }
int  gfx_getline(char *b, int n)               { (void)b;(void)n; return 0; }
int  gfx_get_rect(int a,int b2,int c,int d,int *buf){ (void)a;(void)b2;(void)c;(void)d;(void)buf; return 0; }
void gfx_put_rect(int a,int b2,int c,int d,const int *buf,int m){ (void)a;(void)b2;(void)c;(void)d;(void)buf;(void)m; }
#endif
