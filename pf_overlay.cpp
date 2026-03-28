// pf_overlay.cpp — F6 port-forward management overlay for Felix Terminal
#ifdef USESSH

#include "pf_overlay.h"
#include "port_forward.h"
#include "gl_renderer.h"
#include "ft_font.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

// ============================================================================
// STATE
// ============================================================================

PfOverlay g_pf_overlay;

extern int g_font_size;

// ============================================================================
// LAYOUT HELPERS  — match sftp_overlay conventions exactly
// ============================================================================

static const int PAD = 10;
static int row_h() { return (int)(g_font_size * 1.8f); }

static float pf_draw_text(const char *text, float x, float y,
                          float r, float g, float b, float a) {
    return draw_text(text, x, y, g_font_size, g_font_size, r, g, b, a, 0);
}

// ============================================================================
// PUBLIC API
// ============================================================================

void pf_overlay_open() {
    g_pf_overlay.visible      = true;
    g_pf_overlay.selected     = 0;
    g_pf_overlay.input_active = false;
    g_pf_overlay.input_len    = 0;
    g_pf_overlay.input_buf[0] = '\0';
    g_pf_overlay.input_type   = 'L';
    g_pf_overlay.status[0]    = '\0';
}

// ============================================================================
// RENDER
// ============================================================================

