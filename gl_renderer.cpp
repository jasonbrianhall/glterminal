#include "gl_renderer.h"
#include "gl_terminal.h"
#include <SDL2/SDL.h>
#include <stddef.h>
#include <math.h>
#include <string.h>

// ============================================================================
// TERMINAL GEOMETRY SHADER (unchanged)
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
    "uniform int   mode;\n"        // 0=normal,1=crt,2=lcd,3=vhs,4=focus
    "uniform float time;\n"
    "uniform vec2  resolution;\n"

    // ----------------------------------------------------------------
    // Helpers
    // ----------------------------------------------------------------
    "float rand(vec2 co){\n"
    "  return fract(sin(dot(co,vec2(12.9898,78.233)))*43758.5453);\n"
    "}\n"

    // Barrel distortion for CRT
    "vec2 barrel(vec2 p, float k){\n"
    "  vec2 c = p - 0.5;\n"
    "  float r2 = dot(c,c);\n"
    "  return p + c * r2 * k;\n"
    "}\n"

    // ----------------------------------------------------------------
    // NORMAL
    // ----------------------------------------------------------------
    "vec4 mode_normal(vec2 u){\n"
    "  return texture(tex, u);\n"
    "}\n"

    // ----------------------------------------------------------------
    // CRT
    // ----------------------------------------------------------------
    "vec4 mode_crt(vec2 u){\n"
    // Barrel distortion
    "  vec2 bu = barrel(u, 0.12);\n"
    "  if(bu.x<0.0||bu.x>1.0||bu.y<0.0||bu.y>1.0)\n"
    "    return vec4(0.0,0.0,0.0,1.0);\n"
    // Sample
    "  vec4 col = texture(tex, bu);\n"
    // Scanlines — dark band every other pixel row
    "  float sl = sin(bu.y * resolution.y * 3.14159);\n"
    "  sl = clamp(sl, 0.0, 1.0);\n"
    "  col.rgb *= 0.75 + 0.25 * sl;\n"
    // RGB subpixel shift (chromatic aberration)
    "  float shift = 0.0015;\n"
    "  col.r = texture(tex, bu + vec2( shift, 0.0)).r;\n"
    "  col.b = texture(tex, bu + vec2(-shift, 0.0)).b;\n"
    // Phosphor glow — additive blur approximation via 4 offset samples
    "  float glow = 0.08;\n"
    "  vec2 off = vec2(2.0/resolution.x, 2.0/resolution.y);\n"
    "  vec4 g = texture(tex,bu+vec2(off.x,0))+texture(tex,bu-vec2(off.x,0))\n"
    "          +texture(tex,bu+vec2(0,off.y))+texture(tex,bu-vec2(0,off.y));\n"
    "  col.rgb += g.rgb * glow;\n"
    // Vignette
    "  vec2 vig = bu - 0.5;\n"
    "  col.rgb *= 1.0 - dot(vig,vig) * 1.8;\n"
    // Subtle noise/flicker
    "  float noise = rand(bu + vec2(time*0.1)) * 0.03;\n"
    "  float flicker = 0.97 + 0.03 * rand(vec2(time*7.3, 0.0));\n"
    "  col.rgb = (col.rgb + noise) * flicker;\n"
    "  return clamp(col, 0.0, 1.0);\n"
    "}\n"

    // ----------------------------------------------------------------
    // LCD
    // ----------------------------------------------------------------
    "vec4 mode_lcd(vec2 u){\n"
    "  vec4 col = texture(tex, u);\n"
    // Pixel grid — darken at cell boundaries
    "  vec2 px = u * resolution;\n"
    "  vec2 grid = abs(fract(px) - 0.5);\n"
    "  float hline = smoothstep(0.45, 0.5, grid.y);\n"
    "  float vline = smoothstep(0.45, 0.5, grid.x);\n"
    "  float gridmask = 1.0 - max(hline, vline) * 0.5;\n"
    "  col.rgb *= gridmask;\n"
    // RGB subpixel stripe — tint each third of a pixel
    "  float sub = fract(px.x / 1.0);\n"
    "  vec3 stripe = vec3(\n"
    "    smoothstep(0.0,0.33,sub) * (1.0-smoothstep(0.33,0.66,sub)),\n"
    "    smoothstep(0.33,0.66,sub) * (1.0-smoothstep(0.66,1.0,sub)),\n"
    "    smoothstep(0.66,1.0,sub)\n"
    "  ) * 0.25 + 0.75;\n"
    "  col.rgb *= stripe;\n"
    // Slight green tint like old LCDs
    "  col.rgb *= vec3(0.95, 1.0, 0.92);\n"
    // Vignette
    "  vec2 vig = u - 0.5;\n"
    "  col.rgb *= 1.0 - dot(vig,vig) * 0.6;\n"
    "  return clamp(col, 0.0, 1.0);\n"
    "}\n"

    // ----------------------------------------------------------------
    // VHS
    // ----------------------------------------------------------------
    "vec4 mode_vhs(vec2 u){\n"
    // Horizontal tracking jitter
    "  float jitter_amt = 0.004;\n"
    "  float jitter = (rand(vec2(floor(u.y*resolution.y), time*3.0)) - 0.5) * jitter_amt;\n"
    // Occasional strong tracking line
    "  float track_line = rand(vec2(floor(u.y * 12.0), floor(time*2.0)));\n"
    "  if(track_line > 0.97) jitter *= 8.0;\n"
    "  vec2 ju = vec2(u.x + jitter, u.y);\n"
    // Color bleeding — sample R/G/B at different X offsets
    "  float bleed = 0.004;\n"
    "  float r = texture(tex, ju + vec2( bleed, 0.0)).r;\n"
    "  float g = texture(tex, ju                  ).g;\n"
    "  float b = texture(tex, ju - vec2( bleed, 0.0)).b;\n"
    "  vec4 col = vec4(r, g, b, 1.0);\n"
    // Scanlines
    "  float sl = sin(u.y * resolution.y * 3.14159);\n"
    "  col.rgb *= 0.80 + 0.20 * clamp(sl, 0.0, 1.0);\n"
    // Luma noise
    "  float noise = (rand(u + vec2(time)) - 0.5) * 0.06;\n"
    "  col.rgb += noise;\n"
    // Occasional horizontal white noise band
    "  float band_y = fract(time * 0.17);\n"
    "  float band = smoothstep(0.015, 0.0, abs(u.y - band_y));\n"
    "  col.rgb += band * rand(vec2(u.x * 100.0, time)) * 0.3;\n"
    // Desaturate slightly like worn tape
    "  float luma = dot(col.rgb, vec3(0.299,0.587,0.114));\n"
    "  col.rgb = mix(col.rgb, vec3(luma), 0.25);\n"
    // Vignette
    "  vec2 vig = u - 0.5;\n"
    "  col.rgb *= 1.0 - dot(vig,vig) * 1.2;\n"
    "  return clamp(col, 0.0, 1.0);\n"
    "}\n"

    // ----------------------------------------------------------------
    // FOCUS / DIM — dim top, brighten bottom
    // u.y=0 is TOP of screen, u.y=1 is BOTTOM
    // We want bottom 35% bright, top 65% dimmed
    // ----------------------------------------------------------------
    "vec4 mode_focus(vec2 u){\n"
    "  vec4 col = texture(tex, u);\n"
    // fade=0 at bottom (bright), fade=1 at top (dim)
    // smoothstep(0.35, 0.65, 1.0-u.y): when u.y close to 1 (bottom), 1-u.y~0 → fade=0
    "  float fade = smoothstep(0.35, 0.65, u.y);\n"
    "  col.rgb *= mix(1.0, 0.15, fade);\n"
    "  return col;\n"
    "}\n"

    // ----------------------------------------------------------------
    // BAD COMPOSITE — cheap composite video signal breakdown
    // Rainbow fringing via chroma phase errors, dot crawl, ringing
    // ----------------------------------------------------------------
    "vec4 mode_composite(vec2 u){\n"

    // --- Luma/chroma separation simulation ---
    // Sample a horizontal strip to fake NTSC 4:1:1 chroma subsampling
    "  float px = u.x * resolution.x;\n"
    "  float py = u.y * resolution.y;\n"

    // Chroma phase error — shift R/G/B by different amounts horizontally
    // and modulate the shift with a sine at the chroma subcarrier frequency (~3.58MHz → ~4px period)
    "  float subcarrier = sin(px * 0.785398 + time * 6.0);\n"  // 0.785 ≈ pi/4, 4px period
    "  float subcarrier2 = sin(px * 0.785398 + time * 6.0 + 2.094);\n"  // +120°
    "  float subcarrier3 = sin(px * 0.785398 + time * 6.0 + 4.189);\n"  // +240°

    // Chroma bleed — sample R,G,B at horizontally offset positions
    // The offset is modulated by the subcarrier to create phase-error rainbow
    "  float chroma_spread = 3.5 / resolution.x;\n"
    "  float r_off = subcarrier  * chroma_spread;\n"
    "  float g_off = subcarrier2 * chroma_spread * 0.5;\n"
    "  float b_off = subcarrier3 * chroma_spread;\n"

    "  float r = texture(tex, vec2(u.x + r_off, u.y)).r;\n"
    "  float g = texture(tex, vec2(u.x + g_off, u.y)).g;\n"
    "  float b = texture(tex, vec2(u.x + b_off, u.y)).b;\n"
    "  vec4 col = vec4(r, g, b, 1.0);\n"

    // --- Dot crawl — diagonal checkerboard shimmer at color edges ---
    // Classic NTSC artifact: color info leaks into luma channel
    "  float base_luma = dot(texture(tex, u).rgb, vec3(0.299,0.587,0.114));\n"
    "  float crawl = sin(px * 3.14159 + py * 3.14159 + time * 15.0) * 0.5 + 0.5;\n"
    "  float edge = abs(base_luma - dot(texture(tex, u + vec2(1.0/resolution.x,0)).rgb, vec3(0.299,0.587,0.114)));\n"
    "  col.rgb += crawl * edge * vec3(0.4, -0.2, 0.4) * 0.8;\n"

    // --- Ringing — pre/post echo on horizontal edges (Gibbs phenomenon) ---
    "  float ring_amt = 0.12;\n"
    "  vec2 roff = vec2(4.0/resolution.x, 0.0);\n"
    "  vec4 ring_pre  = texture(tex, u - roff);\n"
    "  vec4 ring_post = texture(tex, u + roff);\n"
    "  float luma_diff = dot(ring_post.rgb - ring_pre.rgb, vec3(0.299,0.587,0.114));\n"
    "  col.rgb += luma_diff * ring_amt;\n"

    // --- Chroma smear — low-pass filter chroma horizontally (bandwidth limit) ---
    "  vec3 smear = vec3(0.0);\n"
    "  for(int i=-3; i<=3; i++){\n"
    "    vec3 s = texture(tex, u + vec2(float(i)*1.5/resolution.x, 0.0)).rgb;\n"
    "    float lum = dot(s, vec3(0.299,0.587,0.114));\n"
    "    smear += s - vec3(lum);\n"  // chroma only
    "  }\n"
    "  smear /= 7.0;\n"
    "  float cur_luma = dot(col.rgb, vec3(0.299,0.587,0.114));\n"
    "  col.rgb = vec3(cur_luma) + smear * 1.4;\n"  // replace chroma with smeared version

    // --- Scanlines ---
    "  float sl = sin(u.y * resolution.y * 3.14159);\n"
    "  col.rgb *= 0.82 + 0.18 * clamp(sl, 0.0, 1.0);\n"

    // --- Y/C noise — separate luma and chroma noise like bad signal ---
    "  float lnoise = (rand(u + vec2(time * 1.3)) - 0.5) * 0.03;\n"
    "  float cnoise_r = (rand(u + vec2(time * 2.1, 0.3)) - 0.5) * 0.06;\n"
    "  float cnoise_b = (rand(u + vec2(time * 1.7, 0.7)) - 0.5) * 0.06;\n"
    "  col.r += lnoise + cnoise_r;\n"
    "  col.g += lnoise;\n"
    "  col.b += lnoise + cnoise_b;\n"

    // --- Horizontal sync jitter ---
    "  float line = floor(u.y * resolution.y);\n"
    "  float hsync_jitter = (rand(vec2(line, floor(time*4.0))) - 0.5) * 0.003;\n"
    "  col = mix(col, texture(tex, vec2(u.x + hsync_jitter, u.y)), 0.3);\n"

    // --- Vignette ---
    "  vec2 vig = u - 0.5;\n"
    "  col.rgb *= 1.0 - dot(vig,vig) * 1.1;\n"

    "  return clamp(col, 0.0, 1.0);\n"
    "}\n"
    // ----------------------------------------------------------------
    "vec4 mode_c64(vec2 u){\n"
    "  float border = 0.018;\n"
    "  bool in_border = u.x < border || u.x > 1.0-border || u.y < border || u.y > 1.0-border;\n"
    "  if (in_border) return vec4(0.263, 0.282, 0.800, 1.0);\n"
    // Remap UV to exclude border so content isn't clipped
    "  vec2 inner = (u - border) / (1.0 - 2.0*border);\n"
    "  vec4 col = texture(tex, inner);\n"
    "  float luma = dot(col.rgb, vec3(0.299,0.587,0.114));\n"
    "  vec3 c64bg  = vec3(0.251, 0.251, 0.722);\n"
    "  vec3 c64fg  = vec3(0.686, 0.686, 1.000);\n"
    "  vec3 tinted = mix(c64bg, c64fg, luma);\n"
    "  float colorfulness = length(col.rgb - vec3(luma));\n"
    "  col.rgb = mix(tinted, col.rgb, clamp(colorfulness * 2.5, 0.0, 0.35));\n"
    // Scanlines
    "  float sl = sin(inner.y * resolution.y * 3.14159);\n"
    "  col.rgb *= 0.88 + 0.12 * clamp(sl, 0.0, 1.0);\n"
    // Phosphor glow
    "  vec2 off = vec2(2.0/resolution.x, 2.0/resolution.y);\n"
    "  vec4 g = texture(tex,inner+vec2(off.x,0))+texture(tex,inner-vec2(off.x,0))\n"
    "          +texture(tex,inner+vec2(0,off.y))+texture(tex,inner-vec2(0,off.y));\n"
    "  col.rgb += mix(c64fg, g.rgb, 0.5) * 0.04;\n"
    // Vignette
    "  vec2 vig = inner - 0.5;\n"
    "  col.rgb *= 1.0 - dot(vig,vig) * 0.9;\n"
    "  return clamp(col, 0.0, 1.0);\n"
    "}\n"

    // ----------------------------------------------------------------
    // MAIN
    // ----------------------------------------------------------------
    "void main(){\n"
    "  if      (mode==1) frag = mode_crt(uv);\n"
    "  else if (mode==2) frag = mode_lcd(uv);\n"
    "  else if (mode==3) frag = mode_vhs(uv);\n"
    "  else if (mode==4) frag = mode_focus(uv);\n"
    "  else if (mode==5) frag = mode_c64(uv);\n"
    "  else if (mode==6) frag = mode_composite(uv);\n"
    "  else              frag = mode_normal(uv);\n"
    "}\n";

