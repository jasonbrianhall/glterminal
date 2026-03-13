#pragma once
#include <stdint.h>

// ============================================================================
// BOUNCING CIRCLE — GL overlay (no Cairo, no Visualizer dependency)
// Mirrors the logic from bouncingcircle.cpp but renders via draw_verts /
// draw_rect / draw_text, exactly like fight mode.
// ============================================================================

#define BC_MAX_TRAIL      500    // matches MAX_TRAIL_POINTS in original
#define BC_TRAIL_WIDTH    5.0f
#define BC_CIRCLE_SEGS   48       // smoothness of filled / outline circles
#define BC_ALPHA          0.55f   // overall overlay transparency

struct BCTrailPoint {
    float x, y;
    float r, g, b, a;  // a: full opacity when added, faded on draw
};

struct BCState {
    // Ball physics
    double pos_x, pos_y;
    double vel_x, vel_y;
    double radius;

    // Colour cycling
    double hue_offset;
    double bounce_counter;  // double to match original
    int    total_bounces;

    // Beat response
    double beat_magnitude;
    double beat_decay;
    double current_gravity;
    double current_growth_rate;

    // Trail ring buffer
    BCTrailPoint trail[BC_MAX_TRAIL];
    int          trail_head;   // next write index
    int          trail_count;  // total ever written (capped display at BC_MAX_TRAIL)
};

// Lifecycle
void bc_init(float win_w, float win_h);
void bc_tick(float win_w, float win_h, float dt);
void bc_render(float win_w, float win_h);

// Optional beat input (0..1)
void bc_beat(double magnitude);

// Enable / disable
void bc_set_enabled(bool en);
bool bc_get_enabled();
