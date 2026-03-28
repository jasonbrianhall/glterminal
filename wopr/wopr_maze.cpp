// wopr_maze.cpp — Falken's Maze
//
// Recursive-backtracker maze generation.
// Player navigates with arrows. Find the exit (@).

#include "wopr.h"
#include <string.h>
#include <stdlib.h>
#include <vector>
#include <algorithm>

#include "wopr_render.h"

// ─── Maze dimensions ──────────────────────────────────────────────────────

static const int MZ_W = 31;   // must be odd
static const int MZ_H = 21;   // must be odd

struct MazeState {
    char  grid[MZ_H][MZ_W + 1]; // '#' wall, ' ' passage, 'S' start, 'E' end
    int   px, py;                // player pos (passage coords)
    bool  won;
    int   steps;
    int   level;
};

static MazeState *mz(WoprState *w) { return (MazeState *)w->sub_state; }

// ─── Generation — recursive backtracker ──────────────────────────────────

static void maze_gen(MazeState *s, int seed) {
    // Fill all walls
    for (int y = 0; y < MZ_H; y++) {
        for (int x = 0; x < MZ_W; x++)
            s->grid[y][x] = '#';
        s->grid[y][MZ_W] = 0;
    }

    // Passage cells are at odd coordinates
    // Use iterative DFS
    struct Cell { int x, y; };
    std::vector<Cell> stack;
    srand(seed);

    auto carve = [&](int x, int y) {
        s->grid[y][x] = ' ';
        stack.push_back({x, y});
    };

    carve(1, 1);

    static const int dx[] = {2, -2, 0,  0};
    static const int dy[] = {0,  0, 2, -2};

    while (!stack.empty()) {
        Cell &cur = stack.back();
        // Shuffle directions
        int dir[4] = {0,1,2,3};
        for (int i = 3; i > 0; i--) std::swap(dir[i], dir[rand()%(i+1)]);
        bool moved = false;
        for (int i = 0; i < 4; i++) {
            int nx = cur.x + dx[dir[i]];
            int ny = cur.y + dy[dir[i]];
            if (nx > 0 && nx < MZ_W-1 && ny > 0 && ny < MZ_H-1
                && s->grid[ny][nx] == '#') {
                // carve wall between
                s->grid[cur.y + dy[dir[i]]/2][cur.x + dx[dir[i]]/2] = ' ';
                carve(nx, ny);
                moved = true;
                break;
            }
        }
        if (!moved) stack.pop_back();
    }

    s->grid[1][1]         = 'S';  // start
    s->grid[MZ_H-2][MZ_W-2] = 'E';  // exit
    s->px = 1; s->py = 1;
    s->won   = false;
    s->steps = 0;
}

// ─── Lifecycle ────────────────────────────────────────────────────────────

void wopr_maze_enter(WoprState *w) {
    MazeState *s = new MazeState{};
    s->level = 1;
    maze_gen(s, 0x1983 + s->level);
    w->sub_state = s;
}

void wopr_maze_free(WoprState *w) {
    delete mz(w);
    w->sub_state = nullptr;
}

void wopr_maze_update(WoprState *w, double /*dt*/) {
    // Nothing to tick — movement is synchronous
}

void wopr_maze_render(WoprState *w, int ox, int oy, int cw, int ch, int cols) {
    MazeState *s = mz(w);
    if (!s) return;

    float scale = 1.f;
    float x0 = (float)ox, y0 = (float)oy;
    float fch = (float)ch, fcw = (float)cw;

    char title[64];
    snprintf(title, sizeof(title),
             "FALKEN'S MAZE  --  LEVEL %d  --  STEPS: %d", s->level, s->steps);
    gl_draw_text(title, x0, y0, 0.f, 1.f, 0.6f, 1.f, scale);
    y0 += fch * 1.5f;

    float cell_w = fcw * 1.5f;
    float cell_h = fch;

    for (int row = 0; row < MZ_H; row++) {
        for (int col = 0; col < MZ_W; col++) {
            char c = s->grid[row][col];
            float cx = x0 + col * cell_w;
            float cy = y0 + row * cell_h;

            float r = 0.f, g = 0.f, b = 0.f;
            char glyph[2] = {c, 0};

            bool is_player = (s->px == col && s->py == row);

            if (is_player) {
                glyph[0] = '@';
                r = 0.f; g = 1.f; b = 0.5f;
                gl_draw_rect(cx, cy, cell_w, cell_h, 0.f, 0.3f, 0.1f, 0.5f);
            } else if (c == '#') {
                r = 0.f; g = 0.35f; b = 0.1f;
            } else if (c == 'E') {
                r = 0.f; g = 1.f; b = 0.3f;
                glyph[0] = '@'; glyph[0] = '>'; // exit marker
                glyph[0] = 'X';
            } else if (c == ' ' || c == 'S') {
                r = 0.f; g = 0.1f; b = 0.03f;
                glyph[0] = ' ';
            }

            gl_draw_text(glyph, cx, cy, r, g, b, 1.f, scale);
        }
    }

    float my = y0 + MZ_H * cell_h + fch * 0.5f;
    if (s->won) {
        char win[64];
        snprintf(win, sizeof(win),
                 "EXIT REACHED IN %d STEPS.  N=NEXT LEVEL   ESC=MENU", s->steps);
        gl_draw_text(win, x0, my, 0.f, 1.f, 0.5f, 1.f, scale);
    } else {
        gl_draw_text("ARROWS=MOVE  N=NEW MAZE  ESC=MENU  REACH [X] TO EXIT",
                     x0, my, 0.f, 0.5f, 0.15f, 1.f, scale);
    }
}

bool wopr_maze_keydown(WoprState *w, SDL_Keycode sym) {
    MazeState *s = mz(w);
    if (!s) return false;

    if (sym == SDLK_n) {
        s->level++;
        maze_gen(s, 0x1983 + s->level * 7);
        return true;
    }
    if (sym == SDLK_r) {
        maze_gen(s, 0x1983 + s->level * 7);
        return true;
    }

    if (s->won) return true;

    int nx = s->px, ny = s->py;
    switch (sym) {
        case SDLK_UP:    ny--; break;
        case SDLK_DOWN:  ny++; break;
        case SDLK_LEFT:  nx--; break;
        case SDLK_RIGHT: nx++; break;
        case SDLK_w:     ny--; break;
        case SDLK_s:     ny++; break;
        case SDLK_a:     nx--; break;
        case SDLK_d:     nx++; break;
        default: return true;
    }

    if (nx < 0 || nx >= MZ_W || ny < 0 || ny >= MZ_H) return true;
    char dest = s->grid[ny][nx];
    if (dest == '#') return true;

    s->px = nx; s->py = ny; s->steps++;
    if (dest == 'E') {
        s->won = true;
    }
    return true;
}
