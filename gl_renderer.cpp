#include "gl_renderer.h"
#include "gl_terminal.h"
#include "sdl_renderer.h"
#include "glyph_atlas.h"
#include <SDL2/SDL.h>
#include <stddef.h>
#include <math.h>
#include <string.h>

// ============================================================================
// TERMINAL GEOMETRY SHADER (solid-colour quads: backgrounds, rects, cursors)
// ============================================================================

static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 pos;\n"
    "layout(location=1) in vec4 col;\n"
    "uniform mat4 proj;\n"
    "out vec4 vCol;\n"
    "void main(){gl_Position=proj*vec4(pos,0,1);vCol=col;}\n";

static const char *FS =
    "#version 330 core\n"
    "in vec4 vCol;\n"
    "out vec4 frag;\n"
    "void main(){frag=vCol;}\n";

// ============================================================================
// GLYPH ATLAS SHADER (textured quads sampling the glyph atlas)
// ============================================================================

static const char *GLYPH_VS =
    "#version 330 core\n"
    "layout(location=0) in vec2  a_pos;\n"
    "layout(location=1) in vec2  a_uv;\n"
    "layout(location=2) in vec4  a_tint;\n"
    "layout(location=3) in float a_color_glyph;\n"
    "uniform mat4 proj;\n"
    "out vec2  v_uv;\n"
    "out vec4  v_tint;\n"
    "out float v_color_glyph;\n"
    "void main() {\n"
    "  gl_Position   = proj * vec4(a_pos, 0.0, 1.0);\n"
    "  v_uv          = a_uv;\n"
    "  v_tint        = a_tint;\n"
    "  v_color_glyph = a_color_glyph;\n"
    "}\n";

// Grayscale glyphs: atlas .a is the coverage mask, multiplied by tint.
// Colour/emoji glyphs: full RGBA from atlas, tint alpha applied.
static const char *GLYPH_FS =
    "#version 330 core\n"
    "in vec2  v_uv;\n"
    "in vec4  v_tint;\n"
    "in float v_color_glyph;\n"
    "uniform sampler2D u_atlas;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "  vec4 s = texture(u_atlas, v_uv);\n"
    "  if (v_color_glyph > 0.5) {\n"
    "    frag = vec4(s.rgb, s.a * v_tint.a);\n"
    "  } else {\n"
    "    frag = vec4(v_tint.rgb, v_tint.a * s.a);\n"
    "  }\n"
    "}\n";

// ============================================================================
// FULLSCREEN QUAD SHADER
// ============================================================================

static const char *QUAD_VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 pos;\n"
    "out vec2 uv;\n"
    "void main(){\n"
    "  uv = pos * 0.5 + 0.5;\n"
    "  gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

// ============================================================================
// POST-PROCESS FRAGMENT SHADER
// ============================================================================

