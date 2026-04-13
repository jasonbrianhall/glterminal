/*
 * commands.c — All BASIC command handlers, command registration table,
 *              statement splitter, and dispatcher.
 */
#include "basic.h"
#include <stdarg.h>
#include "basic_print.h"
#define printf(...) basic_printf(__VA_ARGS__)

#ifdef _WIN32
#include <windows.h>
#endif

BASIC_NS_BEGIN

/* ================================================================
 * PRINT USING formatter
 * ================================================================ */
static void print_using(char *fmt, double val) {
    int before = 0, after = 0, has_dot = 0, has_dollar = 0, has_plus = 0;
    for (char *f = fmt; *f; f++) {
        if      (*f == '$') has_dollar = 1;
        else if (*f == '+') has_plus   = 1;
        else if (*f == '.') has_dot    = 1;
        else if (*f == '#') { if (has_dot) after++; else before++; }
    }
    char numbuf[64];
    if (has_dot)
        snprintf(numbuf, sizeof numbuf, "%*.*f", before + after + 1, after, val);
    else
        snprintf(numbuf, sizeof numbuf, "%*d", before, (int)val);
    if (has_plus && val >= 0) display_putchar('+');
    if (has_dollar)           display_putchar('$');
    display_print(numbuf);
}

/* ================================================================
 * Graphics backend — two compile paths:
 *
 *   USE_SDL_WINDOW  → call basic_gfx.h functions directly (SDL pixel buffer)
 *   (default)       → emit OSC 666 escape sequences to stdout (Felix terminal)
 * ================================================================ */

/* Current screen state — updated by SCREEN, read by graphics cmds */
int g_screen_mode   = 0;
int g_screen_width  = 640;
int g_screen_height = 350;
int g_back_color    = 0;   /* palette index used by CLS */

#ifdef USE_SDL_WINDOW
/* ── SDL direct path ────────────────────────────────────────────────────── */
#include "basic_gfx.h"

/* No-op shims so the rest of the file compiles unchanged */
static void felix_send(char *)  {}
static void felix_sendf(char *, ...) {}
static void felix_draw(char *)  {}
static void felix_drawf(char *, ...) {}

#else
/* ── OSC 666 escape-code path (original Felix terminal protocol) ─────────── */
#include <unistd.h>

static void felix_send(char *cmd) {
    write(STDOUT_FILENO, "\033]666;", 6);
    write(STDOUT_FILENO, cmd, strlen(cmd));
    write(STDOUT_FILENO, "\033\\", 2);
}

static void felix_sendf(char *fmt, ...) {
    char buf[DEFAULT_BUFFER];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    felix_send(buf);
}

/* Draw commands are wrapped in "batch;" so the terminal flushes them
 * together on the next frame. */
static void felix_draw(char *cmd) {
    write(STDOUT_FILENO, "\033]666;batch;", 12);
    write(STDOUT_FILENO, cmd, strlen(cmd));
    write(STDOUT_FILENO, "\033\\", 2);
}

static void felix_drawf(char *fmt, ...) {
    char buf[DEFAULT_BUFFER];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    felix_draw(buf);
}
#endif /* USE_SDL_WINDOW */
/* EGA 64-colour palette: index → (r,g,b) in 0-255 range.
 * Gorilla uses PALETTE idx, ega_color where ega_color is a 6-bit EGA value
 * (bits 5-4-3 = RGB high, bits 2-1-0 = rgb low).
 * For simplicity we map the 16 standard EGA display colours (0-15) and
 * treat any value ≥ 16 as a 6-bit EGA palette entry. */
static void ega_to_rgb(int ega6, int *r, int *g, int *b) {
    /* EGA 6-bit: R1 G1 B1 R0 G0 B0 */
    int r1 = (ega6 >> 5) & 1;
    int g1 = (ega6 >> 4) & 1;
    int b1 = (ega6 >> 3) & 1;
    int r0 = (ega6 >> 2) & 1;
    int g0 = (ega6 >> 1) & 1;
    int b0 = (ega6 >> 0) & 1;
    *r = (r1 * 2 + r0) * 85;
    *g = (g1 * 2 + g0) * 85;
    *b = (b1 * 2 + b0) * 85;
}

/* Screen mode → (width, height) */
static void screen_dims(int mode, int *w, int *h) {
    static const struct { int m, w, h; } modes[] = {
        {1,320,200},{2,640,200},{3,720,348},{4,640,400},{5,160,100},
        {6,160,200},{7,320,200},{8,640,200},{9,640,350},{10,640,350},
        {11,640,480},{12,640,480},{13,320,200},{14,320,200},{15,640,200},
        {16,640,480},{17,640,480},{18,640,480},{19,640,480},{20,512,480},
        {21,640,400},{22,640,480},{23,800,600},{24,160,200},{25,320,200},
        {26,640,200},{27,640,200},{28,720,350},{0,0,0}
    };
    for (int i = 0; modes[i].m || modes[i].w; i++) {
        if (modes[i].m == mode) { *w = modes[i].w; *h = modes[i].h; return; }
    }
    *w = 640; *h = 350;
}

/* Sprite ID registry: maps array variable pointer → sprite ID */
#define MAX_SPRITES 64
static struct { Var *var; int id; } g_sprites[MAX_SPRITES];
static int g_nsprites = 0;
static int g_next_sprite_id = 1;

static int sprite_id_for(Var *var) {
    for (int i = 0; i < g_nsprites; i++)
        if (g_sprites[i].var == var) return g_sprites[i].id;
    if (g_nsprites >= MAX_SPRITES) return -1;
    int id = g_next_sprite_id++;
    g_sprites[g_nsprites].var = var;
    g_sprites[g_nsprites].id  = id;
    g_nsprites++;
    return id;
}

/* Parse  (x, y)  or  x, y  returning pointer past closing paren (if any) */
static char *parse_xy(char *p, double *x, double *y) {
    int paren = (*p == '(');
    if (paren) p = sk(p + 1);
    mpf_t mx, my; mpf_init2(mx, g_prec); mpf_init2(my, g_prec);
    p = sk(eval_expr(p, mx)); *x = mpf_get_d(mx); mpf_clear(mx);
    if (*p == ',') p = sk(p + 1);
    p = sk(eval_expr(p, my)); *y = mpf_get_d(my); mpf_clear(my);
    if (paren && *p == ')') p = sk(p + 1);
    return p;
}

/* No-op stubs */
static int cmd_rem(Interp *ip, char *args)    { (void)ip;(void)args; return 0; }
static int cmd_defseg(Interp *ip, char *args) { (void)ip;(void)args; return 0; }
static int cmd_defdbl(Interp *ip, char *args) { (void)ip;(void)args; return 0; }
static int cmd_key(Interp *ip, char *args)    { (void)ip;(void)args; return 0; }
static int cmd_chain(Interp *ip, char *args)  { (void)ip;(void)args; return 0; }

/* ================================================================
 * SCREEN mode
 * ================================================================ */
static int cmd_screen(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (!*p || *p == ':') return 0;
    mpf_t n; mpf_init2(n, g_prec);
    eval_expr(p, n);
    int mode = (int)mpf_get_si(n);
    mpf_clear(n);
    g_screen_mode = mode;
    screen_dims(mode, &g_screen_width, &g_screen_height);
#ifdef USE_SDL_WINDOW
    gfx_screen(mode);   /* allocates / frees the pixel buffer directly */
#else
    if (mode == 0) felix_draw("cls;0");
    else           felix_sendf("screen;%d", mode);
#endif
    return 0;
}
static int cmd_beep(Interp *ip, char *args) {
    (void)ip; (void)args;
    sound_beep();
    return 0;
}

/* SOUND freq, duration  — freq in Hz, duration in 18.2-tick clock units */
static int cmd_sound(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    mpf_t freq, dur; mpf_init2(freq, g_prec); mpf_init2(dur, g_prec);
    p = sk(eval_expr(p, freq));
    if (*p == ',') p = sk(p + 1);
    eval_expr(p, dur);
    sound_tone(mpf_get_d(freq), mpf_get_d(dur));
    mpf_clears(freq, dur, NULL);
    return 0;
}

/* PLAY "mml-string" — GW-BASIC Music Macro Language */
static int cmd_play(Interp *ip, char *args) {
    (void)ip;
    char mml[1024];
    eval_str_expr(sk(args), mml, sizeof mml);
    sound_play(mml);
    return 0;
}

/* ================================================================
 * Interpreter control
 * ================================================================ */
static int cmd_stop(Interp *ip, char *args) {
    (void)args;
    g_cont_pc = ip->pc + 1;
    ip->running = 0;
    display_print("\nBreak\n");
    return 1;
}

static int cmd_cont(Interp *ip, char *args) {
    (void)args;
    if (g_cont_pc < 0) { display_print("Can't continue\n"); return 0; }
    ip->pc = g_cont_pc;
    g_cont_pc = -1;
    return 1;
}

static int cmd_end(Interp *ip, char *args) {
    (void)args; ip->running = 0;
    for (int i = 1; i <= MAX_FILE_HANDLES; i++)
        if (g_files[i].fp) { fclose(g_files[i].fp); g_files[i].fp = NULL; g_files[i].mode = 0; }
    return 1;
}

/* ================================================================
 * Utility commands
 * ================================================================ */
static int cmd_randomize(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (*p && *p != ':') {
        mpf_t n; mpf_init2(n, g_prec);
        eval_expr(p, n);
        srand((unsigned)mpf_get_si(n));
        mpf_clear(n);
    } else {
        srand((unsigned)time(NULL));
    }
    return 0;
}

static int cmd_swap(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    char name1[MAX_VARNAME], name2[MAX_VARNAME];
    p = sk(read_varname(p, name1));
    if (*p == ',') p = sk(p + 1);
    read_varname(p, name2);
    Var *a = var_get(name1), *b = var_get(name2);
    if (var_is_str_name(name1)) {
        char *tmp = a->str; a->str = b->str; b->str = tmp;
    } else {
        mpf_t tmp; mpf_init2(tmp, g_prec);
        mpf_set(tmp, a->num); mpf_set(a->num, b->num); mpf_set(b->num, tmp);
        mpf_clear(tmp);
    }
    return 0;
}

static int cmd_erase(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    while (*p) {
        char name[MAX_VARNAME];
        p = sk(read_varname(p, name));
        Var *v = var_find(name);
        if (v) {
            if (v->kind == VAR_ARRAY_NUM && v->arr_num) {
                int total = v->dim[0] * (v->ndim == 2 ? v->dim[1] : 1);
                for (int i = 0; i < total; i++) mpf_clear(v->arr_num[i]);
                free(v->arr_num); v->arr_num = NULL;
            } else if (v->kind == VAR_ARRAY_STR && v->arr_str) {
                int total = v->dim[0] * (v->ndim == 2 ? v->dim[1] : 1);
                for (int i = 0; i < total; i++) free(v->arr_str[i]);
                free(v->arr_str); v->arr_str = NULL;
            }
            v->kind = var_is_str_name(name) ? VAR_STR : VAR_NUM;
            v->ndim = 0;
        }
        p = sk(p); if (*p == ',') p = sk(p + 1); else break;
    }
    return 0;
}