void pf_overlay_render(int win_w, int win_h) {
    if (!g_pf_overlay.visible) return;

    std::vector<PfStatus> fwds = pf_status();
    int rh = row_h();

    // Full-screen background
    draw_rect(0, 0, (float)win_w, (float)win_h, 0.07f, 0.07f, 0.09f, 1.f);

    // Title bar
    float title_h = (float)(rh + PAD);
    draw_rect(0, 0, (float)win_w, title_h, 0.12f, 0.12f, 0.18f, 1.f);
    draw_rect(0, title_h - 1, (float)win_w, 1, 0.25f, 0.45f, 0.65f, 1.f);
    pf_draw_text(
        "Port Forwards (F6)    L: add local (-L)    R: add remote (-R)"
        "    D: add SOCKS5 (-D)    Del: remove selected    Esc: close",
        (float)PAD, title_h * 0.72f, 0.75f, 0.88f, 1.0f, 1.f);

    // Column header
    float hdr_y = title_h;
    draw_rect(0, hdr_y, (float)win_w, (float)rh, 0.10f, 0.10f, 0.16f, 1.f);
    draw_rect(0, hdr_y + rh - 1, (float)win_w, 1, 0.20f, 0.25f, 0.40f, 1.f);
    pf_draw_text("Type",        (float)PAD,       hdr_y + rh * 0.72f, 0.55f, 0.65f, 0.80f, 1.f);
    pf_draw_text("Local port",  (float)PAD + 80,  hdr_y + rh * 0.72f, 0.55f, 0.65f, 0.80f, 1.f);
    pf_draw_text("Remote host", (float)PAD + 220, hdr_y + rh * 0.72f, 0.55f, 0.65f, 0.80f, 1.f);
    pf_draw_text("Remote port", (float)PAD + 520, hdr_y + rh * 0.72f, 0.55f, 0.65f, 0.80f, 1.f);
    pf_draw_text("Conns",       (float)PAD + 660, hdr_y + rh * 0.72f, 0.55f, 0.65f, 0.80f, 1.f);
    pf_draw_text("Status",      (float)PAD + 760, hdr_y + rh * 0.72f, 0.55f, 0.65f, 0.80f, 1.f);

    // Forward list
    float list_y = hdr_y + rh;
    int   n      = (int)fwds.size();

    if (n == 0) {
        pf_draw_text("No active port forwards.  Press L, R, or D to add one.",
                     (float)PAD * 3, list_y + rh * 1.2f, 0.45f, 0.50f, 0.60f, 1.f);
    }

    for (int i = 0; i < n; i++) {
        const PfStatus &s = fwds[i];
        float ry  = list_y + i * rh;
        bool  sel = (i == g_pf_overlay.selected);

        if (sel)
            draw_rect(1, ry, (float)win_w - 2, (float)rh, 0.15f, 0.32f, 0.60f, 0.85f);
        else if (i % 2 == 0)
            draw_rect(0, ry, (float)win_w, (float)rh, 1.f, 1.f, 1.f, 0.02f);

        float tr = sel ? 1.0f : 0.82f;
        float tg = sel ? 1.0f : 0.82f;
        float tb = sel ? 1.0f : 0.92f;

        // Type label + colour
        float typr, typg, typb;
        const char *type_label;
        if (s.type == PfStatus::LOCAL) {
            type_label = "-L";
            typr = 0.45f; typg = 0.80f; typb = 0.45f;
        } else if (s.type == PfStatus::REMOTE) {
            type_label = "-R";
            typr = 0.90f; typg = 0.65f; typb = 0.25f;
        } else {
            type_label = "-D";
            typr = 0.55f; typg = 0.45f; typb = 0.90f;
        }
        pf_draw_text(type_label, (float)PAD, ry + rh * 0.72f, typr, typg, typb, 1.f);

        char lport[32];
        snprintf(lport, sizeof(lport), "%d", s.local_port);
        pf_draw_text(lport, (float)PAD + 80, ry + rh * 0.72f, tr, tg, tb, 1.f);

        if (s.type == PfStatus::SOCKS) {
            // No remote host/port for SOCKS — show a description instead
            pf_draw_text("(dynamic)", (float)PAD + 220, ry + rh * 0.72f,
                         0.50f, 0.50f, 0.65f, 1.f);
        } else {
            pf_draw_text(s.remote_host.c_str(), (float)PAD + 220, ry + rh * 0.72f, tr, tg, tb, 1.f);

            char rport[32];
            snprintf(rport, sizeof(rport), "%d", s.remote_port);
            pf_draw_text(rport, (float)PAD + 520, ry + rh * 0.72f, tr, tg, tb, 1.f);
        }

        char conns[16];
        snprintf(conns, sizeof(conns), "%d", s.active_connections);
        float cr = s.active_connections > 0 ? 0.35f : 0.45f;
        float cg = s.active_connections > 0 ? 0.90f : 0.50f;
        float cb = s.active_connections > 0 ? 0.35f : 0.55f;
        pf_draw_text(conns, (float)PAD + 660, ry + rh * 0.72f, cr, cg, cb, 1.f);

        float okr = s.listener_ok ? 0.30f : 1.00f;
        float okg = s.listener_ok ? 1.00f : 0.35f;
        float okb = s.listener_ok ? 0.30f : 0.35f;
        pf_draw_text(s.listener_ok ? "OK" : "FAILED",
                     (float)PAD + 760, ry + rh * 0.72f, okr, okg, okb, 1.f);
    }

    // Input field
    float input_y = list_y + (n > 0 ? n : 1) * rh + rh;
    if (g_pf_overlay.input_active) {
        draw_rect(0, input_y - 2, (float)win_w, (float)(rh + 4), 0.10f, 0.10f, 0.18f, 1.f);
        draw_rect(0, input_y - 2, (float)win_w, 1, 0.30f, 0.55f, 0.90f, 1.f);
        draw_rect(0, input_y + rh + 2, (float)win_w, 1, 0.30f, 0.55f, 0.90f, 1.f);

        char prompt[320];
        if (g_pf_overlay.input_type == 'D') {
            snprintf(prompt, sizeof(prompt),
                     "-D  local_port  >  %s_",
                     g_pf_overlay.input_buf);
        } else {
            snprintf(prompt, sizeof(prompt),
                     "-%c  local_port:remote_host:remote_port  >  %s_",
                     g_pf_overlay.input_type, g_pf_overlay.input_buf);
        }
        pf_draw_text(prompt, (float)PAD, input_y + rh * 0.72f, 0.85f, 0.95f, 1.0f, 1.f);
    } else {
        pf_draw_text("Press L or R to add a local/remote forward, D to add a SOCKS5 proxy.",
                     (float)PAD, input_y + rh * 0.72f, 0.35f, 0.40f, 0.55f, 1.f);
    }

    // Status line at bottom
    float st_y = (float)(win_h - rh - PAD);
    draw_rect(0, st_y - 1, (float)win_w, 1, 0.20f, 0.20f, 0.30f, 1.f);
    draw_rect(0, st_y, (float)win_w, (float)(rh + PAD), 0.09f, 0.09f, 0.13f, 1.f);
    if (g_pf_overlay.status[0]) {
        float sr = g_pf_overlay.status_ok ? 0.30f : 1.00f;
        float sg = g_pf_overlay.status_ok ? 1.00f : 0.40f;
        float sb = g_pf_overlay.status_ok ? 0.30f : 0.40f;
        pf_draw_text(g_pf_overlay.status, (float)PAD, st_y + rh * 0.72f, sr, sg, sb, 1.f);
    }

    gl_flush_verts();
}

