// kitty_graphics.cpp — Kitty Graphics Protocol implementation
//
// Supported:
//   Transmission:  direct (base64 inline), single and multi-chunk
//   Formats:       f=32 RGBA, f=24 RGB, f=100 PNG (via stb_image)
//   Actions:       a=T (transmit+display), a=p (put/display), a=d (delete)
//   Placement:     virtual cell grid (c= cols, r= rows) or pixel (w=,h=)
//   Quiet:         q=1/2 suppresses OK/error responses
//
// Not yet supported:
//   Shared memory / file transmission (a=f, t=s/f)
//   Animation frames
//   Unicode placeholder rendering

#include "kitty_graphics.h"
#include "gl_renderer.h"
#include "term_pty.h"
#include "gl_terminal.h"

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

// ============================================================================
// stb_image — header-only, PNG/JPEG/etc decode
// ============================================================================
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop

// ============================================================================
// TEXTURED QUAD SHADER
// ============================================================================

static const char *IMG_VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 pos;\n"
    "layout(location=1) in vec2 tc;\n"
    "uniform mat4 proj;\n"
    "out vec2 vTC;\n"
    "void main(){ gl_Position = proj * vec4(pos,0,1); vTC = tc; }\n";

static const char *IMG_FS =
    "#version 330 core\n"
    "in vec2 vTC;\n"
    "out vec4 frag;\n"
    "uniform sampler2D img;\n"
    "void main(){ frag = texture(img, vTC); }\n";

static GLuint s_img_prog  = 0;
static GLuint s_img_vao   = 0;
static GLuint s_img_vbo   = 0;
static GLint  s_img_proj  = -1;
static GLint  s_img_tex   = -1;

// ============================================================================
// BASE64 DECODE
// ============================================================================

static const int8_t B64TAB[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

static std::vector<uint8_t> b64_decode(const char *src, int slen) {
    std::vector<uint8_t> out;
    out.reserve((slen * 3) / 4 + 4);
    uint32_t acc = 0;
    int bits = 0;
    for (int i = 0; i < slen; i++) {
        int8_t v = B64TAB[(uint8_t)src[i]];
        if (v < 0) continue;  // skip whitespace / padding
        acc = (acc << 6) | (uint8_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)(acc >> bits));
            acc &= (1u << bits) - 1;
        }
    }
    return out;
}

// ============================================================================
// IMAGE STORE
// ============================================================================

struct KittyImage {
    uint32_t id;
    GLuint   tex;
    int      pw, ph;     // pixel dimensions
};

struct KittyPlacement {
    uint32_t image_id;
    uint32_t placement_id; // p= (0 = unnamed)
    int      x_cell, y_cell;   // top-left cell in terminal grid
    int      src_x, src_y;     // source pixel offset within image
    int      src_w, src_h;     // source pixel region (0=full)
    int      cols, rows;       // display size in cells (0=auto)
    int      z_index;          // z= layering (-inf..+inf, 0=default over text)
};

// In-progress chunked transmission
struct KittyChunk {
    uint32_t         id;
    uint32_t         placement_id;
    int              fmt;   // 32,24,100
    int              pw, ph;
    std::vector<uint8_t> data;
    bool             display_when_done;
    // display params from first chunk
    int x_cell, y_cell, cols, rows, z_index;
    int quiet;
};

// Per-terminal state pointer stored in a map keyed by Terminal*
struct KittyTermState {
    std::vector<KittyPlacement> placements;
    KittyChunk                  pending;   // active chunked transfer
    bool                        has_pending = false;
};

static std::unordered_map<uint32_t, KittyImage>  s_images;   // id → image
static std::unordered_map<Terminal*, KittyTermState> s_terms;

static uint32_t s_next_auto_id = 1;

// ============================================================================
// HELPERS
// ============================================================================

static GLuint upload_texture(const uint8_t *pixels, int w, int h, bool has_alpha) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLenum fmt = has_alpha ? GL_RGBA : GL_RGB;
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

