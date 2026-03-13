#include "gl_bouncingcircle.h"
#include "gl_renderer.h"
#include "ft_font.h"
#include "term_color.h"
#include "crt_audio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// ============================================================================
// CONSTANTS  (mirrors bouncingcircle.cpp defines)
// ============================================================================

#define BC_BASE_GRAVITY        500.0
#define BC_DAMPING             0.99
#define BC_BASE_GROWTH         2.0
#define BC_BEAT_DECAY_RATE     3.0
#define BC_INIT_RADIUS_FACTOR  0.015  // matches INITIAL_BALL_RADIUS_FACTOR
#define BC_RESET_THRESHOLD     0.95   // ball resets when radius >= this * container_radius

// ============================================================================
// STATE
// ============================================================================

static BCState s_bc;
static bool    s_bc_enabled = false;
static bool    s_bc_inited  = false;
static float   s_bc_ww = 800.f, s_bc_wh = 600.f;

extern int g_font_size;   // owned by app_globals / term_ui

// ============================================================================
// HELPERS — HSV → RGB
// ============================================================================

static void hsv_to_rgb(double h, double s, double v,
                        double &r, double &g, double &b) {
    double c = v * s;
    double hp = fmod(h * 6.0, 6.0);
    double x = c * (1.0 - fabs(fmod(hp, 2.0) - 1.0));
    double m = v - c;
    if      (hp < 1.0) { r=c; g=x; b=0; }
    else if (hp < 2.0) { r=x; g=c; b=0; }
    else if (hp < 3.0) { r=0; g=c; b=x; }
    else if (hp < 4.0) { r=0; g=x; b=c; }
    else if (hp < 5.0) { r=x; g=0; b=c; }
    else               { r=c; g=0; b=x; }
    r+=m; g+=m; b+=m;
}

// ============================================================================
// PRIMITIVE HELPERS
// ============================================================================

// Thick line as two triangles
static void bc_line(float x1, float y1, float x2, float y2,
                    float cr, float cg, float cb, float a, float thick = BC_TRAIL_WIDTH) {
    float dx = x2-x1, dy = y2-y1;
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 0.5f) return;
    float nx = -dy/len * thick * 0.5f;
    float ny =  dx/len * thick * 0.5f;
    Vertex v[6];
    auto mk = [&](Vertex &vv, float vx, float vy) { vv = {vx,vy,cr,cg,cb,a}; };
    mk(v[0], x1+nx, y1+ny); mk(v[1], x1-nx, y1-ny); mk(v[2], x2-nx, y2-ny);
    mk(v[3], x1+nx, y1+ny); mk(v[4], x2-nx, y2-ny); mk(v[5], x2+nx, y2+ny);
    draw_verts(v, 6, GL_TRIANGLES);
}

// Filled circle (fan)
static void bc_circle_fill(float x, float y, float r,
                            float cr, float cg, float cb, float a) {
    const int S = BC_CIRCLE_SEGS;
    static std::vector<Vertex> v;
    v.resize(S * 3);
    for (int i = 0; i < S; i++) {
        float a0 = (i      / (float)S) * 6.28318f;
        float a1 = ((i+1)  / (float)S) * 6.28318f;
        v[i*3+0] = {x,                y,                cr,cg,cb,a};
        v[i*3+1] = {x+cosf(a0)*r,     y+sinf(a0)*r,     cr,cg,cb,a};
        v[i*3+2] = {x+cosf(a1)*r,     y+sinf(a1)*r,     cr,cg,cb,a};
    }
    draw_verts(v.data(), S*3, GL_TRIANGLES);
}