static int cmd_option(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (kw_match(p, "BASE")) {
        p = sk(p + 4);
        mpf_t n; mpf_init2(n, g_prec);
        eval_expr(p, n);
        int base = (int)mpf_get_si(n); mpf_clear(n);
        if (base == 0 || base == 1) g_option_base = base;
        else basic_stderr("OPTION BASE must be 0 or 1\n");
    }
    return 0;
}

/* ================================================================
 * Display commands
 * ================================================================ */
static int cmd_system(Interp *ip, char *args) {
    (void)args;
    ip->running = 0;
    /* longjmp out if available (WOPR context), otherwise just stop */
#ifdef INLINEBASIC
    extern void basic_exit_(void);
    basic_exit_();
#endif
    return 1;
}

static int cmd_sleep(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    double secs = 1.0;
    if (*p && *p != ':') {
        mpf_t n; mpf_init2(n, g_prec);
        eval_expr(p, n);
        secs = mpf_get_d(n);
        mpf_clear(n);
    }
    if (secs > 0) {
#ifdef _WIN32
        Sleep((DWORD)(secs * 1000));
#else
        struct timespec ts;
        ts.tv_sec  = (time_t)secs;
        ts.tv_nsec = (long)((secs - (long)secs) * 1e9);
        nanosleep(&ts, NULL);
#endif
    }
    return 0;
}

static int cmd_kill(Interp *ip, char *args) {
    (void)ip;
    char filename[DEFAULT_BUFFER] = "";
    char *p = sk(args);
    if (*p == '"') p++;
    int i = 0;
    while (*p && *p != '"' && i < (int)sizeof(filename) - 1) filename[i++] = *p++;
    filename[i] = '\0';
    if (filename[0]) remove(filename);
    return 0;
}

static int cmd_cls(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    int arg = -1;  /* -1 = no argument */
#ifndef USE_SDL_WINDOW
    printf("\033[2J\033[H");
    fflush(stdout);
#endif
    if (*p && *p != ':') {
        mpf_t n; mpf_init2(n, g_prec);
        eval_expr(p, n);
        arg = (int)mpf_get_si(n);
        mpf_clear(n);
    }
        int c;
        if (arg >= 0) {
            c = arg;
        } else {
            c = g_back_color;
            Var *v = var_find("BACKCOLOR");
            if (v && v->kind == VAR_NUM) c = (int)mpf_get_si(v->num);
        }
#ifdef USE_SDL_WINDOW
        gfx_cls(c);

#else
        if (c == 0) felix_draw("cls;0");
        else        felix_drawf("cls;%d", c);
#endif
        display_cls();
    return 0;
}

static int cmd_width(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    mpf_t n; mpf_init2(n, g_prec);
    p = sk(eval_expr(p, n));
    int cols = (int)mpf_get_si(n);
    mpf_clear(n);
    /* skip optional second argument (rows) */
    if (*p == ',') {
        p = sk(p + 1);
        mpf_t r; mpf_init2(r, g_prec);
        eval_expr(p, r);
        mpf_clear(r);
    }
    display_width(cols);
    return 0;
}

static int cmd_color(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    mpf_t fg, bg; mpf_init2(fg, g_prec); mpf_init2(bg, g_prec);
    mpf_set_ui(fg, 7); mpf_set_ui(bg, 0);
    if (!*p) { mpf_clears(fg, bg, NULL); return 0; } /* bare COLOR — no-op */
    if (*p != ',') p = eval_expr(p, fg);
    p = sk(p);
    if (*p == ',') { p = sk(p + 1); if (*p) p = eval_expr(p, bg); }
    display_color((int)mpf_get_si(fg), (int)mpf_get_si(bg));
    mpf_clears(fg, bg, NULL);
    return 0;
}

static int cmd_locate(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    mpf_t row, col, cur;
    mpf_init2(row, g_prec); mpf_init2(col, g_prec); mpf_init2(cur, g_prec);
    mpf_set_ui(row, 0); mpf_set_ui(col, 0); mpf_set_ui(cur, 1);

    if (*p && *p != ',') p = sk(eval_expr(p, row));
    if (*p == ',') p = sk(p + 1); else goto locate_done;
    if (*p && *p != ',') p = sk(eval_expr(p, col));
    if (*p == ',') p = sk(p + 1); else goto locate_done;
    if (*p && *p != ',') p = sk(eval_expr(p, cur));

    locate_done:
    if (mpf_get_si(row) > 0 || mpf_get_si(col) > 0)
        display_locate((int)mpf_get_si(row), (int)mpf_get_si(col));
    display_cursor((int)mpf_get_si(cur));
    mpf_clears(row, col, cur, NULL);
    return 0;
}

/* ================================================================
 * DO ... LOOP [WHILE|UNTIL cond]
 * WHILE cond ... WEND
 *
 * Both use the control stack with a special frame tag.
 * The loop start pc is stored; on LOOP/WEND we re-evaluate and jump back.
 * ================================================================ */

/* Forward declarations for commands used before their definition */
static int cmd_dim(Interp *ip, char *args);
static int cmd_return(Interp *ip, char *args);
static char *parse_field_varname(char *p, char *out);
static int eval_one_cmp(char **pp);

/* DO [WHILE|UNTIL cond] — push a loop frame pointing at the DO statement */
static int cmd_do(Interp *ip, char *args) {
    /* When LOOP sends us back here, the frame already exists — don't push again.
     * But we must still re-check any DO WHILE / DO UNTIL condition. */
    if (g_ctrl_top > 0 &&
        strcmp(g_ctrl[g_ctrl_top - 1].varname, "\x02" "DO") == 0 &&
        g_ctrl[g_ctrl_top - 1].line_idx == ip->pc) {
        char *p = sk(args);
        if (kw_match(p, "WHILE") || kw_match(p, "UNTIL")) {
            int is_until = kw_match(p, "UNTIL");
            p = sk(p + 5);
            int cond = eval_one_cmp(&p);
            if (is_until) cond = !cond;
            if (!cond) {
                /* skip to after matching LOOP */
                int depth = 1, pc = ip->pc + 1;
                while (pc < g_nlines && depth > 0) {
                    char *t = sk(g_lines[pc].text);
                    if (kw_match(t, "DO")) depth++;
                    else if (kw_match(t, "LOOP")) depth--;
                    pc++;
                }
                mpf_clear(g_ctrl[g_ctrl_top - 1].limit);
                mpf_clear(g_ctrl[g_ctrl_top - 1].step);
                g_ctrl_top--;
                ip->pc = pc; return 1;
            }
        }
        return 0;
    }

    /* First entry: optionally check DO WHILE/UNTIL before pushing */
    char *p = sk(args);
    if (kw_match(p, "WHILE") || kw_match(p, "UNTIL")) {
        int is_until = kw_match(p, "UNTIL");
        p = sk(p + 5);
        int cond = eval_one_cmp(&p);
        if (is_until) cond = !cond;
        if (!cond) {
            int depth = 1, pc = ip->pc + 1;
            while (pc < g_nlines && depth > 0) {
                char *t = sk(g_lines[pc].text);
                if (kw_match(t, "DO")) depth++;
                else if (kw_match(t, "LOOP")) depth--;
                pc++;
            }
            ip->pc = pc; return 1;
        }
    }

    if (g_ctrl_top >= CTRL_STACK_MAX) { basic_stderr("Stack overflow\n"); return -1; }
    CtrlFrame *f = &g_ctrl[g_ctrl_top++];
    strcpy(f->varname, "\x02" "DO");
    f->line_idx = ip->pc;
    mpf_init2(f->limit, g_prec); mpf_set_ui(f->limit, 0);
    mpf_init2(f->step,  g_prec); mpf_set_ui(f->step,  0);
    return 0;
}

/* LOOP [WHILE|UNTIL cond] */
static int cmd_loop(Interp *ip, char *args) {
    /* find the matching DO frame */
    int fi = g_ctrl_top - 1;
    while (fi >= 0 && strcmp(g_ctrl[fi].varname, "\x02" "DO") != 0) fi--;
    if (fi < 0) { basic_stderr("LOOP without DO\n"); return -1; }

    char *p = sk(args);
    int keep_looping = 1;   /* default: infinite loop, need explicit WHILE/UNTIL to stop */

    if (kw_match(p, "WHILE")) {
        p = sk(p + 5);
        keep_looping = eval_one_cmp(&p);
    } else if (kw_match(p, "UNTIL")) {
        p = sk(p + 5);
        keep_looping = !eval_one_cmp(&p);
    }

    if (keep_looping) {
        /* loop back to the DO line itself — cmd_do will skip if already tracked,
           and will re-check DO WHILE/UNTIL condition on re-entry */
        ip->pc = g_ctrl[fi].line_idx;
        return 1;
    } else {
        /* exit — pop frame, return 0 so run loop advances past LOOP line */
        mpf_clear(g_ctrl[fi].limit); mpf_clear(g_ctrl[fi].step);
        g_ctrl_top = fi;
        return 0;
    }
}

/* WHILE cond */
static int cmd_while(Interp *ip, char *args) {
    char *p = sk(args);
    int cond = eval_one_cmp(&p);
    p = sk(p);
    while (kw_match(p,"AND") || kw_match(p,"OR")) {
        int is_and = kw_match(p,"AND");
        p = sk(p + (is_and ? 3 : 2));
        int next = eval_one_cmp(&p);
        cond = is_and ? (cond && next) : (cond || next);
        p = sk(p);
    }

    if (cond) {
        /* Only push a new frame if we're not already tracking this WHILE */
        int already = (g_ctrl_top > 0 &&
                       strcmp(g_ctrl[g_ctrl_top - 1].varname, "\x03" "WHILE") == 0 &&
                       g_ctrl[g_ctrl_top - 1].line_idx == ip->pc);
        if (!already) {
            if (g_ctrl_top >= CTRL_STACK_MAX) { basic_stderr("Stack overflow\n"); return -1; }
            CtrlFrame *f = &g_ctrl[g_ctrl_top++];
            strcpy(f->varname, "\x03" "WHILE");
            f->line_idx = ip->pc;
            mpf_init2(f->limit, g_prec); mpf_set_ui(f->limit, 0);
            mpf_init2(f->step,  g_prec); mpf_set_ui(f->step,  0);
        }
        return 0;
    } else {
        /* Condition false: pop frame if present, skip to WEND */
        if (g_ctrl_top > 0 &&
            strcmp(g_ctrl[g_ctrl_top - 1].varname, "\x03" "WHILE") == 0 &&
            g_ctrl[g_ctrl_top - 1].line_idx == ip->pc) {
            mpf_clear(g_ctrl[g_ctrl_top - 1].limit);
            mpf_clear(g_ctrl[g_ctrl_top - 1].step);
            g_ctrl_top--;
        }
        int depth = 1;
        int pc = ip->pc + 1;
        while (pc < g_nlines && depth > 0) {
            char *t = sk(g_lines[pc].text);
            if (kw_match(t, "WHILE")) depth++;
            else if (kw_match(t, "WEND")) depth--;
            if (depth > 0) pc++;
        }
        ip->pc = pc + 1;
        return 1;
    }
}

