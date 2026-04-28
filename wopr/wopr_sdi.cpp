// wopr_sdi.cpp — WOPR Strategic Defense Initiative sub-game
//
// Classic Missile Command arcade game with:
//   - Radar-scope style cities (vertical line buildings)
//   - Proper explosion effects when cities are destroyed
//   - 6 cities like the original arcade game
//   - Missile trails
//   - Full wave progression

#include "wopr.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <random>
#include <algorithm>

#include "wopr_render.h"

// ─── Physics & Game Constants ─────────────────────────────────────────────

constexpr float PLAYFIELD_WIDTH = 640.f;
constexpr float PLAYFIELD_HEIGHT = 480.f;

constexpr int NUM_CITIES = 6;
constexpr float CITY_HEIGHT = 80.f;
constexpr int INITIAL_AMMO = 30;

constexpr float ENEMY_MISSILE_SPEED = 80.f;
constexpr float INTERCEPT_MISSILE_SPEED = 200.f;

constexpr float BLAST_RADIUS_BASE = 40.f;
constexpr float BLAST_GROW_SPEED = 200.f;

constexpr int CLUSTER_SPLITS = 3;

// ─── Game Objects ─────────────────────────────────────────────────────────

struct City {
    float x;
    float width;           // Building width variations
    bool alive = true;
    int ammo = INITIAL_AMMO;
    float explosion_time = 0.f;  // Time since destroyed (for explosion effect)
};

struct EnemyMissile {
    float x, y;
    float target_x, target_y;
    float vx, vy;
    float lifetime = 0.f;
    bool alive = true;
    int generation = 0;
    std::vector<std::pair<float, float>> trail;
};

struct InterceptMissile {
    float x, y;
    float start_x, start_y;
    float target_x, target_y;
    float travel_time = 0.f;
    float max_travel_time = 0.f;
    bool alive = true;
    int from_city = -1;
    std::vector<std::pair<float, float>> trail;
};

struct Explosion {
    float x, y;
    float radius = 0.f;
    float max_radius = BLAST_RADIUS_BASE;
    float time_alive = 0.f;
    bool expanding = true;
};

struct Particle {
    float x, y;
    float vx, vy;
    float life = 1.f;
};

struct Plane {
    float x, y;
    float vx;
    bool alive = false;
    float lifetime = 0.f;
    float bomb_timer = 0.f;
};

struct Bomb {
    float x, y;
    float vx, vy;
    bool alive = false;
    std::vector<std::pair<float, float>> trail;
};

// ─── Game State ───────────────────────────────────────────────────────────

struct WoprSdiState {
    float field_x0 = 0.f;
    float field_y0 = 0.f;
    float field_scale = 1.f;

    int wave = 1;
    int score = 0;
    int cities_alive = NUM_CITIES;
    float wave_timer = 0.f;
    float missile_spawn_timer = 0.f;
    
    std::vector<City> cities;
    std::vector<EnemyMissile> enemy_missiles;
    std::vector<InterceptMissile> intercept_missiles;
    std::vector<Explosion> explosions;
    std::vector<Particle> particles;
    std::vector<Plane> planes;
    std::vector<Bomb> bombs;

    int missiles_this_wave = 10;
    int missiles_spawned = 0;
    float missile_spawn_rate = 0.8f;

    char status_message[128];
    bool game_over = false;
    double game_over_timer = 0.0;
    int selected_city = 0;

    std::mt19937 rng;

    WoprSdiState() : rng(std::random_device{}()) {}
};

static WoprSdiState *sdi(WoprState *w) { return (WoprSdiState *)w->sub_state; }

// ─── Helpers ──────────────────────────────────────────────────────────────

static float distance(float x1, float y1, float x2, float y2) {
    float dx = x2 - x1, dy = y2 - y1;
    return sqrtf(dx*dx + dy*dy);
}

static float random_float(WoprSdiState *s, float min_val, float max_val) {
    std::uniform_real_distribution<float> dist(min_val, max_val);
    return dist(s->rng);
}

static int random_int(WoprSdiState *s, int min_val, int max_val) {
    std::uniform_int_distribution<int> dist(min_val, max_val);
    return dist(s->rng);
}