// Outline circle (ring of thick line segments)
static void bc_circle_outline(float x, float y, float r,
                               float cr, float cg, float cb, float a, float thick = 3.f) {
    const int S = BC_CIRCLE_SEGS;
    for (int i = 0; i < S; i++) {
        float a0 = (i     / (float)S) * 6.28318f;
        float a1 = ((i+1) / (float)S) * 6.28318f;
        bc_line(x + cosf(a0)*r, y + sinf(a0)*r,
                x + cosf(a1)*r, y + sinf(a1)*r,
                cr, cg, cb, a, thick);
    }
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void bc_init(float win_w, float win_h) {
    s_bc_ww = win_w;
    s_bc_wh = win_h;
    memset(&s_bc, 0, sizeof(s_bc));

    double container_radius = fmin(win_w, win_h) * 0.4;
    s_bc.pos_x  = win_w / 2.0;
    s_bc.pos_y  = win_h / 2.0;
    s_bc.vel_x  = 300.0;
    s_bc.vel_y  = 200.0;
    s_bc.radius = container_radius * BC_INIT_RADIUS_FACTOR;

    s_bc.current_gravity      = BC_BASE_GRAVITY;
    s_bc.current_growth_rate  = BC_BASE_GROWTH;

    s_bc_inited = true;
}

void bc_set_enabled(bool en) {
    s_bc_enabled = en;
    if (en && !s_bc_inited) bc_init(s_bc_ww, s_bc_wh);
}
bool bc_get_enabled() { return s_bc_enabled; }

void bc_beat(double magnitude) {
    if (magnitude < 0.0) magnitude = 0.0;
    if (magnitude > 1.0) magnitude = 1.0;
    s_bc.beat_magnitude = magnitude;
    s_bc.beat_decay     = BC_BEAT_DECAY_RATE;
}

// ============================================================================
// TICK  (pure physics — no GL)
// ============================================================================

void bc_tick(float win_w, float win_h, float dt) {
    if (!s_bc_enabled) return;

    // Re-init if window resized significantly
    if (!s_bc_inited ||
        fabsf(win_w - s_bc_ww) > 1.f || fabsf(win_h - s_bc_wh) > 1.f) {
        bc_init(win_w, win_h);
    }

    BCState &st = s_bc;

    // Beat decay
    if (st.beat_decay > 0.0) {
        st.beat_decay -= dt;
        if (st.beat_decay <= 0.0) { st.beat_decay = 0.0; st.beat_magnitude = 0.0; }
    } else {
        st.beat_magnitude = 0.0;
    }

    // Beat-responsive physics params
    st.current_gravity     = BC_BASE_GRAVITY - (st.beat_magnitude * BC_BASE_GRAVITY * 2.0);
    st.current_growth_rate = BC_BASE_GROWTH  + (st.beat_magnitude * 4.0);

    double cx = win_w / 2.0;
    double cy = win_h / 2.0;
    double container_r = fmin(win_w, win_h) * 0.4;

    // Integrate
    st.vel_y  += st.current_gravity * dt;
    st.pos_x  += st.vel_x * dt;
    st.pos_y  += st.vel_y * dt;

    // Container collision
    double dx   = st.pos_x - cx;
    double dy   = st.pos_y - cy;
    double dist = sqrt(dx*dx + dy*dy);
    double coll = container_r - st.radius;

    if (dist > coll && dist > 0.0001) {
        double nx = dx / dist;
        double ny = dy / dist;

        st.pos_x = cx + nx * coll;
        st.pos_y = cy + ny * coll;

        double vdotn = st.vel_x * nx + st.vel_y * ny;
        st.vel_x = (st.vel_x - 2.0 * vdotn * nx) * BC_DAMPING;
        st.vel_y = (st.vel_y - 2.0 * vdotn * ny) * BC_DAMPING;

        st.bounce_counter++;
        st.total_bounces++;
        st.radius += st.current_growth_rate;

        // Sound: pitch drops as ball grows
        float radius_frac = (float)(st.radius / (container_r * BC_RESET_THRESHOLD));
        bc_audio_bounce(radius_frac);

        // Subtle size-based velocity nudge to keep ball lively
        double sf = 1.0 + (st.radius / (container_r * BC_RESET_THRESHOLD)) * 0.08;
        st.vel_x *= sf;
        st.vel_y *= sf;

        // Reset when ball fills container
        if (st.radius >= container_r * BC_RESET_THRESHOLD) {
            st.radius        = container_r * BC_INIT_RADIUS_FACTOR;
            st.bounce_counter = 0;
            st.hue_offset    = fmod(st.hue_offset + 0.3, 1.0);
            st.pos_x = cx;  st.pos_y = cy;
            st.vel_x = 300.0; st.vel_y = 200.0;
        }
    }

    // Append trail point (ring buffer)
    int idx = st.trail_head;
    BCTrailPoint &tp = st.trail[idx];
    tp.x = (float)st.pos_x;
    tp.y = (float)st.pos_y;

    double hue = fmod((double)st.trail_count * 0.01 + st.hue_offset, 1.0);
    double r, g, b;
    hsv_to_rgb(hue, 0.9, 0.95, r, g, b);
    tp.r = (float)r;
    tp.g = (float)g;
    tp.b = (float)b;
    tp.a = 1.0f; // full opacity when added, faded during render by age

    st.trail_head  = (st.trail_head + 1) % BC_MAX_TRAIL;
    st.trail_count++;
}

// ============================================================================
// RENDER
// ============================================================================

void bc_render(float win_w, float win_h) {
    if (!s_bc_enabled || !s_bc_inited) return;

    BCState &st = s_bc;
    const float A = BC_ALPHA;   // master alpha

    float cx = win_w / 2.f;
    float cy = win_h / 2.f;
    float container_r = fminf(win_w, win_h) * 0.4f;

    // ── Container outline ──────────────────────────────────────────────────
    bc_circle_outline(cx, cy, container_r, 0.2f, 0.2f, 0.25f, A * 0.9f, 3.f);

    // ── Trail ──────────────────────────────────────────────────────────────
    int display_len = st.trail_count < BC_MAX_TRAIL ? st.trail_count : BC_MAX_TRAIL;

    for (int i = 0; i < display_len - 1; i++) {
        // Oldest→newest order through the ring buffer
        int base = (st.trail_count - display_len);
        int idx1 = (base + i    ) % BC_MAX_TRAIL;
        int idx2 = (base + i + 1) % BC_MAX_TRAIL;
        if (idx1 < 0) idx1 += BC_MAX_TRAIL;
        if (idx2 < 0) idx2 += BC_MAX_TRAIL;

        BCTrailPoint &p1 = st.trail[idx1];
        BCTrailPoint &p2 = st.trail[idx2];

        float age   = (float)i / (float)display_len;   // 0=oldest, 1=newest
        float alpha = p1.a * A * age * 0.9f;            // fade older segments, respect stored alpha

        // Blend colours between the two trail points
        float mr = (p1.r + p2.r) * 0.5f;
        float mg = (p1.g + p2.g) * 0.5f;
        float mb = (p1.b + p2.b) * 0.5f;

        bc_line(p1.x, p1.y, p2.x, p2.y, mr, mg, mb, alpha, BC_TRAIL_WIDTH);
    }

    // ── Ball ───────────────────────────────────────────────────────────────
    double hue = fmod((double)st.bounce_counter / 10.0 + st.hue_offset, 1.0);
    double br, bg, bb;
    hsv_to_rgb(hue, 0.9, 1.0, br, bg, bb);
    float fr = (float)br, fg = (float)bg, fb = (float)bb;
    float bx = (float)st.pos_x, by = (float)st.pos_y, brad = (float)st.radius;

    // Glow ring
    float glow_a = A * (0.3f + (float)st.beat_magnitude * 0.4f);
    bc_circle_outline(bx, by, brad + 4.f, fr, fg, fb, glow_a, 6.f);

    // Body
    bc_circle_fill(bx, by, brad, fr, fg, fb, A);

    // Specular highlight
    bc_circle_fill(bx - brad * 0.3f, by - brad * 0.3f,
                   brad * 0.35f, 1.f, 1.f, 1.f, A * 0.5f);

    // ── Info text ──────────────────────────────────────────────────────────
    int fsize = (g_font_size > 0) ? g_font_size : 14;
    char info[128];
    snprintf(info, sizeof(info),
             "Bounces: %d | Size: %.1f/%.1f | Gravity: %.0f | Growth: %.2f",
             st.total_bounces, st.radius, container_r * BC_RESET_THRESHOLD,
             st.current_gravity, st.current_growth_rate);

    float tw = measure_text(info, fsize);
    draw_text(info, win_w * 0.5f - tw * 0.5f, win_h - 12.f,
              fsize, fsize, 0.55f, 0.55f, 0.6f, A * 0.9f);

    // Beat flash label
    if (st.beat_magnitude > 0.02) {
        const char *beat_lbl = "\xe2\x99\xaa BEAT \xe2\x99\xaa";
        float bw = measure_text(beat_lbl, fsize);
        draw_text(beat_lbl, win_w * 0.5f - bw * 0.5f, (float)fsize + 4.f,
                  fsize, fsize,
                  1.f, 0.6f, 0.1f, (float)st.beat_magnitude * A);
    }
}