/* WEND — jump back to the matching WHILE; WHILE will pop if condition fails */
static int cmd_wend(Interp *ip, char *args) {
    (void)args;
    int fi = g_ctrl_top - 1;
    while (fi >= 0 && strcmp(g_ctrl[fi].varname, "\x03" "WHILE") != 0) fi--;
    if (fi < 0) { basic_stderr("WEND without WHILE\n"); return -1; }
    ip->pc = g_ctrl[fi].line_idx;   /* jump to WHILE line — it re-evaluates & pops if done */
    return 1;
}

/* ================================================================
 * SELECT CASE expr
 *   CASE val [, val ...]
 *   CASE IS op val
 *   CASE val TO val
 *   CASE ELSE
 * END SELECT
 * ================================================================ */

/* Forward-scan to find the next CASE or END SELECT at the same depth */
static int find_next_case(int start_pc) {
    int depth = 0;
    for (int pc = start_pc; pc < g_nlines; pc++) {
        char *t = sk(g_lines[pc].text);
        if (kw_match(t, "SELECT")) depth++;
        else if (kw_match(t, "END") && kw_match(sk(t + 3), "SELECT")) {
            if (depth == 0) return pc;
            depth--;
        } else if (depth == 0 && kw_match(t, "CASE")) return pc;
    }
    return g_nlines;
}

static int cmd_select(Interp *ip, char *args) {
    char *p = sk(args);
    /* skip optional CASE keyword after SELECT */
    if (kw_match(p, "CASE")) p = sk(p + 4);

    /* Evaluate the selector — could be string or numeric */
    char sel_s[1024] = ""; double sel_n = 0; int sel_is_str = 0;
    if (is_str_token(p)) {
        eval_str_expr(p, sel_s, sizeof sel_s);
        sel_is_str = 1;
    } else {
        mpf_t v; mpf_init2(v, g_prec);
        eval_expr(p, v);
        sel_n = mpf_get_d(v);
        mpf_clear(v);
    }

    /* Scan forward through CASE clauses */
    int pc = ip->pc + 1;
    while (pc < g_nlines) {
        char *t = sk(g_lines[pc].text);

        if (kw_match(t, "END") && kw_match(sk(t + 3), "SELECT")) {
            ip->pc = pc + 1; return 1;    /* no match — skip to END SELECT */
        }

        if (!kw_match(t, "CASE")) { pc++; continue; }

        t = sk(t + 4);   /* past CASE */

        /* CASE ELSE always matches */
        if (kw_match(t, "ELSE")) { ip->pc = pc + 1; return 1; }

        /* Try each comma-separated value list */
        int matched = 0;
        while (*t && !matched) {
            t = sk(t);
            if (sel_is_str) {
                char cval[1024];
                t = eval_str_expr(t, cval, sizeof cval);
                matched = (strcmp(sel_s, cval) == 0);
            } else if (kw_match(t, "IS")) {
                /* CASE IS op val */
                t = sk(t + 2);
                char op[3] = {t[0], t[1], '\0'}; int ol = 2;
                if (!strcmp(op,"<>")||!strcmp(op,"><")||!strcmp(op,"<=")||
                    !strcmp(op,"=<")||!strcmp(op,">=")||!strcmp(op,"=>")) ;
                else { op[1] = '\0'; ol = 1; }
                t = sk(t + ol);
                mpf_t cv; mpf_init2(cv, g_prec);
                t = eval_expr(t, cv);
                double cv_d = mpf_get_d(cv); mpf_clear(cv);
                if      (!strcmp(op,"<>")||!strcmp(op,"><")) matched=(sel_n!=cv_d);
                else if (!strcmp(op,"<=")||!strcmp(op,"=<")) matched=(sel_n<=cv_d);
                else if (!strcmp(op,">=")||!strcmp(op,"=>")) matched=(sel_n>=cv_d);
                else if (op[0]=='<') matched=(sel_n<cv_d);
                else if (op[0]=='>') matched=(sel_n>cv_d);
                else                 matched=(sel_n==cv_d);
            } else {
                mpf_t cv; mpf_init2(cv, g_prec);
                t = eval_expr(t, cv);
                double lo = mpf_get_d(cv); mpf_clear(cv);
                t = sk(t);
                if (kw_match(t, "TO")) {
                    t = sk(t + 2);
                    mpf_t cv2; mpf_init2(cv2, g_prec);
                    t = eval_expr(t, cv2);
                    double hi = mpf_get_d(cv2); mpf_clear(cv2);
                    matched = (sel_n >= lo && sel_n <= hi);
                } else {
                    matched = (sel_n == lo);
                }
            }
            t = sk(t);
            if (*t == ',') t = sk(t + 1); else break;
        }

        if (matched) { ip->pc = pc + 1; return 1; }
        pc = find_next_case(pc + 1);
    }
    ip->pc = pc + 1; return 1;
}

static int cmd_end_select(Interp *ip, char *args) {
    (void)ip; (void)args; return 0;   /* normal fall-through after a matched CASE */
}

/* ================================================================
 * EXIT SUB / EXIT FUNCTION / EXIT FOR / EXIT DO
 * ================================================================ */
static int cmd_exit(Interp *ip, char *args) {
    char *p = sk(args);
    if (kw_match(p, "FOR")) {
        /* pop to the innermost FOR frame */
        for (int fi = g_ctrl_top - 1; fi >= 0; fi--) {
            if (g_ctrl[fi].varname[0] != '\x01' &&
                g_ctrl[fi].varname[0] != '\x02' &&
                g_ctrl[fi].varname[0] != '\x03') {
                /* scan forward to NEXT */
                int depth = 1, pc = ip->pc + 1;
                while (pc < g_nlines && depth > 0) {
                    char *t = sk(g_lines[pc].text);
                    if (kw_match(t, "FOR")) depth++;
                    else if (kw_match(t, "NEXT")) depth--;
                    pc++;
                }
                mpf_clear(g_ctrl[fi].limit); mpf_clear(g_ctrl[fi].step);
                g_ctrl_top = fi;
                ip->pc = pc; return 1;
            }
        }
    } else if (kw_match(p, "DO")) {
        /* pop to innermost DO frame, scan forward to LOOP */
        for (int fi = g_ctrl_top - 1; fi >= 0; fi--) {
            if (strcmp(g_ctrl[fi].varname, "\x02" "DO") == 0) {
                int depth = 1, pc = ip->pc + 1;
                while (pc < g_nlines && depth > 0) {
                    char *t = sk(g_lines[pc].text);
                    if (kw_match(t, "DO")) depth++;
                    else if (kw_match(t, "LOOP")) depth--;
                    pc++;
                }
                mpf_clear(g_ctrl[fi].limit); mpf_clear(g_ctrl[fi].step);
                g_ctrl_top = fi;
                ip->pc = pc; return 1;
            }
        }
    } else if (kw_match(p, "SUB") || kw_match(p, "FUNCTION")) {
        /* unwind to the nearest GOSUB return frame */
        for (int fi = g_ctrl_top - 1; fi >= 0; fi--) {
            if (strcmp(g_ctrl[fi].varname, "\x01" "GOSUB") == 0) {
                ip->pc = g_ctrl[fi].line_idx;
                mpf_clear(g_ctrl[fi].limit); mpf_clear(g_ctrl[fi].step);
                g_ctrl_top = fi;
                return 1;
            }
        }
        ip->running = 0; return 1;
    }
    return 0;
}

/* ================================================================
 * REDIM — same as DIM for our purposes (we don't track initialisation)
 * ================================================================ */
static int cmd_redim(Interp *ip, char *args) {
    return cmd_dim(ip, args);
}

/* ================================================================
 * ON ERROR GOTO / RESUME — stub implementations
 * We don't support full error trapping, but we need these to not crash.
 * ON ERROR GOTO 0 disables any pending handler (no-op for us).
 * RESUME NEXT advances past the erroring line (no-op in stub).
 * ================================================================ */
static int cmd_on_error(Interp *ip, char *args) {
    char *p = sk(args);
    if (kw_match(p, "GOTO")) {
        p = sk(p + 4);
        if (*p == '0' && !isalnum((unsigned char)p[1])) {
            /* ON ERROR GOTO 0 — disable handler */
            g_error_handler[0] = '\0';
            return 0;
        }
        /* Register handler label/line — do NOT jump now */
        int i = 0;
        while (*p && !isspace((unsigned char)*p) && i < MAX_VARNAME - 1)
            g_error_handler[i++] = *p++;
        g_error_handler[i] = '\0';
        return 0;
    }
    return 0;
}

static int cmd_resume(Interp *ip, char *args) {
    char *p = sk(args);
    if (kw_match(p, "NEXT")) {
        /* RESUME NEXT — continue at the line after the error */
        if (g_error_resume_pc >= 0) {
            ip->pc = g_error_resume_pc + 1;
            g_error_resume_pc = -1;
            return 1;
        }
        return 0;
    }
    /* RESUME — retry the line that caused the error */
    if (g_error_resume_pc >= 0) {
        ip->pc = g_error_resume_pc;
        g_error_resume_pc = -1;
        return 1;
    }
    return 0;
}

/* ================================================================
 * VIEW PRINT [top TO bottom] — set text viewport (stub: just clear)
 * ================================================================ */
static int cmd_view_print(Interp *ip, char *args) {
    (void)ip; (void)args;
    /* TODO: real viewport when we have a graphical backend */
    return 0;
}

/* ================================================================
 * PALETTE idx, ega_color — maps EGA palette slot to display colour.
 * gorilla.bas uses PALETTE 4, 0 style (EGA 6-bit color number).
 * ================================================================ */
static int cmd_palette(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    mpf_t idx, col; mpf_init2(idx, g_prec); mpf_init2(col, g_prec);
    p = sk(eval_expr(p, idx));
    if (*p == ',') p = sk(p + 1);
    eval_expr(p, col);
    int i = (int)mpf_get_si(idx);
    int c = (int)mpf_get_si(col);
    mpf_clears(idx, col, NULL);
    int r, g2, b;
    ega_to_rgb(c, &r, &g2, &b);
#ifdef USE_SDL_WINDOW
    gfx_palette(i, r, g2, b);
#else
    felix_sendf("palette;%d;%d;%d;%d", i, r, g2, b);
#endif
    return 0;
}

/* ================================================================
 * POKE addr, val — stub
 * ================================================================ */
static int cmd_poke(Interp *ip, char *args) {
    (void)ip; (void)args; return 0;
}

/* ================================================================
 * END SUB / END FUNCTION / END SELECT — handled by their parent,
 * but we need them registered so they don't print "unknown".
 * END IF is similar.
 * ================================================================ */
static int cmd_end_sub(Interp *ip, char *args) {
    /* Treat as RETURN — end of an inline sub body */
    return cmd_return(ip, args);
}

/* ================================================================
 * CALL subname [args] — for now, treat as GOSUB to label
 * ================================================================ */
static int cmd_call(Interp *ip, char *args) {
    char *p = sk(args);
    char name[MAX_VARNAME]; int i = 0;
    while ((isalnum((unsigned char)*p) || *p == '_') && i < MAX_VARNAME - 1)
        name[i++] = *p++;
    name[i] = '\0';
    /* skip argument list — SUBs aren't scoped yet */
    return cmd_gosub(ip, name);
}

