/*
 * basic.c — Modular BASIC interpreter with GNU MP precision
 *
 * Adding a new command:
 *   1. Write a handler:  static int cmd_foo(Interp *ip, const char *args)
 *   2. Register it:      COMMAND("FOO", cmd_foo)  in the commands[] table
 *   That's it.
 *
 * Build:  gcc -O2 -o basic basic.c -lgmp -lm
 * Usage:  ./basic program.bas [precision_bits]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <gmp.h>

/* ================================================================
 * Precision
 * ================================================================ */
#define DEFAULT_PREC  512
#define PRINT_DIGITS  60
static mp_bitcnt_t g_prec = DEFAULT_PREC;

/* ================================================================
 * Interpreter state (passed to every command handler)
 * ================================================================ */
typedef struct Interp Interp;

/* ================================================================
 * Variable store
 * ================================================================ */
#define MAX_VARS    256
#define MAX_VARNAME  32

typedef struct { char name[MAX_VARNAME]; mpf_t value; } Var;
static Var g_vars[MAX_VARS];
static int g_nvar = 0;

static mpf_t *var_get(const char *name, int create)
{
    for (int i = 0; i < g_nvar; i++)
        if (strcasecmp(g_vars[i].name, name) == 0) return &g_vars[i].value;
    if (!create) return NULL;
    if (g_nvar >= MAX_VARS) { fprintf(stderr, "Too many variables\n"); exit(1); }
    strncpy(g_vars[g_nvar].name, name, MAX_VARNAME - 1);
    mpf_init2(g_vars[g_nvar].value, g_prec);
    mpf_set_ui(g_vars[g_nvar].value, 0);
    return &g_vars[g_nvar++].value;
}

static void var_set(const char *name, mpf_t val)
{
    mpf_set(*var_get(name, 1), val);
}

/* ================================================================
 * Program store
 * ================================================================ */
#define MAX_LINES    4096
#define MAX_LINE_LEN  256

typedef struct { int linenum; char text[MAX_LINE_LEN]; } Line;
static Line g_lines[MAX_LINES];
static int  g_nlines = 0;

static int line_cmp(const void *a, const void *b)
    { return ((Line*)a)->linenum - ((Line*)b)->linenum; }

static int find_line_idx(int num) {
    for (int i = 0; i < g_nlines; i++)
        if (g_lines[i].linenum == num) return i;
    return -1;
}

/* ================================================================
 * FOR/NEXT + GOSUB call stack
 * ================================================================ */
#define CTRL_STACK_MAX 64

typedef struct {
    char  varname[MAX_VARNAME];  /* "\x01GOSUB" sentinel for subroutine frames */
    mpf_t limit, step;
    int   line_idx;
} CtrlFrame;

static CtrlFrame g_ctrl[CTRL_STACK_MAX];
static int       g_ctrl_top = 0;

/* ================================================================
 * Interpreter state
 * ================================================================ */
struct Interp {
    int pc;       /* current line index */
    int running;  /* 0 = halt */
};

/* ================================================================
 * Expression evaluator (recursive descent)
 * ================================================================ */
typedef struct { const char *p; } Parser;

static void parse_expr(Parser *ps, mpf_t result);
static void parse_term(Parser *ps, mpf_t result);
static void parse_unary(Parser *ps, mpf_t result);
static void parse_power(Parser *ps, mpf_t result);
static void parse_primary(Parser *ps, mpf_t result);

static void skip_ws(Parser *ps) { while (isspace((unsigned char)*ps->p)) ps->p++; }

static void parse_expr(Parser *ps, mpf_t result) {
    mpf_t tmp; mpf_init2(tmp, g_prec);
    parse_term(ps, result);
    skip_ws(ps);
    while (*ps->p == '+' || *ps->p == '-') {
        char op = *ps->p++;
        parse_term(ps, tmp);
        if (op == '+') mpf_add(result, result, tmp);
        else           mpf_sub(result, result, tmp);
        skip_ws(ps);
    }
    mpf_clear(tmp);
}

static void parse_term(Parser *ps, mpf_t result) {
    mpf_t tmp; mpf_init2(tmp, g_prec);
    parse_unary(ps, result);
    skip_ws(ps);
    while (*ps->p == '*' || *ps->p == '/') {
        char op = *ps->p++;
        parse_unary(ps, tmp);
        if (op == '*') mpf_mul(result, result, tmp);
        else {
            if (mpf_sgn(tmp) == 0) { fprintf(stderr, "Division by zero\n"); exit(1); }
            mpf_div(result, result, tmp);
        }
        skip_ws(ps);
    }
    mpf_clear(tmp);
}

