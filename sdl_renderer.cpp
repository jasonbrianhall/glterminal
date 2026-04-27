// ============================================================================
// sdl_renderer.cpp
//
// SDL2 fallback renderer.  Always compiled into the binary alongside
// gl_renderer.cpp.  The public API functions (gl_init_renderer, draw_verts,
// etc.) are defined ONLY in gl_renderer.cpp; this file implements the SDL
// equivalents under sdl_* names and is called by the dispatch layer that is
// added to gl_renderer.cpp (see gl_renderer.cpp.diff).
//
// When g_use_sdl_renderer is false every dispatch stub in gl_renderer.cpp
// falls straight through to the original GL code with zero overhead.
// ============================================================================

#include "gl_renderer.h"   // Vertex, Mat4, GLState, MAX_VERTS, mat4_ortho
#include "sdl_renderer.h"
#include "glyph_atlas.h"   // g_atlas, GlyphVertex
#include "gl_terminal.h"   // MAX_VERTS cross-check
#include <SDL2/SDL.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <algorithm>

// ============================================================================
// GLOBALS
// ============================================================================

bool          g_use_sdl_renderer = false;
SDL_Renderer *g_sdl_renderer     = nullptr;

static SDL_Texture *s_term_tex      = nullptr;
static SDL_Texture *s_composite_tex = nullptr;
SDL_Texture *s_basic_tex     = nullptr;  // persistent BASIC graphics layer
static int          s_tex_w = 0, s_tex_h = 0;
bool                s_basic_has_content = false;

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