static const char *POST_FS =
    "#version 330 core\n"
    "in vec2 uv;\n"
    "out vec4 frag;\n"
    "uniform sampler2D tex;\n"
    "uniform int   mode;\n"
    "uniform float time;\n"
    "uniform vec2  resolution;\n"

    "float rand(vec2 co){\n"
    "  return fract(sin(dot(co,vec2(12.9898,78.233)))*43758.5453);\n"
    "}\n"

    "vec2 barrel(vec2 p, float k){\n"
    "  vec2 c = p - 0.5;\n"
    "  float r2 = dot(c,c);\n"
    "  return p + c * r2 * k;\n"
    "}\n"

    "vec4 mode_normal(vec2 u){\n"
    "  return texture(tex, u);\n"
    "}\n"

    "vec4 mode_crt(vec2 u){\n"
    "  vec2 bu = barrel(u, 0.12);\n"
    "  if(bu.x<0.0||bu.x>1.0||bu.y<0.0||bu.y>1.0)\n"
    "    return vec4(0.0,0.0,0.0,1.0);\n"
    "  vec4 col = texture(tex, bu);\n"
    "  float sl = sin(bu.y * resolution.y * 3.14159);\n"
    "  sl = clamp(sl, 0.0, 1.0);\n"
    "  col.rgb *= 0.75 + 0.25 * sl;\n"
    "  float shift = 0.0015;\n"
    "  col.r = texture(tex, bu + vec2( shift, 0.0)).r;\n"
    "  col.b = texture(tex, bu + vec2(-shift, 0.0)).b;\n"
    "  float glow = 0.08;\n"
    "  vec2 off = vec2(2.0/resolution.x, 2.0/resolution.y);\n"
    "  vec4 g = texture(tex,bu+vec2(off.x,0))+texture(tex,bu-vec2(off.x,0))\n"
    "          +texture(tex,bu+vec2(0,off.y))+texture(tex,bu-vec2(0,off.y));\n"
    "  col.rgb += g.rgb * glow;\n"
    "  vec2 vig = bu - 0.5;\n"
    "  col.rgb *= 1.0 - dot(vig,vig) * 1.8;\n"
    "  float noise = rand(bu + vec2(time*0.1)) * 0.03;\n"
    "  float flicker = 0.97 + 0.03 * rand(vec2(time*7.3, 0.0));\n"
    "  col.rgb = (col.rgb + noise) * flicker;\n"
    "  return clamp(col, 0.0, 1.0);\n"
    "}\n"

    "vec4 mode_lcd(vec2 u){\n"
    "  vec4 col = texture(tex, u);\n"
    "  vec2 px = u * resolution;\n"
    "  vec2 grid = abs(fract(px) - 0.5);\n"
    "  float hline = smoothstep(0.45, 0.5, grid.y);\n"
    "  float vline = smoothstep(0.45, 0.5, grid.x);\n"
    "  float gridmask = 1.0 - max(hline, vline) * 0.5;\n"
    "  col.rgb *= gridmask;\n"
    "  float sub = fract(px.x / 1.0);\n"
    "  vec3 stripe = vec3(\n"
    "    smoothstep(0.0,0.33,sub) * (1.0-smoothstep(0.33,0.66,sub)),\n"
    "    smoothstep(0.33,0.66,sub) * (1.0-smoothstep(0.66,1.0,sub)),\n"
    "    smoothstep(0.66,1.0,sub)\n"
    "  ) * 0.25 + 0.75;\n"
    "  col.rgb *= stripe;\n"
    "  col.rgb *= vec3(0.95, 1.0, 0.92);\n"
    "  vec2 vig = u - 0.5;\n"
    "  col.rgb *= 1.0 - dot(vig,vig) * 0.6;\n"
    "  return clamp(col, 0.0, 1.0);\n"
    "}\n"

    "vec4 mode_vhs(vec2 u){\n"
    "  float jitter_amt = 0.004;\n"
    "  float jitter = (rand(vec2(floor(u.y*resolution.y), time*3.0)) - 0.5) * jitter_amt;\n"
    "  float track_line = rand(vec2(floor(u.y * 12.0), floor(time*2.0)));\n"
    "  if(track_line > 0.97) jitter *= 8.0;\n"
    "  vec2 ju = vec2(u.x + jitter, u.y);\n"
    "  float bleed = 0.004;\n"
    "  float r = texture(tex, ju + vec2( bleed, 0.0)).r;\n"
    "  float g = texture(tex, ju                  ).g;\n"
    "  float b = texture(tex, ju - vec2( bleed, 0.0)).b;\n"
    "  vec4 col = vec4(r, g, b, 1.0);\n"
    "  float sl = sin(u.y * resolution.y * 3.14159);\n"
    "  col.rgb *= 0.80 + 0.20 * clamp(sl, 0.0, 1.0);\n"
    "  float noise = (rand(u + vec2(time)) - 0.5) * 0.06;\n"
    "  col.rgb += noise;\n"
    "  float band_y = fract(time * 0.17);\n"
    "  float band = smoothstep(0.015, 0.0, abs(u.y - band_y));\n"
    "  col.rgb += band * rand(vec2(u.x * 100.0, time)) * 0.3;\n"
    "  float luma = dot(col.rgb, vec3(0.299,0.587,0.114));\n"
    "  col.rgb = mix(col.rgb, vec3(luma), 0.25);\n"
    "  vec2 vig = u - 0.5;\n"
    "  col.rgb *= 1.0 - dot(vig,vig) * 1.2;\n"
    "  return clamp(col, 0.0, 1.0);\n"
    "}\n"

    "vec4 mode_focus(vec2 u){\n"
    "  vec4 col = texture(tex, u);\n"
    "  float fade = smoothstep(0.35, 0.65, u.y);\n"
    "  col.rgb *= mix(1.0, 0.15, fade);\n"
    "  return col;\n"
    "}\n"

    "vec4 mode_composite(vec2 u){\n"
    "  float px = u.x * resolution.x;\n"
    "  float py = u.y * resolution.y;\n"
    "  float subcarrier  = sin(px * 0.785398 + time * 6.0);\n"
    "  float subcarrier2 = sin(px * 0.785398 + time * 6.0 + 2.094);\n"
    "  float subcarrier3 = sin(px * 0.785398 + time * 6.0 + 4.189);\n"
    "  float chroma_spread = 3.5 / resolution.x;\n"
    "  float r_off = subcarrier  * chroma_spread;\n"
    "  float g_off = subcarrier2 * chroma_spread * 0.5;\n"
    "  float b_off = subcarrier3 * chroma_spread;\n"
    "  float r = texture(tex, vec2(u.x + r_off, u.y)).r;\n"
    "  float g = texture(tex, vec2(u.x + g_off, u.y)).g;\n"
    "  float b = texture(tex, vec2(u.x + b_off, u.y)).b;\n"
    "  vec4 col = vec4(r, g, b, 1.0);\n"
    "  float base_luma = dot(texture(tex, u).rgb, vec3(0.299,0.587,0.114));\n"
    "  float crawl = sin(px * 3.14159 + py * 3.14159 + time * 15.0) * 0.5 + 0.5;\n"
    "  float edge = abs(base_luma - dot(texture(tex, u + vec2(1.0/resolution.x,0)).rgb, vec3(0.299,0.587,0.114)));\n"
    "  col.rgb += crawl * edge * vec3(0.4, -0.2, 0.4) * 0.8;\n"
    "  float ring_amt = 0.12;\n"
    "  vec2 roff = vec2(4.0/resolution.x, 0.0);\n"
    "  vec4 ring_pre  = texture(tex, u - roff);\n"
    "  vec4 ring_post = texture(tex, u + roff);\n"
    "  float luma_diff = dot(ring_post.rgb - ring_pre.rgb, vec3(0.299,0.587,0.114));\n"
    "  col.rgb += luma_diff * ring_amt;\n"
    "  vec3 smear = vec3(0.0);\n"
    "  for(int i=-3; i<=3; i++){\n"
    "    vec3 s = texture(tex, u + vec2(float(i)*1.5/resolution.x, 0.0)).rgb;\n"
    "    float lum = dot(s, vec3(0.299,0.587,0.114));\n"
    "    smear += s - vec3(lum);\n"
    "  }\n"
    "  smear /= 7.0;\n"
    "  float cur_luma = dot(col.rgb, vec3(0.299,0.587,0.114));\n"
    "  col.rgb = vec3(cur_luma) + smear * 1.4;\n"
    "  float sl = sin(u.y * resolution.y * 3.14159);\n"
    "  col.rgb *= 0.82 + 0.18 * clamp(sl, 0.0, 1.0);\n"
    "  float lnoise   = (rand(u + vec2(time * 1.3)) - 0.5) * 0.03;\n"
    "  float cnoise_r = (rand(u + vec2(time * 2.1, 0.3)) - 0.5) * 0.06;\n"
    "  float cnoise_b = (rand(u + vec2(time * 1.7, 0.7)) - 0.5) * 0.06;\n"
    "  col.r += lnoise + cnoise_r;\n"
    "  col.g += lnoise;\n"
    "  col.b += lnoise + cnoise_b;\n"
    "  float line = floor(u.y * resolution.y);\n"
    "  float hsync_jitter = (rand(vec2(line, floor(time*4.0))) - 0.5) * 0.003;\n"
    "  col = mix(col, texture(tex, vec2(u.x + hsync_jitter, u.y)), 0.3);\n"
    "  vec2 vig = u - 0.5;\n"
    "  col.rgb *= 1.0 - dot(vig,vig) * 1.1;\n"
    "  return clamp(col, 0.0, 1.0);\n"
    "}\n"

    "vec4 mode_c64(vec2 u){\n"
    "  float border = 0.018;\n"
    "  bool in_border = u.x < border || u.x > 1.0-border || u.y < border || u.y > 1.0-border;\n"
    "  if (in_border) return vec4(0.263, 0.282, 0.800, 1.0);\n"
    "  vec2 inner = (u - border) / (1.0 - 2.0*border);\n"
    "  vec4 col = texture(tex, inner);\n"
    "  float luma = dot(col.rgb, vec3(0.299,0.587,0.114));\n"
    "  vec3 c64bg  = vec3(0.251, 0.251, 0.722);\n"
    "  vec3 c64fg  = vec3(0.686, 0.686, 1.000);\n"
    "  vec3 tinted = mix(c64bg, c64fg, luma);\n"
    "  float colorfulness = length(col.rgb - vec3(luma));\n"
    "  col.rgb = mix(tinted, col.rgb, clamp(colorfulness * 2.5, 0.0, 0.35));\n"
    "  float sl = sin(inner.y * resolution.y * 3.14159);\n"
    "  col.rgb *= 0.88 + 0.12 * clamp(sl, 0.0, 1.0);\n"
    "  vec2 off = vec2(2.0/resolution.x, 2.0/resolution.y);\n"
    "  vec4 g = texture(tex,inner+vec2(off.x,0))+texture(tex,inner-vec2(off.x,0))\n"
    "          +texture(tex,inner+vec2(0,off.y))+texture(tex,inner-vec2(0,off.y));\n"
    "  col.rgb += mix(c64fg, g.rgb, 0.5) * 0.04;\n"
    "  vec2 vig = inner - 0.5;\n"
    "  col.rgb *= 1.0 - dot(vig,vig) * 0.9;\n"
    "  return clamp(col, 0.0, 1.0);\n"
    "}\n"

    "float bloom_bright(vec3 c){\n"
    "  float luma = dot(c, vec3(0.299, 0.587, 0.114));\n"
    "  return max(0.0, luma - 0.08);\n"
    "}\n"
    "vec4 mode_bloom(vec2 u){\n"
    "  vec4 base = texture(tex, u);\n"
    "  vec2 px = vec2(1.0/resolution.x, 1.0/resolution.y);\n"
    "  vec3 glow = vec3(0.0);\n"
    "  float total_w = 0.0;\n"
    "#define BTAP(ox,oy,w) { vec3 s=texture(tex,u+vec2(ox,oy)*px).rgb; float b=bloom_bright(s); glow+=s*b*(w); total_w+=(w); }\n"
    "  BTAP( 5.0,  0.0, 1.0) BTAP(-5.0,  0.0, 1.0)\n"
    "  BTAP( 0.0,  5.0, 1.0) BTAP( 0.0, -5.0, 1.0)\n"
    "  BTAP( 3.5,  3.5, 0.7) BTAP(-3.5,  3.5, 0.7)\n"
    "  BTAP( 3.5, -3.5, 0.7) BTAP(-3.5, -3.5, 0.7)\n"
    "  BTAP(12.0,  0.0, 0.5) BTAP(-12.0, 0.0, 0.5)\n"
    "  BTAP( 0.0, 12.0, 0.5) BTAP( 0.0,-12.0, 0.5)\n"
    "  BTAP( 8.5,  8.5, 0.35) BTAP(-8.5, 8.5, 0.35)\n"
    "  BTAP( 8.5, -8.5, 0.35) BTAP(-8.5,-8.5, 0.35)\n"
    "#undef BTAP\n"
    "  glow = (total_w > 0.0) ? glow / total_w : vec3(0.0);\n"
    "  glow *= 4.5;\n"
    "  vec3 col = base.rgb + glow;\n"
    "  return vec4(col, base.a);\n"
    "}\n"

    "uniform sampler2D tex2;\n"
    "vec4 mode_ghosting(vec2 u){\n"
    "  vec4 cur   = texture(tex,  u);\n"
    "  vec4 ghost = texture(tex2, u);\n"
    "  vec3 col = max(cur.rgb, ghost.rgb);\n"
    "  vec3 tinted = ghost.rgb * vec3(0.5, 1.1, 0.7);\n"
    "  col = max(col, tinted * 0.6);\n"
    "  return clamp(vec4(col, 1.0), 0.0, 1.0);\n"
    "}\n"

    "vec4 mode_wireframe(vec2 u){\n"
    "  vec4 col = texture(tex, u);\n"
    "  col.rgb *= 0.18;\n"
    "  vec2 gpx = u * resolution;\n"
    "  vec2 cell_sz = vec2(9.0, 18.0);\n"
    "  vec2 gf = fract(gpx / cell_sz);\n"
    "  float gridline = 1.0 - smoothstep(0.0, 0.08, min(gf.x, gf.y));\n"
    "  col.rgb += vec3(0.0, gridline * 0.12, gridline * 0.18);\n"
    "  float bright = dot(col.rgb, vec3(1.0));\n"
    "  if(bright > 0.08) col.rgb = col.rgb * 1.6 + vec3(0.0, 0.05, 0.1);\n"
    "  return clamp(col, 0.0, 1.0);\n"
    "}\n"

    "void main(){\n"
    "  if      (mode==1) frag = mode_crt(uv);\n"
    "  else if (mode==2) frag = mode_lcd(uv);\n"
    "  else if (mode==3) frag = mode_vhs(uv);\n"
    "  else if (mode==4) frag = mode_focus(uv);\n"
    "  else if (mode==5) frag = mode_c64(uv);\n"
    "  else if (mode==6) frag = mode_composite(uv);\n"
    "  else if (mode==7) frag = mode_bloom(uv);\n"
    "  else if (mode==8) frag = mode_ghosting(uv);\n"
    "  else if (mode==9) frag = mode_wireframe(uv);\n"
    "  else              frag = mode_normal(uv);\n"
    "}\n";