/* ================================================================
 * STATIC — variable declaration inside a SUB, treat like DIM
 * ================================================================ */
static int cmd_static(Interp *ip, char *args) {
    return cmd_dim(ip, args);
}


static int cmd_const(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    while (*p) {
        char name[MAX_VARNAME];
        p = sk(read_varname(sk(p), name));
        p = sk(p);
        if (*p == '=') p = sk(p + 1);
        /* string constant? */
        if (*p == '"') {
            char val[MAX_LINE_LEN]; int i = 0;
            p++;
            while (*p && *p != '"' && i < (int)sizeof(val) - 1) val[i++] = *p++;
            if (*p == '"') p++;
            val[i] = '\0';
            const_set(name, val, 1);
        } else {
            /* numeric — store the raw expression text for lazy eval */
            char *start = p;
            /* consume until comma or end (skipping parens) */
            int depth = 0;
            while (*p) {
                if (*p == '(') depth++;
                else if (*p == ')') { if (depth == 0) break; depth--; }
                else if (*p == ',' && depth == 0) break;
                p++;
            }
            char val[MAX_LINE_LEN];
            int len = (int)(p - start);
            if (len >= (int)sizeof(val)) len = (int)sizeof(val) - 1;
            memcpy(val, start, len);
            /* trim trailing whitespace */
            while (len > 0 && isspace((unsigned char)val[len - 1])) len--;
            val[len] = '\0';
            const_set(name, val, 0);
        }
        p = sk(p);
        if (*p == ',') p = sk(p + 1); else break;
    }
    return 0;
}

/* ================================================================
 * DIM — strip optional AS typename suffix before processing
 * ================================================================ */
static int cmd_dim(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    /* SHARED keyword — skip it, all our vars are already global */
    if (kw_match(p, "SHARED")) p = sk(p + 6);
    while (*p && *p != '\'') {
        char name[MAX_VARNAME];
        p = sk(read_varname(p, name));
        if (!name[0]) break;
        /* strip type sigil if present but not already in name */
        if ((*p == '&' || *p == '!' || *p == '#' || *p == '%') &&
            name[strlen(name)-1] != '$') p++;
        Var *v = var_find(name);
        if (!v) v = var_create(name);
        if (*p == '(') {
            p = sk(p + 1);

            #define PARSE_DIM(out_size) do { \
                mpf_t _a; mpf_init2(_a, g_prec); \
                p = sk(eval_expr(sk(p), _a)); \
                if (kw_match(p, "TO")) { \
                    mpf_clear(_a); \
                    p = sk(p + 2); \
                    mpf_t _b; mpf_init2(_b, g_prec); \
                    p = sk(eval_expr(sk(p), _b)); \
                    (out_size) = (int)mpf_get_si(_b) + 1 - g_option_base; \
                    mpf_clear(_b); \
                } else { \
                    (out_size) = (int)mpf_get_si(_a) + 1 - g_option_base; \
                    mpf_clear(_a); \
                } \
            } while(0)

            int dim1, dim2 = 1, ndim = 1;
            PARSE_DIM(dim1);
            if (*p == ',') { p = sk(p + 1); PARSE_DIM(dim2); ndim = 2; }
            #undef PARSE_DIM

            if (*p == ')') p++;
            if (dim1 < 1) dim1 = 1;
            if (dim2 < 1) dim2 = 1;
            int total = dim1 * dim2;
            if (total > MAX_ARRAY_SIZE) { basic_stderr("Array too large: %d\n", total); total = MAX_ARRAY_SIZE; }
            if (var_is_str_name(name)) {
                v->kind = VAR_ARRAY_STR; v->dim[0] = dim1; v->dim[1] = dim2; v->ndim = ndim;
                v->arr_str = (char **)calloc(total, sizeof(char *));
                for (int i = 0; i < total; i++) v->arr_str[i] = str_dup("");
            } else {
                v->kind = VAR_ARRAY_NUM; v->dim[0] = dim1; v->dim[1] = dim2; v->ndim = ndim;
                v->arr_num = (mpf_t *)calloc(total, sizeof(mpf_t));
                for (int i = 0; i < total; i++) { mpf_init2(v->arr_num[i], g_prec); mpf_set_ui(v->arr_num[i], 0); }
            }
        }
        p = sk(p);
        /* skip optional AS typename — e.g. "AS INTEGER", "AS PlayerData" */
        if (kw_match(p, "AS")) {
            p = sk(p + 2);
            /* read the type name */
            char type_name[MAX_VARNAME]; int tni = 0;
            while ((isalnum((unsigned char)*p) || *p == '_') && tni < MAX_VARNAME - 1)
                type_name[tni++] = (char)toupper((unsigned char)*p++);
            type_name[tni] = '\0';
            p = sk(p);
            if (*p == '*') { p = sk(p + 1); while (isdigit((unsigned char)*p)) p++; }
            p = sk(p);

            /* If it's a user-defined TYPE, create flat field variables */
            TypeDef *td = typedef_find(type_name);
            if (td) {
                /* Determine the array size (if any) for this var */
                int arr_count = 1, arr_base = g_option_base;
                Var *base_v = var_find(name);
                if (base_v && (base_v->kind == VAR_ARRAY_NUM || base_v->kind == VAR_ARRAY_STR)) {
                    arr_count = base_v->dim[0] * (base_v->ndim == 2 ? base_v->dim[1] : 1);
                    arr_base  = 0; /* already-created arrays are 0-based internally */
                }

                for (int ai = 0; ai < arr_count; ai++) {
                    int elem_idx = arr_base + ai;
                    for (int fi = 0; fi < td->nfields; fi++) {
                        char flatname[MAX_VARNAME];
                        if (arr_count > 1)
                            snprintf(flatname, sizeof flatname, "%s.%d.%s",
                                     name, elem_idx, td->fields[fi].name);
                        else
                            snprintf(flatname, sizeof flatname, "%s.%s",
                                     name, td->fields[fi].name);

                        if (td->fields[fi].is_str) {
                            char sname[MAX_VARNAME];
                            snprintf(sname, sizeof sname, "%s$", flatname);
                            if (!var_find(sname)) {
                                Var *fv = var_create(sname);
                                (void)fv;
                            }
                        } else {
                            if (!var_find(flatname)) {
                                Var *fv = var_create(flatname);
                                (void)fv;
                            }
                        }
                    }
                }
            }
        }
        if (*p == ',') p = sk(p + 1);
    }
    return 0;
}

/* ================================================================
 * LET / implicit assignment
 * ================================================================ */
static int cmd_let(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);

    /* Check for struct field access: identifier optionally followed by (idx).field */
    {
        char *save = p;
        char flatname[MAX_VARNAME];
        char *after = parse_field_varname(p, flatname);
        if (after) {
            after = sk(after);
            if (*after == '=') {
                /* It's a field assignment */
                after = sk(after + 1);
                int is_str = (flatname[strlen(flatname)-1] == '$') ||
                             strrchr(flatname, '.') != NULL;
                /* Determine by trying string eval if it looks like a string */
                if (is_str_token(after)) {
                    char sbuf[1024];
                    eval_str_expr(after, sbuf, sizeof sbuf);
                    /* store as string — append $ sigil if not present */
                    char sname[MAX_VARNAME];
                    snprintf(sname, sizeof sname, "%s$", flatname);
                    Var *v = var_get(sname);
                    free(v->str); v->str = str_dup(sbuf);
                } else {
                    mpf_t val; mpf_init2(val, g_prec);
                    eval_expr(after, val);
                    Var *v = var_get(flatname);
                    mpf_set(v->num, val);
                    mpf_clear(val);
                }
                return 0;
            }
        }
        p = save;
    }

    char name[MAX_VARNAME];
    p = sk(read_varname(p, name));
    int arr_i = 0, arr_j = 1, is_arr = 0;
    if (*p == '(') {
        is_arr = 1; p = sk(p + 1);
        mpf_t i1; mpf_init2(i1, g_prec);
        p = sk(eval_expr(p, i1)); arr_i = (int)mpf_get_si(i1); mpf_clear(i1);
        if (*p == ',') { p=sk(p+1); mpf_t i2; mpf_init2(i2,g_prec); p=sk(eval_expr(p,i2)); arr_j=(int)mpf_get_si(i2); mpf_clear(i2); }
        if (*p == ')') p = sk(p + 1);
    }
    if (*p == '=') p = sk(p + 1);
    if (var_is_str_name(name)) {
        char sbuf[1024];
        eval_str_expr(sk(p), sbuf, sizeof sbuf);
        Var *v = var_get(name);
        if (is_arr && v->kind == VAR_ARRAY_STR) {
            char **slot = arr_str_elem(v, arr_i, arr_j);
            free(*slot); *slot = str_dup(sbuf);
        } else {
            free(v->str); v->str = str_dup(sbuf);
        }
    } else {
        mpf_t val; mpf_init2(val, g_prec);
        eval_expr(sk(p), val);
        if (is_arr) { Var *v = var_get(name); mpf_set(*arr_num_elem(v, arr_i, arr_j), val); }
        else        { Var *v = var_get(name); mpf_set(v->num, val); }
        mpf_clear(val);
    }
    return 0;
}

/* ================================================================
 * PRINT
 * ================================================================ */
static int cmd_print_file(Interp *ip, char *args);  /* forward */

static int cmd_print(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (*p == '#') return cmd_print_file(ip, args);

    if (kw_match(p, "USING")) {
        p = sk(p + 5);
        char fmt[128]; int fi = 0;
        if (*p == '"') {
            p++;
            while (*p && *p != '"' && fi < (int)sizeof(fmt) - 1) fmt[fi++] = *p++;
            if (*p == '"') p++;
        }
        fmt[fi] = '\0';
        p = sk(p); if (*p == ';') p = sk(p + 1);
        int trailing = 0;
        while (*p) {
            mpf_t val; mpf_init2(val, g_prec);
            p = sk(eval_expr(p, val));
            print_using(fmt, mpf_get_d(val));
            mpf_clear(val);
            if (*p == ';') { p = sk(p + 1); trailing = 1; }
            else           { trailing = 0; break; }
        }
        if (!trailing) display_newline();
        return 0;
    }

    int trailing_sep = 0;
    while (*p) {
        trailing_sep = 0;
        if (is_str_token(p)) {
            if (kw_match(p, "SPC")) {
                p = sk(p + 3); if (*p == '(') p++;
                mpf_t n; mpf_init2(n, g_prec); p = eval_expr(sk(p), n);
                display_spc((int)mpf_get_si(n)); mpf_clear(n);
                p = sk(p); if (*p == ')') p = sk(p + 1);
            } else if (kw_match(p, "TAB")) {
                p = sk(p + 3); if (*p == '(') p++;
                mpf_t n; mpf_init2(n, g_prec); p = eval_expr(sk(p), n);
                int col = (int)mpf_get_si(n) - 1; mpf_clear(n);
                p = sk(p); if (*p == ')') p = sk(p + 1);
                if (col > 0) display_spc(col);
            } else {
                char sbuf[1024];
                p = eval_str_expr(p, sbuf, sizeof sbuf);
                display_print(sbuf);
            }
        } else {
            mpf_t val; mpf_init2(val, g_prec);
            p = eval_expr(p, val);
            double d = mpf_get_d(val);
            if (d == floor(d) && fabs(d) < 1e15) {
                if (d >= 0) printf(" %.0f ", d);
                else        printf("%.0f ", d);
            } else {
                char buf[64];
                snprintf(buf, sizeof buf, "%.7G", d);
                if (strchr(buf, '.') && !strchr(buf, 'E')) {
                    char *end = buf + strlen(buf) - 1;
                    while (*end == '0') *end-- = '\0';
                    if (*end == '.') *end = '\0';
                }
                if (d >= 0) printf(" %s ", buf);
                else        printf("%s ", buf);
            }
            mpf_clear(val);
        }
        p = sk(p);
        if (*p == ';') { trailing_sep = 1; p = sk(p + 1); }
        else if (*p == ',') { display_putchar('\t'); trailing_sep = 1; p = sk(p + 1); }
        else break;
    }
    if (!trailing_sep) display_newline();
    return 0;
}