// ============================================================================
// GL STATE
// ============================================================================

GLState G = {};
int     g_render_mode = RENDER_MODE_NORMAL;


// FBO state
static GLuint s_fbo       = 0;
static GLuint s_fbo_tex   = 0;
static GLuint s_fbo_rb    = 0;  // depth/stencil renderbuffer
static int    s_fbo_w     = 0;
static int    s_fbo_h     = 0;

// Post-process quad
static GLuint s_quad_prog  = 0;
static GLuint s_quad_vao   = 0;
static GLuint s_quad_vbo   = 0;
static GLint  s_loc_tex    = -1;
static GLint  s_loc_mode   = -1;
static GLint  s_loc_time   = -1;
static GLint  s_loc_res    = -1;

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

static void create_fbo(int w, int h) {
    if (s_fbo) {
        glDeleteFramebuffers(1, &s_fbo);
        glDeleteTextures(1, &s_fbo_tex);
        glDeleteRenderbuffers(1, &s_fbo_rb);
    }
    s_fbo_w = w; s_fbo_h = h;

    glGenFramebuffers(1, &s_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);

    glGenTextures(1, &s_fbo_tex);
    glBindTexture(GL_TEXTURE_2D, s_fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_fbo_tex, 0);

    glGenRenderbuffers(1, &s_fbo_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, s_fbo_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_fbo_rb);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ============================================================================
// INIT
// ============================================================================

void gl_init_renderer(int w, int h) {
    glewExperimental = GL_TRUE;
    glewInit();

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Terminal geometry program
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

    // Post-process quad program
    s_quad_prog = link_program(QUAD_VS, POST_FS);
    s_loc_tex   = glGetUniformLocation(s_quad_prog, "tex");
    s_loc_mode  = glGetUniformLocation(s_quad_prog, "mode");
    s_loc_time  = glGetUniformLocation(s_quad_prog, "time");
    s_loc_res   = glGetUniformLocation(s_quad_prog, "resolution");

    // Fullscreen quad VAO (NDC -1..1)
    float quad[] = { -1,-1,  1,-1,  1,1,  -1,-1,  1,1,  -1,1 };
    glGenVertexArrays(1, &s_quad_vao);
    glGenBuffers(1, &s_quad_vbo);
    glBindVertexArray(s_quad_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // FBO
    create_fbo(w, h);
}

void gl_resize_fbo(int w, int h) {
    create_fbo(w, h);
}

// ============================================================================
// VERTEX ACCUMULATOR — one draw call per frame
// ============================================================================

static Vertex s_accum[MAX_VERTS];
static int    s_accum_n = 0;

void gl_flush_verts(void) {
    if (s_accum_n == 0) return;
    glUseProgram(G.prog);
    glUniformMatrix4fv(G.proj_loc, 1, GL_FALSE, G.proj.m);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, s_accum_n * sizeof(Vertex), s_accum);
    glBindVertexArray(G.vao);
    glDrawArrays(GL_TRIANGLES, 0, s_accum_n);
    s_accum_n = 0;
}

// Accumulates vertices; flushes automatically when the buffer is nearly full.
void draw_verts(Vertex *v, int n, GLenum /*mode*/) {
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
            return;
        }
    }
    memcpy(s_accum + s_accum_n, v, n * sizeof(Vertex));
    s_accum_n += n;
}

void draw_rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    Vertex v[6] = {
        {x,   y,   r,g,b,a}, {x+w, y,   r,g,b,a}, {x+w, y+h, r,g,b,a},
        {x,   y,   r,g,b,a}, {x+w, y+h, r,g,b,a}, {x,   y+h, r,g,b,a},
    };
    draw_verts(v, 6, GL_TRIANGLES);
}

// ============================================================================
// FRAME
// ============================================================================

void gl_begin_frame(void) {
    s_accum_n = 0;   // discard any leftover from a previous (incomplete) frame
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
}

void gl_end_frame(float time, int win_w, int win_h) {
    // Flush all accumulated geometry into the FBO in one draw call.
    gl_flush_verts();

    // Unbind FBO — render to screen
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, win_w, win_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    // Disable blending for the fullscreen blit so we get exact alpha
    glDisable(GL_BLEND);

    glUseProgram(s_quad_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_fbo_tex);
    glUniform1i(s_loc_tex,  0);
    glUniform1i(s_loc_mode, g_render_mode);
    glUniform1f(s_loc_time, time);
    glUniform2f(s_loc_res,  (float)win_w, (float)win_h);

    // Focus mode: pass cursor row as 0..1 UV

    glBindVertexArray(s_quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