// ============================================================================
// GL STATE
// ============================================================================

GLState      G            = {};
GlyphGLState GG           = {};
uint32_t     g_render_mode = 0;

// FBO state — composite (post-process) FBO
static GLuint s_fbo       = 0;
static GLuint s_fbo_tex   = 0;
static GLuint s_fbo_rb    = 0;
static int    s_fbo_w     = 0;
static int    s_fbo_h     = 0;

// Terminal cache FBO — only redrawn when terminal content changes
static GLuint s_term_fbo     = 0;
static GLuint s_term_fbo_tex = 0;
static GLuint s_term_fbo_rb  = 0;

// Ping-pong FBO
static GLuint s_ping_fbo     = 0;
static GLuint s_ping_fbo_tex = 0;
static GLuint s_ping_fbo_rb  = 0;

// BASIC graphics persistent FBO
static GLuint s_basic_fbo     = 0;
static GLuint s_basic_fbo_tex = 0;
static GLuint s_basic_fbo_rb  = 0;
// s_basic_has_content defined in sdl_renderer.cpp, declared extern via sdl_renderer.h

// Post-process quad
static GLuint s_quad_prog  = 0;
static GLuint s_quad_vao   = 0;
static GLuint s_quad_vbo   = 0;
static GLint  s_loc_tex    = -1;
static GLint  s_loc_mode   = -1;
static GLint  s_loc_time   = -1;
static GLint  s_loc_res    = -1;
static GLint  s_loc_tex2   = -1;