static GLuint build_image(const std::vector<uint8_t> &raw, int fmt, int pw, int ph) {
    if (fmt == 100) {
        // PNG (or JPEG/BMP via stb_image)
        int w, h, ch;
        uint8_t *dec = stbi_load_from_memory(raw.data(), (int)raw.size(), &w, &h, &ch, 4);
        if (!dec) {
            SDL_Log("[Kitty] stb_image failed: %s\n", stbi_failure_reason());
            return 0;
        }
        GLuint tex = upload_texture(dec, w, h, true);
        stbi_image_free(dec);
        return tex;
    }
    if (fmt == 32) {
        int expected = pw * ph * 4;
        if ((int)raw.size() < expected) {
            SDL_Log("[Kitty] f=32 data too short: got %d expected %d\n", (int)raw.size(), expected);
            return 0;
        }
        return upload_texture(raw.data(), pw, ph, true);
    }
    if (fmt == 24) {
        int expected = pw * ph * 3;
        if ((int)raw.size() < expected) {
            SDL_Log("[Kitty] f=24 data too short\n");
            return 0;
        }
        return upload_texture(raw.data(), pw, ph, false);
    }
    SDL_Log("[Kitty] unsupported format %d\n", fmt);
    return 0;
}

static void send_response(Terminal *t, uint32_t id, int quiet, bool ok, const char *msg) {
    if (quiet >= 2) return;
    if (quiet == 1 && ok) return;
    char buf[128];
    if (ok) {
        int n = snprintf(buf, sizeof(buf), "\x1b_Gi=%u;OK\x1b\\", id);
        term_write(t, buf, n);
    } else {
        int n = snprintf(buf, sizeof(buf), "\x1b_Gi=%u;ERROR:%s\x1b\\", id, msg ? msg : "unknown");
        term_write(t, buf, n);
    }
}

// ============================================================================
// PAYLOAD PARSER
// ============================================================================

// Parse "key=value,key=value,...;base64data" or "key=value,key=value"
struct ApcParams {
    int    a    = 0;    // action: 'T'=transmit+display, 'p'=put, 'd'=delete, 't'=transmit only, 'q'=query
    int    f    = 32;   // format: 32=RGBA,24=RGB,100=PNG
    int    m    = 0;    // more chunks: 0=last, 1=more coming
    uint32_t i  = 0;    // image id (0=auto-assign)
    uint32_t p  = 0;    // placement id
    int    s    = 0;    // pixel width
    int    v    = 0;    // pixel height
    int    S    = 0;    // data size hint (ignored)
    int    x    = 0;    // source x pixel
    int    y    = 0;    // source y pixel
    int    w    = 0;    // source pixel width
    int    h    = 0;    // source pixel height
    int    c    = 0;    // display columns
    int    r    = 0;    // display rows
    int    X    = 0;    // pixel x offset within cell
    int    Y    = 0;    // pixel y offset within cell
    int    z    = 0;    // z-index
    int    q    = 0;    // quiet: 0=always respond, 1=ok silent, 2=all silent
    int    d    = 0;    // delete target: 'a'=all,'i'=image,'p'=placement,'z'=zindex,'c'/'r'=col/row
    int    t_   = 'd';  // transmission medium: 'd'=direct,'f'=file,'s'=shm
    const char *payload = nullptr;
    int    payload_len  = 0;
};

static ApcParams parse_apc(const char *data, int len) {
    ApcParams p;
    // Find the semicolon separating keys from data
    int semi = -1;
    for (int i = 0; i < len; i++) {
        if (data[i] == ';') { semi = i; break; }
    }
    int kv_end = (semi >= 0) ? semi : len;
    if (semi >= 0) {
        p.payload     = data + semi + 1;
        p.payload_len = len - semi - 1;
    }
    // Parse key=value pairs
    const char *kp = data;
    const char *end = data + kv_end;
    while (kp < end) {
        const char *eq = nullptr;
        const char *comma = kp;
        while (comma < end && *comma != ',') comma++;
        for (const char *c = kp; c < comma; c++) {
            if (*c == '=') { eq = c; break; }
        }
        if (eq) {
            char key = *kp;
            int  val = atoi(eq + 1);
            // handle string value for 'a' and 't'
            switch (key) {
            case 'a': p.a    = *(eq+1); break;  // char value
            case 't': p.t_   = *(eq+1); break;
            case 'd': p.d    = *(eq+1); break;  // delete target char
            case 'f': p.f    = val; break;
            case 'm': p.m    = val; break;
            case 'i': p.i    = (uint32_t)val; break;
            case 'p': p.p    = (uint32_t)val; break;
            case 's': p.s    = val; break;
            case 'v': p.v    = val; break;
            case 'S': p.S    = val; break;
            case 'x': p.x    = val; break;
            case 'y': p.y    = val; break;
            case 'w': p.w    = val; break;
            case 'h': p.h    = val; break;
            case 'c': p.c    = val; break;
            case 'r': p.r    = val; break;
            case 'X': p.X    = val; break;
            case 'Y': p.Y    = val; break;
            case 'z': p.z    = val; break;
            case 'q': p.q    = val; break;
            }
        }
        kp = comma + 1;
    }
    return p;
}