/* ================================================================
 * LINE INPUT / INPUT
 * ================================================================ */
static int cmd_line_input_file(Interp *ip, char *args);  /* forward */
static int cmd_input_file(Interp *ip, char *args);       /* forward */

static int cmd_line_input(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (*p == '#') return cmd_line_input_file(ip, args);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') display_putchar(*p++);
        if (*p == '"') p++;
    }
    p = sk(p); if (*p == ';' || *p == ',') p = sk(p + 1);
    char name[MAX_VARNAME];
    read_varname(p, name);
    char linebuf[DEFAULT_BUFFER];
    display_cursor(1);
    display_getline(linebuf, sizeof linebuf);
    display_newline();
    Var *v = var_get(name);
    free(v->str); v->str = str_dup(linebuf);
    return 0;
}

static int cmd_input(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (*p == '#') return cmd_input_file(ip, args);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') display_putchar(*p++);
        if (*p == '"') p++;
        p = sk(p);
        if (*p == ';' || *p == ',') p = sk(p + 1);
        display_print("? ");
    } else {
        display_print("? ");
    }
    char linebuf[DEFAULT_BUFFER];
    display_cursor(1);
    display_getline(linebuf, sizeof linebuf);
    display_newline();
    char *tok = linebuf;
    while (*p) {
        char name[MAX_VARNAME];
        p = sk(read_varname(sk(p), name));
        char *comma = strchr(tok, ',');
        char val_str[DEFAULT_BUFFER];
        if (comma) {
            size_t len = (size_t)(comma - tok);
            if (len >= sizeof val_str) len = sizeof val_str - 1;
            memcpy(val_str, tok, len); val_str[len] = '\0';
            tok = comma + 1;
        } else {
            strncpy(val_str, tok, sizeof val_str - 1);
            val_str[sizeof val_str - 1] = '\0';
            tok += strlen(tok);
        }
        char *v_start = val_str;
        while (isspace((unsigned char)*v_start)) v_start++;
        char *v_end = v_start + strlen(v_start);
        while (v_end > v_start && isspace((unsigned char)v_end[-1])) *--v_end = '\0';
        Var *v = var_get(name);
        if (var_is_str_name(name)) { free(v->str); v->str = str_dup(v_start); }
        else                       { mpf_set_d(v->num, atof(v_start)); }
        p = sk(p); if (*p == ',') p = sk(p + 1); else break;
    }
    return 0;
}

/* ================================================================
 * File I/O commands
 * ================================================================ */
static int cmd_open(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    char filename[DEFAULT_BUFFER]; int fi = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && fi < (int)sizeof(filename) - 1) filename[fi++] = *p++;
        if (*p == '"') p++;
    }
    filename[fi] = '\0';
    p = sk(p);
    char mode_ch = 'O';
    if (kw_match(p, "FOR")) {
        p = sk(p + 3);
        if      (kw_match(p, "INPUT"))  { mode_ch = 'I'; p = sk(p + 5); }
        else if (kw_match(p, "OUTPUT")) { mode_ch = 'O'; p = sk(p + 6); }
        else if (kw_match(p, "APPEND")) { mode_ch = 'A'; p = sk(p + 6); }
    }
    if (kw_match(p, "AS")) p = sk(p + 2);
    if (*p == '#') p = sk(p + 1);
    mpf_t num; mpf_init2(num, g_prec);
    p = eval_expr(p, num);
    int n = (int)mpf_get_si(num); mpf_clear(num);
    FileHandle *fh = fh_get(n);
    if (fh->fp) { fclose(fh->fp); fh->fp = NULL; }
    fh->fp = fopen(filename, (mode_ch == 'I') ? "r" : (mode_ch == 'A') ? "a" : "w");
    if (!fh->fp) { perror(filename); return 0; }
    fh->mode = mode_ch;
    return 0;
}

static int cmd_close(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (!*p) {
        for (int i = 1; i <= MAX_FILE_HANDLES; i++)
            if (g_files[i].fp) { fclose(g_files[i].fp); g_files[i].fp = NULL; g_files[i].mode = 0; }
        return 0;
    }
    while (*p) {
        if (*p == '#') p = sk(p + 1);
        mpf_t num; mpf_init2(num, g_prec);
        p = sk(eval_expr(p, num));
        int n = (int)mpf_get_si(num); mpf_clear(num);
        FileHandle *fh = fh_get(n);
        if (fh->fp) { fclose(fh->fp); fh->fp = NULL; fh->mode = 0; }
        if (*p == ',') p = sk(p + 1); else break;
    }
    return 0;
}

static int cmd_input_file(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (*p == '#') p = sk(p + 1);
    mpf_t num; mpf_init2(num, g_prec);
    p = sk(eval_expr(p, num)); int n = (int)mpf_get_si(num); mpf_clear(num);
    if (*p == ',') p = sk(p + 1);
    FileHandle *fh = fh_get(n);
    if (!fh->fp || fh->mode != 'I') { basic_stderr("File #%d not open for input\n", n); return 0; }
    while (*p) {
        char name[MAX_VARNAME];
        p = sk(read_varname(sk(p), name));
        char linebuf[DEFAULT_BUFFER];
        if (!fgets(linebuf, sizeof linebuf, fh->fp)) linebuf[0] = '\0';
        linebuf[strcspn(linebuf, "\r\n")] = '\0';
        char *val = linebuf;
        while (isspace((unsigned char)*val)) val++;
        if (*val == '"') { val++; char *q = strchr(val, '"'); if (q) *q = '\0'; }
        Var *v = var_get(name);
        if (var_is_str_name(name)) { free(v->str); v->str = str_dup(val); }
        else                       { mpf_set_d(v->num, atof(val)); }
        p = sk(p); if (*p == ',') p = sk(p + 1); else break;
    }
    return 0;
}

static int cmd_print_file(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (*p == '#') p = sk(p + 1);
    mpf_t num; mpf_init2(num, g_prec);
    p = sk(eval_expr(p, num)); int n = (int)mpf_get_si(num); mpf_clear(num);
    if (*p == ',') p = sk(p + 1);
    FileHandle *fh = fh_get(n);
    if (!fh->fp || fh->mode == 'I') { basic_stderr("File #%d not open for output\n", n); return 0; }
    int trailing = 0;
    while (*p) {
        trailing = 0;
        if (is_str_token(p)) {
            char sbuf[1024]; p = eval_str_expr(p, sbuf, sizeof sbuf); fputs(sbuf, fh->fp);
        } else {
            mpf_t val; mpf_init2(val, g_prec);
            p = eval_expr(p, val);
            double d = mpf_get_d(val); mpf_clear(val);
            if (d == floor(d) && fabs(d) < 1e15)
                fprintf(fh->fp, d >= 0 ? " %.0f " : "%.0f ", d);
            else
                fprintf(fh->fp, d >= 0 ? " %g " : "%g ", d);
        }
        p = sk(p);
        if      (*p == ';') { trailing = 1; p = sk(p + 1); }
        else if (*p == ',') { fputc('\t', fh->fp); trailing = 1; p = sk(p + 1); }
        else break;
    }
    if (!trailing) fputc('\n', fh->fp);
    return 0;
}

static int cmd_line_input_file(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (*p == '#') p = sk(p + 1);
    mpf_t num; mpf_init2(num, g_prec);
    p = sk(eval_expr(p, num)); int n = (int)mpf_get_si(num); mpf_clear(num);
    if (*p == ',') p = sk(p + 1);
    FileHandle *fh = fh_get(n);
    if (!fh->fp || fh->mode != 'I') { basic_stderr("File #%d not open for input\n", n); return 0; }
    char name[MAX_VARNAME];
    read_varname(sk(p), name);
    char linebuf[DEFAULT_BUFFER];
    if (!fgets(linebuf, sizeof linebuf, fh->fp)) linebuf[0] = '\0';
    linebuf[strcspn(linebuf, "\r\n")] = '\0';
    Var *v = var_get(name);
    free(v->str); v->str = str_dup(linebuf);
    return 0;
}

/* ================================================================
 * GET/PUT graphics — Felix sprite capture/blit.
 * GET (x1,y1)-(x2,y2), array_var
 * PUT (x,y), array_var [, PSET|XOR]
 * ================================================================ */
static int cmd_get_graphics(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    double x1, y1, x2, y2;
    p = parse_xy(p, &x1, &y1);
    if (*p == '-') p = sk(p + 1);
    p = parse_xy(p, &x2, &y2);
    if (*p == ',') p = sk(p + 1);
    char vname[MAX_VARNAME];
    read_varname(sk(p), vname);
    Var *v = var_get(vname);
    int id = sprite_id_for(v);
#ifdef USE_SDL_WINDOW
    gfx_get(id, (int)x1, (int)y1, (int)x2, (int)y2);
#else
    felix_drawf("get;%d;%d;%d;%d;%d", id,
                (int)x1, (int)y1, (int)x2, (int)y2);
#endif
    return 0;
}

static int cmd_put_graphics(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    double x, y;
    p = parse_xy(p, &x, &y);
    if (*p == ',') p = sk(p + 1);
    char vname[MAX_VARNAME];
    p = sk(read_varname(sk(p), vname));
    Var *v = var_get(vname);
    int id = sprite_id_for(v);
    /* optional mode: PSET or XOR */
    char *mode = "pset";
    p = sk(p); if (*p == ',') p = sk(p + 1);
    if (kw_match(p, "XOR"))  mode = "xor";
#ifdef USE_SDL_WINDOW
    gfx_put(id, (int)x, (int)y, (strcmp(mode, "xor") == 0) ? 1 : 0);
#else
    felix_drawf("put;%d;%d;%d;%s", id, (int)x, (int)y, mode);
#endif
    return 0;
}

static int cmd_draw(Interp *ip, char *args) {
    (void)ip; (void)args; return 0;  /* DRAW string mini-language — not needed for gorilla */
}

/* ================================================================
 * CIRCLE (x, y), r [, color [, start_angle, end_angle [, aspect]]]
 * ================================================================ */