// Ghost FBO pair
static GLuint s_ghost_fbo       = 0;
static GLuint s_ghost_fbo_tex   = 0;
static GLuint s_ghost_fbo_rb    = 0;
static GLuint s_ghost2_fbo      = 0;
static GLuint s_ghost2_fbo_tex  = 0;
static GLuint s_ghost2_fbo_rb   = 0;

bool g_wireframe_cells = false;

// ============================================================================
// HELPERS
// ============================================================================

Mat4 mat4_ortho(float l, float r, float b, float t, float n, float f) {
    Mat4 m = {};
    m.m[0]  =  2.f/(r-l);
    m.m[5]  =  2.f/(t-b);
    m.m[10] = -2.f/(f-n);
    m.m[12] = -(r+l)/(r-l);
    m.m[13] = -(t+b)/(t-b);
    m.m[14] = -(f+n)/(f-n);
    m.m[15] =  1.f;
    return m;
}

static GLuint compile_shader(const char *src, GLenum type) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    int ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; glGetShaderInfoLog(s, sizeof(log), NULL, log);
        SDL_Log("[GL] shader error: %s\n", log);
    }
    return s;
}

static GLuint link_program(const char *vs_src, const char *fs_src) {
    GLuint vs = compile_shader(vs_src, GL_VERTEX_SHADER);
    GLuint fs = compile_shader(fs_src, GL_FRAGMENT_SHADER);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs); glDeleteShader(fs);
    return prog;
}