static void parse_unary(Parser *ps, mpf_t result) {
    skip_ws(ps);
    if (*ps->p == '-') { ps->p++; parse_power(ps, result); mpf_neg(result, result); }
    else parse_power(ps, result);
}

static void parse_power(Parser *ps, mpf_t result) {
    parse_primary(ps, result);
    skip_ws(ps);
    if (*ps->p == '^') {
        ps->p++;
        mpf_t exp; mpf_init2(exp, g_prec);
        parse_unary(ps, exp);
        long e = mpf_get_si(exp);
        mpf_t base; mpf_init2(base, g_prec); mpf_set(base, result);
        mpf_set_ui(result, 1);
        int neg = (e < 0); if (neg) e = -e;
        for (long i = 0; i < e; i++) mpf_mul(result, result, base);
        if (neg) mpf_ui_div(result, 1, result);
        mpf_clears(exp, base, NULL);
    }
}

static void parse_primary(Parser *ps, mpf_t result) {
    skip_ws(ps);
    if (*ps->p == '(') {
        ps->p++; parse_expr(ps, result);
        skip_ws(ps); if (*ps->p == ')') ps->p++;
        return;
    }
    if (isdigit((unsigned char)*ps->p) || *ps->p == '.') {
        char buf[64]; int i = 0;
        while (isdigit((unsigned char)*ps->p) || *ps->p == '.'
               || *ps->p == 'E' || *ps->p == 'e')
            buf[i++] = *ps->p++;
        buf[i] = '\0';
        mpf_set_str(result, buf, 10);
        return;
    }
    if (isalpha((unsigned char)*ps->p)) {
        char name[MAX_VARNAME]; int i = 0;
        while (isalnum((unsigned char)*ps->p) || *ps->p == '_')
            name[i++] = (char)toupper((unsigned char)*ps->p++);
        name[i] = '\0';
        mpf_set(result, *var_get(name, 1));
        return;
    }
    fprintf(stderr, "Parse error near: %.20s\n", ps->p); exit(1);
}

/* Evaluate expression string, return pointer past it */
static const char *eval_expr(const char *s, mpf_t result) {
    Parser ps = { s };
    parse_expr(&ps, result);
    return ps.p;
}

/* ================================================================
 * Utility
 * ================================================================ */
static const char *sk(const char *p) { while (isspace((unsigned char)*p)) p++; return p; }

static const char *read_varname(const char *p, char *name) {
    int i = 0;
    while (isalnum((unsigned char)*p) || *p == '_')
        name[i++] = (char)toupper((unsigned char)*p++);
    name[i] = '\0';
    return p;
}

/* ================================================================
 * Command handler type
 *
 *   ip   — interpreter state
 *   args — text after the keyword, leading whitespace stripped
 *
 *   Return 0: run() will advance pc by 1
 *   Return 1: handler already set ip->pc; run() will not touch it
 * ================================================================ */
typedef int (*CmdFn)(Interp *ip, const char *args);
typedef struct { const char *keyword; CmdFn fn; } Command;

/* ================================================================
 * Command handlers — add new ones here
 * ================================================================ */

static int cmd_rem(Interp *ip, const char *args)
{
    (void)ip; (void)args;
    return 0;
}

static int cmd_end(Interp *ip, const char *args)
{
    (void)args;
    ip->running = 0;
    return 1;
}

static int cmd_let(Interp *ip, const char *args)
{
    (void)ip;
    const char *p = sk(args);
    char name[MAX_VARNAME];
    p = sk(read_varname(p, name));
    if (*p != '=') { fprintf(stderr, "LET: expected '='\n"); exit(1); }
    mpf_t val; mpf_init2(val, g_prec);
    eval_expr(sk(p + 1), val);
    var_set(name, val);
    mpf_clear(val);
    return 0;
}