// ============================================================================
// kitty_handle_apc — main entry point
// ============================================================================

void kitty_handle_apc(Terminal *t, const char *payload, int len) {
    // Must start with "G"
    if (len < 1 || payload[0] != 'G') return;
    payload++; len--;

    ApcParams p = parse_apc(payload, len);
    KittyTermState &ts = s_terms[t];

    // ---- QUERY action (a=q) ----
    if (p.a == 'q') {
        bool found = (p.i != 0 && s_images.count(p.i) > 0);
        send_response(t, p.i, p.q, found, found ? nullptr : "image not found");
        return;
    }

    // ---- DELETE action (a=d) ----
    if (p.a == 'd' || p.a == 'D') {
        auto &pv = ts.placements;
        int dtarget = p.d ? p.d : 'a';  // default: delete all visible
        switch (dtarget) {
        case 'a': case 'A':
            // Delete all placements (upper=also free image data)
            pv.clear();
            if (p.d == 'A') {
                if (p.i) { auto it=s_images.find(p.i); if(it!=s_images.end()){glDeleteTextures(1,&it->second.tex);s_images.erase(it);} }
                else { for(auto &kv:s_images) glDeleteTextures(1,&kv.second.tex); s_images.clear(); }
            }
            break;
        case 'i': case 'I':
            pv.erase(std::remove_if(pv.begin(),pv.end(),[&](const KittyPlacement &pl){return pl.image_id==p.i;}),pv.end());
            if (p.d == 'I') { auto it=s_images.find(p.i); if(it!=s_images.end()){glDeleteTextures(1,&it->second.tex);s_images.erase(it);} }
            break;
        case 'p': case 'P':
            pv.erase(std::remove_if(pv.begin(),pv.end(),[&](const KittyPlacement &pl){
                return pl.image_id==p.i && pl.placement_id==p.p; }),pv.end());
            break;
        case 'z': case 'Z':
            pv.erase(std::remove_if(pv.begin(),pv.end(),[&](const KittyPlacement &pl){return pl.z_index==p.z;}),pv.end());
            break;
        case 'c': case 'C':
            // Delete placements whose column range contains cursor col
            pv.erase(std::remove_if(pv.begin(),pv.end(),[&](const KittyPlacement &pl){
                return t->cur_col >= pl.x_cell && t->cur_col < pl.x_cell + (pl.cols?pl.cols:1);
            }),pv.end());
            break;
        case 'r': case 'R':
            pv.erase(std::remove_if(pv.begin(),pv.end(),[&](const KittyPlacement &pl){
                return t->cur_row >= pl.y_cell && t->cur_row < pl.y_cell + (pl.rows?pl.rows:1);
            }),pv.end());
            break;
        case 'x': case 'X':
            pv.erase(std::remove_if(pv.begin(),pv.end(),[&](const KittyPlacement &pl){return pl.x_cell==p.x;}),pv.end());
            break;
        case 'y': case 'Y':
            pv.erase(std::remove_if(pv.begin(),pv.end(),[&](const KittyPlacement &pl){return pl.y_cell==p.y;}),pv.end());
            break;
        case 'n': case 'N':
            // Delete newest (last) placement
            if (!pv.empty()) pv.pop_back();
            break;
        case 'o': case 'O':
            // Delete oldest (first) placement
            if (!pv.empty()) pv.erase(pv.begin());
            break;
        default:
            pv.clear();
            break;
        }
        return;
    }

    // ---- TRANSMIT or TRANSMIT+DISPLAY ----
    bool is_transmit = (p.a == 'T' || p.a == 't' || p.a == 0);
    bool is_display  = (p.a == 'T' || p.a == 'p' || p.a == 0);

    if (is_transmit && p.payload_len > 0) {
        if (ts.has_pending) {
            if (p.i != 0 && p.i != ts.pending.id) {
                // Different id — abandon previous
                ts.has_pending = false;
            }
        }
        if (!ts.has_pending) {
            ts.pending = {};
            ts.pending.id           = p.i ? p.i : s_next_auto_id++;
            ts.pending.placement_id = p.p;
            ts.pending.fmt          = p.f;
            ts.pending.pw           = p.s;
            ts.pending.ph           = p.v;
            ts.pending.display_when_done = is_display;
            ts.pending.x_cell  = t->cur_col;
            ts.pending.y_cell  = t->cur_row;
            ts.pending.cols    = p.c;
            ts.pending.rows    = p.r;
            ts.pending.z_index = p.z;
            ts.pending.quiet   = p.q;
            ts.has_pending     = true;
        }
        // Decode and append base64 payload
        std::vector<uint8_t> chunk = b64_decode(p.payload, p.payload_len);
        auto &pd = ts.pending.data;
        pd.insert(pd.end(), chunk.begin(), chunk.end());

        if (p.m == 0) {
            // Last chunk — build texture
            uint32_t img_id = ts.pending.id;
            // Delete old texture if re-using id
            auto it = s_images.find(img_id);
            if (it != s_images.end()) {
                glDeleteTextures(1, &it->second.tex);
                s_images.erase(it);
            }

            KittyImage img = {};
            img.id = img_id;

            if (ts.pending.fmt == 100) {
                // PNG/JPEG/BMP — decode once, store dims + upload
                int w, h, ch;
                uint8_t *dec = stbi_load_from_memory(
                    ts.pending.data.data(), (int)ts.pending.data.size(),
                    &w, &h, &ch, 4);
                if (dec) {
                    img.pw  = w; img.ph = h;
                    img.tex = upload_texture(dec, w, h, true);
                    stbi_image_free(dec);
                } else {
                    SDL_Log("[Kitty] stb_image failed: %s\n", stbi_failure_reason());
                }
            } else {
                img.pw  = ts.pending.pw;
                img.ph  = ts.pending.ph;
                img.tex = build_image(ts.pending.data, ts.pending.fmt,
                                      ts.pending.pw, ts.pending.ph);
            }

            if (img.tex) {
                s_images[img_id] = img;
                send_response(t, img_id, ts.pending.quiet, true, nullptr);

                if (ts.pending.display_when_done) {
                    KittyPlacement pl = {};
                    pl.image_id    = img_id;
                    pl.placement_id = ts.pending.placement_id;
                    pl.x_cell      = ts.pending.x_cell;
                    pl.y_cell      = ts.pending.y_cell;
                    pl.src_x       = p.x; pl.src_y = p.y;
                    pl.src_w       = p.w; pl.src_h = p.h;
                    pl.cols        = ts.pending.cols;
                    pl.rows        = ts.pending.rows;
                    pl.z_index     = ts.pending.z_index;
                    ts.placements.push_back(pl);

                    // Advance cursor past image rows per spec
                    int rows_used = pl.rows ? pl.rows :
                        (img.ph > 0 && t->cell_h > 0) ?
                            (int)((img.ph + (int)t->cell_h - 1) / (int)t->cell_h) : 1;
                    t->cur_row = SDL_min(t->cur_row + rows_used, t->rows - 1);
                    t->cur_col = 0;
                }
            } else {
                send_response(t, img_id, ts.pending.quiet, false, "decode failed");
            }
            ts.has_pending = false;
        }
    } else if (is_display && !is_transmit) {
        // a=p: place an already-uploaded image
        if (p.i == 0 || s_images.find(p.i) == s_images.end()) {
            send_response(t, p.i, p.q, false, "image not found");
            return;
        }
        KittyPlacement pl = {};
        pl.image_id     = p.i;
        pl.placement_id = p.p;
        pl.x_cell       = t->cur_col;
        pl.y_cell       = t->cur_row;
        pl.src_x        = p.x; pl.src_y = p.y;
        pl.src_w        = p.w; pl.src_h = p.h;
        pl.cols         = p.c; pl.rows  = p.r;
        pl.z_index      = p.z;
        ts.placements.push_back(pl);
        send_response(t, p.i, p.q, true, nullptr);
    }
}