// ============================================================================
// FBO
// ============================================================================

static void make_color_fbo(GLuint *fbo, GLuint *tex, GLuint *rb, int w, int h) {
    if (*fbo) {
        glDeleteFramebuffers(1, fbo);
        glDeleteTextures(1, tex);
        glDeleteRenderbuffers(1, rb);
    }
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);

    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);

    glGenRenderbuffers(1, rb);
    glBindRenderbuffer(GL_RENDERBUFFER, *rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, *rb);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void create_fbo(int w, int h) {
    s_fbo_w = w; s_fbo_h = h;
    make_color_fbo(&s_fbo,       &s_fbo_tex,       &s_fbo_rb,       w, h);
    make_color_fbo(&s_term_fbo,  &s_term_fbo_tex,  &s_term_fbo_rb,  w, h);
    make_color_fbo(&s_ping_fbo,  &s_ping_fbo_tex,  &s_ping_fbo_rb,  w, h);
    make_color_fbo(&s_basic_fbo, &s_basic_fbo_tex, &s_basic_fbo_rb, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, s_basic_fbo);
    glClearColor(0,0,0,0);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_basic_has_content = false;
    make_color_fbo(&s_ghost_fbo,  &s_ghost_fbo_tex,  &s_ghost_fbo_rb,  w, h);
    make_color_fbo(&s_ghost2_fbo, &s_ghost2_fbo_tex, &s_ghost2_fbo_rb, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, s_term_fbo);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ============================================================================
// INIT
// ============================================================================

void gl_init_renderer(int w, int h) {
    if (g_use_sdl_renderer) { sdl_init_renderer(w, h); return; }
    glewExperimental = GL_TRUE;
    glewInit();

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // ── Solid-colour geometry program ────────────────────────────────────────
    G.prog = link_program(VS, FS);
    glGenVertexArrays(1, &G.vao);
    glGenBuffers(1, &G.vbo);
    glBindVertexArray(G.vao);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glBufferData(GL_ARRAY_BUFFER, MAX_VERTS * sizeof(Vertex), NULL, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex,x));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex,r));
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    glUseProgram(G.prog);
    G.proj_loc = glGetUniformLocation(G.prog, "proj");
    G.proj = mat4_ortho(0, (float)w, (float)h, 0, -1, 1);
    glUniformMatrix4fv(G.proj_loc, 1, GL_FALSE, G.proj.m);
    glUseProgram(0);

    G.cr = G.cg = G.cb = G.ca = 1.f;

    // ── Glyph atlas program ──────────────────────────────────────────────────
    GG.prog      = link_program(GLYPH_VS, GLYPH_FS);
    GG.proj_loc  = glGetUniformLocation(GG.prog, "proj");
    GG.atlas_loc = glGetUniformLocation(GG.prog, "u_atlas");

    glGenVertexArrays(1, &GG.vao);
    glGenBuffers(1, &GG.vbo);
    glBindVertexArray(GG.vao);
    glBindBuffer(GL_ARRAY_BUFFER, GG.vbo);
    glBufferData(GL_ARRAY_BUFFER, MAX_GLYPH_VERTS * sizeof(GlyphVertex), nullptr, GL_DYNAMIC_DRAW);
    // a_pos
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GlyphVertex),
                          (void*)offsetof(GlyphVertex, x));
    glEnableVertexAttribArray(0);
    // a_uv
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GlyphVertex),
                          (void*)offsetof(GlyphVertex, u));
    glEnableVertexAttribArray(1);
    // a_tint
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(GlyphVertex),
                          (void*)offsetof(GlyphVertex, tint_r));
    glEnableVertexAttribArray(2);
    // a_color_glyph
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(GlyphVertex),
                          (void*)offsetof(GlyphVertex, color_glyph));
    glEnableVertexAttribArray(3);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    // ── Post-process quad program ─────────────────────────────────────────────
    s_quad_prog = link_program(QUAD_VS, POST_FS);
    s_loc_tex   = glGetUniformLocation(s_quad_prog, "tex");
    s_loc_mode  = glGetUniformLocation(s_quad_prog, "mode");
    s_loc_time  = glGetUniformLocation(s_quad_prog, "time");
    s_loc_res   = glGetUniformLocation(s_quad_prog, "resolution");
    s_loc_tex2  = glGetUniformLocation(s_quad_prog, "tex2");

    float quad[] = { -1,-1,  1,-1,  1,1,  -1,-1,  1,1,  -1,1 };
    glGenVertexArrays(1, &s_quad_vao);
    glGenBuffers(1, &s_quad_vbo);
    glBindVertexArray(s_quad_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // ── FBO + atlas ───────────────────────────────────────────────────────────
    create_fbo(w, h);
    g_atlas.init();
}