static int cmd_circle(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    double x, y;
    p = sk(parse_xy(p, &x, &y));
    if (*p == ',') p = sk(p + 1);
    mpf_t mr; mpf_init2(mr, g_prec);
    p = sk(eval_expr(p, mr));
    double r = mpf_get_d(mr); mpf_clear(mr);
    int color = 15;
    if (*p == ',') {
        p = sk(p + 1);
        mpf_t mc; mpf_init2(mc, g_prec);
        p = sk(eval_expr(p, mc));
        color = (int)mpf_get_si(mc); mpf_clear(mc);
    }
    /* consume optional start_angle, end_angle, aspect — not supported by Felix,
     * just draw the full circle. Handle empty args like CIRCLE x,y,r,c,,,aspect */
    while (*p == ',') {
        p = sk(p + 1);
        if (*p == ',' || *p == '\0' || *p == ':') continue; /* empty arg */
        mpf_t tmp; mpf_init2(tmp, g_prec);
        p = sk(eval_expr(p, tmp));
        mpf_clear(tmp);
    }
#ifdef USE_SDL_WINDOW
    gfx_circle((int)x, (int)y, (int)(r + 0.5), color);
#else
    felix_drawf("circle;%d;%d;%d;%d",
                (int)x, (int)y, (int)(r + 0.5), color);
#endif
    return 0;
}

/* ================================================================
 * LINE [(x1,y1)]-(x2,y2), color [, B[F]]
 * ================================================================ */
static int cmd_line_gfx(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    double x1 = 0, y1 = 0, x2, y2;

    /* optional start point */
    if (*p == '(') {
        p = sk(parse_xy(p, &x1, &y1));
    }
    if (*p == '-') p = sk(p + 1);
    p = sk(parse_xy(p, &x2, &y2));

    int color = 15;
    if (*p == ',') {
        p = sk(p + 1);
        mpf_t mc; mpf_init2(mc, g_prec);
        p = sk(eval_expr(p, mc));
        color = (int)mpf_get_si(mc); mpf_clear(mc);
    }

    char *suffix = "";
    p = sk(p);
    if (*p == ',') {
        p = sk(p + 1);
        if (kw_match(p, "BF")) suffix = ";BF";
        else if (*p == 'B' || *p == 'b') suffix = ";B";
    }

#ifdef USE_SDL_WINDOW
    if      (strcmp(suffix, ";BF") == 0) gfx_boxfill((int)x1,(int)y1,(int)x2,(int)y2, color);
    else if (strcmp(suffix, ";B")  == 0) gfx_box    ((int)x1,(int)y1,(int)x2,(int)y2, color);
    else                                 gfx_line   ((int)x1,(int)y1,(int)x2,(int)y2, color);
#else
    felix_drawf("line;%d;%d;%d;%d;%d%s",
                (int)x1, (int)y1, (int)x2, (int)y2, color, suffix);
#endif
    return 0;
}

/* ================================================================
 * PAINT (x, y), fill_color [, border_color]
 * ================================================================ */
static int cmd_paint(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    double x, y;
    p = sk(parse_xy(p, &x, &y));
    int fill = 15, border = -1;
    if (*p == ',') {
        p = sk(p + 1);
        mpf_t mc; mpf_init2(mc, g_prec);
        p = sk(eval_expr(p, mc));
        fill = (int)mpf_get_si(mc); mpf_clear(mc);
    }
    if (*p == ',') {
        p = sk(p + 1);
        mpf_t mc; mpf_init2(mc, g_prec);
        eval_expr(p, mc);
        border = (int)mpf_get_si(mc); mpf_clear(mc);
    }
    int bc = (border >= 0) ? border : fill;
#ifdef USE_SDL_WINDOW
    gfx_paint((int)x, (int)y, fill, bc);
#else
    if (border >= 0)
        felix_drawf("paint;%d;%d;%d;%d", (int)x, (int)y, fill, border);
    else
        felix_drawf("paint;%d;%d;%d", (int)x, (int)y, fill);
#endif
    return 0;
}

/* ================================================================
 * PSET (x, y) [, color]
 * ================================================================ */
static int cmd_pset(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    double x, y;
    p = sk(parse_xy(p, &x, &y));
    int color = 15;
    if (*p == ',') {
        p = sk(p + 1);
        mpf_t mc; mpf_init2(mc, g_prec);
        eval_expr(p, mc);
        color = (int)mpf_get_si(mc); mpf_clear(mc);
    }
#ifdef USE_SDL_WINDOW
    gfx_pset((int)x, (int)y, color);
#else
    felix_drawf("pset;%d;%d;%d", (int)x, (int)y, color);
#endif
    return 0;
}

/* ================================================================
 * Utility: build a flat variable name for struct field access.
 * "PDat(2).PNam" → "PDAT.2.PNAM"
 * "Settings.UseSound" → "SETTINGS.USESOUND"
 * Result written into out (must be MAX_VARNAME bytes).
 * Returns pointer past the parsed text, or NULL on failure.
 * ================================================================ */
static char *parse_field_varname(char *p, char *out) {
    /* read base name */
    char base[MAX_VARNAME]; int bi = 0;
    while ((isalnum((unsigned char)*p) || *p == '_') && bi < MAX_VARNAME - 1)
        base[bi++] = (char)toupper((unsigned char)*p++);
    base[bi] = '\0';
    if (!bi) return NULL;

    /* optional array index */
    char idx_str[32] = "";
    if (*p == '(') {
        p = sk(p + 1);
        mpf_t v; mpf_init2(v, g_prec);
        p = sk(eval_expr(p, v));
        int idx1 = (int)mpf_get_si(v);
        mpf_clear(v);
        snprintf(idx_str, sizeof idx_str, "%d", idx1);
        if (*p == ',') {
            p = sk(p + 1);
            mpf_t v2; mpf_init2(v2, g_prec);
            p = sk(eval_expr(p, v2));
            int idx2 = (int)mpf_get_si(v2);
            mpf_clear(v2);
            char tmp2[16]; snprintf(tmp2, sizeof tmp2, ",%d", idx2);
            strncat(idx_str, tmp2, sizeof idx_str - strlen(idx_str) - 1);
        }
        if (*p == ')') p++;
    }
    p = sk(p);
    if (*p != '.') return NULL;
    p = sk(p + 1);

    /* read field name */
    char field[MAX_VARNAME]; int fi = 0;
    while ((isalnum((unsigned char)*p) || *p == '_') && fi < MAX_VARNAME - 1)
        field[fi++] = (char)toupper((unsigned char)*p++);
    field[fi] = '\0';
    if (!fi) return NULL;

    /* build flat name: BASE.IDX.FIELD or BASE.FIELD */
    if (idx_str[0])
        snprintf(out, MAX_VARNAME, "%s.%s.%s", base, idx_str, field);
    else
        snprintf(out, MAX_VARNAME, "%s.%s", base, field);

    return p;
}
static int cmd_for(Interp *ip, char *args) {
    char *p = sk(args);
    char vname[MAX_VARNAME];
    p = sk(read_varname(p, vname));
    if (*p == '=') p = sk(p + 1);
    mpf_t start, limit, step;
    mpf_init2(start, g_prec); mpf_init2(limit, g_prec); mpf_init2(step, g_prec);
    mpf_set_ui(step, 1);
    p = sk(eval_expr(p, start));
    if (strncasecmp(p, "TO", 2) == 0) p = sk(p + 2);
    p = sk(eval_expr(p, limit));
    if (strncasecmp(p, "STEP", 4) == 0) { p = sk(p + 4); eval_expr(p, step); }
    Var *v = var_get(vname); mpf_set(v->num, start);
    if (g_ctrl_top >= CTRL_STACK_MAX) { basic_stderr("Stack overflow\n"); return -1; }
    CtrlFrame *f = &g_ctrl[g_ctrl_top++];
    strncpy(f->varname, vname, MAX_VARNAME - 1);
    mpf_init2(f->limit, g_prec); mpf_set(f->limit, limit);
    mpf_init2(f->step,  g_prec); mpf_set(f->step,  step);
    f->line_idx = ip->pc;
    mpf_clears(start, limit, step, NULL);
    return 0;
}

static int cmd_next(Interp *ip, char *args) {
    char *p = sk(args);
    char vname[MAX_VARNAME] = "";
    if (isalpha((unsigned char)*p)) read_varname(p, vname);
    int fi = g_ctrl_top - 1;
    if (*vname) {
        for (fi = g_ctrl_top - 1; fi >= 0; fi--)
            if (strcasecmp(g_ctrl[fi].varname, vname) == 0) break;
        if (fi < 0) { basic_stderr("NEXT without FOR: %s\n", vname); return -1 ; }
    }
    CtrlFrame *f = &g_ctrl[fi];
    Var *cv = var_get(f->varname);
    mpf_add(cv->num, cv->num, f->step);
    int done = (mpf_sgn(f->step) > 0)
             ? (mpf_cmp(cv->num, f->limit) > 0)
             : (mpf_cmp(cv->num, f->limit) < 0);
    if (done) { mpf_clear(f->limit); mpf_clear(f->step); g_ctrl_top = fi; return 0; }
    ip->pc = f->line_idx + 1; return 1;
}

int cmd_goto(Interp *ip, char *args) {
    char *p = sk(args);
    int idx;
    if (isdigit((unsigned char)*p)) {
        idx = find_line_idx(atoi(p));
    } else {
        /* label-based jump: read the identifier and look it up */
        char lname[MAX_VARNAME]; int i = 0;
        while ((isalnum((unsigned char)*p) || *p == '_') && i < MAX_VARNAME - 1)
            lname[i++] = *p++;
        lname[i] = '\0';
        idx = find_line_by_label(lname);
    }
    if (idx < 0) { basic_stderr("GOTO: target not found: %s\n", sk(args)); return -1; }
    ip->pc = idx; return 1;
}

int cmd_gosub(Interp *ip, char *args) {
    if (g_ctrl_top >= CTRL_STACK_MAX) { basic_stderr("Stack overflow\n"); return-1; }
    CtrlFrame *f = &g_ctrl[g_ctrl_top++];
    strcpy(f->varname, "\x01" "GOSUB");
    f->line_idx = ip->pc + 1;
    mpf_init2(f->limit, g_prec); mpf_set_ui(f->limit, 0);
    mpf_init2(f->step,  g_prec); mpf_set_ui(f->step,  0);
    return cmd_goto(ip, args);
}

static int cmd_return(Interp *ip, char *args) {
    (void)args;
    for (int fi = g_ctrl_top - 1; fi >= 0; fi--) {
        if (strcmp(g_ctrl[fi].varname, "\x01" "GOSUB") == 0) {
            ip->pc = g_ctrl[fi].line_idx;
            mpf_clear(g_ctrl[fi].limit); mpf_clear(g_ctrl[fi].step);
            g_ctrl_top = fi; return 1;
        }
    }
    basic_stderr("RETURN without GOSUB\n"); return -1;
}

/* ================================================================
 * IF / THEN / ELSE
 * ================================================================ */