// ============================================================================
// KEYBOARD
// ============================================================================

bool pf_overlay_keydown(SDL_Keycode sym) {
    if (!g_pf_overlay.visible) return false;

    // Global hotkeys always pass through to the main loop
    if (sym == SDLK_F11) return false;

    // --- Input field active: collecting a forward spec ---
    if (g_pf_overlay.input_active) {
        if (sym == SDLK_ESCAPE) {
            g_pf_overlay.input_active = false;
            g_pf_overlay.input_len    = 0;
            g_pf_overlay.input_buf[0] = '\0';
            g_pf_overlay.status[0]    = '\0';
            return true;
        }
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
            if (g_pf_overlay.input_type == 'D') {
                // SOCKS5: input is just a port number
                int port = atoi(g_pf_overlay.input_buf);
                if (port <= 0 || port > 65535) {
                    snprintf(g_pf_overlay.status, sizeof(g_pf_overlay.status),
                             "Bad port — expected a number between 1 and 65535");
                    g_pf_overlay.status_ok = false;
                } else {
                    bool ok = pf_add_socks(port);
                    if (ok) {
                        snprintf(g_pf_overlay.status, sizeof(g_pf_overlay.status),
                                 "Added -D  %d  (SOCKS5 proxy)", port);
                        g_pf_overlay.status_ok = true;
                    } else {
                        snprintf(g_pf_overlay.status, sizeof(g_pf_overlay.status),
                                 "Failed to bind SOCKS5 proxy — port in use?");
                        g_pf_overlay.status_ok = false;
                    }
                    std::vector<PfStatus> fwds = pf_status();
                    g_pf_overlay.selected = (int)fwds.size() - 1;
                    if (g_pf_overlay.selected < 0) g_pf_overlay.selected = 0;
                }
            } else {
                int lp, rp; std::string rh;
                if (!pf_parse_spec(g_pf_overlay.input_buf, &lp, &rh, &rp)) {
                    snprintf(g_pf_overlay.status, sizeof(g_pf_overlay.status),
                             "Bad spec — expected local_port:remote_host:remote_port");
                    g_pf_overlay.status_ok = false;
                } else {
                    bool ok = (g_pf_overlay.input_type == 'L')
                        ? pf_add_local(lp, rh, rp)
                        : pf_add_remote(lp, rh, rp);
                    if (ok) {
                        snprintf(g_pf_overlay.status, sizeof(g_pf_overlay.status),
                                 "Added -%c  %d:%s:%d",
                                 g_pf_overlay.input_type, lp, rh.c_str(), rp);
                        g_pf_overlay.status_ok = true;
                    } else {
                        snprintf(g_pf_overlay.status, sizeof(g_pf_overlay.status),
                                 "Failed to add forward — check port and SSH session");
                        g_pf_overlay.status_ok = false;
                    }
                    std::vector<PfStatus> fwds = pf_status();
                    g_pf_overlay.selected = (int)fwds.size() - 1;
                    if (g_pf_overlay.selected < 0) g_pf_overlay.selected = 0;
                }
            }
            g_pf_overlay.input_active = false;
            g_pf_overlay.input_len    = 0;
            g_pf_overlay.input_buf[0] = '\0';
            return true;
        }
        if (sym == SDLK_BACKSPACE && g_pf_overlay.input_len > 0) {
            g_pf_overlay.input_buf[--g_pf_overlay.input_len] = '\0';
            return true;
        }
        return true;  // swallow everything else while input is open
    }

    // --- Normal mode ---
    switch (sym) {
    case SDLK_ESCAPE:
    case SDLK_F6:
        g_pf_overlay.visible = false;
        return true;

    case SDLK_UP: {
        if (g_pf_overlay.selected > 0) g_pf_overlay.selected--;
        return true;
    }
    case SDLK_DOWN: {
        std::vector<PfStatus> fwds = pf_status();
        int n = (int)fwds.size();
        if (g_pf_overlay.selected < n - 1) g_pf_overlay.selected++;
        return true;
    }

    case SDLK_l:
        g_pf_overlay.input_active        = true;
        g_pf_overlay.skip_next_textinput = true;
        g_pf_overlay.input_type          = 'L';
        g_pf_overlay.input_len           = 0;
        g_pf_overlay.input_buf[0]        = '\0';
        g_pf_overlay.status[0]           = '\0';
        return true;

    case SDLK_r:
        g_pf_overlay.input_active        = true;
        g_pf_overlay.skip_next_textinput = true;
        g_pf_overlay.input_type          = 'R';
        g_pf_overlay.input_len           = 0;
        g_pf_overlay.input_buf[0]        = '\0';
        g_pf_overlay.status[0]           = '\0';
        return true;

    case SDLK_d:
        g_pf_overlay.input_active        = true;
        g_pf_overlay.skip_next_textinput = true;
        g_pf_overlay.input_type          = 'D';
        g_pf_overlay.input_len           = 0;
        g_pf_overlay.input_buf[0]        = '\0';
        g_pf_overlay.status[0]           = '\0';
        return true;

    case SDLK_DELETE: {
        // Remove the selected forward by rebuilding without it
        std::vector<PfStatus> fwds = pf_status();
        int n = (int)fwds.size();
        if (n == 0 || g_pf_overlay.selected >= n) return true;

        // Snapshot all forwards, shut down everything, re-add all except selected
        std::vector<PfStatus> keep;
        for (int i = 0; i < n; i++)
            if (i != g_pf_overlay.selected) keep.push_back(fwds[i]);

        pf_shutdown_all();

        for (const PfStatus &s : keep) {
            if (s.type == PfStatus::LOCAL)
                pf_add_local(s.local_port, s.remote_host, s.remote_port);
            else if (s.type == PfStatus::REMOTE)
                pf_add_remote(s.remote_port, s.remote_host, s.local_port);
            else
                pf_add_socks(s.local_port);
        }

        if (g_pf_overlay.selected >= (int)keep.size() && g_pf_overlay.selected > 0)
            g_pf_overlay.selected--;

        snprintf(g_pf_overlay.status, sizeof(g_pf_overlay.status), "Forward removed.");
        g_pf_overlay.status_ok = true;
        return true;
    }

    default: return false;
    }
}

// SDL_TEXTINPUT handler — feed printable chars into the input buffer.
// Call from the SDL_TEXTINPUT case in the main event loop when the overlay is visible.
void pf_overlay_textinput(const char *text) {
    if (!g_pf_overlay.visible || !g_pf_overlay.input_active) return;
    if (g_pf_overlay.skip_next_textinput) {
        g_pf_overlay.skip_next_textinput = false;
        return;
    }
    for (const char *p = text; *p; p++) {
        if (g_pf_overlay.input_len < (int)sizeof(g_pf_overlay.input_buf) - 1) {
            g_pf_overlay.input_buf[g_pf_overlay.input_len++] = *p;
            g_pf_overlay.input_buf[g_pf_overlay.input_len]   = '\0';
        }
    }
}

#endif // USESSH