static int cmd_print(Interp *ip, const char *args)
{
    (void)ip;
    const char *p = sk(args);
    while (*p) {
        if (*p == '"') {
            p++;
            while (*p && *p != '"') putchar(*p++);
            if (*p == '"') p++;
        } else if (*p == ';') {
            p = sk(p + 1);
        } else if (*p == ',') {
            putchar('\t'); p = sk(p + 1);
        } else {
            mpf_t val; mpf_init2(val, g_prec);
            p = sk(eval_expr(p, val));
            gmp_printf("%.*Ff", PRINT_DIGITS, val);
            mpf_clear(val);
        }
    }
    putchar('\n');
    return 0;
}

static int cmd_for(Interp *ip, const char *args)
{
    const char *p = sk(args);
    char vname[MAX_VARNAME];
    p = sk(read_varname(p, vname));
    if (*p != '=') { fprintf(stderr, "FOR: expected '='\n"); exit(1); }
    p = sk(p + 1);

    mpf_t start, limit, step;
    mpf_init2(start, g_prec); mpf_init2(limit, g_prec); mpf_init2(step, g_prec);
    mpf_set_ui(step, 1);

    p = sk(eval_expr(p, start));
    if (strncasecmp(p, "TO", 2) != 0) { fprintf(stderr, "FOR: expected TO\n"); exit(1); }
    p = sk(p + 2);
    p = sk(eval_expr(p, limit));
    if (strncasecmp(p, "STEP", 4) == 0)
        eval_expr(sk(p + 4), step);

    var_set(vname, start);

    if (g_ctrl_top >= CTRL_STACK_MAX) { fprintf(stderr, "Stack overflow\n"); exit(1); }
    CtrlFrame *f = &g_ctrl[g_ctrl_top++];
    strncpy(f->varname, vname, MAX_VARNAME - 1);
    mpf_init2(f->limit, g_prec); mpf_set(f->limit, limit);
    mpf_init2(f->step,  g_prec); mpf_set(f->step,  step);
    f->line_idx = ip->pc;

    mpf_clears(start, limit, step, NULL);
    return 0;
}

static int cmd_next(Interp *ip, const char *args)
{
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
    mpf_t *cv = var_get(f->varname, 0);
    mpf_add(*cv, *cv, f->step);

    int done = (mpf_sgn(f->step) > 0) ? (mpf_cmp(*cv, f->limit) > 0)
                                       : (mpf_cmp(*cv, f->limit) < 0);
    if (done) {
        mpf_clear(f->limit); mpf_clear(f->step);
        g_ctrl_top = fi;
        return 0;
    }
    ip->pc = f->line_idx + 1;
    return 1;
}

static int cmd_goto(Interp *ip, const char *args)
{
    int idx = find_line_idx(atoi(sk(args)));
    if (idx < 0) { fprintf(stderr, "GOTO: line not found\n"); exit(1); }
    ip->pc = idx;
    return 1;
}

static int cmd_gosub(Interp *ip, const char *args)
{
    if (g_ctrl_top >= CTRL_STACK_MAX) { fprintf(stderr, "Stack overflow\n"); exit(1); }
    CtrlFrame *f = &g_ctrl[g_ctrl_top++];
    strcpy(f->varname, "\x01GOSUB");
    f->line_idx = ip->pc + 1;
    mpf_init2(f->limit, g_prec); mpf_set_ui(f->limit, 0);
    mpf_init2(f->step,  g_prec); mpf_set_ui(f->step,  0);
    return cmd_goto(ip, args);
}

static int cmd_return(Interp *ip, const char *args)
{
    (void)args;
    for (int fi = g_ctrl_top - 1; fi >= 0; fi--) {
        if (strcmp(g_ctrl[fi].varname, "\x01GOSUB") == 0) {
            ip->pc = g_ctrl[fi].line_idx;
            mpf_clear(g_ctrl[fi].limit);
            mpf_clear(g_ctrl[fi].step);
            g_ctrl_top = fi;
            return 1;
        }
    }
    fprintf(stderr, "RETURN without GOSUB\n"); exit(1);
}

static int dispatch(Interp *ip, const char *line);  /* forward decl for IF */