static int eval_one_cmp(char **pp) {
    char *p = sk(*pp);
    int cmp = 0;

    if (*p == '(') {
        p++;
        if (is_str_token(p)) {
            char lhs[1024], rhs[1024];
            p = sk(eval_str_or_inkey(p, lhs, sizeof lhs));
            char op2[3] = {p[0],p[1],'\0'}; int oplen = 2;
            if (!strcmp(op2,"<>")||!strcmp(op2,"><")||!strcmp(op2,"<=")||
                !strcmp(op2,"=<")||!strcmp(op2,">=")||!strcmp(op2,"=>")) ;
            else { op2[1] = '\0'; oplen = 1; }
            p = sk(eval_str_or_inkey(sk(p + oplen), rhs, sizeof rhs));
            int c = strcmp(lhs, rhs);
            if      (!strcmp(op2,"<>")||!strcmp(op2,"><")) cmp=(c!=0);
            else if (!strcmp(op2,"<=")||!strcmp(op2,"=<")) cmp=(c<=0);
            else if (!strcmp(op2,">=")||!strcmp(op2,"=>")) cmp=(c>=0);
            else if (op2[0]=='<') cmp=(c<0); else if (op2[0]=='>') cmp=(c>0);
            else cmp=(c==0);
        } else {
            mpf_t lhs; mpf_init2(lhs, g_prec);
            p = sk(eval_expr(p, lhs));
            char op2[3] = {p[0],p[1],'\0'}; int oplen = 2;
            if (!strcmp(op2,"<>")||!strcmp(op2,"><")||!strcmp(op2,"<=")||
                !strcmp(op2,"=<")||!strcmp(op2,">=")||!strcmp(op2,"=>")) ;
            else if (p[0]=='<'||p[0]=='>'||p[0]=='=') { op2[1]='\0'; oplen=1; }
            else { cmp=(mpf_sgn(lhs)!=0); mpf_clear(lhs); goto closeparen; }
            mpf_t rhs; mpf_init2(rhs, g_prec);
            p = sk(eval_expr(sk(p + oplen), rhs));
            int c = mpf_cmp(lhs, rhs);
            if      (!strcmp(op2,"<>")||!strcmp(op2,"><")) cmp=(c!=0);
            else if (!strcmp(op2,"<=")||!strcmp(op2,"=<")) cmp=(c<=0);
            else if (!strcmp(op2,">=")||!strcmp(op2,"=>")) cmp=(c>=0);
            else if (op2[0]=='<') cmp=(c<0); else if (op2[0]=='>') cmp=(c>0);
            else cmp=(c==0);
            mpf_clears(lhs, rhs, NULL);
            while (kw_match(p,"AND") || kw_match(p,"OR")) {
                int is_and = kw_match(p,"AND");
                p = sk(p + (is_and ? 3 : 2));
                mpf_t lhs2; mpf_init2(lhs2, g_prec);
                p = sk(eval_expr(p, lhs2));
                char op3[3]={p[0],p[1],'\0'}; int ol3=2;
                if (!strcmp(op3,"<>")||!strcmp(op3,"><")||!strcmp(op3,"<=")||
                    !strcmp(op3,"=<")||!strcmp(op3,">=")||!strcmp(op3,"=>")) ;
                else if (p[0]=='<'||p[0]=='>'||p[0]=='=') { op3[1]='\0'; ol3=1; }
                else { int r2=(mpf_sgn(lhs2)!=0); mpf_clear(lhs2);
                       cmp=is_and?(cmp&&r2):(cmp||r2); continue; }
                mpf_t rhs2; mpf_init2(rhs2, g_prec);
                p = sk(eval_expr(sk(p + ol3), rhs2));
                int c2 = mpf_cmp(lhs2, rhs2), cmp2;
                if      (!strcmp(op3,"<>")||!strcmp(op3,"><")) cmp2=(c2!=0);
                else if (!strcmp(op3,"<=")||!strcmp(op3,"=<")) cmp2=(c2<=0);
                else if (!strcmp(op3,">=")||!strcmp(op3,"=>")) cmp2=(c2>=0);
                else if (op3[0]=='<') cmp2=(c2<0); else if (op3[0]=='>') cmp2=(c2>0);
                else cmp2=(c2==0);
                mpf_clears(lhs2, rhs2, NULL);
                cmp = is_and ? (cmp && cmp2) : (cmp || cmp2);
            }
        }
        closeparen:
        p = sk(p); if (*p == ')') p = sk(p + 1);
        *pp = p;
        return cmp;
    }

    if (is_str_token(p)) {
        char lhs[1024], rhs[1024];
        p = sk(eval_str_or_inkey(p, lhs, sizeof lhs));
        char op2[3] = {p[0],p[1],'\0'}; int oplen = 2;
        if (!strcmp(op2,"<>")||!strcmp(op2,"><")||!strcmp(op2,"<=")||
            !strcmp(op2,"=<")||!strcmp(op2,">=")||!strcmp(op2,"=>")) ;
        else { op2[1]='\0'; oplen=1; }
        p = sk(eval_str_or_inkey(sk(p + oplen), rhs, sizeof rhs));
        int c = strcmp(lhs, rhs);
        if      (!strcmp(op2,"<>")||!strcmp(op2,"><")) cmp=(c!=0);
        else if (!strcmp(op2,"<=")||!strcmp(op2,"=<")) cmp=(c<=0);
        else if (!strcmp(op2,">=")||!strcmp(op2,"=>")) cmp=(c>=0);
        else if (op2[0]=='<') cmp=(c<0); else if (op2[0]=='>') cmp=(c>0);
        else cmp=(c==0);
    } else {
        mpf_t lhs; mpf_init2(lhs, g_prec);
        p = sk(eval_expr(p, lhs));
        char op2[3] = {p[0],p[1],'\0'}; int oplen = 2;
        if (!strcmp(op2,"<>")||!strcmp(op2,"><")||!strcmp(op2,"<=")||
            !strcmp(op2,"=<")||!strcmp(op2,">=")||!strcmp(op2,"=>")) ;
        else if (p[0]=='<'||p[0]=='>'||p[0]=='=') { op2[1]='\0'; oplen=1; }
        else { cmp=(mpf_sgn(lhs)!=0); mpf_clear(lhs); *pp=p; return cmp; }
        mpf_t rhs; mpf_init2(rhs, g_prec);
        p = sk(eval_expr(sk(p + oplen), rhs));
        int c = mpf_cmp(lhs, rhs);
        if      (!strcmp(op2,"<>")||!strcmp(op2,"><")) cmp=(c!=0);
        else if (!strcmp(op2,"<=")||!strcmp(op2,"=<")) cmp=(c<=0);
        else if (!strcmp(op2,">=")||!strcmp(op2,"=>")) cmp=(c>=0);
        else if (op2[0]=='<') cmp=(c<0); else if (op2[0]=='>') cmp=(c>0);
        else cmp=(c==0);
        mpf_clears(lhs, rhs, NULL);
    }
    *pp = p;
    return cmp;
}

static char *find_else(char *p) {
    int in_str = 0;
    while (*p) {
        if (*p == '"') { in_str = !in_str; p++; continue; }
        if (!in_str && kw_match(p, "ELSE")) return p;
        p++;
    }
    return NULL;
}

static int cmd_if(Interp *ip, char *args) {
    char *p = sk(args);
    int result = eval_one_cmp(&p);
    p = sk(p);
    while (kw_match(p,"AND") || kw_match(p,"OR")) {
        int is_and = kw_match(p,"AND");
        p = sk(p + (is_and ? 3 : 2));
        int next = eval_one_cmp(&p);
        result = is_and ? (result && next) : (result || next);
        p = sk(p);
    }
    if (kw_match(p,"THEN")) p = sk(p + 4);
    char *else_p = find_else(p);
    if (result) {
        char then_clause[MAX_LINE_LEN];
        if (else_p) {
            size_t len = (size_t)(else_p - p);
            if (len >= MAX_LINE_LEN) len = MAX_LINE_LEN - 1;
            memcpy(then_clause, p, len); then_clause[len] = '\0';
            p = then_clause;
        }
        /* THEN linenum  — bare number jumps directly */
        if (isdigit((unsigned char)*p)) return cmd_goto(ip, p);
        int jumped = dispatch_multi(ip, p);
        if (!jumped) ip->pc++;
        return 1;
    } else {
        ip->pc++;
        if (!else_p) return 1;
        p = sk(else_p + 4);
        /* ELSE linenum — bare number jumps directly */
        if (isdigit((unsigned char)*p)) return cmd_goto(ip, p);
        dispatch_multi(ip, p);
        return 1;
    }
}

/* ================================================================
 * DATA / READ / RESTORE
 * ================================================================ */
static int cmd_data(Interp *ip, char *args)    { (void)ip;(void)args; return 0; }
static int cmd_restore(Interp *ip, char *args) { (void)ip;(void)args; g_data_pos=0; return 0; }

static int cmd_read(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    while (*p) {
        char name[MAX_VARNAME];
        p = sk(read_varname(sk(p), name));
        int arr_i = 0, arr_j = 1, is_arr = 0;
        if (*p == '(') {
            is_arr = 1; p = sk(p + 1);
            mpf_t i1; mpf_init2(i1, g_prec);
            p = sk(eval_expr(p, i1)); arr_i = (int)mpf_get_si(i1); mpf_clear(i1);
            if (*p==',') { p=sk(p+1); mpf_t i2; mpf_init2(i2,g_prec); p=sk(eval_expr(p,i2)); arr_j=(int)mpf_get_si(i2); mpf_clear(i2); }
            if (*p==')') p = sk(p + 1);
        }
        if (g_data_pos >= g_data_count) { basic_stderr("READ: out of data\n"); return -1; }
        char *item = g_data[g_data_pos++];
        Var *v = var_get(name);
        if (var_is_str_name(name)) {
            if (is_arr && v->kind == VAR_ARRAY_STR) {
                char **slot = arr_str_elem(v, arr_i, arr_j);
                free(*slot); *slot = str_dup(item);
            } else { free(v->str); v->str = str_dup(item); }
        } else {
            if (is_arr) mpf_set_d(*arr_num_elem(v, arr_i, arr_j), atof(item));
            else        mpf_set_d(v->num, atof(item));
        }
        p = sk(p); if (*p == ',') p = sk(p + 1); else break;
    }
    return 0;
}

/* ================================================================
 * DEF FN
 * ================================================================ */
static int cmd_def(Interp *ip, char *args) {
    (void)ip;
    char *p = sk(args);
    if (!(toupper((unsigned char)p[0])=='F' && toupper((unsigned char)p[1])=='N'
          && isalnum((unsigned char)p[2]))) return 0;
    p += 2;
    if (g_defn_count >= MAX_DEF_FN) return 0;
    DefFn *fn = &g_defn[g_defn_count++];
    int i = 0;
    fn->name[i++] = 'F'; fn->name[i++] = 'N';
    while (isalnum((unsigned char)*p) && i < MAX_VARNAME - 1) fn->name[i++] = (char)toupper(*p++);
    fn->name[i] = '\0';
    p = sk(p);
    fn->param[0] = '\0';
    if (*p == '(') {
        p = sk(p + 1); int j = 0;
        while (*p && *p != ')' && j < MAX_VARNAME - 1) fn->param[j++] = (char)toupper(*p++);
        fn->param[j] = '\0';
        if (*p == ')') p++;
    }
    p = sk(p); if (*p == '=') p = sk(p + 1);
    strncpy(fn->body, p, MAX_LINE_LEN - 1);
    return 0;
}