static void spawn_particles(WoprSdiState *s, float x, float y, int count) {
    for (int i = 0; i < count; i++) {
        Particle p;
        p.x = x; p.y = y;
        float angle = random_float(s, 0.f, 6.28f);
        float speed = random_float(s, 100.f, 300.f);
        p.vx = cosf(angle) * speed;
        p.vy = sinf(angle) * speed;
        p.life = 1.f;
        s->particles.push_back(p);
    }
}

static void spawn_explosion(WoprSdiState *s, float x, float y, float radius) {
    Explosion e;
    e.x = x;
    e.y = y;
    e.radius = 5.f;
    e.max_radius = radius;
    e.time_alive = 0.f;
    e.expanding = true;
    s->explosions.push_back(e);
    spawn_particles(s, x, y, 20);  // More particles for city destruction
}

// ─── Lifecycle ────────────────────────────────────────────────────────────

void wopr_sdi_enter(WoprState *w) {
    WoprSdiState *s = new WoprSdiState{};

    // Create 6 cities evenly spaced across bottom
    float city_spacing = PLAYFIELD_WIDTH / (NUM_CITIES + 1);
    for (int i = 0; i < NUM_CITIES; i++) {
        City c;
        c.x = (i + 1) * city_spacing;  // Properly space 6 cities
        c.width = 30.f + random_float(s, -10.f, 10.f);  // Consistent width
        c.alive = true;
        c.ammo = INITIAL_AMMO;
        c.explosion_time = 0.f;
        s->cities.push_back(c);
    }
    s->cities_alive = NUM_CITIES;

    s->wave = 1;
    s->score = 0;
    s->missiles_this_wave = 5 + (s->wave * 3);
    s->missiles_spawned = 0;
    s->missile_spawn_timer = 0.f;
    s->selected_city = 0;

    snprintf(s->status_message, sizeof(s->status_message), "WAVE %d", s->wave);
    w->sub_state = s;
}

void wopr_sdi_free(WoprState *w) {
    delete sdi(w);
    w->sub_state = nullptr;
}

// ─── Update ───────────────────────────────────────────────────────────────