static int cmd_if(Interp *ip, const char *args)
{
    const char *p = sk(args);
    mpf_t lhs, rhs; mpf_init2(lhs, g_prec); mpf_init2(rhs, g_prec);
    p = sk(eval_expr(p, lhs));

    /* two-char operators first */
    int cmp = 0;
    char op2[3] = { p[0], p[1], '\0' };
    int oplen = 2;
    if      (!strcmp(op2,"<>")||!strcmp(op2,"><")) ;
    else if (!strcmp(op2,"<=")||!strcmp(op2,"=<")) ;
    else if (!strcmp(op2,">=")||!strcmp(op2,"=>")) ;
    else { op2[1] = '\0'; oplen = 1; }

    p = sk(eval_expr(sk(p + oplen), rhs));
    int c = mpf_cmp(lhs, rhs);
    if      (!strcmp(op2,"<>")||!strcmp(op2,"><")) cmp = (c != 0);
    else if (!strcmp(op2,"<=")||!strcmp(op2,"=<")) cmp = (c <= 0);
    else if (!strcmp(op2,">=")||!strcmp(op2,"=>")) cmp = (c >= 0);
    else if (op2[0] == '<')                        cmp = (c <  0);
    else if (op2[0] == '>')                        cmp = (c >  0);
    else                                           cmp = (c == 0);
    mpf_clears(lhs, rhs, NULL);

    if (!cmp) return 0;

    if (strncasecmp(p, "THEN", 4) == 0) p = sk(p + 4);

    /* bare line number → GOTO */
    if (isdigit((unsigned char)*p)) return cmd_goto(ip, p);

    /* inline statement */
    return dispatch(ip, p);
}

/* ================================================================
 * Command registration table — ADD NEW COMMANDS HERE
 * ================================================================ */
#define COMMAND(kw, fn) { kw, fn }

static const Command commands[] = {
    COMMAND("REM",    cmd_rem),
    COMMAND("END",    cmd_end),
    COMMAND("LET",    cmd_let),
    COMMAND("PRINT",  cmd_print),
    COMMAND("FOR",    cmd_for),
    COMMAND("NEXT",   cmd_next),
    COMMAND("GOTO",   cmd_goto),
    COMMAND("GOSUB",  cmd_gosub),
    COMMAND("RETURN", cmd_return),
    COMMAND("IF",     cmd_if),
    { NULL, NULL }   /* sentinel */
};

/* ================================================================
 * Dispatcher
 * ================================================================ */
static int dispatch(Interp *ip, const char *line)
{
    const char *p = sk(line);

    for (int i = 0; commands[i].keyword; i++) {
        const char *kw  = commands[i].keyword;
        size_t      len = strlen(kw);
        if (strncasecmp(p, kw, len) == 0
            && !isalnum((unsigned char)p[len]) && p[len] != '_')
        {
            return commands[i].fn(ip, sk(p + len));
        }
    }

    /* bare assignment: var = expr */
    if (isalpha((unsigned char)*p)) {
        const char *q = p;
        char name[MAX_VARNAME];
        q = sk(read_varname(q, name));
        if (*q == '=') return cmd_let(ip, p);
    }

    fprintf(stderr, "Warning: unknown statement: %.60s\n", p);
    return 0;
}

/* ================================================================
 * Main interpreter loop
 * ================================================================ */
static void run(void)
{
    Interp ip = { .pc = 0, .running = 1 };
    while (ip.running && ip.pc < g_nlines) {
        int old_pc = ip.pc;
        int jumped  = dispatch(&ip, g_lines[ip.pc].text);
        if (!jumped) ip.pc = old_pc + 1;
    }
}

/* ================================================================
 * Loader
 * ================================================================ */
static void load(const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (!f) { perror(filename); exit(1); }
    char buf[MAX_LINE_LEN];
    while (fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        char *p = buf;
        while (isspace((unsigned char)*p)) p++;
        if (!*p || !isdigit((unsigned char)*p)) continue;
        int num = (int)strtol(p, &p, 10);
        while (isspace((unsigned char)*p)) p++;
        if (g_nlines >= MAX_LINES) { fprintf(stderr, "Too many lines\n"); exit(1); }
        g_lines[g_nlines].linenum = num;
        strncpy(g_lines[g_nlines].text, p, MAX_LINE_LEN - 1);
        g_nlines++;
    }
    fclose(f);
    qsort(g_lines, g_nlines, sizeof(Line), line_cmp);
}

/* ================================================================
 * Entry point
 * ================================================================ */
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s program.bas [precision_bits]\n", argv[0]);
        return 1;
    }
    g_prec = (argc >= 3) ? (mp_bitcnt_t)atoi(argv[2]) : DEFAULT_PREC;
    mpf_set_default_prec(g_prec);
    load(argv[1]);
    run();
    return 0;
}