void gl_resize_fbo(int w, int h) {
    if (g_use_sdl_renderer) { sdl_resize_fbo(w, h); return; }
    create_fbo(w, h);

    // Update projection for both programs
    G.proj = mat4_ortho(0, (float)w, (float)h, 0, -1, 1);
    glUseProgram(G.prog);
    glUniformMatrix4fv(G.proj_loc, 1, GL_FALSE, G.proj.m);
    glUseProgram(0);
}

// ============================================================================
// SOLID-COLOR VERTEX ACCUMULATOR
// ============================================================================

static Vertex s_accum[MAX_VERTS];
static int    s_accum_n = 0;

void gl_flush_verts(void) {
    if (g_use_sdl_renderer) { sdl_flush_verts(); return; }
    if (s_accum_n == 0) return;
    glUseProgram(G.prog);
    glUniformMatrix4fv(G.proj_loc, 1, GL_FALSE, G.proj.m);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, s_accum_n * sizeof(Vertex), s_accum);
    glBindVertexArray(G.vao);
    glDrawArrays(GL_TRIANGLES, 0, s_accum_n);
    glBindVertexArray(0);
    glUseProgram(0);
    s_accum_n = 0;
}

void draw_verts(Vertex *v, int n, GLenum /*mode*/) {
    if (g_use_sdl_renderer) { sdl_draw_verts(v, n); return; }
    if (n <= 0) return;
    if (s_accum_n + n > MAX_VERTS) {
        gl_flush_verts();
        if (n > MAX_VERTS) {
            glUseProgram(G.prog);
            glUniformMatrix4fv(G.proj_loc, 1, GL_FALSE, G.proj.m);
            glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
            int drawn = 0;
            while (drawn < n) {
                int chunk = (n - drawn < MAX_VERTS) ? (n - drawn) : MAX_VERTS;
                glBufferSubData(GL_ARRAY_BUFFER, 0, chunk * sizeof(Vertex), v + drawn);
                glBindVertexArray(G.vao);
                glDrawArrays(GL_TRIANGLES, 0, chunk);
                drawn += chunk;
            }
            glBindVertexArray(0);
            glUseProgram(0);
            return;
        }
    }
    memcpy(s_accum + s_accum_n, v, n * sizeof(Vertex));
    s_accum_n += n;
}

void draw_rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    if (g_use_sdl_renderer) {
        Vertex v[6] = {
            {x,   y,   r,g,b,a}, {x+w, y,   r,g,b,a}, {x+w, y+h, r,g,b,a},
            {x,   y,   r,g,b,a}, {x+w, y+h, r,g,b,a}, {x,   y+h, r,g,b,a},
        };
        sdl_draw_verts(v, 6);
        return;
    }
    Vertex v[6] = {
        {x,   y,   r,g,b,a}, {x+w, y,   r,g,b,a}, {x+w, y+h, r,g,b,a},
        {x,   y,   r,g,b,a}, {x+w, y+h, r,g,b,a}, {x,   y+h, r,g,b,a},
    };
    draw_verts(v, 6, GL_TRIANGLES);
}