// ============================================================================
// kitty_render
// ============================================================================

void kitty_render(Terminal *t, int ox, int oy) {
    auto tit = s_terms.find(t);
    if (tit == s_terms.end()) return;
    KittyTermState &ts = tit->second;
    if (ts.placements.empty()) return;

    // Flush accumulated colored-vertex geometry before switching shaders
    gl_flush_verts();

    // Sort by z_index so layering is correct; stable_sort preserves insertion order for ties
    std::stable_sort(ts.placements.begin(), ts.placements.end(),
        [](const KittyPlacement &a, const KittyPlacement &b){ return a.z_index < b.z_index; });

    glUseProgram(s_img_prog);
    glUniformMatrix4fv(s_img_proj, 1, GL_FALSE, G.proj.m);
    glUniform1i(s_img_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s_img_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_img_vbo);

    for (const KittyPlacement &pl : ts.placements) {
        auto iit = s_images.find(pl.image_id);
        if (iit == s_images.end()) continue;
        const KittyImage &img = iit->second;
        if (!img.tex) continue;

        // Pixel dimensions of display area
        float cw = t->cell_w, ch = t->cell_h;
        float disp_w = pl.cols ? pl.cols * cw : (float)img.pw;
        float disp_h = pl.rows ? pl.rows * ch : (float)img.ph;

        float dx = ox + pl.x_cell * cw;
        float dy = oy + pl.y_cell * ch;

        // Source UV region
        float u0 = 0.f, v0 = 0.f, u1 = 1.f, v1 = 1.f;
        if (img.pw > 0 && img.ph > 0) {
            int sw = pl.src_w ? pl.src_w : img.pw;
            int sh = pl.src_h ? pl.src_h : img.ph;
            u0 = (float)pl.src_x / img.pw;
            v0 = (float)pl.src_y / img.ph;
            u1 = u0 + (float)sw / img.pw;
            v1 = v0 + (float)sh / img.ph;
        }

        // pos(2) + tc(2) = 4 floats per vertex, 6 verts = 24 floats
        float verts[24] = {
            dx,        dy,        u0, v0,
            dx+disp_w, dy,        u1, v0,
            dx+disp_w, dy+disp_h, u1, v1,
            dx,        dy,        u0, v0,
            dx+disp_w, dy+disp_h, u1, v1,
            dx,        dy+disp_h, u0, v1,
        };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glBindTexture(GL_TEXTURE_2D, img.tex);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glBindVertexArray(0);
    glUseProgram(0);

    // Restore colored-vertex program state for subsequent draws
    glUseProgram(G.prog);
    glUniformMatrix4fv(G.proj_loc, 1, GL_FALSE, G.proj.m);
    glUseProgram(0);
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void kitty_init(void) {
    // Build shader
    auto compile = [](const char *src, GLenum type) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, NULL);
        glCompileShader(s);
        int ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[512]; glGetShaderInfoLog(s,512,NULL,log); SDL_Log("[Kitty] shader: %s\n",log); }
        return s;
    };
    GLuint vs = compile(IMG_VS, GL_VERTEX_SHADER);
    GLuint fs = compile(IMG_FS, GL_FRAGMENT_SHADER);
    s_img_prog = glCreateProgram();
    glAttachShader(s_img_prog, vs);
    glAttachShader(s_img_prog, fs);
    glLinkProgram(s_img_prog);
    glDeleteShader(vs); glDeleteShader(fs);

    s_img_proj = glGetUniformLocation(s_img_prog, "proj");
    s_img_tex  = glGetUniformLocation(s_img_prog, "img");

    // VAO / VBO for a single quad (updated every draw)
    glGenVertexArrays(1, &s_img_vao);
    glGenBuffers(1, &s_img_vbo);
    glBindVertexArray(s_img_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_img_vbo);
    glBufferData(GL_ARRAY_BUFFER, 24 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    // pos
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    // texcoord
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2*sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

void kitty_clear(Terminal *t) {
    auto it = s_terms.find(t);
    if (it != s_terms.end()) {
        it->second.placements.clear();
        it->second.has_pending = false;
    }
}

void kitty_scroll(Terminal *t, int lines) {
    auto it = s_terms.find(t);
    if (it == s_terms.end()) return;
    auto &pv = it->second.placements;
    for (auto &pl : pv) pl.y_cell -= lines;
    // Remove placements that have fully scrolled off the top
    pv.erase(std::remove_if(pv.begin(), pv.end(),
        [&](const KittyPlacement &pl) {
            int h = pl.rows ? pl.rows : 1;
            return (pl.y_cell + h) <= 0;
        }), pv.end());
}

void kitty_shutdown(void) {
    for (auto &kv : s_images)
        glDeleteTextures(1, &kv.second.tex);
    s_images.clear();
    s_terms.clear();
    if (s_img_prog) { glDeleteProgram(s_img_prog); s_img_prog = 0; }
    if (s_img_vao)  { glDeleteVertexArrays(1, &s_img_vao); s_img_vao = 0; }
    if (s_img_vbo)  { glDeleteBuffers(1, &s_img_vbo); s_img_vbo = 0; }
}