void wopr_sdi_update(WoprState *w, double dt_sec) {
    WoprSdiState *s = sdi(w);
    if (!s) return;

    float dt = (float)dt_sec;

    if (s->game_over) {
        s->game_over_timer -= dt;
        if (s->game_over_timer < 0.0) {
            s->game_over = false;
            s->wave = 1;
            s->score = 0;
            for (auto &c : s->cities) {
                c.alive = true;
                c.ammo = INITIAL_AMMO;
                c.explosion_time = 0.f;
            }
            s->cities_alive = NUM_CITIES;
            s->missiles_spawned = 0;
            s->missile_spawn_timer = 0.f;
            s->missiles_this_wave = 5 + (s->wave * 3);
            s->enemy_missiles.clear();
            s->intercept_missiles.clear();
            s->explosions.clear();
            s->particles.clear();
            s->planes.clear();
            s->bombs.clear();
            snprintf(s->status_message, sizeof(s->status_message), "WAVE %d", s->wave);
        }
        return;
    }

    // ── Spawn enemy missiles ──
    if (s->missiles_spawned < s->missiles_this_wave) {
        s->missile_spawn_timer += dt;
        float spawn_interval = 1.f / s->missile_spawn_rate;
        
        while (s->missile_spawn_timer >= spawn_interval && s->missiles_spawned < s->missiles_this_wave) {
            s->missile_spawn_timer -= spawn_interval;
            
            EnemyMissile m;
            m.x = random_float(s, 20.f, PLAYFIELD_WIDTH - 20.f);
            m.y = 0.f;
            m.generation = 0;
            
            if (s->cities_alive > 0) {
                int city_idx = random_int(s, 0, NUM_CITIES - 1);
                if (s->cities[city_idx].alive) {
                    m.target_x = s->cities[city_idx].x;
                    m.target_y = PLAYFIELD_HEIGHT - 40.f;
                } else {
                    m.target_x = random_float(s, 50.f, PLAYFIELD_WIDTH - 50.f);  // Stay on screen
                    m.target_y = PLAYFIELD_HEIGHT - 40.f;
                }
            } else {
                m.target_x = random_float(s, 50.f, PLAYFIELD_WIDTH - 50.f);  // Stay on screen
                m.target_y = PLAYFIELD_HEIGHT - 40.f;
            }
            
            // Also clamp spawn position
            m.x = std::max(10.f, std::min(PLAYFIELD_WIDTH - 10.f, m.x));
            
            float dist = distance(m.x, m.y, m.target_x, m.target_y);
            m.lifetime = dist / ENEMY_MISSILE_SPEED;
            m.vx = (m.target_x - m.x) / m.lifetime;
            m.vy = (m.target_y - m.y) / m.lifetime;
            m.alive = true;
            
            s->enemy_missiles.push_back(m);
            s->missiles_spawned++;
        }
    }

    // ── Update enemy missiles ──
    for (auto &m : s->enemy_missiles) {
        if (!m.alive) continue;
        
        m.x += m.vx * dt;
        m.y += m.vy * dt;
        m.lifetime -= dt;
        
        // Add to trail
        if (m.trail.empty() || distance(m.trail.back().first, m.trail.back().second, m.x, m.y) > 3.f) {
            m.trail.push_back({m.x, m.y});
            if (m.trail.size() > 50) m.trail.erase(m.trail.begin());
        }
        
        // Reached target
        if (m.lifetime <= 0.f) {
            m.alive = false;
            spawn_explosion(s, m.target_x, m.target_y, BLAST_RADIUS_BASE + 20.f);
            
            // Check if it hit a city
            for (int i = 0; i < NUM_CITIES; i++) {
                if (!s->cities[i].alive) continue;
                if (distance(m.target_x, m.target_y, s->cities[i].x, PLAYFIELD_HEIGHT - 25.f) < 45.f) {
                    s->cities[i].alive = false;
                    s->cities[i].explosion_time = 0.f;
                    s->cities_alive--;
                    // LOUD CITY DESTRUCTION SOUND
                    wopr_audio_play_keystroke();
                    wopr_audio_play_keystroke();
                    wopr_audio_play_keystroke();
                    break;
                }
            }
        }
    }

    // ── Update intercept missiles ──
    for (auto &im : s->intercept_missiles) {
        if (!im.alive) continue;
        
        im.travel_time += dt;
        float t = std::min(1.f, im.travel_time / im.max_travel_time);
        
        im.x = im.start_x + (im.target_x - im.start_x) * t;
        im.y = im.start_y + (im.target_y - im.start_y) * t;
        
        // Add to trail
        if (im.trail.empty() || distance(im.trail.back().first, im.trail.back().second, im.x, im.y) > 2.f) {
            im.trail.push_back({im.x, im.y});
            if (im.trail.size() > 40) im.trail.erase(im.trail.begin());
        }
        
        if (im.travel_time >= im.max_travel_time) {
            im.alive = false;
            float blast_rad = BLAST_RADIUS_BASE;
            spawn_explosion(s, im.target_x, im.target_y, blast_rad);
            wopr_audio_play_keystroke();
            
            // Destroy nearby enemy missiles
            for (auto &em : s->enemy_missiles) {
                if (!em.alive) continue;
                float d = distance(im.target_x, im.target_y, em.x, em.y);
                if (d < blast_rad) {
                    em.alive = false;
                    s->score += 10;
                    
                    if (em.generation == 0) {
                        for (int i = 0; i < CLUSTER_SPLITS; i++) {
                            EnemyMissile cluster;
                            float angle = (6.28f / CLUSTER_SPLITS) * i;
                            cluster.x = em.x + cosf(angle) * 20.f;
                            cluster.y = em.y + sinf(angle) * 20.f;
                            cluster.generation = 1;
                            
                            cluster.target_x = em.target_x + random_float(s, -30.f, 30.f);
                            cluster.target_y = em.target_y;
                            
                            float dist = distance(cluster.x, cluster.y, cluster.target_x, cluster.target_y);
                            cluster.lifetime = dist / (ENEMY_MISSILE_SPEED * 0.7f);
                            cluster.vx = (cluster.target_x - cluster.x) / cluster.lifetime;
                            cluster.vy = (cluster.target_y - cluster.y) / cluster.lifetime;
                            cluster.alive = true;
                            
                            s->enemy_missiles.push_back(cluster);
                        }
                    }
                }
            }
            
            // Destroy nearby bombs
            for (auto &b : s->bombs) {
                if (!b.alive) continue;
                float d = distance(im.target_x, im.target_y, b.x, b.y);
                if (d < blast_rad) {
                    b.alive = false;
                    s->score += 15;  // Bonus for destroying bombs
                }
            }
        }
    }

    // ── Spawn planes occasionally ──
    static float plane_spawn_timer = 0.f;
    plane_spawn_timer += dt;
    if (plane_spawn_timer > 8.f && random_int(s, 0, 99) < 15) {  // ~15% chance every 8 seconds
        plane_spawn_timer = 0.f;
        Plane p;
        if (random_int(s, 0, 1) == 0) {
            p.x = -20.f;
            p.vx = 150.f;  // Right
        } else {
            p.x = PLAYFIELD_WIDTH + 20.f;
            p.vx = -150.f;  // Left
        }
        p.y = random_float(s, 50.f, PLAYFIELD_HEIGHT / 2.f);
        p.alive = true;
        p.lifetime = 0.f;
        s->planes.push_back(p);
    }

    // ── Update planes ──
    for (auto &p : s->planes) {
        if (!p.alive) continue;
        p.x += p.vx * dt;
        p.lifetime += dt;
        p.bomb_timer += dt;
        
        // Drop bomb occasionally
        if (p.bomb_timer > 2.f && random_int(s, 0, 100) < 20) {
            p.bomb_timer = 0.f;
            Bomb b;
            b.x = p.x;
            b.y = p.y;
            b.vx = p.vx * 0.5f;  // Inherit some plane velocity
            b.vy = 80.f;  // Fall down
            b.alive = true;
            s->bombs.push_back(b);
        }
        
        if (p.lifetime > 20.f || p.x < -50.f || p.x > PLAYFIELD_WIDTH + 50.f) {
            p.alive = false;
        }
    }
    s->planes.erase(
        std::remove_if(s->planes.begin(), s->planes.end(),
                       [](const Plane &p) { return !p.alive; }),
        s->planes.end()
    );

    // ── Update bombs ──
    for (auto &b : s->bombs) {
        if (!b.alive) continue;
        
        b.x += b.vx * dt;
        b.y += b.vy * dt;
        b.vy += 150.f * dt;  // Gravity
        
        // Add to trail
        if (b.trail.empty() || distance(b.trail.back().first, b.trail.back().second, b.x, b.y) > 3.f) {
            b.trail.push_back({b.x, b.y});
            if (b.trail.size() > 30) b.trail.erase(b.trail.begin());
        }
        
        // Check if it hit a city
        if (b.y >= PLAYFIELD_HEIGHT - 50.f) {
            b.alive = false;
            spawn_explosion(s, b.x, b.y, BLAST_RADIUS_BASE + 30.f);
            
            for (int i = 0; i < NUM_CITIES; i++) {
                if (!s->cities[i].alive) continue;
                if (distance(b.x, b.y, s->cities[i].x, PLAYFIELD_HEIGHT - 25.f) < 50.f) {
                    s->cities[i].alive = false;
                    s->cities[i].explosion_time = 0.f;
                    s->cities_alive--;
                    wopr_audio_play_keystroke();
                    wopr_audio_play_keystroke();
                    wopr_audio_play_keystroke();
                    break;
                }
            }
        }
    }
    s->bombs.erase(
        std::remove_if(s->bombs.begin(), s->bombs.end(),
                       [](const Bomb &b) { return !b.alive; }),
        s->bombs.end()
    );

    // ── Update explosions ──
    for (auto &e : s->explosions) {
        e.time_alive += dt;
        
        if (e.expanding) {
            e.radius += BLAST_GROW_SPEED * dt;
            if (e.radius >= e.max_radius) {
                e.radius = e.max_radius;
                e.expanding = false;
            }
        } else {
            e.radius -= BLAST_GROW_SPEED * dt * 0.7f;
            if (e.radius < 1.f) e.radius = 1.f;
        }
    }
    s->explosions.erase(
        std::remove_if(s->explosions.begin(), s->explosions.end(),
                       [](const Explosion &e) { return e.time_alive > 1.5f; }),
        s->explosions.end()
    );

    // ── Update city destruction effects ──
    for (auto &c : s->cities) {
        if (!c.alive) {
            c.explosion_time += dt;
        }
    }

    // ── Update particles ──
    for (auto &p : s->particles) {
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.vy += 200.f * dt;
        p.life -= dt * 1.5f;
    }
    s->particles.erase(
        std::remove_if(s->particles.begin(), s->particles.end(),
                       [](const Particle &p) { return p.life < 0.f; }),
        s->particles.end()
    );

    // Clean up
    s->enemy_missiles.erase(
        std::remove_if(s->enemy_missiles.begin(), s->enemy_missiles.end(),
                       [](const EnemyMissile &m) { return !m.alive; }),
        s->enemy_missiles.end()
    );
    s->intercept_missiles.erase(
        std::remove_if(s->intercept_missiles.begin(), s->intercept_missiles.end(),
                       [](const InterceptMissile &m) { return !m.alive; }),
        s->intercept_missiles.end()
    );

    // ── Check wave complete ──
    if (s->missiles_spawned >= s->missiles_this_wave && 
        s->enemy_missiles.empty() && 
        s->intercept_missiles.empty() &&
        s->explosions.empty()) {
        
        if (s->cities_alive > 0) {
            s->wave++;
            s->missiles_this_wave = 5 + (s->wave * 3);
            s->missiles_spawned = 0;
            s->missile_spawn_timer = 0.f;
            s->missile_spawn_rate = 0.6f + (s->wave * 0.2f);
            for (auto &c : s->cities) {
                if (c.alive) c.ammo = INITIAL_AMMO + (s->wave - 1) * 5;
            }
            snprintf(s->status_message, sizeof(s->status_message), "WAVE %d", s->wave);
        } else {
            s->game_over = true;
            s->game_over_timer = 3.0;
            snprintf(s->status_message, sizeof(s->status_message), 
                     "CITIES DESTROYED. SCORE: %d", s->score);
        }
    }
}