// ============================================================================
// GLYPH VERTEX ACCUMULATOR
// ============================================================================

static GlyphVertex s_glyph_accum[MAX_GLYPH_VERTS];
static int         s_glyph_n = 0;

void gl_flush_glyphs(void) {
    if (g_use_sdl_renderer) { sdl_flush_glyphs(); return; }
    if (s_glyph_n == 0) return;

    // Flush any pending solid draws first so Z-order is correct
    gl_flush_verts();

    glUseProgram(GG.prog);
    glUniformMatrix4fv(GG.proj_loc, 1, GL_FALSE, G.proj.m);
    glUniform1i(GG.atlas_loc, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_atlas.tex);

    glBindBuffer(GL_ARRAY_BUFFER, GG.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, s_glyph_n * sizeof(GlyphVertex), s_glyph_accum);
    glBindVertexArray(GG.vao);
    glDrawArrays(GL_TRIANGLES, 0, s_glyph_n);
    glBindVertexArray(0);
    glUseProgram(0);

    s_glyph_n = 0;
}

void draw_glyph_verts(GlyphVertex *v, int n) {
    if (g_use_sdl_renderer) { sdl_draw_glyph_verts(v, n); return; }
    if (n <= 0) return;
    if (s_glyph_n + n > MAX_GLYPH_VERTS)
        gl_flush_glyphs();
    memcpy(s_glyph_accum + s_glyph_n, v, n * sizeof(GlyphVertex));
    s_glyph_n += n;
}

// ============================================================================
// GHOSTING
// ============================================================================

static int s_ghost_slot = 0;