/* ================================================================
 * ON x GOTO / ON x GOSUB
 * ================================================================ */
static int cmd_on(Interp *ip, char *args) {
    char *p = sk(args);
    mpf_t val; mpf_init2(val, g_prec);
    p = sk(eval_expr(p, val));
    int idx = (int)mpf_get_d(val);
    mpf_clear(val);
    int is_gosub = 0;
    if      (kw_match(p,"GOSUB")) { is_gosub=1; p=sk(p+5); }
    else if (kw_match(p,"GOTO"))  {             p=sk(p+4); }
    else return 0;
    int targets[64]; int nt = 0;
    while (*p && nt < 64) {
        p = sk(p);
        if (!isdigit((unsigned char)*p)) break;
        targets[nt++] = atoi(p);
        while (isdigit((unsigned char)*p)) p++;
        p = sk(p); if (*p == ',') p++;
    }
    if (idx < 1 || idx > nt) return 0;
    char num[32]; snprintf(num, sizeof num, "%d", targets[idx - 1]);
    return is_gosub ? cmd_gosub(ip, num) : cmd_goto(ip, num);
}

/* ================================================================
 * DEFINT / DEFSNG / DEFDBL / DEFSTR stubs
 * ================================================================ */
static int cmd_defint(Interp *ip, char *args) { (void)ip;(void)args; return 0; }

/* ================================================================
 * Command registration table
 * ================================================================ */
const Command commands[] = {
    { "REM",        cmd_rem        },
    { "'",          cmd_rem        },
    { "SYSTEM",     cmd_system     },
    { "SLEEP",      cmd_sleep      },
    { "KILL",       cmd_kill       },
    { "GET",        cmd_get_graphics },
    { "PUT",        cmd_put_graphics },
    { "DRAW",       cmd_draw       },
    { "CIRCLE",     cmd_circle     },
    { "PSET",       cmd_pset       },
    { "PAINT",      cmd_paint      },
    { "END SELECT", cmd_end_select },
    { "END SUB",    cmd_end_sub    },
    { "END FUNCTION",cmd_end_sub   },
    { "END IF",     cmd_rem        },
    { "END",        cmd_end        },
    { "ELSE",       cmd_rem        },
    { "SUB",        cmd_rem        },
    { "FUNCTION",   cmd_rem        },
    { "EXIT",       cmd_exit       },
    { "STOP",       cmd_stop       },
    { "CONT",       cmd_cont       },
    { "RANDOMIZE",  cmd_randomize  },
    { "SWAP",       cmd_swap       },
    { "ERASE",      cmd_erase      },
    { "OPTION",     cmd_option     },
    { "CONST",      cmd_const      },
    { "LET",        cmd_let        },
    { "PRINT",      cmd_print      },
    { "CLS",        cmd_cls        },
    { "BEEP",       cmd_beep       },
    { "SOUND",      cmd_sound      },
    { "PLAY",       cmd_play       },
    { "COLOR",      cmd_color      },
    { "LOCATE",     cmd_locate     },
    { "WIDTH",      cmd_width      },
    { "SCREEN",     cmd_screen     },
    { "KEY",        cmd_key        },
    { "PALETTE",    cmd_palette    },
    { "POKE",       cmd_poke       },
    { "VIEW PRINT", cmd_view_print },
    { "VIEW",       cmd_rem        },
    { "REDIM",      cmd_redim      },
    { "DIM",        cmd_dim        },
    { "STATIC",     cmd_static     },
    { "FOR",        cmd_for        },
    { "NEXT",       cmd_next       },
    { "DO",         cmd_do         },
    { "LOOP",       cmd_loop       },
    { "WHILE",      cmd_while      },
    { "WEND",       cmd_wend       },
    { "SELECT",     cmd_select     },
    { "CASE",       cmd_rem        },  /* consumed by cmd_select scan; skip if reached */
    { "GOTO",       cmd_goto       },
    { "GOSUB",      cmd_gosub      },
    { "RETURN",     cmd_return     },
    { "CALL",       cmd_call       },
    { "IF",         cmd_if         },
    { "LINE INPUT", cmd_line_input },
    { "LINE",       cmd_line_gfx   },
    { "INPUT",      cmd_input      },
    { "OPEN",       cmd_open       },
    { "CLOSE",      cmd_close      },
    { "DEF SEG",    cmd_defseg     },
    { "DEF",        cmd_def        },
    { "DEFDBL",     cmd_defdbl     },
    { "DEFINT",     cmd_defint     },
    { "DEFSNG",     cmd_defint     },
    { "DEFSTR",     cmd_defint     },
    { "ON ERROR",   cmd_on_error   },
    { "ON",         cmd_on         },
    { "RESUME",     cmd_resume     },
    { "READ",       cmd_read       },
    { "DATA",       cmd_data       },
    { "RESTORE",    cmd_restore    },
    { "CHAIN",      cmd_chain      },
    { NULL,         NULL           }
};

/* ================================================================
 * Statement splitter — splits a line on unquoted colons
 * ================================================================ */
static int split_statements(char *line, char *segs[], char **buf_out) {
    char *buf = str_dup(line);
    *buf_out = buf;
    int n = 0, in_str = 0;
    segs[n++] = buf;
    char *trimmed = buf;
    while (isspace((unsigned char)*trimmed)) trimmed++;
    if (strncasecmp(trimmed,"REM",3)==0 && !isalnum((unsigned char)trimmed[3]) && trimmed[3]!='_') return n;
    if (*trimmed == '\'') return n;
    for (char *p = buf; *p; p++) {
        if (*p == '"') in_str = !in_str;
        if (!in_str && *p == ':') {
            char *rest = p + 1;
            while (isspace((unsigned char)*rest)) rest++;
            if (strncasecmp(rest,"REM",3)==0 && !isalnum((unsigned char)rest[3]) && rest[3]!='_')
                { *p = '\0'; break; }
            if (strncasecmp(rest,"IF",2)==0 && !isalnum((unsigned char)rest[2]) && rest[2]!='_')
                { *p = '\0'; if (n < MAX_STMTS) segs[n++] = rest; break; }
            *p = '\0';
            if (n < MAX_STMTS) segs[n++] = p + 1;
        }
    }
    return n;
}

/* ================================================================
 * Dispatcher
 * ================================================================ */
/* Format an mpf value sensibly:
 * - plain decimal for numbers that fit reasonably (exponent -6..15)
 * - scientific notation with trailing zeros trimmed otherwise */
static void print_mpf(mpf_t val) {
    int bufsz = PRINT_DIGITS + 64;
    char *buf = (char *)malloc(bufsz);
    if (!buf) return;

    /* Get scientific form to inspect the exponent */
    char *tmp = (char *)malloc(bufsz);
    if (!tmp) { free(buf); return; }
    gmp_snprintf(tmp, bufsz, "%.*Fe", PRINT_DIGITS, val);

    /* Parse exponent */
    char *ep = strchr(tmp, 'e');
    int exp = ep ? atoi(ep + 1) : 0;

    if (exp >= -6 && exp <= 15) {
        /* Plain decimal — figure out decimal places needed */
        int dp = PRINT_DIGITS - exp;
        if (dp < 0) dp = 0;
        if (dp > PRINT_DIGITS) dp = PRINT_DIGITS;
        gmp_snprintf(buf, bufsz, "%.*Ff", dp, val);
        /* Trim trailing zeros after decimal point */
        if (strchr(buf, '.')) {
            char *end = buf + strlen(buf) - 1;
            while (*end == '0') *end-- = '\0';
            if (*end == '.') *end = '\0';
        }
    } else {
        /* Scientific notation — trim trailing zeros in significand */
        gmp_snprintf(buf, bufsz, "%.*Fe", PRINT_DIGITS, val);
        char *e = strchr(buf, 'e');
        if (e) {
            char *z = e - 1;
            while (z > buf && *z == '0') z--;
            if (*z == '.') z--;
            memmove(z + 1, e, strlen(e) + 1);
        }
    }
    free(tmp);

    int len = (int)strlen(buf);
    if (len + 1 < bufsz) { buf[len] = '\n'; buf[len+1] = '\0'; }
    display_print(buf);
    free(buf);
}

int dispatch_one(Interp *ip, char *stmt, char *full_line) {
    char *p = sk(stmt);
    if (!*p) return 0;

    for (int i = 0; commands[i].keyword; i++) {
        char *kw = commands[i].keyword;
        size_t len = strlen(kw);
        if (strncasecmp(p, kw, len) == 0) {
            char next = p[len];
            if (!isalnum((unsigned char)next) && next != '_' && next != '$') {
                if (strcasecmp(kw,"IF") == 0 && full_line) {
                    char *fl = sk(full_line);
                    if (strncasecmp(fl,"IF",2) == 0) fl = sk(fl + 2);
                    return commands[i].fn(ip, fl);
                }
                return commands[i].fn(ip, sk(p + len));
            }
        }
    }

    /* Bare assignment: var = expr, arr(i) = expr, or var.field = expr */
    if (isalpha((unsigned char)*p)) {
        char name[MAX_VARNAME];
        char *after = read_varname(p, name);
        /* skip type sigil (#, !, %, &) after varname */
        if (*after == '#' || *after == '!' || *after == '%' || *after == '&') after++;
        after = sk(after);
        if (*after == '=') return cmd_let(ip, p);
        /* struct field: name.field = ... */
        if (*after == '.') return cmd_let(ip, p);
        /* name(...) — peek past the argument list to decide: assignment or bare expression */
        if (*after == '(') {
            char *peek = after + 1;
            int depth = 1;
            while (*peek && depth > 0) {
                if (*peek == '(') depth++;
                else if (*peek == ')') depth--;
                peek++;
            }
            peek = sk(peek);
            /* struct field after subscript: arr(i).field = ... */
            if (*peek == '.') return cmd_let(ip, p);
            /* array assignment: arr(i) = ... */
            if (*peek == '=') return cmd_let(ip, p);
            /* No '=' — treat as a bare numeric expression and print the result.
             * Handles: tan(5), sin(3.14), sqr(2), (3+4)*2, etc. */
            {
                mpf_t result; mpf_init2(result, g_prec);
                eval_expr(p, result);
                print_mpf(result);
                mpf_clear(result);
                return 0;
            }
        }
        /* Bare sub call without CALL keyword */
        return cmd_call(ip, p);
    }

    /* Bare numeric expression starting with unary sign, digit, or paren */
    if (*p == '-' || *p == '+' || *p == '(' || isdigit((unsigned char)*p)) {
        mpf_t result; mpf_init2(result, g_prec);
        eval_expr(p, result);
        print_mpf(result);
        mpf_clear(result);
        return 0;
    }

    basic_stderr("Warning: unknown: %.60s\n", p);
    return 0;
}

int dispatch(Interp *ip, char *line) {
    return dispatch_one(ip, line, line);
}

int dispatch_multi(Interp *ip, char *clause) {
    char *segs[MAX_STMTS];
    char *buf;
    int n = split_statements(clause, segs, &buf);
    int jumped = 0;
    for (int i = 0; i < n && !jumped; i++)
        jumped = dispatch_one(ip, segs[i], segs[i]);
    free(buf);
    return jumped;
}

BASIC_NS_END