static SDL_Texture *sdl_make_target(int w, int h) {
    SDL_Texture *t = SDL_CreateTexture(
        g_sdl_renderer, SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_TARGET, w, h);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

static inline SDL_Vertex to_sv(const Vertex &v) {
    SDL_Vertex sv;
    sv.position.x  = v.x;
    sv.position.y  = v.y;
    sv.color.r     = (Uint8)(v.r * 255.f);
    sv.color.g     = (Uint8)(v.g * 255.f);
    sv.color.b     = (Uint8)(v.b * 255.f);
    sv.color.a     = (Uint8)(v.a * 255.f);
    sv.tex_coord.x = sv.tex_coord.y = 0.f;
    return sv;
}

static SDL_Vertex* s_accum;
static int         s_accum_n = 0;

struct SDLAccumInit {
    SDLAccumInit() {
        s_accum = new SDL_Vertex[MAX_VERTS];
    }
    ~SDLAccumInit() {
        delete[] s_accum;
    }
};

static SDLAccumInit _sdl_accum_init;


// ============================================================================
// PUBLIC SDL-PATH ENTRY POINTS  (called by dispatch stubs in gl_renderer.cpp)
// ============================================================================

void sdl_init_renderer(int w, int h) {
    // Renderer created by gl_terminal_main before this call; just build textures.
    s_tex_w = w; s_tex_h = h;
    if (s_term_tex)      SDL_DestroyTexture(s_term_tex);
    if (s_composite_tex) SDL_DestroyTexture(s_composite_tex);
    if (s_basic_tex)     SDL_DestroyTexture(s_basic_tex);
    s_term_tex      = sdl_make_target(w, h);
    s_composite_tex = sdl_make_target(w, h);
    s_basic_tex     = sdl_make_target(w, h);
    // Clear term tex to black
    SDL_SetRenderTarget(g_sdl_renderer, s_term_tex);
    SDL_SetRenderDrawColor(g_sdl_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_sdl_renderer);
    // Clear basic tex to transparent
    SDL_SetRenderTarget(g_sdl_renderer, s_basic_tex);
    SDL_SetRenderDrawColor(g_sdl_renderer, 0, 0, 0, 0);
    SDL_RenderClear(g_sdl_renderer);
    SDL_SetRenderTarget(g_sdl_renderer, nullptr);
    s_basic_has_content = false;
    // Populate stub GLState so mat4/proj references in shared code don't crash
    G.cr = G.cg = G.cb = G.ca = 1.f;
    G.proj = mat4_ortho(0, (float)w, (float)h, 0, -1, 1);
    // Initialize glyph atlas for SDL path
    g_atlas.init();
}

void sdl_resize_fbo(int w, int h) {
    sdl_init_renderer(w, h);  // recreate textures at new size
}

// Glyph accumulator for SDL path
static std::vector<SDL_Vertex> s_glyph_sv;
static std::vector<GlyphVertex> s_glyph_pending;

void sdl_flush_glyphs() {
    // Flush any pending solid draws first so Z-order is correct (rects behind text)
    sdl_flush_verts();

    if (s_glyph_pending.empty()) return;
    if (!g_atlas.sdl_tex) { s_glyph_pending.clear(); return; }

    s_glyph_sv.clear();
    s_glyph_sv.reserve(s_glyph_pending.size());

    for (auto &gv : s_glyph_pending) {
        SDL_Vertex sv;
        sv.position.x  = gv.x;
        sv.position.y  = gv.y;
        sv.tex_coord.x = gv.u;
        sv.tex_coord.y = gv.v;
        if (gv.color_glyph > 0.5f) {
            sv.color.r = 255;
            sv.color.g = 255;
            sv.color.b = 255;
            sv.color.a = (Uint8)(gv.tint_a * 255.f);
        } else {
            // Grayscale: tint color * atlas alpha (coverage baked into .a)
            sv.color.r = (Uint8)(gv.tint_r * 255.f);
            sv.color.g = (Uint8)(gv.tint_g * 255.f);
            sv.color.b = (Uint8)(gv.tint_b * 255.f);
            sv.color.a = (Uint8)(gv.tint_a * 255.f);
        }
        s_glyph_sv.push_back(sv);
    }

    SDL_RenderGeometry(g_sdl_renderer, g_atlas.sdl_tex,
                       s_glyph_sv.data(), (int)s_glyph_sv.size(), nullptr, 0);
    s_glyph_pending.clear();
}

void sdl_draw_glyph_verts(GlyphVertex *v, int n) {
    for (int i = 0; i < n; i++)
        s_glyph_pending.push_back(v[i]);
}

void sdl_flush_verts(void) {
    if (s_accum_n == 0) return;
    SDL_RenderGeometry(g_sdl_renderer, nullptr, s_accum, s_accum_n, nullptr, 0);
    s_accum_n = 0;
}

void sdl_draw_verts(Vertex *v, int n) {
    if (n <= 0) return;
    if (s_accum_n + n > MAX_VERTS) sdl_flush_verts();
    if (n > MAX_VERTS) {
        int done = 0;
        while (done < n) {
            int chunk = (n - done < MAX_VERTS) ? (n - done) : MAX_VERTS;
            for (int i = 0; i < chunk; i++) s_accum[i] = to_sv(v[done + i]);
            SDL_RenderGeometry(g_sdl_renderer, nullptr, s_accum, chunk, nullptr, 0);
            done += chunk;
        }
        return;
    }
    for (int i = 0; i < n; i++) s_accum[s_accum_n + i] = to_sv(v[i]);
    s_accum_n += n;
}

void sdl_begin_term_frame(int, int, float, float, float) {
    s_accum_n = 0;
    SDL_SetRenderTarget(g_sdl_renderer, s_term_tex);
}

void sdl_clear_term_frame(int, int, float bg_r, float bg_g, float bg_b) {
    SDL_SetRenderTarget(g_sdl_renderer, s_term_tex);
    if (s_basic_has_content) {
        // BASIC owns the background — clear term to transparent so glyphs
        // composite over the BASIC layer without an opaque bg covering it.
        SDL_SetRenderDrawColor(g_sdl_renderer, 0, 0, 0, 0);
    } else {
        SDL_SetRenderDrawColor(g_sdl_renderer,
            (Uint8)(bg_r*255), (Uint8)(bg_g*255), (Uint8)(bg_b*255), 255);
    }
    SDL_RenderClear(g_sdl_renderer);
}

void sdl_end_term_frame(void) {
    // Flush only solid-colour geometry (backgrounds, cursor) into s_term_tex.
    // Glyphs are deferred — see sdl_end_frame where they're drawn to the screen
    // after the composite blit, avoiding the streaming-texture-as-source-while-
    // texture-is-render-target conflict in SDL's opengl backend.
    sdl_flush_verts();
    SDL_SetRenderTarget(g_sdl_renderer, nullptr);
}

void sdl_begin_frame(void) {
    s_accum_n = 0;
    // Blit s_term_tex (backgrounds only) to screen first
    SDL_SetRenderTarget(g_sdl_renderer, nullptr);
    SDL_SetRenderDrawColor(g_sdl_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_sdl_renderer);
    if (s_basic_has_content && s_basic_tex) {
        SDL_SetTextureBlendMode(s_basic_tex, SDL_BLENDMODE_NONE);
        SDL_RenderCopy(g_sdl_renderer, s_basic_tex, nullptr, nullptr);
        SDL_SetTextureBlendMode(s_term_tex, SDL_BLENDMODE_BLEND);
        SDL_RenderCopy(g_sdl_renderer, s_term_tex, nullptr, nullptr);
    } else {
        SDL_SetTextureBlendMode(s_term_tex, SDL_BLENDMODE_NONE);
        SDL_RenderCopy(g_sdl_renderer, s_term_tex, nullptr, nullptr);
    }
    // Flush deferred terminal glyphs directly to screen (target=nullptr).
    // This avoids SDL opengl backend's conflict between a STREAMING source
    // texture and a TARGET render target.
    sdl_flush_glyphs();
}

// ============================================================================
// SOFTWARE POST-PROCESS
// ============================================================================

static inline float sfx_rand(float x, float y) {
    float v = sinf(x * 12.9898f + y * 78.233f) * 43758.5453f;
    return v - floorf(v);
}
static inline float sfx_c01(float v) { return v<0?0:v>1?1:v; }
static inline float sfx_ss(float e0, float e1, float x) {
    float t = sfx_c01((x-e0)/(e1-e0)); return t*t*(3.f-2.f*t);
}
static inline void sfx_get(const uint8_t *s, int w, int h, int px, int py,
                            float *r, float *g, float *b) {
    if (px<0)px=0; if(px>=w)px=w-1; if(py<0)py=0; if(py>=h)py=h-1;
    const uint8_t *p = s+(py*w+px)*4;
    *r=p[0]/255.f; *g=p[1]/255.f; *b=p[2]/255.f;
}
static inline void sfx_samp(const uint8_t *s, int w, int h, float u, float v,
                              float *r, float *g, float *b) {
    float px=u*w-0.5f, py=v*h-0.5f;
    int x0=(int)px, y0=(int)py;
    float fx=px-x0, fy=py-y0;
    float r00,g00,b00,r10,g10,b10,r01,g01,b01,r11,g11,b11;
    sfx_get(s,w,h,x0,  y0,  &r00,&g00,&b00); sfx_get(s,w,h,x0+1,y0,  &r10,&g10,&b10);
    sfx_get(s,w,h,x0,  y0+1,&r01,&g01,&b01); sfx_get(s,w,h,x0+1,y0+1,&r11,&g11,&b11);
    *r=r00*(1-fx)*(1-fy)+r10*fx*(1-fy)+r01*(1-fx)*fy+r11*fx*fy;
    *g=g00*(1-fx)*(1-fy)+g10*fx*(1-fy)+g01*(1-fx)*fy+g11*fx*fy;
    *b=b00*(1-fx)*(1-fy)+b10*fx*(1-fy)+b01*(1-fx)*fy+b11*fx*fy;
}

static void sfx_crt(uint8_t *d, const uint8_t *s, int w, int h, float t) {
    for (int py=0;py<h;py++) for (int px=0;px<w;px++) {
        float u=(px+.5f)/w, v=(py+.5f)/h;
        float cx=u-.5f, cy=v-.5f, r2=cx*cx+cy*cy;
        float bu=u+cx*r2*.12f, bv=v+cy*r2*.12f;
        uint8_t *o=d+(py*w+px)*4;
        if(bu<0||bu>1||bv<0||bv>1){o[0]=o[1]=o[2]=0;o[3]=255;continue;}
        float r,g,b,d1,d2;
        sfx_samp(s,w,h,bu,bv,&r,&g,&b);
        float sl=sinf(bv*h*3.14159f); sl=sl<0?0:sl;
        r*=.75f+.25f*sl; g*=.75f+.25f*sl; b*=.75f+.25f*sl;
        float sh=.0015f;
        sfx_samp(s,w,h,bu+sh,bv,&r,&d1,&d2); sfx_samp(s,w,h,bu-sh,bv,&d1,&d2,&b);
        float ox=2.f/w,oy=2.f/h,gr,gg,gb,tr,tg,tb;
        sfx_samp(s,w,h,bu+ox,bv,&gr,&gg,&gb); sfx_samp(s,w,h,bu-ox,bv,&tr,&tg,&tb); gr+=tr;gg+=tg;gb+=tb;
        sfx_samp(s,w,h,bu,bv+oy,&tr,&tg,&tb); gr+=tr;gg+=tg;gb+=tb; sfx_samp(s,w,h,bu,bv-oy,&tr,&tg,&tb); gr+=tr;gg+=tg;gb+=tb;
        r+=gr*.08f; g+=gg*.08f; b+=gb*.08f;
        float vig=1.f-(cx*cx+cy*cy)*1.8f;
        r*=vig; g*=vig; b*=vig;
        float noise=sfx_rand(bu+t*.1f,bv)*.03f, flicker=.97f+.03f*sfx_rand(t*7.3f,0);
        r=(r+noise)*flicker; g=(g+noise)*flicker; b=(b+noise)*flicker;
        o[0]=(uint8_t)(sfx_c01(r)*255); o[1]=(uint8_t)(sfx_c01(g)*255); o[2]=(uint8_t)(sfx_c01(b)*255); o[3]=255;
    }
}
static void sfx_lcd(uint8_t *d, const uint8_t *s, int w, int h) {
    for (int py=0;py<h;py++) for (int px=0;px<w;px++) {
        float u=(px+.5f)/w, v=(py+.5f)/h;
        float r,g,b; sfx_samp(s,w,h,u,v,&r,&g,&b);
        float hline=sfx_ss(.45f,.5f,fabsf(fmodf((float)py,1.f)-.5f));
        float vline=sfx_ss(.45f,.5f,fabsf(fmodf((float)px,1.f)-.5f));
        float gm=1.f-fmaxf(hline,vline)*.5f; r*=gm;g*=gm;b*=gm;
        float sub=(float)(px%3)/3.f+.167f; sub=sub-floorf(sub);
        r*=sfx_ss(0,.33f,sub)*(1.f-sfx_ss(.33f,.66f,sub))*.25f+.75f;
        g*=sfx_ss(.33f,.66f,sub)*(1.f-sfx_ss(.66f,1.f,sub))*.25f+.75f;
        b*=sfx_ss(.66f,1.f,sub)*.25f+.75f;
        r*=.95f; b*=.92f;
        float vx=u-.5f,vy=v-.5f,vig=1.f-(vx*vx+vy*vy)*.6f; r*=vig;g*=vig;b*=vig;
        uint8_t *o=d+(py*w+px)*4;
        o[0]=(uint8_t)(sfx_c01(r)*255); o[1]=(uint8_t)(sfx_c01(g)*255); o[2]=(uint8_t)(sfx_c01(b)*255); o[3]=255;
    }
}
static void sfx_vhs(uint8_t *d, const uint8_t *s, int w, int h, float t) {
    float by=fmodf(t*.17f,1.f);
    for (int py=0;py<h;py++) {
        float v=(py+.5f)/h;
        float jitter=(sfx_rand((float)py,t*3.f)-.5f)*.004f;
        if(sfx_rand(floorf(v*12.f),floorf(t*2.f))>.97f) jitter*=8.f;
        for (int px=0;px<w;px++) {
            float u=(px+.5f)/w+jitter, bl=.004f, r,g,b,d1,d2;
            sfx_samp(s,w,h,u+bl,v,&r,&d1,&d2); sfx_samp(s,w,h,u,v,&d1,&g,&d2); sfx_samp(s,w,h,u-bl,v,&d1,&d2,&b);
            float sl=sinf(v*h*3.14159f),sf=.80f+.20f*(sl<0?0:sl); r*=sf;g*=sf;b*=sf;
            float noise=(sfx_rand(u,v+t)-.5f)*.06f; r+=noise;g+=noise;b+=noise;
            float band=sfx_ss(.015f,0.f,fabsf(v-by)), bn=sfx_rand(u*100.f,t)*.3f*band; r+=bn;g+=bn;b+=bn;
            float lm=r*.299f+g*.587f+b*.114f; r=r*.75f+lm*.25f; g=g*.75f+lm*.25f; b=b*.75f+lm*.25f;
            float vx=u-.5f,vy=v-.5f,vig=1.f-(vx*vx+vy*vy)*1.2f; r*=vig;g*=vig;b*=vig;
            uint8_t *o=d+(py*w+px)*4;
            o[0]=(uint8_t)(sfx_c01(r)*255); o[1]=(uint8_t)(sfx_c01(g)*255); o[2]=(uint8_t)(sfx_c01(b)*255); o[3]=255;
        }
    }
}
static void sfx_focus(uint8_t *d, const uint8_t *s, int w, int h) {
    for (int py=0;py<h;py++) {
        float v=(py+.5f)/h, mul=1.f*(1.f-sfx_ss(.35f,.65f,v))+.15f*sfx_ss(.35f,.65f,v);
        for (int px=0;px<w;px++) {
            const uint8_t *in=s+(py*w+px)*4; uint8_t *o=d+(py*w+px)*4;
            o[0]=(uint8_t)(in[0]*mul); o[1]=(uint8_t)(in[1]*mul); o[2]=(uint8_t)(in[2]*mul); o[3]=in[3];
        }
    }
}
static void sfx_c64(uint8_t *d, const uint8_t *s, int w, int h) {
    for (int py=0;py<h;py++) for (int px=0;px<w;px++) {
        float u=(px+.5f)/w,v=(py+.5f)/h; uint8_t *o=d+(py*w+px)*4;
        const float brd=.018f;
        if(u<brd||u>1-brd||v<brd||v>1-brd){o[0]=(uint8_t)(.263f*255);o[1]=(uint8_t)(.282f*255);o[2]=(uint8_t)(.800f*255);o[3]=255;continue;}
        float iu=(u-brd)/(1.f-2*brd),iv=(v-brd)/(1.f-2*brd),r,g,b;
        sfx_samp(s,w,h,iu,iv,&r,&g,&b);
        float lm=r*.299f+g*.587f+b*.114f;
        float tr=.251f*(1-lm)+.686f*lm, tg=.251f*(1-lm)+.686f*lm, tb=.722f*(1-lm)+1.f*lm;
        float cf=sqrtf((r-lm)*(r-lm)+(g-lm)*(g-lm)+(b-lm)*(b-lm)); if(cf>.35f)cf=.35f;
        r=tr*(1-cf)+r*cf; g=tg*(1-cf)+g*cf; b=tb*(1-cf)+b*cf;
        float sl=sinf(iv*h*3.14159f),sf=.88f+.12f*(sl<0?0:sl); r*=sf;g*=sf;b*=sf;
        float ox=2.f/w,oy=2.f/h,gr,gg,gb,tr2,tg2,tb2;
        sfx_samp(s,w,h,iu+ox,iv,&gr,&gg,&gb); sfx_samp(s,w,h,iu-ox,iv,&tr2,&tg2,&tb2); gr+=tr2;gg+=tg2;gb+=tb2;
        sfx_samp(s,w,h,iu,iv+oy,&tr2,&tg2,&tb2); gr+=tr2;gg+=tg2;gb+=tb2; sfx_samp(s,w,h,iu,iv-oy,&tr2,&tg2,&tb2); gr+=tr2;gg+=tg2;gb+=tb2;
        r+=(.686f*.5f+gr*.5f)*.04f; g+=(.686f*.5f+gg*.5f)*.04f; b+=(1.f*.5f+gb*.5f)*.04f;
        float vx=iu-.5f,vy=iv-.5f,vig=1.f-(vx*vx+vy*vy)*.9f; r*=vig;g*=vig;b*=vig;
        o[0]=(uint8_t)(sfx_c01(r)*255); o[1]=(uint8_t)(sfx_c01(g)*255); o[2]=(uint8_t)(sfx_c01(b)*255); o[3]=255;
    }
}
static void sfx_composite(uint8_t *d, const uint8_t *s, int w, int h, float t) {
    for (int py=0;py<h;py++) {
        float v=(py+.5f)/h, hs=(sfx_rand((float)py,floorf(t*4.f))-.5f)*.003f;
        for (int px=0;px<w;px++) {
            float u=(px+.5f)/w, fp=(float)px;
            float sc1=sinf(fp*.785398f+t*6.f),sc2=sinf(fp*.785398f+t*6.f+2.094f),sc3=sinf(fp*.785398f+t*6.f+4.189f);
            float cs=3.5f/w, r,g,b,d1,d2;
            sfx_samp(s,w,h,u+sc1*cs,v,&r,&d1,&d2); sfx_samp(s,w,h,u+sc2*cs*.5f,v,&d1,&g,&d2); sfx_samp(s,w,h,u+sc3*cs,v,&d1,&d2,&b);
            float br,bg2,bb2,nr,ng,nb; sfx_samp(s,w,h,u,v,&br,&bg2,&bb2); sfx_samp(s,w,h,u+1.f/w,v,&nr,&ng,&nb);
            float bl=br*.299f+bg2*.587f+bb2*.114f, nl=nr*.299f+ng*.587f+nb*.114f;
            float crawl=sinf(fp*3.14159f+v*h*3.14159f+t*15.f)*.5f+.5f, edge=fabsf(bl-nl);
            r+=crawl*edge*.32f; g-=crawl*edge*.16f; b+=crawl*edge*.32f;
            float rr,rg,rb,pr,pg2,pb; sfx_samp(s,w,h,u+4.f/w,v,&rr,&rg,&rb); sfx_samp(s,w,h,u-4.f/w,v,&pr,&pg2,&pb);
            float ld=(rr-pr)*.299f+(rg-pg2)*.587f+(rb-pb)*.114f; r+=ld*.12f;g+=ld*.12f;b+=ld*.12f;
            float smr=0,smg=0,smb=0;
            for(int i=-3;i<=3;i++){float sr,sg2,sb2; sfx_samp(s,w,h,u+i*1.5f/w,v,&sr,&sg2,&sb2); float lm=sr*.299f+sg2*.587f+sb2*.114f; smr+=sr-lm;smg+=sg2-lm;smb+=sb2-lm;}
            smr/=7.f;smg/=7.f;smb/=7.f; float cl=r*.299f+g*.587f+b*.114f; r=cl+smr*1.4f;g=cl+smg*1.4f;b=cl+smb*1.4f;
            float sl=sinf(v*h*3.14159f),sf=.82f+.18f*(sl<0?0:sl); r*=sf;g*=sf;b*=sf;
            float ln=(sfx_rand(u+t*1.3f,v)-.5f)*.03f,cnr=(sfx_rand(u+t*2.1f,.3f)-.5f)*.06f,cnb=(sfx_rand(u+t*1.7f,.7f)-.5f)*.06f;
            r+=ln+cnr;g+=ln;b+=ln+cnb;
            float hjr,hjg,hjb; sfx_samp(s,w,h,u+hs,v,&hjr,&hjg,&hjb); r=r*.7f+hjr*.3f;g=g*.7f+hjg*.3f;b=b*.7f+hjb*.3f;
            float vx=u-.5f,vy=v-.5f,vig=1.f-(vx*vx+vy*vy)*1.1f; r*=vig;g*=vig;b*=vig;
            uint8_t *o=d+(py*w+px)*4;
            o[0]=(uint8_t)(sfx_c01(r)*255); o[1]=(uint8_t)(sfx_c01(g)*255); o[2]=(uint8_t)(sfx_c01(b)*255); o[3]=255;
        }
    }
}
static void sfx_bloom(uint8_t *d, const uint8_t *s, int w, int h) {
    static const float T[][3]={{5,0,1},{-5,0,1},{0,5,1},{0,-5,1},{3.5f,3.5f,.7f},{-3.5f,3.5f,.7f},{3.5f,-3.5f,.7f},{-3.5f,-3.5f,.7f},{12,0,.5f},{-12,0,.5f},{0,12,.5f},{0,-12,.5f},{8.5f,8.5f,.35f},{-8.5f,8.5f,.35f},{8.5f,-8.5f,.35f},{-8.5f,-8.5f,.35f}};
    float tw=0; for(int i=0;i<16;i++) tw+=T[i][2];
    for(int py=0;py<h;py++) for(int px=0;px<w;px++) {
        const uint8_t *base=s+(py*w+px)*4;
        float br=base[0]/255.f,bg=base[1]/255.f,bb=base[2]/255.f,gr=0,gg=0,gb=0;
        for(int i=0;i<16;i++){float sr,sg,sb; sfx_get(s,w,h,px+(int)T[i][0],py+(int)T[i][1],&sr,&sg,&sb); float lm=sr*.299f+sg*.587f+sb*.114f,bright=lm-.08f; if(bright<0)bright=0; gr+=sr*bright*T[i][2];gg+=sg*bright*T[i][2];gb+=sb*bright*T[i][2];}
        if(tw>0){gr/=tw;gg/=tw;gb/=tw;} gr*=4.5f;gg*=4.5f;gb*=4.5f;
        uint8_t *o=d+(py*w+px)*4;
        o[0]=(uint8_t)(sfx_c01(br+gr)*255); o[1]=(uint8_t)(sfx_c01(bg+gg)*255); o[2]=(uint8_t)(sfx_c01(bb+gb)*255); o[3]=255;
    }
}
static void sfx_ghosting(uint8_t *d, const uint8_t *cur, const uint8_t *ghost, int w, int h) {
    for(int i=0;i<w*h;i++){
        float cr=cur[i*4]/255.f,cg=cur[i*4+1]/255.f,cb=cur[i*4+2]/255.f;
        float gr=ghost[i*4]/255.f,gg=ghost[i*4+1]/255.f,gb=ghost[i*4+2]/255.f;
        float tr=gr*.5f,tg=gg*1.1f,tb=gb*.7f;
        d[i*4+0]=(uint8_t)(sfx_c01(fmaxf(cr,fmaxf(gr,tr*.6f)))*255);
        d[i*4+1]=(uint8_t)(sfx_c01(fmaxf(cg,fmaxf(gg,tg*.6f)))*255);
        d[i*4+2]=(uint8_t)(sfx_c01(fmaxf(cb,fmaxf(gb,tb*.6f)))*255);
        d[i*4+3]=255;
    }
}
static void sfx_wireframe(uint8_t *d, const uint8_t *s, int w, int h) {
    for(int py=0;py<h;py++) for(int px=0;px<w;px++) {
        const uint8_t *in=s+(py*w+px)*4; uint8_t *o=d+(py*w+px)*4;
        float r=in[0]/255.f*.18f, g=in[1]/255.f*.18f, b=in[2]/255.f*.18f;
        float gfx=fmodf((float)px/9.f,1.f), gfy=fmodf((float)py/18.f,1.f);
        float gl=1.f-sfx_ss(0,.08f,fminf(gfx,gfy)); g+=gl*.12f; b+=gl*.18f;
        if(r+g+b>.08f){r*=1.6f; g=g*1.6f+.05f; b=b*1.6f+.1f;}
        o[0]=(uint8_t)(sfx_c01(r)*255); o[1]=(uint8_t)(sfx_c01(g)*255); o[2]=(uint8_t)(sfx_c01(b)*255); o[3]=255;
    }
}

void sdl_end_frame(float time, int win_w, int win_h) {
    sdl_flush_verts();
    SDL_SetRenderTarget(g_sdl_renderer, nullptr);

    if (!g_render_mode) {
        // Screen already has backgrounds + glyphs from sdl_begin_frame.
        // Nothing more to do for the normal (no post-process) path.
        return;
    }

    // Post-process: read back the screen, apply effects, write back.
    // We need to read what sdl_begin_frame drew to the screen.
    int tw = s_tex_w, th = s_tex_h;
    std::vector<uint8_t> buf_a(tw * th * 4), buf_b(tw * th * 4);

    SDL_RenderReadPixels(g_sdl_renderer, nullptr, SDL_PIXELFORMAT_ABGR8888, buf_a.data(), tw * 4);

    uint8_t *src = buf_a.data(), *dst2 = buf_b.data();

    // Ghost buffer — persistent across frames
    static std::vector<uint8_t> s_ghost; static int s_gw=0,s_gh=0;
    if(s_gw!=tw||s_gh!=th){s_ghost.assign(tw*th*4,0);s_gw=tw;s_gh=th;}

    for (int m = 1; m < RENDER_MODE_COUNT; m++) {
        if (!(g_render_mode & (1u << m))) continue;
        switch(m) {
        case RENDER_MODE_CRT:       sfx_crt      (dst2,src,tw,th,time); break;
        case RENDER_MODE_LCD:       sfx_lcd      (dst2,src,tw,th);      break;
        case RENDER_MODE_VHS:       sfx_vhs      (dst2,src,tw,th,time); break;
        case RENDER_MODE_FOCUS:     sfx_focus    (dst2,src,tw,th);      break;
        case RENDER_MODE_C64:       sfx_c64      (dst2,src,tw,th);      break;
        case RENDER_MODE_COMPOSITE: sfx_composite(dst2,src,tw,th,time); break;
        case RENDER_MODE_BLOOM:     sfx_bloom    (dst2,src,tw,th);      break;
        case RENDER_MODE_GHOSTING:
            for(int i=0;i<tw*th*4;i++) s_ghost[i]=(uint8_t)(s_ghost[i]*.75f+src[i]*.35f);
            sfx_ghosting(dst2,src,s_ghost.data(),tw,th);
            break;
        case RENDER_MODE_WIREFRAME: sfx_wireframe(dst2,src,tw,th);      break;
        default: memcpy(dst2,src,tw*th*4); break;
        }
        std::swap(src, dst2);
    }

    static SDL_Texture *s_post=nullptr; static int s_pw=0,s_ph=0;
    if(!s_post||s_pw!=tw||s_ph!=th){
        if(s_post) SDL_DestroyTexture(s_post);
        s_post=SDL_CreateTexture(g_sdl_renderer,SDL_PIXELFORMAT_ABGR8888,SDL_TEXTUREACCESS_STREAMING,tw,th);
        s_pw=tw; s_ph=th;
    }
    SDL_UpdateTexture(s_post, nullptr, src, tw*4);
    SDL_Rect dst={0,0,win_w,win_h};
    SDL_SetTextureBlendMode(s_post, SDL_BLENDMODE_NONE);
    SDL_RenderCopy(g_sdl_renderer, s_post, nullptr, &dst);
}

// sdl_update_ghost: no-op — ghost state is managed inside sdl_end_frame
void sdl_update_ghost(int, int) {}