void gl_update_ghost(int win_w, int win_h) {
    if (g_use_sdl_renderer) { sdl_update_ghost(win_w, win_h); return; }
    if (!(g_render_mode & RENDER_BIT_GHOSTING)) return;

    float fw = (float)win_w, fh = (float)win_h;

    GLuint read_tex  = s_ghost_slot ? s_ghost2_fbo_tex : s_ghost_fbo_tex;
    GLuint write_fbo = s_ghost_slot ? s_ghost_fbo      : s_ghost2_fbo;

    glBindFramebuffer(GL_FRAMEBUFFER, write_fbo);
    glViewport(0, 0, win_w, win_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(s_quad_prog);
    glUniform1i(s_loc_mode, RENDER_MODE_NORMAL);
    glUniform1f(s_loc_time, 0.f);
    glUniform2f(s_loc_res, fw, fh);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(s_loc_tex, 0);
    glBindVertexArray(s_quad_vao);

    glBlendColor(0.75f, 0.75f, 0.75f, 1.0f);
    glBlendFunc(GL_CONSTANT_COLOR, GL_ONE);
    glBindTexture(GL_TEXTURE_2D, read_tex);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBlendColor(0.35f, 0.35f, 0.35f, 1.0f);
    glBindTexture(GL_TEXTURE_2D, s_term_fbo_tex);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    s_ghost_slot ^= 1;

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendColor(1, 1, 1, 1);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ============================================================================
// TERM FRAME
// ============================================================================

void gl_begin_term_frame(int win_w, int win_h, float bg_r, float bg_g, float bg_b) {
    if (g_use_sdl_renderer) { sdl_begin_term_frame(win_w, win_h, bg_r, bg_g, bg_b); return; }
    s_accum_n   = 0;
    s_glyph_n   = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, s_term_fbo);
    glViewport(0, 0, win_w, win_h);
    (void)bg_r; (void)bg_g; (void)bg_b;
}

void gl_clear_term_frame(int win_w, int win_h, float bg_r, float bg_g, float bg_b) {
    if (g_use_sdl_renderer) { sdl_clear_term_frame(win_w, win_h, bg_r, bg_g, bg_b); return; }
    glBindFramebuffer(GL_FRAMEBUFFER, s_term_fbo);
    glViewport(0, 0, win_w, win_h);
    if (s_basic_has_content)
        glClearColor(0.f, 0.f, 0.f, 0.f);
    else
        glClearColor(bg_r, bg_g, bg_b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void gl_end_term_frame(void) {
    if (g_use_sdl_renderer) { sdl_end_term_frame(); return; }
    gl_flush_glyphs();  // flushes solid verts internally first, then glyphs
    gl_flush_verts();   // catch any trailing solid draws after last glyph batch
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ============================================================================
// BASIC GRAPHICS FBO
// ============================================================================

void gl_basic_begin(int win_w, int win_h) {
    if (g_use_sdl_renderer) {
        sdl_flush_verts();
        if (s_basic_tex) SDL_SetRenderTarget(g_sdl_renderer, s_basic_tex);
        return;
    }
    gl_flush_glyphs();
    gl_flush_verts();
    glBindFramebuffer(GL_FRAMEBUFFER, s_basic_fbo);
    glViewport(0, 0, win_w, win_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void gl_basic_end(void) {
    if (g_use_sdl_renderer) {
        sdl_flush_verts();
        SDL_SetRenderTarget(g_sdl_renderer, nullptr);
        s_basic_has_content = true;
        return;
    }
    gl_flush_glyphs();
    gl_flush_verts();
    s_basic_has_content = true;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void gl_basic_clear(int win_w, int win_h) {
    if (g_use_sdl_renderer) {
        if (s_basic_tex) {
            SDL_SetRenderTarget(g_sdl_renderer, s_basic_tex);
            SDL_SetRenderDrawColor(g_sdl_renderer, 0, 0, 0, 0);
            SDL_RenderClear(g_sdl_renderer);
            SDL_SetRenderTarget(g_sdl_renderer, nullptr);
        }
        s_basic_has_content = false;
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_basic_fbo);
    glViewport(0, 0, win_w, win_h);
    glClearColor(0,0,0,0);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_basic_has_content = false;
}

// ============================================================================
// FRAME — composite + post-process
// ============================================================================

void gl_begin_frame(void) {
    if (g_use_sdl_renderer) { sdl_begin_frame(); return; }
    s_accum_n = 0;
    s_glyph_n = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);

    // Always clear s_fbo before compositing — without this, prior frame
    // contents accumulate and text/graphics ghost across frames.
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, s_fbo_w, s_fbo_h);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (s_basic_has_content && s_basic_fbo_tex) {
        // Layer 1: blit BASIC graphics into s_fbo (opaque base).
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_basic_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
        glBlitFramebuffer(0, 0, s_fbo_w, s_fbo_h,
                          0, 0, s_fbo_w, s_fbo_h,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);

        // Layer 2: alpha-blend the terminal FBO on top so cells with a
        // transparent background (s_basic_palette_active) show BASIC beneath.
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
        glViewport(0, 0, s_fbo_w, s_fbo_h);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(s_quad_prog);
        glUniform1i(s_loc_mode, RENDER_MODE_NORMAL);
        glUniform1f(s_loc_time, 0.f);
        glUniform2f(s_loc_res, (float)s_fbo_w, (float)s_fbo_h);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(s_loc_tex, 0);
        glBindTexture(GL_TEXTURE_2D, s_term_fbo_tex);
        glBindVertexArray(s_quad_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
        glUseProgram(0);
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_term_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
        glBlitFramebuffer(0, 0, s_fbo_w, s_fbo_h,
                          0, 0, s_fbo_w, s_fbo_h,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
}

void gl_end_frame(float time, int win_w, int win_h) {
    if (g_use_sdl_renderer) { sdl_end_frame(time, win_w, win_h); return; }
    gl_flush_glyphs();
    gl_flush_verts();

    glDisable(GL_BLEND);
    glUseProgram(s_quad_prog);
    glUniform1f(s_loc_time, time);
    glUniform2f(s_loc_res,  (float)win_w, (float)win_h);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(s_loc_tex, 0);

    GLuint ghost_read_tex = s_ghost_slot ? s_ghost_fbo_tex : s_ghost2_fbo_tex;
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ghost_read_tex);
    glUniform1i(s_loc_tex2, 1);
    glActiveTexture(GL_TEXTURE0);

    g_wireframe_cells = (g_render_mode & RENDER_BIT_WIREFRAME) != 0;

    int passes[RENDER_MODE_COUNT];
    int npass = 0;
    for (int m = 1; m < RENDER_MODE_COUNT; m++) {
        if (g_render_mode & (1u << m))
            passes[npass++] = m;
    }
    if (npass == 0) { passes[0] = RENDER_MODE_NORMAL; npass = 1; }

    GLuint src_tex  = s_fbo_tex;
    GLuint fbo_pair[2] = { s_fbo,     s_ping_fbo     };
    GLuint tex_pair[2] = { s_fbo_tex, s_ping_fbo_tex };
    int    dst_slot    = 1;

    for (int pi = 0; pi < npass; pi++) {
        bool last = (pi == npass - 1);

        if (last) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, win_w, win_h);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo_pair[dst_slot]);
            glViewport(0, 0, win_w, win_h);
        }

        glBindTexture(GL_TEXTURE_2D, src_tex);
        glUniform1i(s_loc_mode, passes[pi]);
        glBindVertexArray(s_quad_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        if (!last) {
            src_tex  = tex_pair[dst_slot];
            dst_slot = 1 - dst_slot;
        }
    }

    glBindVertexArray(0);
    glUseProgram(0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