// ─── Render ───────────────────────────────────────────────────────────────

void wopr_sdi_render(WoprState *w, int ox, int oy, int cw, int ch, int cols) {
    WoprSdiState *s = sdi(w);
    if (!s) return;

    const float scale = 1.f;
    float x0 = (float)ox;
    float y0 = (float)oy;
    float fch = (float)ch;
    float fcw = (float)cw;

    gl_draw_text("MISSILE DEFENSE SYSTEM", x0, y0, 0.f, 1.f, 0.4f, 1.f, scale);
    y0 += fch * 1.5f;

    char info[256];
    snprintf(info, sizeof(info), "WAVE: %d   SCORE: %d   CITIES: %d", 
             s->wave, s->score, s->cities_alive);
    gl_draw_text(info, x0, y0, 0.f, 0.8f, 0.8f, 1.f, scale);
    y0 += fch * 1.5f;

    // ── Calculate field dimensions ──
    float available_w = (cols > 0) ? (float)cols * fcw * 0.98f : 1024.f;
    float available_h = 600.f;
    
    float scale_x = available_w / PLAYFIELD_WIDTH;
    float scale_y = available_h / PLAYFIELD_HEIGHT;
    s->field_scale = std::min(scale_x, scale_y);

    s->field_x0 = x0;
    s->field_y0 = y0;

    float field_w = PLAYFIELD_WIDTH * s->field_scale;
    float field_h = PLAYFIELD_HEIGHT * s->field_scale;

    auto world_to_screen = [&](float wx, float wy) {
        return std::make_pair(
            s->field_x0 + wx * s->field_scale,
            s->field_y0 + wy * s->field_scale
        );
    };

    // ── Background ──
    gl_draw_rect(s->field_x0, s->field_y0, field_w, field_h, 
                 0.02f, 0.02f, 0.05f, 0.95f);
    
    // ── Border ──
    gl_draw_rect(s->field_x0 - 1, s->field_y0 - 1, field_w + 2, 1.f,
                 0.f, 0.5f, 0.8f, 1.f);
    gl_draw_rect(s->field_x0 - 1, s->field_y0 + field_h, field_w + 2, 1.f,
                 0.f, 0.5f, 0.8f, 1.f);
    gl_draw_rect(s->field_x0 - 1, s->field_y0, 1.f, field_h,
                 0.f, 0.5f, 0.8f, 1.f);
    gl_draw_rect(s->field_x0 + field_w, s->field_y0, 1.f, field_h,
                 0.f, 0.5f, 0.8f, 1.f);

    // ── Military bases cityscape (bottom) ──
    float city_baseline = PLAYFIELD_HEIGHT - 25.f;
    for (int i = 0; i < NUM_CITIES; i++) {
        auto [sx, sy] = world_to_screen(s->cities[i].x, city_baseline);
        float building_w = 28.f * s->field_scale;  // Smaller
        float building_h = 45.f * s->field_scale;  // Smaller

        if (s->cities[i].alive) {
            // Draw building structure (green with dark roof)
            gl_draw_rect(sx - building_w/2, sy - building_h, building_w, building_h,
                         0.1f, 0.4f, 0.1f, 0.95f);  // Dark green base
            
            // Lighter green main structure
            gl_draw_rect(sx - building_w/2 + 1, sy - building_h + 2, building_w - 2, building_h - 3,
                         0.2f, 0.8f, 0.2f, 0.9f);  // Bright green
            
            // Windows grid - 2 columns x 3 rows (static pattern, no flashing)
            float window_w = building_w / 5.f;
            float window_h = building_h / 6.f;
            float start_x = sx - building_w/2 + building_w * 0.15f;
            float start_y = sy - building_h + building_h * 0.15f;
            
            for (int row = 0; row < 3; row++) {
                for (int col = 0; col < 2; col++) {
                    float wx = start_x + col * (window_w * 1.8f);
                    float wy = start_y + row * (window_h * 1.6f);
                    
                    // Static window pattern - always the same (no random)
                    float r = 0.2f;  // Dark windows
                    float g = 0.3f;
                    float b = 0.1f;
                    
                    gl_draw_rect(wx, wy, window_w * 0.8f, window_h * 0.8f,
                                r, g, b, 0.9f);
                }
            }
            
            // Roof antenna
            gl_draw_rect(sx - 1, sy - building_h - 6 * s->field_scale, 2.f, 6 * s->field_scale,
                         0.5f, 0.5f, 0.5f, 0.8f);
            
            // Ammo counter
            char ammo_str[16];
            snprintf(ammo_str, sizeof(ammo_str), "%d", s->cities[i].ammo);
            float ammo_w = gl_text_width(ammo_str, scale);
            gl_draw_text(ammo_str, sx - ammo_w/2, sy + fch * 0.5f, 
                        0.0f, 1.f, 0.5f, 1.f, scale);
        } else {
            // Destroyed city - show explosion effect
            float exp_time = s->cities[i].explosion_time;
            if (exp_time < 0.8f) {  // Explosion lasts 0.8 seconds
                float explosion_radius = (exp_time / 0.8f) * 50.f * s->field_scale;
                float alpha = 1.f - (exp_time / 0.8f);
                
                // Expanding red explosion bloom
                gl_draw_rect(sx - explosion_radius, sy - explosion_radius - building_h/2, 
                            explosion_radius * 2, explosion_radius * 2,
                            1.0f, 0.4f, 0.0f, alpha * 0.8f);
                gl_draw_rect(sx - explosion_radius*0.6f, sy - explosion_radius*0.6f - building_h/2, 
                            explosion_radius * 1.2f, explosion_radius * 1.2f,
                            1.0f, 0.8f, 0.2f, alpha);
            } else {
                // Destroyed - draw rubble (dark)
                gl_draw_rect(sx - building_w/2, sy - building_h, building_w, building_h,
                             0.3f, 0.2f, 0.2f, 0.7f);
            }
        }
    }

    // ── Missile trails ──
    for (const auto &m : s->enemy_missiles) {
        for (size_t i = 1; i < m.trail.size(); i++) {
            auto [sx1, sy1] = world_to_screen(m.trail[i-1].first, m.trail[i-1].second);
            auto [sx2, sy2] = world_to_screen(m.trail[i].first, m.trail[i].second);
            float age = (float)i / (float)m.trail.size();
            gl_draw_rect(sx2 - 0.5f, sy2 - 0.5f, 1.f, 1.f,
                         0.9f, 0.3f, 0.05f, age * 0.6f);
        }
    }

    for (const auto &im : s->intercept_missiles) {
        for (size_t i = 1; i < im.trail.size(); i++) {
            auto [sx1, sy1] = world_to_screen(im.trail[i-1].first, im.trail[i-1].second);
            auto [sx2, sy2] = world_to_screen(im.trail[i].first, im.trail[i].second);
            float age = (float)i / (float)im.trail.size();
            gl_draw_rect(sx2 - 0.5f, sy2 - 0.5f, 1.f, 1.f,
                         0.2f, 0.9f, 0.2f, age * 0.6f);
        }
    }

    // ── Enemy missiles ──
    for (const auto &m : s->enemy_missiles) {
        auto [sx, sy] = world_to_screen(m.x, m.y);
        float m_w = 2.f;
        float m_h = 4.f;
        gl_draw_rect(sx - m_w/2, sy - m_h/2, m_w, m_h,
                     0.9f, 0.2f, 0.05f, 0.95f);
    }

    // ── Intercept missiles ──
    for (const auto &im : s->intercept_missiles) {
        auto [sx, sy] = world_to_screen(im.x, im.y);
        gl_draw_rect(sx - 1.5f, sy - 1.5f, 3.f, 3.f,
                     0.2f, 0.9f, 0.2f, 0.95f);
    }

    // ── Bomb trails ──
    for (const auto &b : s->bombs) {
        for (size_t i = 1; i < b.trail.size(); i++) {
            auto [sx1, sy1] = world_to_screen(b.trail[i-1].first, b.trail[i-1].second);
            auto [sx2, sy2] = world_to_screen(b.trail[i].first, b.trail[i].second);
            float age = (float)i / (float)b.trail.size();
            gl_draw_rect(sx2 - 0.5f, sy2 - 0.5f, 1.f, 1.f,
                         0.8f, 0.5f, 0.1f, age * 0.5f);  // Orange/brown trail
        }
    }

    // ── Bombs ──
    for (const auto &b : s->bombs) {
        auto [sx, sy] = world_to_screen(b.x, b.y);
        // Draw bomb as small dark circle
        gl_draw_rect(sx - 2.f, sy - 2.f, 4.f, 4.f,
                     0.3f, 0.3f, 0.3f, 0.9f);
    }

    // ── Planes flying across ──
    for (const auto &p : s->planes) {
        auto [sx, sy] = world_to_screen(p.x, p.y);
        // Draw simple plane shape (triangle)
        float plane_w = 8.f;
        float plane_h = 4.f;
        gl_draw_rect(sx - plane_w/2, sy - plane_h/2, plane_w, plane_h,
                     0.3f, 1.0f, 0.3f, 0.8f);  // Green plane
        // Wings
        gl_draw_rect(sx - plane_w - 2.f, sy - plane_h/4, 4.f, plane_h/2.f,
                     0.2f, 0.8f, 0.2f, 0.7f);
        gl_draw_rect(sx + plane_w - 2.f, sy - plane_h/4, 4.f, plane_h/2.f,
                     0.2f, 0.8f, 0.2f, 0.7f);
    }

    // ── Explosions ──
    for (const auto &e : s->explosions) {
        auto [sx, sy] = world_to_screen(e.x, e.y);
        float r_px = e.radius * s->field_scale;
        
        float age_factor = e.time_alive / 1.5f;
        float alpha = 1.f - (age_factor * age_factor);
        
        gl_draw_rect(sx - r_px, sy - r_px, r_px * 2, r_px * 2,
                     0.9f, 0.5f, 0.f, alpha * 0.7f);
        float core = r_px * 0.5f;
        gl_draw_rect(sx - core, sy - core, core * 2, core * 2,
                     1.f, 1.f, 0.4f, alpha * 0.95f);
    }

    y0 += field_h + fch * 1.5f;

    float msg_r = 0.f, msg_g = 1.f, msg_b = 0.5f;
    if (s->game_over) { msg_r = 1.f; msg_g = 0.2f; msg_b = 0.2f; }
    
    gl_draw_text(s->status_message, x0, y0, msg_r, msg_g, msg_b, 1.f, scale);
    y0 += fch * 1.5f;
    
    gl_draw_text("CLICK/ARROWS AIM  SPACE/CLICK FIRE  R=RESTART  ESC=MENU",
                 x0, y0, 0.f, 0.4f, 0.12f, 1.f, scale);
}

