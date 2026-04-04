/*
 * commands.c — All BASIC command handlers, command registration table,
 *              statement splitter, and dispatcher.
 */
#include "basic.h"
#include "basic_print.h"
#define printf(...) basic_printf(__VA_ARGS__)

/* ================================================================
 * PRINT USING formatter
 * ================================================================ */
static void print_using(const char *fmt, double val) {
    int before = 0, after = 0, has_dot = 0, has_dollar = 0, has_plus = 0;
    for (const char *f = fmt; *f; f++) {
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
 * No-op stubs
 * ================================================================ */
static int cmd_rem(Interp *ip, const char *args)    { (void)ip;(void)args; return 0; }
static int cmd_defseg(Interp *ip, const char *args) { (void)ip;(void)args; return 0; }
static int cmd_defdbl(Interp *ip, const char *args) { (void)ip;(void)args; return 0; }
static int cmd_key(Interp *ip, const char *args)    { (void)ip;(void)args; return 0; }
static int cmd_chain(Interp *ip, const char *args)  { (void)ip;(void)args; return 0; }
static int cmd_screen(Interp *ip, const char *args) { (void)ip;(void)args; return 0; }
static int cmd_beep(Interp *ip, const char *args) {
    (void)ip; (void)args;
    sound_beep();
    return 0;
}

/* SOUND freq, duration  — freq in Hz, duration in 18.2-tick clock units */
static int cmd_sound(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
    mpf_t freq, dur; mpf_init2(freq, g_prec); mpf_init2(dur, g_prec);
    p = sk(eval_expr(p, freq));
    if (*p == ',') p = sk(p + 1);
    eval_expr(p, dur);
    sound_tone(mpf_get_d(freq), mpf_get_d(dur));
    mpf_clears(freq, dur, NULL);
    return 0;
}

/* PLAY "mml-string" — GW-BASIC Music Macro Language */
static int cmd_play(Interp *ip, const char *args) {
    (void)ip;
    char mml[1024];
    eval_str_expr(sk(args), mml, sizeof mml);
    sound_play(mml);
    return 0;
}

/* ================================================================
 * Interpreter control
 * ================================================================ */
static int cmd_stop(Interp *ip, const char *args) {
    (void)args;
    g_cont_pc = ip->pc + 1;
    ip->running = 0;
    display_print("\nBreak\n");
    return 1;
}

static int cmd_cont(Interp *ip, const char *args) {
    (void)args;
    if (g_cont_pc < 0) { display_print("Can't continue\n"); return 0; }
    ip->pc = g_cont_pc;
    g_cont_pc = -1;
    return 1;
}

static int cmd_end(Interp *ip, const char *args) {
    (void)args; ip->running = 0;
    for (int i = 1; i <= MAX_FILE_HANDLES; i++)
        if (g_files[i].fp) { fclose(g_files[i].fp); g_files[i].fp = NULL; g_files[i].mode = 0; }
    return 1;
}

/* ================================================================
 * Utility commands
 * ================================================================ */
static int cmd_randomize(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
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

static int cmd_swap(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
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

static int cmd_erase(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
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

static int cmd_option(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
    if (kw_match(p, "BASE")) {
        p = sk(p + 4);
        mpf_t n; mpf_init2(n, g_prec);
        eval_expr(p, n);
        int base = (int)mpf_get_si(n); mpf_clear(n);
        if (base == 0 || base == 1) g_option_base = base;
        else fprintf(stderr, "OPTION BASE must be 0 or 1\n");
    }
    return 0;
}

/* ================================================================
 * Display commands
 * ================================================================ */
static int cmd_cls(Interp *ip, const char *args) {
    (void)ip; (void)args; display_cls(); return 0;
}

static int cmd_width(Interp *ip, const char *args) {
    (void)ip;
    mpf_t n; mpf_init2(n, g_prec);
    eval_expr(sk(args), n);
    display_width((int)mpf_get_si(n));
    mpf_clear(n);
    return 0;
}

static int cmd_color(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
    mpf_t fg, bg; mpf_init2(fg, g_prec); mpf_init2(bg, g_prec);
    mpf_set_ui(bg, 0);
    p = eval_expr(p, fg); p = sk(p);
    if (*p == ',') { p = sk(p + 1); p = eval_expr(p, bg); }
    display_color((int)mpf_get_si(fg), (int)mpf_get_si(bg));
    mpf_clears(fg, bg, NULL);
    return 0;
}

static int cmd_locate(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
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
static int cmd_dim(Interp *ip, const char *args);
static int cmd_return(Interp *ip, const char *args);

/* DO [WHILE|UNTIL cond] — push a loop frame pointing at the DO statement */
static int cmd_do(Interp *ip, const char *args) {
    /* When LOOP sends us back here, the frame already exists — don't push again.
     * But we must still re-check any DO WHILE / DO UNTIL condition. */
    if (g_ctrl_top > 0 &&
        strcmp(g_ctrl[g_ctrl_top - 1].varname, "\x02" "DO") == 0 &&
        g_ctrl[g_ctrl_top - 1].line_idx == ip->pc) {
        const char *p = sk(args);
        if (kw_match(p, "WHILE") || kw_match(p, "UNTIL")) {
            int is_until = kw_match(p, "UNTIL");
            p = sk(p + 5);
            mpf_t v; mpf_init2(v, g_prec);
            eval_expr(p, v);
            int cond = (mpf_sgn(v) != 0);
            mpf_clear(v);
            if (is_until) cond = !cond;
            if (!cond) {
                /* skip to after matching LOOP */
                int depth = 1, pc = ip->pc + 1;
                while (pc < g_nlines && depth > 0) {
                    const char *t = sk(g_lines[pc].text);
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
    const char *p = sk(args);
    if (kw_match(p, "WHILE") || kw_match(p, "UNTIL")) {
        int is_until = kw_match(p, "UNTIL");
        p = sk(p + 5);
        mpf_t v; mpf_init2(v, g_prec);
        eval_expr(p, v);
        int cond = (mpf_sgn(v) != 0);
        mpf_clear(v);
        if (is_until) cond = !cond;
        if (!cond) {
            int depth = 1, pc = ip->pc + 1;
            while (pc < g_nlines && depth > 0) {
                const char *t = sk(g_lines[pc].text);
                if (kw_match(t, "DO")) depth++;
                else if (kw_match(t, "LOOP")) depth--;
                pc++;
            }
            ip->pc = pc; return 1;
        }
    }

    if (g_ctrl_top >= CTRL_STACK_MAX) { fprintf(stderr, "Stack overflow\n"); exit(1); }
    CtrlFrame *f = &g_ctrl[g_ctrl_top++];
    strcpy(f->varname, "\x02" "DO");
    f->line_idx = ip->pc;
    mpf_init2(f->limit, g_prec); mpf_set_ui(f->limit, 0);
    mpf_init2(f->step,  g_prec); mpf_set_ui(f->step,  0);
    return 0;
}

/* LOOP [WHILE|UNTIL cond] */
static int cmd_loop(Interp *ip, const char *args) {
    /* find the matching DO frame */
    int fi = g_ctrl_top - 1;
    while (fi >= 0 && strcmp(g_ctrl[fi].varname, "\x02" "DO") != 0) fi--;
    if (fi < 0) { fprintf(stderr, "LOOP without DO\n"); exit(1); }

    const char *p = sk(args);
    int keep_looping = 1;   /* default: infinite loop, need explicit WHILE/UNTIL to stop */

    if (kw_match(p, "WHILE")) {
        p = sk(p + 5);
        mpf_t v; mpf_init2(v, g_prec);
        eval_expr(p, v);
        keep_looping = (mpf_sgn(v) != 0);
        mpf_clear(v);
    } else if (kw_match(p, "UNTIL")) {
        p = sk(p + 5);
        mpf_t v; mpf_init2(v, g_prec);
        eval_expr(p, v);
        keep_looping = (mpf_sgn(v) == 0);   /* UNTIL: loop while condition is FALSE */
        mpf_clear(v);
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
static int cmd_while(Interp *ip, const char *args) {
    const char *p = sk(args);
    mpf_t v; mpf_init2(v, g_prec);
    eval_expr(p, v);
    int cond = (mpf_sgn(v) != 0);
    mpf_clear(v);

    if (cond) {
        /* Only push a new frame if we're not already tracking this WHILE */
        int already = (g_ctrl_top > 0 &&
                       strcmp(g_ctrl[g_ctrl_top - 1].varname, "\x03" "WHILE") == 0 &&
                       g_ctrl[g_ctrl_top - 1].line_idx == ip->pc);
        if (!already) {
            if (g_ctrl_top >= CTRL_STACK_MAX) { fprintf(stderr, "Stack overflow\n"); exit(1); }
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
            const char *t = sk(g_lines[pc].text);
            if (kw_match(t, "WHILE")) depth++;
            else if (kw_match(t, "WEND")) depth--;
            if (depth > 0) pc++;
        }
        ip->pc = pc + 1;
        return 1;
    }
}

/* WEND — jump back to the matching WHILE; WHILE will pop if condition fails */
static int cmd_wend(Interp *ip, const char *args) {
    (void)args;
    int fi = g_ctrl_top - 1;
    while (fi >= 0 && strcmp(g_ctrl[fi].varname, "\x03" "WHILE") != 0) fi--;
    if (fi < 0) { fprintf(stderr, "WEND without WHILE\n"); exit(1); }
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
        const char *t = sk(g_lines[pc].text);
        if (kw_match(t, "SELECT")) depth++;
        else if (kw_match(t, "END") && kw_match(sk(t + 3), "SELECT")) {
            if (depth == 0) return pc;
            depth--;
        } else if (depth == 0 && kw_match(t, "CASE")) return pc;
    }
    return g_nlines;
}

static int cmd_select(Interp *ip, const char *args) {
    const char *p = sk(args);
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
        const char *t = sk(g_lines[pc].text);

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

static int cmd_end_select(Interp *ip, const char *args) {
    (void)ip; (void)args; return 0;   /* normal fall-through after a matched CASE */
}

/* ================================================================
 * EXIT SUB / EXIT FUNCTION / EXIT FOR / EXIT DO
 * ================================================================ */
static int cmd_exit(Interp *ip, const char *args) {
    const char *p = sk(args);
    if (kw_match(p, "FOR")) {
        /* pop to the innermost FOR frame */
        for (int fi = g_ctrl_top - 1; fi >= 0; fi--) {
            if (g_ctrl[fi].varname[0] != '\x01' &&
                g_ctrl[fi].varname[0] != '\x02' &&
                g_ctrl[fi].varname[0] != '\x03') {
                /* scan forward to NEXT */
                int depth = 1, pc = ip->pc + 1;
                while (pc < g_nlines && depth > 0) {
                    const char *t = sk(g_lines[pc].text);
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
                    const char *t = sk(g_lines[pc].text);
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
static int cmd_redim(Interp *ip, const char *args) {
    return cmd_dim(ip, args);
}

/* ================================================================
 * ON ERROR GOTO / RESUME — stub implementations
 * We don't support full error trapping, but we need these to not crash.
 * ON ERROR GOTO 0 disables any pending handler (no-op for us).
 * RESUME NEXT advances past the erroring line (no-op in stub).
 * ================================================================ */
static int cmd_on_error(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
    /* ON ERROR GOTO 0 — disable handler */
    if (kw_match(p, "GOTO")) {
        p = sk(p + 4);
        if (*p == '0' && !isalnum((unsigned char)p[1])) return 0; /* disable */
        /* Otherwise treat as a regular GOTO to the label/line */
        return cmd_goto(ip, p);
    }
    return 0;
}

static int cmd_resume(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
    /* RESUME NEXT — advance past current line (already done by run loop) */
    if (kw_match(p, "NEXT")) return 0;
    /* RESUME — retry current line (just continue for now) */
    return 0;
}

/* ================================================================
 * VIEW PRINT [top TO bottom] — set text viewport (stub: just clear)
 * ================================================================ */
static int cmd_view_print(Interp *ip, const char *args) {
    (void)ip; (void)args;
    /* TODO: real viewport when we have a graphical backend */
    return 0;
}

/* ================================================================
 * PALETTE — stub (needed when running without graphics)
 * ================================================================ */
static int cmd_palette(Interp *ip, const char *args) {
    (void)ip; (void)args; return 0;
}

/* ================================================================
 * POKE addr, val — stub
 * ================================================================ */
static int cmd_poke(Interp *ip, const char *args) {
    (void)ip; (void)args; return 0;
}

/* ================================================================
 * END SUB / END FUNCTION / END SELECT — handled by their parent,
 * but we need them registered so they don't print "unknown".
 * END IF is similar.
 * ================================================================ */
static int cmd_end_sub(Interp *ip, const char *args) {
    /* Treat as RETURN — end of an inline sub body */
    return cmd_return(ip, args);
}

/* ================================================================
 * CALL subname [args] — for now, treat as GOSUB to label
 * ================================================================ */
static int cmd_call(Interp *ip, const char *args) {
    const char *p = sk(args);
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
static int cmd_static(Interp *ip, const char *args) {
    return cmd_dim(ip, args);
}


static int cmd_const(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
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
            const char *start = p;
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
static int cmd_dim(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
    /* SHARED keyword — skip it, all our vars are already global */
    if (kw_match(p, "SHARED")) p = sk(p + 6);
    while (*p) {
        char name[MAX_VARNAME];
        p = sk(read_varname(p, name));
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
            if (total > MAX_ARRAY_SIZE) { fprintf(stderr, "Array too large: %d\n", total); total = MAX_ARRAY_SIZE; }
            if (var_is_str_name(name)) {
                v->kind = VAR_ARRAY_STR; v->dim[0] = dim1; v->dim[1] = dim2; v->ndim = ndim;
                v->arr_str = calloc(total, sizeof(char *));
                for (int i = 0; i < total; i++) v->arr_str[i] = str_dup("");
            } else {
                v->kind = VAR_ARRAY_NUM; v->dim[0] = dim1; v->dim[1] = dim2; v->ndim = ndim;
                v->arr_num = calloc(total, sizeof(mpf_t));
                for (int i = 0; i < total; i++) { mpf_init2(v->arr_num[i], g_prec); mpf_set_ui(v->arr_num[i], 0); }
            }
        }
        p = sk(p);
        /* skip optional AS typename — e.g. "AS INTEGER", "AS PlayerData" */
        if (kw_match(p, "AS")) {
            p = sk(p + 2);
            /* skip the type name, including optional * size for STRING * n */
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            p = sk(p);
            if (*p == '*') { p = sk(p + 1); while (isdigit((unsigned char)*p)) p++; }
            p = sk(p);
        }
        if (*p == ',') p = sk(p + 1);
    }
    return 0;
}

/* ================================================================
 * LET / implicit assignment
 * ================================================================ */
static int cmd_let(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
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
static int cmd_print_file(Interp *ip, const char *args);  /* forward */

static int cmd_print(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
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
static int cmd_line_input_file(Interp *ip, const char *args);  /* forward */
static int cmd_input_file(Interp *ip, const char *args);       /* forward */

static int cmd_line_input(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
    if (*p == '#') return cmd_line_input_file(ip, args);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') display_putchar(*p++);
        if (*p == '"') p++;
    }
    p = sk(p); if (*p == ';' || *p == ',') p = sk(p + 1);
    char name[MAX_VARNAME];
    read_varname(p, name);
    char linebuf[512];
    display_cursor(1);
    display_getline(linebuf, sizeof linebuf);
    display_newline();
    Var *v = var_get(name);
    free(v->str); v->str = str_dup(linebuf);
    return 0;
}

static int cmd_input(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
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
    char linebuf[512];
    display_cursor(1);
    display_getline(linebuf, sizeof linebuf);
    display_newline();
    char *tok = linebuf;
    while (*p) {
        char name[MAX_VARNAME];
        p = sk(read_varname(sk(p), name));
        char *comma = strchr(tok, ',');
        char val_str[256];
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
static int cmd_open(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
    char filename[512]; int fi = 0;
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

static int cmd_close(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
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

static int cmd_input_file(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
    if (*p == '#') p = sk(p + 1);
    mpf_t num; mpf_init2(num, g_prec);
    p = sk(eval_expr(p, num)); int n = (int)mpf_get_si(num); mpf_clear(num);
    if (*p == ',') p = sk(p + 1);
    FileHandle *fh = fh_get(n);
    if (!fh->fp || fh->mode != 'I') { fprintf(stderr, "File #%d not open for input\n", n); return 0; }
    while (*p) {
        char name[MAX_VARNAME];
        p = sk(read_varname(sk(p), name));
        char linebuf[512];
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

static int cmd_print_file(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
    if (*p == '#') p = sk(p + 1);
    mpf_t num; mpf_init2(num, g_prec);
    p = sk(eval_expr(p, num)); int n = (int)mpf_get_si(num); mpf_clear(num);
    if (*p == ',') p = sk(p + 1);
    FileHandle *fh = fh_get(n);
    if (!fh->fp || fh->mode == 'I') { fprintf(stderr, "File #%d not open for output\n", n); return 0; }
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

static int cmd_line_input_file(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
    if (*p == '#') p = sk(p + 1);
    mpf_t num; mpf_init2(num, g_prec);
    p = sk(eval_expr(p, num)); int n = (int)mpf_get_si(num); mpf_clear(num);
    if (*p == ',') p = sk(p + 1);
    FileHandle *fh = fh_get(n);
    if (!fh->fp || fh->mode != 'I') { fprintf(stderr, "File #%d not open for input\n", n); return 0; }
    char name[MAX_VARNAME];
    read_varname(sk(p), name);
    char linebuf[512];
    if (!fgets(linebuf, sizeof linebuf, fh->fp)) linebuf[0] = '\0';
    linebuf[strcspn(linebuf, "\r\n")] = '\0';
    Var *v = var_get(name);
    free(v->str); v->str = str_dup(linebuf);
    return 0;
}

/* ================================================================
 * FOR / NEXT / GOTO / GOSUB / RETURN
 * ================================================================ */
static int cmd_for(Interp *ip, const char *args) {
    const char *p = sk(args);
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
    if (g_ctrl_top >= CTRL_STACK_MAX) { fprintf(stderr, "Stack overflow\n"); exit(1); }
    CtrlFrame *f = &g_ctrl[g_ctrl_top++];
    strncpy(f->varname, vname, MAX_VARNAME - 1);
    mpf_init2(f->limit, g_prec); mpf_set(f->limit, limit);
    mpf_init2(f->step,  g_prec); mpf_set(f->step,  step);
    f->line_idx = ip->pc;
    mpf_clears(start, limit, step, NULL);
    return 0;
}

static int cmd_next(Interp *ip, const char *args) {
    const char *p = sk(args);
    char vname[MAX_VARNAME] = "";
    if (isalpha((unsigned char)*p)) read_varname(p, vname);
    int fi = g_ctrl_top - 1;
    if (*vname) {
        for (fi = g_ctrl_top - 1; fi >= 0; fi--)
            if (strcasecmp(g_ctrl[fi].varname, vname) == 0) break;
        if (fi < 0) { fprintf(stderr, "NEXT without FOR: %s\n", vname); exit(1); }
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

int cmd_goto(Interp *ip, const char *args) {
    const char *p = sk(args);
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
    if (idx < 0) { fprintf(stderr, "GOTO: target not found: %s\n", sk(args)); exit(1); }
    ip->pc = idx; return 1;
}

int cmd_gosub(Interp *ip, const char *args) {
    if (g_ctrl_top >= CTRL_STACK_MAX) { fprintf(stderr, "Stack overflow\n"); exit(1); }
    CtrlFrame *f = &g_ctrl[g_ctrl_top++];
    strcpy(f->varname, "\x01" "GOSUB");
    f->line_idx = ip->pc + 1;
    mpf_init2(f->limit, g_prec); mpf_set_ui(f->limit, 0);
    mpf_init2(f->step,  g_prec); mpf_set_ui(f->step,  0);
    return cmd_goto(ip, args);
}

static int cmd_return(Interp *ip, const char *args) {
    (void)args;
    for (int fi = g_ctrl_top - 1; fi >= 0; fi--) {
        if (strcmp(g_ctrl[fi].varname, "\x01" "GOSUB") == 0) {
            ip->pc = g_ctrl[fi].line_idx;
            mpf_clear(g_ctrl[fi].limit); mpf_clear(g_ctrl[fi].step);
            g_ctrl_top = fi; return 1;
        }
    }
    fprintf(stderr, "RETURN without GOSUB\n"); exit(1);
}

/* ================================================================
 * IF / THEN / ELSE
 * ================================================================ */
static int eval_one_cmp(const char **pp) {
    const char *p = sk(*pp);
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

static const char *find_else(const char *p) {
    int in_str = 0;
    while (*p) {
        if (*p == '"') { in_str = !in_str; p++; continue; }
        if (!in_str && kw_match(p, "ELSE")) return p;
        p++;
    }
    return NULL;
}

static int cmd_if(Interp *ip, const char *args) {
    const char *p = sk(args);
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
    const char *else_p = find_else(p);
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
static int cmd_data(Interp *ip, const char *args)    { (void)ip;(void)args; return 0; }
static int cmd_restore(Interp *ip, const char *args) { (void)ip;(void)args; g_data_pos=0; return 0; }

static int cmd_read(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
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
        if (g_data_pos >= g_data_count) { fprintf(stderr, "READ: out of data\n"); exit(1); }
        const char *item = g_data[g_data_pos++];
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
static int cmd_def(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
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
static int cmd_on(Interp *ip, const char *args) {
    const char *p = sk(args);
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
static int cmd_defint(Interp *ip, const char *args) { (void)ip;(void)args; return 0; }

/* ================================================================
 * Command registration table
 * ================================================================ */
const Command commands[] = {
    { "REM",        cmd_rem        },
    { "'",          cmd_rem        },
    { "END SELECT", cmd_end_select },
    { "END SUB",    cmd_end_sub    },
    { "END FUNCTION",cmd_end_sub   },
    { "END IF",     cmd_rem        },
    { "END",        cmd_end        },
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
static int split_statements(const char *line, char *segs[], char **buf_out) {
    char *buf = str_dup(line);
    *buf_out = buf;
    int n = 0, in_str = 0;
    segs[n++] = buf;
    const char *trimmed = buf;
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
int dispatch_one(Interp *ip, const char *stmt, const char *full_line) {
    const char *p = sk(stmt);
    if (!*p) return 0;

    for (int i = 0; commands[i].keyword; i++) {
        const char *kw = commands[i].keyword;
        size_t len = strlen(kw);
        if (strncasecmp(p, kw, len) == 0) {
            char next = p[len];
            if (!isalnum((unsigned char)next) && next != '_' && next != '$') {
                if (strcasecmp(kw,"IF") == 0 && full_line) {
                    const char *fl = sk(full_line);
                    if (strncasecmp(fl,"IF",2) == 0) fl = sk(fl + 2);
                    return commands[i].fn(ip, fl);
                }
                return commands[i].fn(ip, sk(p + len));
            }
        }
    }

    /* Bare assignment: var = expr or arr(i) = expr */
    if (isalpha((unsigned char)*p)) {
        char name[MAX_VARNAME];
        const char *after = read_varname(p, name);
        after = sk(after);
        if (*after == '=' || *after == '(') return cmd_let(ip, p);
    }

    fprintf(stderr, "Warning: unknown: %.60s\n", p);
    return 0;
}

int dispatch(Interp *ip, const char *line) {
    return dispatch_one(ip, line, line);
}

int dispatch_multi(Interp *ip, const char *clause) {
    char *segs[MAX_STMTS];
    char *buf;
    int n = split_statements(clause, segs, &buf);
    int jumped = 0;
    for (int i = 0; i < n && !jumped; i++)
        jumped = dispatch_one(ip, segs[i], segs[i]);
    free(buf);
    return jumped;
}