// ─── Input ────────────────────────────────────────────────────────────────

bool wopr_sdi_keydown(WoprState *w, SDL_Keycode sym) {
    WoprSdiState *s = sdi(w);
    if (!s) return false;

    if (sym == SDLK_r) {
        s->game_over = true;
        s->game_over_timer = 0.1;
        return true;
    }

    if (sym == SDLK_LEFT) {
        s->selected_city = (s->selected_city - 1 + NUM_CITIES) % NUM_CITIES;
        return true;
    }
    if (sym == SDLK_RIGHT) {
        s->selected_city = (s->selected_city + 1) % NUM_CITIES;
        return true;
    }

    if (sym == SDLK_SPACE) {
        City &city = s->cities[s->selected_city];
        if (city.alive && city.ammo > 0) {
            InterceptMissile im;
            im.start_x = city.x;
            im.start_y = PLAYFIELD_HEIGHT - 25.f;
            im.target_x = PLAYFIELD_WIDTH / 2.f;
            im.target_y = PLAYFIELD_HEIGHT / 2.f;
            
            float dist = distance(im.start_x, im.start_y, im.target_x, im.target_y);
            im.max_travel_time = dist / INTERCEPT_MISSILE_SPEED;
            im.travel_time = 0.f;
            im.alive = true;
            im.from_city = s->selected_city;
            
            s->intercept_missiles.push_back(im);
            city.ammo--;
            wopr_audio_play_keystroke();
        }
        return true;
    }

    return true;
}

void wopr_sdi_mousedown(WoprState *w, int px, int py, int button) {
    WoprSdiState *s = sdi(w);
    if (!s || button != 1) return;

    float world_x = (float)(px - (int)s->field_x0) / s->field_scale;
    float world_y = (float)(py - (int)s->field_y0) / s->field_scale;

    if (world_x < 0.f || world_x > PLAYFIELD_WIDTH ||
        world_y < 0.f || world_y > PLAYFIELD_HEIGHT) return;

    int best_city = -1;
    float best_dist = 9999.f;
    for (int i = 0; i < NUM_CITIES; i++) {
        if (!s->cities[i].alive) continue;
        float d = distance(world_x, world_y, s->cities[i].x, PLAYFIELD_HEIGHT - 25.f);
        if (d < best_dist) {
            best_dist = d;
            best_city = i;
        }
    }

    if (best_city < 0) return;

    City &city = s->cities[best_city];
    if (city.ammo > 0) {
        InterceptMissile im;
        im.start_x = city.x;
        im.start_y = PLAYFIELD_HEIGHT - 25.f;
        im.target_x = world_x;
        im.target_y = world_y;
        
        float dist = distance(im.start_x, im.start_y, im.target_x, im.target_y);
        im.max_travel_time = dist / INTERCEPT_MISSILE_SPEED;
        im.travel_time = 0.f;
        im.alive = true;
        im.from_city = best_city;
        
        s->intercept_missiles.push_back(im);
        city.ammo--;
        wopr_audio_play_keystroke();
    }
}

void wopr_sdi_mousemove(WoprState *w, int px, int py) {
}
