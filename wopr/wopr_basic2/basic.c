/*
 * basic.c — Modular BASIC interpreter with GNU MP precision
 *           and pluggable display backend (display.h)
 *
 * Build:
 *   gcc -O2 -o basic basic.c display_ansi.c -lgmp -lm
 *
 * Usage:
 *   ./basic program.bas [precision_bits]
 *
 * Adding a command:
 *   1. Write:    static int cmd_foo(Interp *ip, const char *args)
 *   2. Register: COMMAND("FOO", cmd_foo)  in the commands[] table
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <gmp.h>
#include "display.h"

/* ================================================================
 * Configuration
 * ================================================================ */
#define DEFAULT_PREC   512
#define PRINT_DIGITS    60
#define MAX_VARS       512
#define MAX_VARNAME     64
#define MAX_LINES     8192
#define MAX_LINE_LEN   512
#define CTRL_STACK_MAX  64
#define MAX_ARRAY_DIMS   2
#define MAX_ARRAY_SIZE 4096   /* total elements per array */

static mp_bitcnt_t g_prec = DEFAULT_PREC;

/* ================================================================
 * String helper
 * ================================================================ */
static char *str_dup(const char *s) {
    char *d = malloc(strlen(s) + 1);
    if (!d) { fprintf(stderr, "OOM\n"); exit(1); }
    strcpy(d, d); /* compiler-friendly no-op bait — real copy below */
    strcpy(d, s);
    return d;
}

/* ================================================================
 * Interpreter state forward declaration
 * ================================================================ */
typedef struct Interp Interp;

/* ================================================================
 * Variable store — numeric (mpf) and string
 * ================================================================ */
typedef enum { VAR_NUM, VAR_STR, VAR_ARRAY_NUM, VAR_ARRAY_STR } VarKind;

typedef struct {
    char    name[MAX_VARNAME];
    VarKind kind;
    /* scalar */
    mpf_t   num;
    char   *str;
    /* array (up to 2D) */
    int     dim[MAX_ARRAY_DIMS];
    int     ndim;
    mpf_t  *arr_num;
    char  **arr_str;
} Var;

static Var g_vars[MAX_VARS];
static int g_nvar = 0;

static int var_is_str_name(const char *name) {
    return name[strlen(name)-1] == '$';
}

static Var *var_find(const char *name) {
    for (int i = 0; i < g_nvar; i++)
        if (strcasecmp(g_vars[i].name, name) == 0) return &g_vars[i];
    return NULL;
}

static Var *var_create(const char *name) {
    if (g_nvar >= MAX_VARS) { fprintf(stderr, "Too many variables\n"); exit(1); }
    Var *v = &g_vars[g_nvar++];
    memset(v, 0, sizeof(*v));
    strncpy(v->name, name, MAX_VARNAME - 1);
    if (var_is_str_name(name)) {
        v->kind = VAR_STR;
        v->str  = str_dup("");
    } else {
        v->kind = VAR_NUM;
        mpf_init2(v->num, g_prec);
        mpf_set_ui(v->num, 0);
    }
    return v;
}

static Var *var_get(const char *name) {
    Var *v = var_find(name);
    return v ? v : var_create(name);
}

/* Array element access (1-based indices) */
static mpf_t *arr_num_elem(Var *v, int i, int j) {
    /* BASIC arrays are 0-based; DIM X(n) creates indices 0..n */
    int idx = (v->ndim == 2) ? (i * v->dim[1] + j) : i;
    int total = v->dim[0] * (v->ndim == 2 ? v->dim[1] : 1);
    if (idx < 0 || idx >= total)
        { fprintf(stderr, "Array out of bounds: index %d (size %d)\n", idx, total); exit(1); }
    return &v->arr_num[idx];
}

static char **arr_str_elem(Var *v, int i, int j) {
    int idx = (v->ndim == 2) ? (i * v->dim[1] + j) : i;
    int total = v->dim[0] * (v->ndim == 2 ? v->dim[1] : 1);
    if (idx < 0 || idx >= total)
        { fprintf(stderr, "Array out of bounds: index %d (size %d)\n", idx, total); exit(1); }
    return &v->arr_str[idx];
}

/* ================================================================
 * Program store
 * ================================================================ */
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
 * Control stack — FOR loops and GOSUB frames
 * ================================================================ */
typedef struct {
    char  varname[MAX_VARNAME];   /* "\x01GOSUB" for subroutine frames */
    mpf_t limit, step;
    int   line_idx;               /* FOR: loop start; GOSUB: return address */
} CtrlFrame;

static CtrlFrame g_ctrl[CTRL_STACK_MAX];
static int       g_ctrl_top = 0;

/* ================================================================
 * Interpreter state
 * ================================================================ */
struct Interp {
    int pc;
    int running;
};

/* ================================================================
 * Forward declarations
 * ================================================================ */
static int dispatch(Interp *ip, const char *line);
static int dispatch_multi(Interp *ip, const char *clause);
static const char *eval_expr(const char *s, mpf_t result);
static const char *eval_str_expr(const char *s, char *buf, int bufsz);
static int is_str_token(const char *p);

/* ================================================================
 * Utility
 * ================================================================ */
static const char *sk(const char *p) { while (isspace((unsigned char)*p)) p++; return p; }

static const char *read_varname(const char *p, char *name) {
    int i = 0;
    while ((isalnum((unsigned char)*p) || *p == '_') && i < MAX_VARNAME-2)
        name[i++] = (char)toupper((unsigned char)*p++);
    if (*p == '$') name[i++] = *p++;   /* string sigil */
    name[i] = '\0';
    return p;
}

/* Check if keyword kw matches at p (word-boundary aware) */
static int kw_match(const char *p, const char *kw) {
    size_t len = strlen(kw);
    if (strncasecmp(p, kw, len) != 0) return 0;
    char next = p[len];
    return !isalnum((unsigned char)next) && next != '_' && next != '$';
}

/* ================================================================
 * String expression evaluator
 * Handles: literal "...", variable$, CHR$(...), STRING$(...), SPC(...),
 *          MID$(...), STR$(...), concatenation with +
 * Returns pointer past expression.
 * ================================================================ */
static const char *eval_str_primary(const char *p, char *buf, int bufsz);

static const char *eval_str_expr(const char *s, char *buf, int bufsz) {
    buf[0] = '\0';
    s = sk(s);
    char tmp[1024];
    s = eval_str_primary(s, tmp, sizeof tmp);
    strncat(buf, tmp, bufsz - strlen(buf) - 1);
    while (*sk(s) == '+') {
        s = sk(s) + 1;
        s = eval_str_primary(sk(s), tmp, sizeof tmp);
        strncat(buf, tmp, bufsz - strlen(buf) - 1);
    }
    return s;
}

static const char *eval_str_primary(const char *p, char *buf, int bufsz) {
    p = sk(p);
    buf[0] = '\0';

    /* SPC(n) — n spaces, usable in string context too */
    if (kw_match(p, "SPC")) {
        p = sk(p + 3);
        if (*p == '(') p++;
        mpf_t n; mpf_init2(n, g_prec);
        p = eval_expr(sk(p), n);
        int count = (int)mpf_get_si(n); mpf_clear(n);
        if (count < 0) count = 0;
        if (count >= bufsz) count = bufsz - 1;
        memset(buf, ' ', count); buf[count] = '\0';
        if (*sk(p) == ')') p = sk(p) + 1;
        return p;
    }

    /* LEFT$(str$, n) */
    if (kw_match(p, "LEFT$")) {
        p = sk(p + 5);
        if (*p == '(') p++;
        char src[1024];
        p = sk(eval_str_expr(sk(p), src, sizeof src));
        if (*p == ',') p = sk(p + 1);
        mpf_t n; mpf_init2(n, g_prec);
        p = sk(eval_expr(sk(p), n));
        int len = (int)mpf_get_si(n); mpf_clear(n);
        if (*sk(p) == ')') p = sk(p) + 1;
        int srclen = (int)strlen(src);
        if (len > srclen) len = srclen;
        if (len >= bufsz) len = bufsz - 1;
        memcpy(buf, src, len); buf[len] = '\0';
        return p;
    }

    /* RIGHT$(str$, n) */
    if (kw_match(p, "RIGHT$")) {
        p = sk(p + 6);
        if (*p == '(') p++;
        char src[1024];
        p = sk(eval_str_expr(sk(p), src, sizeof src));
        if (*p == ',') p = sk(p + 1);
        mpf_t n; mpf_init2(n, g_prec);
        p = sk(eval_expr(sk(p), n));
        int len = (int)mpf_get_si(n); mpf_clear(n);
        if (*sk(p) == ')') p = sk(p) + 1;
        int srclen = (int)strlen(src);
        if (len > srclen) len = srclen;
        if (len >= bufsz) len = bufsz - 1;
        memcpy(buf, src + srclen - len, len); buf[len] = '\0';
        return p;
    }

    /* INKEY$ — non-blocking key read */
    if (kw_match(p, "INKEY$")) {
        int ch = display_inkey();
        buf[0] = ch ? (char)ch : '\0';
        buf[1] = '\0';
        return p + 6;
    }

    /* String literal */
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < bufsz-1) buf[i++] = *p++;
        buf[i] = '\0';
        if (*p == '"') p++;
        return p;
    }

    /* CHR$(n) */
    if (kw_match(p, "CHR$")) {
        p = sk(p + 4);
        if (*p == '(') p++;
        mpf_t n; mpf_init2(n, g_prec);
        p = eval_expr(sk(p), n);
        buf[0] = (char)mpf_get_si(n); buf[1] = '\0';
        mpf_clear(n);
        if (*sk(p) == ')') p = sk(p)+1;
        return p;
    }

    /* STRING$(n, c) */
    if (kw_match(p, "STRING$")) {
        p = sk(p + 7);
        if (*p == '(') p++;
        mpf_t n; mpf_init2(n, g_prec);
        p = sk(eval_expr(sk(p), n));
        int count = (int)mpf_get_si(n); mpf_clear(n);
        if (*p == ',') p = sk(p+1);
        /* second arg: char code or string */
        int ch;
        if (*p == '"') { p++; ch = (unsigned char)*p; while (*p && *p!='"') p++; if(*p=='"')p++; }
        else { mpf_t c2; mpf_init2(c2, g_prec); p=eval_expr(p,c2); ch=(int)mpf_get_si(c2); mpf_clear(c2); }
        if (count < 0) count = 0;
        if (count >= bufsz) count = bufsz-1;
        memset(buf, ch, count); buf[count] = '\0';
        if (*sk(p) == ')') p = sk(p)+1;
        return p;
    }

    /* MID$(var$, start [,len]) */
    if (kw_match(p, "MID$")) {
        p = sk(p + 4);
        if (*p == '(') p++;
        char vname[MAX_VARNAME];
        p = sk(read_varname(sk(p), vname));
        Var *v = var_get(vname);
        const char *src = v->str ? v->str : "";
        if (*p == ',') p = sk(p+1);
        mpf_t st; mpf_init2(st, g_prec);
        p = sk(eval_expr(sk(p), st));
        int start = (int)mpf_get_si(st) - 1; mpf_clear(st);
        int len = -1;
        if (*p == ',') {
            p = sk(p+1);
            mpf_t ln; mpf_init2(ln, g_prec);
            p = sk(eval_expr(sk(p), ln));
            len = (int)mpf_get_si(ln); mpf_clear(ln);
        }
        if (*sk(p) == ')') p = sk(p)+1;
        int srclen = (int)strlen(src);
        if (start < 0) start = 0;
        if (start >= srclen) { buf[0]='\0'; return p; }
        if (len < 0 || start+len > srclen) len = srclen - start;
        if (len >= bufsz) len = bufsz-1;
        memcpy(buf, src+start, len); buf[len]='\0';
        return p;
    }

    /* STR$(n) */
    if (kw_match(p, "STR$")) {
        p = sk(p + 4);
        if (*p == '(') p++;
        mpf_t n; mpf_init2(n, g_prec);
        p = eval_expr(sk(p), n);
        gmp_snprintf(buf, bufsz, "%.6Fg", n);
        mpf_clear(n);
        if (*sk(p) == ')') p = sk(p)+1;
        return p;
    }

    /* String variable (scalar or array) */
    if (isalpha((unsigned char)*p)) {
        char vname[MAX_VARNAME];
        const char *after = read_varname(p, vname);
        if (var_is_str_name(vname)) {
            Var *v = var_get(vname);
            after = sk(after);
            if (*after == '(' && v->kind == VAR_ARRAY_STR) {
                after = sk(after+1);
                mpf_t i1; mpf_init2(i1, g_prec);
                after = sk(eval_expr(after, i1));
                int ii = (int)mpf_get_si(i1); mpf_clear(i1);
                int jj = 1;
                if (*after == ',') {
                    after = sk(after+1);
                    mpf_t i2; mpf_init2(i2, g_prec);
                    after = sk(eval_expr(after, i2));
                    jj = (int)mpf_get_si(i2); mpf_clear(i2);
                }
                if (*after == ')') after++;
                char **slot = arr_str_elem(v, ii, jj);
                strncpy(buf, *slot ? *slot : "", bufsz-1);
            } else {
                strncpy(buf, v->str ? v->str : "", bufsz-1);
            }
            buf[bufsz-1]='\0';
            return after;
        }
    }

    /* fallback: numeric, convert to string */
    mpf_t n; mpf_init2(n, g_prec);
    p = eval_expr(p, n);
    gmp_snprintf(buf, bufsz, "%.6Fg", n);
    mpf_clear(n);
    return p;
}

/* ================================================================
 * Numeric expression evaluator (recursive descent)
 * Handles: +, -, *, /, ^, unary -, (), numeric literals, variables,
 *          built-in functions: INT, ABS, SQR, VAL, ASC, LEN
 * ================================================================ */
typedef struct { const char *p; } Parser;

static void parse_expr_p(Parser *ps, mpf_t result);
static void parse_term_p(Parser *ps, mpf_t result);
static void parse_unary_p(Parser *ps, mpf_t result);
static void parse_power_p(Parser *ps, mpf_t result);
static void parse_primary_p(Parser *ps, mpf_t result);

static void skip_ws_p(Parser *ps) { while (isspace((unsigned char)*ps->p)) ps->p++; }

static void parse_expr_p(Parser *ps, mpf_t result) {
    mpf_t tmp; mpf_init2(tmp, g_prec);
    parse_term_p(ps, result);
    skip_ws_p(ps);
    while (*ps->p == '+' || *ps->p == '-') {
        char op = *ps->p++;
        parse_term_p(ps, tmp);
        if (op == '+') mpf_add(result, result, tmp);
        else           mpf_sub(result, result, tmp);
        skip_ws_p(ps);
    }
    mpf_clear(tmp);
}

static void parse_term_p(Parser *ps, mpf_t result) {
    mpf_t tmp; mpf_init2(tmp, g_prec);
    parse_unary_p(ps, result);
    skip_ws_p(ps);
    while (*ps->p == '*' || *ps->p == '/') {
        char op = *ps->p++;
        parse_unary_p(ps, tmp);
        if (op == '*') mpf_mul(result, result, tmp);
        else {
            if (mpf_sgn(tmp)==0) { fprintf(stderr,"Division by zero\n"); exit(1); }
            mpf_div(result, result, tmp);
        }
        skip_ws_p(ps);
    }
    mpf_clear(tmp);
}

static void parse_unary_p(Parser *ps, mpf_t result) {
    skip_ws_p(ps);
    if (*ps->p == '-') { ps->p++; parse_power_p(ps, result); mpf_neg(result, result); }
    else if (*ps->p == '+') { ps->p++; parse_power_p(ps, result); }
    else parse_power_p(ps, result);
}

static void parse_power_p(Parser *ps, mpf_t result) {
    parse_primary_p(ps, result);
    skip_ws_p(ps);
    if (*ps->p == '^') {
        ps->p++;
        mpf_t exp; mpf_init2(exp, g_prec);
        parse_unary_p(ps, exp);
        /* use double for exponentiation */
        double base_d = mpf_get_d(result);
        double exp_d  = mpf_get_d(exp);
        mpf_set_d(result, pow(base_d, exp_d));
        mpf_clear(exp);
    }
}

/* ----------------------------------------------------------------
 * DEF FN — user-defined functions
 * ---------------------------------------------------------------- */
#define MAX_DEF_FN 32
typedef struct { char name[MAX_VARNAME]; char param[MAX_VARNAME]; char body[MAX_LINE_LEN]; } DefFn;
static DefFn   g_defn[MAX_DEF_FN];
static int     g_defn_count = 0;

static int try_eval_defn(Parser *ps, mpf_t result) {
    /* Match FN followed by at least one alphanumeric (FNA, FNB, FND, etc.) */
    if (!(toupper((unsigned char)ps->p[0])=='F' && toupper((unsigned char)ps->p[1])=='N'
          && isalnum((unsigned char)ps->p[2]))) return 0;
    const char *start = ps->p;
    const char *p = ps->p + 2;
    char fname[MAX_VARNAME]; int i = 0;
    fname[i++] = 'F'; fname[i++] = 'N';
    while (isalnum((unsigned char)*p) && i < MAX_VARNAME-1) fname[i++] = (char)toupper(*p++);
    fname[i] = '\0';
    /* find the definition */
    DefFn *fn = NULL;
    for (int k = 0; k < g_defn_count; k++)
        if (strcasecmp(g_defn[k].name, fname) == 0) { fn = &g_defn[k]; break; }
    if (!fn) { ps->p = start; return 0; }
    ps->p = p; skip_ws_p(ps);
    /* evaluate argument */
    double arg_val = 0;
    if (*ps->p == '(') {
        ps->p++;
        mpf_t arg; mpf_init2(arg, g_prec);
        parse_expr_p(ps, arg);
        arg_val = mpf_get_d(arg);
        mpf_clear(arg);
        skip_ws_p(ps);
        if (*ps->p == ')') ps->p++;
    }
    /* temporarily set param variable, evaluate body, restore */
    Var *pv = NULL;
    double old_val = 0;
    if (fn->param[0]) {
        pv = var_get(fn->param);
        old_val = mpf_get_d(pv->num);
        mpf_set_d(pv->num, arg_val);
    }
    eval_expr(fn->body, result);
    if (pv) mpf_set_d(pv->num, old_val);
    return 1;
}

static void parse_primary_p(Parser *ps, mpf_t result) {
    skip_ws_p(ps);

    if (*ps->p == '(') {
        ps->p++;
        skip_ws_p(ps);

        /* evaluate one comparison term inside parens */
        #define EVAL_CMP_TERM(res) do { \
            skip_ws_p(ps); \
            if (is_str_token(ps->p)) { \
                char _lhs[1024], _rhs[1024]; \
                ps->p = sk(eval_str_expr(ps->p, _lhs, sizeof _lhs)); \
                char _op[3]={ps->p[0],ps->p[1],'\0'}; int _ol=2; \
                if(!strcmp(_op,"<>")||!strcmp(_op,"><")||!strcmp(_op,"<=")||!strcmp(_op,"=<")||!strcmp(_op,">=")||!strcmp(_op,"=>"));else{_op[1]='\0';_ol=1;} \
                ps->p=sk(ps->p+_ol); ps->p=sk(eval_str_expr(ps->p,_rhs,sizeof _rhs)); \
                int _c=strcmp(_lhs,_rhs),_cmp; \
                if(!strcmp(_op,"<>")||!strcmp(_op,"><"))_cmp=(_c!=0); \
                else if(!strcmp(_op,"<=")||!strcmp(_op,"=<"))_cmp=(_c<=0); \
                else if(!strcmp(_op,">=")||!strcmp(_op,"=>"))_cmp=(_c>=0); \
                else if(_op[0]=='<')_cmp=(_c<0); else if(_op[0]=='>')_cmp=(_c>0); else _cmp=(_c==0); \
                mpf_set_si(res,_cmp?-1:0); \
            } else { \
                parse_expr_p(ps,res); skip_ws_p(ps); \
                char _op[3]={ps->p[0],ps->p[1],'\0'}; int _ol=2; \
                if(!strcmp(_op,"<>")||!strcmp(_op,"><")||!strcmp(_op,"<=")||!strcmp(_op,"=<")||!strcmp(_op,">=")||!strcmp(_op,"=>"));else if(ps->p[0]=='<'||ps->p[0]=='>'||ps->p[0]=='='){_op[1]='\0';_ol=1;}else _ol=0; \
                if(_ol>0){ ps->p=sk(ps->p+_ol); mpf_t _r; mpf_init2(_r,g_prec); parse_expr_p(ps,_r); \
                int _c=mpf_cmp(res,_r),_cmp; \
                if(!strcmp(_op,"<>")||!strcmp(_op,"><"))_cmp=(_c!=0); \
                else if(!strcmp(_op,"<=")||!strcmp(_op,"=<"))_cmp=(_c<=0); \
                else if(!strcmp(_op,">=")||!strcmp(_op,"=>"))_cmp=(_c>=0); \
                else if(_op[0]=='<')_cmp=(_c<0); else if(_op[0]=='>')_cmp=(_c>0); else _cmp=(_c==0); \
                mpf_clear(_r); mpf_set_si(res,_cmp?-1:0); } \
            } \
        } while(0)

        EVAL_CMP_TERM(result);
        skip_ws_p(ps);

        while (kw_match(ps->p,"AND") || kw_match(ps->p,"OR")) {
            int is_and = kw_match(ps->p,"AND");
            ps->p += is_and ? 3 : 2;
            skip_ws_p(ps);
            mpf_t rhs; mpf_init2(rhs,g_prec);
            EVAL_CMP_TERM(rhs);
            long lv=mpf_get_si(result), rv=mpf_get_si(rhs);
            mpf_set_si(result, is_and ? (lv & rv) : (lv | rv));
            mpf_clear(rhs);
            skip_ws_p(ps);
        }
        #undef EVAL_CMP_TERM

        if (*ps->p == ')') ps->p++;
        return;
    }

    /* Numeric literal — including hex &H */
    if (*ps->p == '&' && (ps->p[1]=='H'||ps->p[1]=='h')) {
        ps->p += 2;
        long v = strtol(ps->p, (char**)&ps->p, 16);
        mpf_set_si(result, v);
        return;
    }
    if (isdigit((unsigned char)*ps->p) || *ps->p == '.') {
        char buf[64]; int i = 0;
        while (isdigit((unsigned char)*ps->p) || *ps->p=='.' || *ps->p=='E' || *ps->p=='e'
               || (i>0 && (ps->p[-1]=='E'||ps->p[-1]=='e') && (*ps->p=='+'||*ps->p=='-')))
            buf[i++] = *ps->p++;
        /* strip BASIC type suffixes */
        if (*ps->p=='!'||*ps->p=='#'||*ps->p=='%') ps->p++;
        buf[i]='\0';
        mpf_set_str(result, buf, 10);
        return;
    }

    /* Built-in numeric functions */
    /* PEEK(addr) — stub, returns 0 except &H410 reports 80-col CGA */
    if (kw_match(ps->p, "PEEK")) {
        ps->p+=4; skip_ws_p(ps); if(*ps->p=='(') ps->p++;
        mpf_t addr; mpf_init2(addr, g_prec);
        parse_expr_p(ps, addr);
        long a = mpf_get_si(addr); mpf_clear(addr);
        /* &H410 = equipment flags: bits 5-4 = 00 means EGA/VGA (80 col) */
        long val = (a == 0x410) ? 0x00 : 0;
        mpf_set_si(result, val);
        skip_ws_p(ps); if(*ps->p==')') ps->p++;
        return;
    }
    /* INT(x) */
    if (kw_match(ps->p, "INT")) {
        ps->p+=3; skip_ws_p(ps); if(*ps->p=='(') ps->p++;
        parse_expr_p(ps, result);
        double d = mpf_get_d(result);
        mpf_set_d(result, floor(d));
        skip_ws_p(ps); if(*ps->p==')') ps->p++;
        return;
    }
    /* ABS(x) */
    if (kw_match(ps->p, "ABS")) {
        ps->p+=3; skip_ws_p(ps); if(*ps->p=='(') ps->p++;
        parse_expr_p(ps, result);
        mpf_abs(result, result);
        skip_ws_p(ps); if(*ps->p==')') ps->p++;
        return;
    }
    /* SQR(x) */
    if (kw_match(ps->p, "SQR")) {
        ps->p+=3; skip_ws_p(ps); if(*ps->p=='(') ps->p++;
        parse_expr_p(ps, result);
        double d = mpf_get_d(result);
        mpf_set_d(result, sqrt(d));
        skip_ws_p(ps); if(*ps->p==')') ps->p++;
        return;
    }
    /* VAL(str$) */
    if (kw_match(ps->p, "VAL")) {
        ps->p+=3; skip_ws_p(ps); if(*ps->p=='(') ps->p++;
        char sbuf[256];
        ps->p = eval_str_expr(ps->p, sbuf, sizeof sbuf);
        mpf_set_d(result, atof(sbuf));
        skip_ws_p(ps); if(*ps->p==')') ps->p++;
        return;
    }
    /* ASC(str$) */
    if (kw_match(ps->p, "ASC")) {
        ps->p+=3; skip_ws_p(ps); if(*ps->p=='(') ps->p++;
        char sbuf[256];
        ps->p = eval_str_expr(ps->p, sbuf, sizeof sbuf);
        mpf_set_si(result, (unsigned char)sbuf[0]);
        skip_ws_p(ps); if(*ps->p==')') ps->p++;
        return;
    }
    /* LEN(str$) */
    if (kw_match(ps->p, "LEN")) {
        ps->p+=3; skip_ws_p(ps); if(*ps->p=='(') ps->p++;
        char sbuf[256];
        ps->p = eval_str_expr(ps->p, sbuf, sizeof sbuf);
        mpf_set_si(result, (long)strlen(sbuf));
        skip_ws_p(ps); if(*ps->p==')') ps->p++;
        return;
    }

    /* RND(x) — returns random float in [0,1) regardless of x */
    if (kw_match(ps->p, "RND")) {
        ps->p += 3; skip_ws_p(ps);
        if (*ps->p == '(') {
            int depth = 1; ps->p++;
            while (*ps->p && depth > 0) {
                if (*ps->p == '(') depth++;
                else if (*ps->p == ')') depth--;
                ps->p++;
            }
        }
        mpf_set_d(result, (double)rand() / ((double)RAND_MAX + 1.0));
        return;
    }

    /* User-defined functions: FNA(x), FNB(x), etc. */
    if (toupper((unsigned char)ps->p[0])=='F' && toupper((unsigned char)ps->p[1])=='N'
        && isalnum((unsigned char)ps->p[2])) {
        if (try_eval_defn(ps, result)) return;
    }

    /* Variable or array element */
    if (isalpha((unsigned char)*ps->p)) {
        char name[MAX_VARNAME];
        ps->p = read_varname(ps->p, name);
        skip_ws_p(ps);

        /* String-typed name (ends in $) in numeric context — evaluate as
           string and return 0; handles stray CHR$(...) etc in expressions */
        if (var_is_str_name(name) || strcasecmp(name,"SPC")==0) {
            char sbuf[1024];
            /* if followed by '(' it's a string function call — back up and
               let eval_str_primary handle it, then return 0 */
            const char *save = ps->p;
            char tmp2[1024]; tmp2[0]='\0';
            /* rewind to start of name */
            ps->p = save; /* already past name; call str evaluator with full token */
            /* We can't easily rewind, so just consume args and return 0 */
            if (*ps->p == '(') {
                int depth=1; ps->p++;
                while (*ps->p && depth>0) {
                    if (*ps->p=='(') depth++;
                    else if (*ps->p==')') depth--;
                    ps->p++;
                }
            }
            mpf_set_ui(result, 0);
            return;
        }

        /* Numeric array access: name(i) or name(i,j) */
        if (*ps->p == '(') {
            ps->p++;
            mpf_t idx1; mpf_init2(idx1, g_prec);
            parse_expr_p(ps, idx1);
            int i1 = (int)mpf_get_si(idx1); mpf_clear(idx1);
            int i2 = 1;
            skip_ws_p(ps);
            if (*ps->p == ',') {
                ps->p++;
                mpf_t idx2; mpf_init2(idx2, g_prec);
                parse_expr_p(ps, idx2);
                i2 = (int)mpf_get_si(idx2); mpf_clear(idx2);
                skip_ws_p(ps);
            }
            if (*ps->p == ')') ps->p++;
            Var *v = var_get(name);
            if (v->kind != VAR_ARRAY_NUM) { fprintf(stderr,"Not an array: %s\n",name); exit(1); }
            mpf_set(result, *arr_num_elem(v, i1, i2));
            return;
        }
        Var *v = var_get(name);
        mpf_set(result, v->num);
        return;
    }

    fprintf(stderr, "Parse error near: %.20s\n", ps->p); exit(1);
}

static const char *eval_expr(const char *s, mpf_t result) {
    Parser ps = { s };
    parse_expr_p(&ps, result);
    return ps.p;
}

/* ================================================================
 * PRINT USING formatter
 * Format chars: # = digit placeholder, . = decimal, $ = dollar sign,
 *               + = leading sign, , = thousands separator (ignored here)
 * Field is right-justified to the total width implied by # count.
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
 * Command handler type
 *   Return 0: caller advances pc+1
 *   Return 1: handler already set ip->pc
 * ================================================================ */
typedef int (*CmdFn)(Interp *ip, const char *args);
typedef struct { const char *keyword; CmdFn fn; } Command;

/* ================================================================
 * Command handlers
 * ================================================================ */

static int cmd_rem(Interp *ip, const char *args)    { (void)ip;(void)args; return 0; }
static int cmd_defseg(Interp *ip, const char *args) { (void)ip;(void)args; return 0; }
static int cmd_defdbl(Interp *ip, const char *args) { (void)ip;(void)args; return 0; }
static int cmd_key(Interp *ip, const char *args)    { (void)ip;(void)args; return 0; }
static int cmd_chain(Interp *ip, const char *args)  { (void)ip;(void)args; return 0; }
static int cmd_screen(Interp *ip, const char *args) { (void)ip;(void)args; return 0; }

static int cmd_end(Interp *ip, const char *args) {
    (void)args; ip->running = 0; return 1;
}

static int cmd_cls(Interp *ip, const char *args) {
    (void)ip;(void)args; display_cls(); return 0;
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
    mpf_t fg, bg; mpf_init2(fg,g_prec); mpf_init2(bg,g_prec);
    mpf_set_ui(bg, 0);
    p = eval_expr(p, fg); p = sk(p);
    if (*p == ',') { p=sk(p+1); p=eval_expr(p,bg); }
    display_color((int)mpf_get_si(fg), (int)mpf_get_si(bg));
    mpf_clears(fg,bg,NULL);
    return 0;
}

static int cmd_locate(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
    mpf_t row, col, cur;
    mpf_init2(row,g_prec); mpf_init2(col,g_prec); mpf_init2(cur,g_prec);
    mpf_set_ui(row, 0); mpf_set_ui(col, 0); mpf_set_ui(cur, 1);

    /* row — optional */
    if (*p && *p != ',') p = sk(eval_expr(p, row));
    if (*p == ',') p = sk(p+1); else goto locate_done;
    /* col — optional */
    if (*p && *p != ',') p = sk(eval_expr(p, col));
    if (*p == ',') p = sk(p+1); else goto locate_done;
    /* cursor — optional */
    if (*p && *p != ',') p = sk(eval_expr(p, cur));

    locate_done:
    if (mpf_get_si(row) > 0 || mpf_get_si(col) > 0)
        display_locate((int)mpf_get_si(row), (int)mpf_get_si(col));
    display_cursor((int)mpf_get_si(cur));
    mpf_clears(row,col,cur,NULL);
    return 0;
}

static int cmd_dim(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
    while (*p) {
        char name[MAX_VARNAME];
        p = sk(read_varname(p, name));
        Var *v = var_find(name);
        if (!v) v = var_create(name);
        if (*p == '(') {
            p++;
            mpf_t d1; mpf_init2(d1,g_prec);
            p = sk(eval_expr(sk(p), d1));
            int dim1 = (int)mpf_get_si(d1)+1; mpf_clear(d1);
            int dim2 = 1, ndim = 1;
            if (*p == ',') {
                p = sk(p+1);
                mpf_t d2; mpf_init2(d2,g_prec);
                p = sk(eval_expr(sk(p),d2));
                dim2 = (int)mpf_get_si(d2)+1; mpf_clear(d2);
                ndim = 2;
            }
            if (*p == ')') p++;
            int total = dim1 * dim2;
            if (total > MAX_ARRAY_SIZE) { fprintf(stderr,"Array too large\n"); exit(1); }
            if (var_is_str_name(name)) {
                v->kind = VAR_ARRAY_STR;
                v->dim[0] = dim1; v->dim[1] = dim2; v->ndim = ndim;
                v->arr_str = calloc(total, sizeof(char*));
                for (int i=0;i<total;i++) v->arr_str[i] = str_dup("");
            } else {
                v->kind = VAR_ARRAY_NUM;
                v->dim[0] = dim1; v->dim[1] = dim2; v->ndim = ndim;
                v->arr_num = calloc(total, sizeof(mpf_t));
                for (int i=0;i<total;i++) { mpf_init2(v->arr_num[i],g_prec); mpf_set_ui(v->arr_num[i],0); }
            }
        }
        p = sk(p);
        if (*p == ',') p = sk(p+1);
    }
    return 0;
}

static int cmd_let(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
    char name[MAX_VARNAME];
    p = sk(read_varname(p, name));
    int arr_i = 0, arr_j = 1, is_arr = 0;
    if (*p == '(') {
        is_arr = 1; p = sk(p+1);
        mpf_t i1; mpf_init2(i1,g_prec);
        p = sk(eval_expr(p, i1)); arr_i = (int)mpf_get_si(i1); mpf_clear(i1);
        if (*p==',') { p=sk(p+1); mpf_t i2; mpf_init2(i2,g_prec); p=sk(eval_expr(p,i2)); arr_j=(int)mpf_get_si(i2); mpf_clear(i2); }
        if (*p==')') p=sk(p+1);
    }
    if (*p=='=') p=sk(p+1);
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
        mpf_t val; mpf_init2(val,g_prec);
        eval_expr(sk(p), val);
        if (is_arr) {
            Var *v = var_get(name);
            mpf_set(*arr_num_elem(v, arr_i, arr_j), val);
        } else {
            Var *v = var_get(name);
            mpf_set(v->num, val);
        }
        mpf_clear(val);
    }
    return 0;
}

/* PRINT [USING fmt;] items — returns 1 if trailing separator (no newline) */
static int cmd_print(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);

    /* PRINT USING "fmt"; expr [;expr...] [;] */
    if (kw_match(p, "USING")) {
        p = sk(p + 5);
        char fmt[128]; int fi = 0;
        if (*p == '"') {
            p++;
            while (*p && *p != '"' && fi < (int)sizeof(fmt)-1) fmt[fi++] = *p++;
            if (*p == '"') p++;
        }
        fmt[fi] = '\0';
        p = sk(p);
        if (*p == ';') p = sk(p + 1);

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

    /* Regular PRINT */
    int trailing_sep = 0;
    while (*p) {
        trailing_sep = 0;
        if (is_str_token(p)) {
            if (kw_match(p,"SPC")) {
                p=sk(p+3); if(*p=='(') p++;
                mpf_t n; mpf_init2(n,g_prec); p=eval_expr(sk(p),n);
                display_spc((int)mpf_get_si(n)); mpf_clear(n);
                p=sk(p); if(*p==')') p=sk(p+1);
            } else if (kw_match(p,"TAB")) {
                p=sk(p+3); if(*p=='(') p++;
                mpf_t n; mpf_init2(n,g_prec); p=eval_expr(sk(p),n);
                int col=(int)mpf_get_si(n)-1; mpf_clear(n);
                p=sk(p); if(*p==')') p=sk(p+1);
                /* move to column by printing spaces — approximate since we
                   don't track cursor position; just emit the spaces */
                if(col>0) display_spc(col);
            } else {
                char sbuf[1024];
                p = eval_str_expr(p, sbuf, sizeof sbuf);
                display_print(sbuf);
            }
        } else {
            mpf_t val; mpf_init2(val,g_prec);
            p = eval_expr(p, val);
            double d = mpf_get_d(val);
            /* Microsoft BASIC: numbers always printed with a leading space
               (where the sign would be for positives) and a trailing space */
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
        if (*p == ';') { trailing_sep=1; p=sk(p+1); }
        else if (*p == ',') { display_putchar('\t'); trailing_sep=1; p=sk(p+1); }
        else break;
    }
    if (!trailing_sep) display_newline();
    return 0;
}

/* LINE INPUT "prompt"; var$ */
static int cmd_line_input(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
    if (*p == '"') {
        p++;
        while (*p && *p!='"') display_putchar(*p++);
        if (*p=='"') p++;
    }
    p=sk(p); if(*p==';'||*p==',') p=sk(p+1);
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

/* INPUT ["prompt";] var [, var ...]
 * Displays prompt (or "? "), reads a line, assigns to each var.
 * Numeric vars get VAL(), string vars get the raw text. */
static int cmd_input(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);

    /* optional prompt string */
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

    /* read one line */
    char linebuf[512];
    display_cursor(1);
    display_getline(linebuf, sizeof linebuf);
    display_newline();

    /* assign to each comma-separated variable */
    /* for simplicity: single var gets the whole line;
       multiple vars split on commas */
    char *tok = linebuf;
    while (*p) {
        char name[MAX_VARNAME];
        p = sk(read_varname(sk(p), name));

        /* find end of this token in input */
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

        /* strip leading/trailing whitespace from token */
        char *v_start = val_str;
        while (isspace((unsigned char)*v_start)) v_start++;
        char *v_end = v_start + strlen(v_start);
        while (v_end > v_start && isspace((unsigned char)v_end[-1])) *--v_end = '\0';

        Var *v = var_get(name);
        if (var_is_str_name(name)) {
            free(v->str); v->str = str_dup(v_start);
        } else {
            mpf_set_d(v->num, atof(v_start));
        }

        p = sk(p);
        if (*p == ',') p = sk(p + 1); else break;
    }
    return 0;
}

/* FOR var = start TO limit [STEP step] */
static int cmd_for(Interp *ip, const char *args) {
    const char *p = sk(args);
    char vname[MAX_VARNAME];
    p = sk(read_varname(p, vname));
    if (*p=='=') p=sk(p+1);
    mpf_t start,limit,step; mpf_init2(start,g_prec); mpf_init2(limit,g_prec); mpf_init2(step,g_prec);
    mpf_set_ui(step,1);
    p=sk(eval_expr(p,start));
    if (kw_match(p,"TO")) p=sk(p+2);
    p=sk(eval_expr(p,limit));
    if (kw_match(p,"STEP")) { p=sk(p+4); eval_expr(p,step); }
    Var *v = var_get(vname); mpf_set(v->num, start);
    if (g_ctrl_top>=CTRL_STACK_MAX) { fprintf(stderr,"Stack overflow\n"); exit(1); }
    CtrlFrame *f=&g_ctrl[g_ctrl_top++];
    strncpy(f->varname,vname,MAX_VARNAME-1);
    mpf_init2(f->limit,g_prec); mpf_set(f->limit,limit);
    mpf_init2(f->step, g_prec); mpf_set(f->step, step);
    f->line_idx=ip->pc;
    mpf_clears(start,limit,step,NULL);
    return 0;
}

/* NEXT [var] */
static int cmd_next(Interp *ip, const char *args) {
    const char *p=sk(args);
    char vname[MAX_VARNAME]="";
    if (isalpha((unsigned char)*p)) read_varname(p,vname);
    int fi=g_ctrl_top-1;
    if (*vname) {
        for (fi=g_ctrl_top-1;fi>=0;fi--)
            if (strcasecmp(g_ctrl[fi].varname,vname)==0) break;
        if (fi<0) { fprintf(stderr,"NEXT without FOR: %s\n",vname); exit(1); }
    }
    CtrlFrame *f=&g_ctrl[fi];
    Var *cv=var_get(f->varname);
    mpf_add(cv->num,cv->num,f->step);
    int done=(mpf_sgn(f->step)>0)?(mpf_cmp(cv->num,f->limit)>0):(mpf_cmp(cv->num,f->limit)<0);
    if (done) { mpf_clear(f->limit); mpf_clear(f->step); g_ctrl_top=fi; return 0; }
    ip->pc=f->line_idx+1; return 1;
}

/* GOTO n */
static int cmd_goto(Interp *ip, const char *args) {
    int idx=find_line_idx(atoi(sk(args)));
    if (idx<0) { fprintf(stderr,"GOTO: line not found: %s\n",args); exit(1); }
    ip->pc=idx; return 1;
}

/* GOSUB n */
static int cmd_gosub(Interp *ip, const char *args) {
    if (g_ctrl_top>=CTRL_STACK_MAX) { fprintf(stderr,"Stack overflow\n"); exit(1); }
    CtrlFrame *f=&g_ctrl[g_ctrl_top++];
    strcpy(f->varname,"\x01GOSUB");
    f->line_idx=ip->pc+1;
    mpf_init2(f->limit,g_prec); mpf_set_ui(f->limit,0);
    mpf_init2(f->step, g_prec); mpf_set_ui(f->step, 0);
    return cmd_goto(ip,args);
}

/* RETURN */
static int cmd_return(Interp *ip, const char *args) {
    (void)args;
    for (int fi=g_ctrl_top-1;fi>=0;fi--) {
        if (strcmp(g_ctrl[fi].varname,"\x01GOSUB")==0) {
            ip->pc=g_ctrl[fi].line_idx;
            mpf_clear(g_ctrl[fi].limit); mpf_clear(g_ctrl[fi].step);
            g_ctrl_top=fi; return 1;
        }
    }
    fprintf(stderr,"RETURN without GOSUB\n"); exit(1);
}

static int is_str_token(const char *p) {
    p = sk(p);
    if (*p == '"') return 1;
    if (kw_match(p,"INKEY$"))  return 1;
    if (kw_match(p,"CHR$"))    return 1;
    if (kw_match(p,"STRING$")) return 1;
    if (kw_match(p,"MID$"))    return 1;
    if (kw_match(p,"STR$"))    return 1;
    if (kw_match(p,"SPC"))     return 1;
    if (kw_match(p,"SPACE$"))  return 1;
    if (kw_match(p,"TAB"))     return 1;
    if (kw_match(p,"LEFT$"))   return 1;
    if (kw_match(p,"RIGHT$"))  return 1;
    if (kw_match(p,"INPUT$"))  return 1;
    if (isalpha((unsigned char)*p)) {
        char name[MAX_VARNAME];
        read_varname(p, name);
        if (var_is_str_name(name)) return 1;
    }
    return 0;
}

static const char *eval_str_or_inkey(const char *p, char *buf, int bufsz) {
    p = sk(p);
    if (kw_match(p, "INKEY$")) {
        int ch = display_inkey();
        buf[0] = ch ? (char)ch : '\0';
        buf[1] = '\0';
        return p + 6;
    }
    return eval_str_expr(p, buf, bufsz);
}

/* evaluate one comparison unit — handles:
 *   (expr op expr)   parenthesized comparison → boolean
 *   expr op expr     standard comparison
 *   expr             bare expression, non-zero = true
 */
static int eval_one_cmp(const char **pp) {
    const char *p = sk(*pp);
    int cmp = 0;

    /* parenthesized sub-condition: (lhs op rhs) */
    if (*p == '(') {
        p++;
        /* peek: is there a comparison operator inside? */
        /* evaluate lhs, check for op */
        if (is_str_token(p)) {
            char lhs[1024], rhs[1024];
            p = sk(eval_str_or_inkey(p, lhs, sizeof lhs));
            char op2[3] = {p[0],p[1],'\0'}; int oplen=2;
            if (!strcmp(op2,"<>")||!strcmp(op2,"><")||!strcmp(op2,"<=")||
                !strcmp(op2,"=<")||!strcmp(op2,">=")||!strcmp(op2,"=>")) ;
            else { op2[1]='\0'; oplen=1; }
            p = sk(eval_str_or_inkey(sk(p+oplen), rhs, sizeof rhs));
            int c = strcmp(lhs, rhs);
            if      (!strcmp(op2,"<>")||!strcmp(op2,"><")) cmp=(c!=0);
            else if (!strcmp(op2,"<=")||!strcmp(op2,"=<")) cmp=(c<=0);
            else if (!strcmp(op2,">=")||!strcmp(op2,"=>")) cmp=(c>=0);
            else if (op2[0]=='<') cmp=(c<0);
            else if (op2[0]=='>') cmp=(c>0);
            else                  cmp=(c==0);
        } else {
            mpf_t lhs; mpf_init2(lhs,g_prec);
            p = sk(eval_expr(p, lhs));
            /* check for comparison operator */
            char op2[3] = {p[0],p[1],'\0'}; int oplen=2;
            if (!strcmp(op2,"<>")||!strcmp(op2,"><")||!strcmp(op2,"<=")||
                !strcmp(op2,"=<")||!strcmp(op2,">=")||!strcmp(op2,"=>")) ;
            else if (p[0]=='<'||p[0]=='>'||p[0]=='=') { op2[1]='\0'; oplen=1; }
            else { /* bare expression in parens */ cmp=(mpf_sgn(lhs)!=0); mpf_clear(lhs); goto closeparen; }
            mpf_t rhs; mpf_init2(rhs,g_prec);
            p = sk(eval_expr(sk(p+oplen), rhs));
            int c = mpf_cmp(lhs,rhs);
            if      (!strcmp(op2,"<>")||!strcmp(op2,"><")) cmp=(c!=0);
            else if (!strcmp(op2,"<=")||!strcmp(op2,"=<")) cmp=(c<=0);
            else if (!strcmp(op2,">=")||!strcmp(op2,"=>")) cmp=(c>=0);
            else if (op2[0]=='<') cmp=(c<0);
            else if (op2[0]=='>') cmp=(c>0);
            else                  cmp=(c==0);
            mpf_clears(lhs,rhs,NULL);
            /* chain AND/OR comparisons: (A <> 9 AND A <> 12) */
            while (kw_match(p,"AND") || kw_match(p,"OR")) {
                int is_and = kw_match(p,"AND");
                p = sk(p + (is_and ? 3 : 2));
                mpf_t lhs2; mpf_init2(lhs2,g_prec);
                p = sk(eval_expr(p, lhs2));
                char op3[3]={p[0],p[1],'\0'}; int ol3=2;
                if (!strcmp(op3,"<>")||!strcmp(op3,"><")||!strcmp(op3,"<=")||
                    !strcmp(op3,"=<")||!strcmp(op3,">=")||!strcmp(op3,"=>")) ;
                else if (p[0]=='<'||p[0]=='>'||p[0]=='='){op3[1]='\0';ol3=1;}
                else { int r2=(mpf_sgn(lhs2)!=0); mpf_clear(lhs2);
                       cmp=is_and?(cmp&&r2):(cmp||r2); continue; }
                mpf_t rhs2; mpf_init2(rhs2,g_prec);
                p = sk(eval_expr(sk(p+ol3), rhs2));
                int c2=mpf_cmp(lhs2,rhs2),cmp2;
                if      (!strcmp(op3,"<>")||!strcmp(op3,"><")) cmp2=(c2!=0);
                else if (!strcmp(op3,"<=")||!strcmp(op3,"=<")) cmp2=(c2<=0);
                else if (!strcmp(op3,">=")||!strcmp(op3,"=>")) cmp2=(c2>=0);
                else if (op3[0]=='<') cmp2=(c2<0);
                else if (op3[0]=='>') cmp2=(c2>0);
                else cmp2=(c2==0);
                mpf_clears(lhs2,rhs2,NULL);
                cmp = is_and ? (cmp && cmp2) : (cmp || cmp2);
            }
        }
        closeparen:
        p = sk(p); if (*p==')') p=sk(p+1);
        *pp = p;
        return cmp;
    }

    if (is_str_token(p)) {
        char lhs[1024], rhs[1024];
        p = sk(eval_str_or_inkey(p, lhs, sizeof lhs));
        char op2[3] = {p[0],p[1],'\0'}; int oplen=2;
        if (!strcmp(op2,"<>")||!strcmp(op2,"><")||!strcmp(op2,"<=")||
            !strcmp(op2,"=<")||!strcmp(op2,">=")||!strcmp(op2,"=>")) ;
        else { op2[1]='\0'; oplen=1; }
        p = sk(eval_str_or_inkey(sk(p+oplen), rhs, sizeof rhs));
        int c = strcmp(lhs, rhs);
        if      (!strcmp(op2,"<>")||!strcmp(op2,"><")) cmp=(c!=0);
        else if (!strcmp(op2,"<=")||!strcmp(op2,"=<")) cmp=(c<=0);
        else if (!strcmp(op2,">=")||!strcmp(op2,"=>")) cmp=(c>=0);
        else if (op2[0]=='<') cmp=(c<0);
        else if (op2[0]=='>') cmp=(c>0);
        else                  cmp=(c==0);
    } else {
        mpf_t lhs; mpf_init2(lhs,g_prec);
        p = sk(eval_expr(p, lhs));
        /* check for comparison operator */
        char op2[3] = {p[0],p[1],'\0'}; int oplen=2;
        if (!strcmp(op2,"<>")||!strcmp(op2,"><")||!strcmp(op2,"<=")||
            !strcmp(op2,"=<")||!strcmp(op2,">=")||!strcmp(op2,"=>")) ;
        else if (p[0]=='<'||p[0]=='>'||p[0]=='=') { op2[1]='\0'; oplen=1; }
        else { /* bare numeric condition: non-zero = true */
            cmp = (mpf_sgn(lhs) != 0);
            mpf_clear(lhs);
            *pp = p;
            return cmp;
        }
        mpf_t rhs; mpf_init2(rhs,g_prec);
        p = sk(eval_expr(sk(p+oplen), rhs));
        int c = mpf_cmp(lhs,rhs);
        if      (!strcmp(op2,"<>")||!strcmp(op2,"><")) cmp=(c!=0);
        else if (!strcmp(op2,"<=")||!strcmp(op2,"=<")) cmp=(c<=0);
        else if (!strcmp(op2,">=")||!strcmp(op2,"=>")) cmp=(c>=0);
        else if (op2[0]=='<') cmp=(c<0);
        else if (op2[0]=='>') cmp=(c>0);
        else                  cmp=(c==0);
        mpf_clears(lhs,rhs,NULL);
    }
    *pp = p;
    return cmp;
}

/* find_else: scan p for ELSE outside quotes, return pointer to it or NULL */
static const char *find_else(const char *p) {
    int in_str = 0;
    while (*p) {
        if (*p == '"') { in_str = !in_str; p++; continue; }
        if (!in_str && kw_match(p, "ELSE")) return p;
        p++;
    }
    return NULL;
}

/* IF cond [AND|OR cond ...] THEN stmt[:stmt] [ELSE stmt[:stmt]]
 *
 * The 'args' pointer here comes from dispatch_one which has already
 * stripped the leading "IF" — but because IF consumes the entire
 * remaining line (including colon-separated trailing statements that
 * belong to the THEN or ELSE branch), we mark a jump so the caller
 * does NOT also execute the next colon segment.  We achieve this by
 * returning 0 normally (no pc jump) but having consumed everything.
 *
 * The trick: split_statements already split the line, so 'args' only
 * contains up to the first unquoted colon.  To get the full line we
 * use the original g_lines[ip->pc].text.  We re-parse from after "IF".
 */
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
    if (kw_match(p,"THEN")) p = sk(p+4);

    /* Find ELSE boundary in the full remaining text */
    const char *else_p = find_else(p);

    if (result) {
        char then_clause[MAX_LINE_LEN];
        if (else_p) {
            size_t len = (size_t)(else_p - p);
            if (len >= MAX_LINE_LEN) len = MAX_LINE_LEN - 1;
            memcpy(then_clause, p, len); then_clause[len] = '\0';
            p = then_clause;
        }
        if (isdigit((unsigned char)*p)) return cmd_goto(ip, p);
        /* dispatch the THEN clause; only advance pc if dispatch didn't jump */
        int jumped = dispatch_multi(ip, p);
        if (!jumped) ip->pc++;
        return 1;
    } else {
        /* advance past this line regardless */
        ip->pc++;
        if (!else_p) return 1;
        p = sk(else_p + 4);
        if (isdigit((unsigned char)*p)) return cmd_goto(ip, p);
        dispatch_multi(ip, p);
        return 1;
    }
}

/* INKEY$ — used in assignments: CMD$ = INKEY$ */
/* Handled in var assignment path — see dispatch */

/* ================================================================
 * Multi-statement colon support handled in dispatch
 * ================================================================ */

/* ================================================================
 * Command registration table — ADD NEW COMMANDS HERE
 * ================================================================ */
#define COMMAND(kw,fn) { kw, fn }

/* ----------------------------------------------------------------
 * DATA / READ / RESTORE support
 * ---------------------------------------------------------------- */
#define MAX_DATA_ITEMS 4096
static char *g_data[MAX_DATA_ITEMS];
static int   g_data_count = 0;
static int   g_data_pos   = 0;

/* Called at load time to pre-scan all DATA statements */
static void prescan_data(void) {
    for (int li = 0; li < g_nlines; li++) {
        const char *p = sk(g_lines[li].text);
        if (!kw_match(p, "DATA")) continue;
        p = sk(p + 4);
        while (*p) {
            /* skip leading whitespace */
            p = sk(p);
            char item[512]; int i = 0;
            if (*p == '"') {
                p++; /* quoted string */
                while (*p && *p != '"' && i < (int)sizeof(item)-1) item[i++] = *p++;
                if (*p == '"') p++;
            } else {
                while (*p && *p != ',' && i < (int)sizeof(item)-1) item[i++] = *p++;
                /* trim trailing whitespace */
                while (i > 0 && item[i-1] == ' ') i--;
            }
            item[i] = '\0';
            if (g_data_count < MAX_DATA_ITEMS)
                g_data[g_data_count++] = str_dup(item);
            p = sk(p);
            if (*p == ',') p++;
        }
    }
}

static int cmd_data(Interp *ip, const char *args) {
    (void)ip; (void)args; return 0; /* handled at prescan */
}

static int cmd_restore(Interp *ip, const char *args) {
    (void)ip; (void)args; g_data_pos = 0; return 0;
}

static int cmd_read(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
    while (*p) {
        char name[MAX_VARNAME];
        p = sk(read_varname(sk(p), name));
        /* parse optional subscript */
        int arr_i = 0, arr_j = 1, is_arr = 0;
        if (*p == '(') {
            is_arr = 1; p = sk(p+1);
            mpf_t i1; mpf_init2(i1,g_prec);
            p = sk(eval_expr(p, i1)); arr_i = (int)mpf_get_si(i1); mpf_clear(i1);
            if (*p==',') { p=sk(p+1); mpf_t i2; mpf_init2(i2,g_prec); p=sk(eval_expr(p,i2)); arr_j=(int)mpf_get_si(i2); mpf_clear(i2); }
            if (*p==')') p=sk(p+1);
        }
        if (g_data_pos >= g_data_count) {
            fprintf(stderr, "READ: out of data\n"); exit(1);
        }
        const char *item = g_data[g_data_pos++];
        Var *v = var_get(name);
        if (var_is_str_name(name)) {
            if (is_arr && v->kind == VAR_ARRAY_STR) {
                char **slot = arr_str_elem(v, arr_i, arr_j);
                free(*slot); *slot = str_dup(item);
            } else {
                free(v->str); v->str = str_dup(item);
            }
        } else {
            if (is_arr) {
                mpf_set_d(*arr_num_elem(v, arr_i, arr_j), atof(item));
            } else {
                mpf_set_d(v->num, atof(item));
            }
        }
        p = sk(p);
        if (*p == ',') p = sk(p+1); else break;
    }
    return 0;
}

static int cmd_def(Interp *ip, const char *args) {
    (void)ip;
    const char *p = sk(args);
    /* DEF SEG handled separately; DEF FNx(param) = expr */
    if (!(toupper((unsigned char)p[0])=='F' && toupper((unsigned char)p[1])=='N'
          && isalnum((unsigned char)p[2]))) return 0;
    p += 2; /* skip "FN", leave rest for name reading below */
    if (g_defn_count >= MAX_DEF_FN) return 0;
    DefFn *fn = &g_defn[g_defn_count++];
    /* read function name: FNA, FNB, etc. */
    int i = 0;
    fn->name[i++] = 'F'; fn->name[i++] = 'N';
    while (isalnum((unsigned char)*p) && i < MAX_VARNAME-1) fn->name[i++] = (char)toupper(*p++);
    fn->name[i] = '\0';
    p = sk(p);
    /* parameter name */
    fn->param[0] = '\0';
    if (*p == '(') {
        p = sk(p+1);
        int j = 0;
        while (*p && *p != ')' && j < MAX_VARNAME-1) fn->param[j++] = (char)toupper(*p++);
        fn->param[j] = '\0';
        if (*p == ')') p++;
    }
    p = sk(p);
    if (*p == '=') p = sk(p+1);
    strncpy(fn->body, p, MAX_LINE_LEN-1);
    return 0;
}

/* Evaluate a user-defined function FNx(arg) — called from parse_primary_p */
/* ----------------------------------------------------------------
 * ON x GOTO / ON x GOSUB
 * ---------------------------------------------------------------- */
static int cmd_on(Interp *ip, const char *args) {
    const char *p = sk(args);
    mpf_t val; mpf_init2(val, g_prec);
    p = sk(eval_expr(p, val));
    int idx = (int)mpf_get_d(val); /* 1-based */
    mpf_clear(val);
    int is_gosub = 0;
    if (kw_match(p, "GOSUB")) { is_gosub = 1; p = sk(p+5); }
    else if (kw_match(p, "GOTO")) { p = sk(p+4); }
    else return 0;
    /* collect comma-separated line numbers */
    int targets[64]; int nt = 0;
    while (*p && nt < 64) {
        p = sk(p);
        if (!isdigit((unsigned char)*p)) break;
        targets[nt++] = atoi(p);
        while (isdigit((unsigned char)*p)) p++;
        p = sk(p);
        if (*p == ',') p++;
    }
    if (idx < 1 || idx > nt) return 0; /* out of range: fall through */
    char num[32]; snprintf(num, sizeof num, "%d", targets[idx-1]);
    if (is_gosub) return cmd_gosub(ip, num);
    return cmd_goto(ip, num);
}

/* ----------------------------------------------------------------
 * DEFINT / DEFSNG / DEFDBL / DEFSTR — type declaration stubs
 * ---------------------------------------------------------------- */
static int cmd_defint(Interp *ip, const char *args) { (void)ip;(void)args; return 0; }

static const Command commands[] = {
    COMMAND("REM",        cmd_rem),
    COMMAND("'",          cmd_rem),
    COMMAND("END",        cmd_end),
    COMMAND("STOP",       cmd_end),
    COMMAND("LET",        cmd_let),
    COMMAND("PRINT",      cmd_print),
    COMMAND("CLS",        cmd_cls),
    COMMAND("COLOR",      cmd_color),
    COMMAND("LOCATE",     cmd_locate),
    COMMAND("WIDTH",      cmd_width),
    COMMAND("SCREEN",     cmd_screen),
    COMMAND("KEY",        cmd_key),
    COMMAND("DIM",        cmd_dim),
    COMMAND("FOR",        cmd_for),
    COMMAND("NEXT",       cmd_next),
    COMMAND("GOTO",       cmd_goto),
    COMMAND("GOSUB",      cmd_gosub),
    COMMAND("RETURN",     cmd_return),
    COMMAND("IF",         cmd_if),
    COMMAND("LINE INPUT", cmd_line_input),
    COMMAND("INPUT",      cmd_input),
    COMMAND("DEF SEG",    cmd_defseg),
    COMMAND("DEF",        cmd_def),
    COMMAND("DEFDBL",     cmd_defdbl),
    COMMAND("DEFINT",     cmd_defint),
    COMMAND("DEFSNG",     cmd_defint),
    COMMAND("DEFSTR",     cmd_defint),
    COMMAND("ON",         cmd_on),
    COMMAND("READ",       cmd_read),
    COMMAND("DATA",       cmd_data),
    COMMAND("RESTORE",    cmd_restore),
    COMMAND("CHAIN",      cmd_chain),
    { NULL, NULL }
};

/* ================================================================
 * split_statements: split a line on unquoted colons into segments.
 * Returns number of segments; fills segs[] with pointers into a
 * copy of line (caller must free the copy).
 * ================================================================ */
#define MAX_STMTS 32
static int split_statements(const char *line, char *segs[], char **buf_out) {
    char *buf = str_dup(line);
    *buf_out = buf;
    int n = 0;
    int in_str = 0;
    segs[n++] = buf;

    /* if line starts with REM or ', don't split — it's all a comment */
    const char *trimmed = buf;
    while (isspace((unsigned char)*trimmed)) trimmed++;
    if (strncasecmp(trimmed, "REM", 3) == 0 &&
        !isalnum((unsigned char)trimmed[3]) && trimmed[3] != '_')
        return n;
    if (*trimmed == '\'') return n;

    for (char *p = buf; *p; p++) {
        if (*p == '"') in_str = !in_str;
        if (!in_str && *p == ':') {
            /* check if what follows is a REM — if so, stop splitting */
            char *rest = p + 1;
            while (isspace((unsigned char)*rest)) rest++;
            if (strncasecmp(rest, "REM", 3) == 0 &&
                !isalnum((unsigned char)rest[3]) && rest[3] != '_') {
                *p = '\0';
                break;
            }
            /* check if what follows is an IF — stop splitting here so
             * the entire IF/THEN/ELSE clause (including any trailing
             * colon-separated statements in THEN or ELSE) is handed to
             * cmd_if as one unsplit unit. */
            if (strncasecmp(rest, "IF", 2) == 0 &&
                !isalnum((unsigned char)rest[2]) && rest[2] != '_') {
                *p = '\0';
                if (n < MAX_STMTS) segs[n++] = rest;
                break;
            }
            *p = '\0';
            if (n < MAX_STMTS) segs[n++] = p + 1;
        }
    }
    return n;
}

/* dispatch_one: dispatch exactly one statement (no colons).
 * For IF statements, 'full_line' gives the complete unsplit line
 * so THEN/ELSE can consume trailing colon-segments. */
static int dispatch_one(Interp *ip, const char *stmt, const char *full_line) {
    const char *p = sk(stmt);
    if (!*p) return 0;

    for (int i = 0; commands[i].keyword; i++) {
        const char *kw = commands[i].keyword;
        size_t len = strlen(kw);
        if (strncasecmp(p, kw, len) == 0) {
            char next = p[len];
            if (!isalnum((unsigned char)next) && next != '_' && next != '$') {
                /* For IF: pass the full unsplit line so ELSE works */
                if (strcasecmp(kw, "IF") == 0 && full_line) {
                    const char *fl = sk(full_line);
                    if (strncasecmp(fl, "IF", 2) == 0)
                        fl = sk(fl + 2);
                    return commands[i].fn(ip, fl);
                }
                return commands[i].fn(ip, sk(p + len));
            }
        }
    }

    /* Bare assignment: var = expr  or  arr(i) = expr */
    if (isalpha((unsigned char)*p)) {
        char name[MAX_VARNAME];
        const char *after = read_varname(p, name);
        after = sk(after);
        if (*after == '=' || *after == '(')
            return cmd_let(ip, p);
    }

    fprintf(stderr, "Warning: unknown: %.60s\n", p);
    return 0;
}

/* ================================================================
 * Dispatcher — each line is already a single statement (loader split them).
 * ================================================================ */
static int dispatch(Interp *ip, const char *line) {
    return dispatch_one(ip, line, line);
}

/* dispatch_multi: used by cmd_if for THEN/ELSE clauses which may
 * contain colon-separated statements inline. */
static int dispatch_multi(Interp *ip, const char *clause) {
    char *segs[MAX_STMTS];
    char *buf;
    int n = split_statements(clause, segs, &buf);
    int jumped = 0;
    for (int i = 0; i < n && !jumped; i++)
        jumped = dispatch_one(ip, segs[i], segs[i]);
    free(buf);
    return jumped;
}

/* ================================================================
 * INKEY$ in expression context:
 * We handle CMD$ = INKEY$ by patching the string eval path.
 * eval_str_primary already handles arbitrary string vars;
 * we add INKEY$ as a special token there via a post-process hook.
 *
 * Actually, INKEY$ appears on the RHS of string assignments and in
 * IF comparisons.  We detect it in eval_str_primary:
 * ================================================================ */
/* (Already handled below in eval_str_primary — see the INKEY$ block added there) */

/* We need to reopen eval_str_primary to add INKEY$ — done via a second pass.
   Since C doesn't allow reopening functions, we extend parse_primary to catch
   INKEY$ as a numeric-returning 0/char approach, but it's a string.
   Cleanest fix: handle in eval_str_primary directly. */

/* ================================================================
 * Patch: INKEY$ in string primary
 * We add it here as a post-init override by wrapping the original
 * eval_str_expr. Because C is single-pass, we use a flag instead. ↓
 *
 * Simpler approach: eval_str_primary already has a "string variable"
 * fallthrough, so we add INKEY$ before that block.
 * But eval_str_primary is already defined above. We'll use a shim:
 * the INKEY$ keyword is intercepted in cmd_let when parsing the RHS.
 *
 * Actually the cleanest fix: add a global hook called from inside
 * eval_str_primary. We achieve this with a simple strcmp check inserted
 * via the compiler — see INKEY$ handling inside eval_str_primary via the
 * read_varname path: if name == "INKEY$" we call display_inkey().
 *
 * That path IS already there via "String variable" fallthrough, but
 * INKEY$ isn't stored in g_vars — we need special case.
 * Since we can't go back, we handle it in cmd_let's RHS detection.
 * ================================================================ */

/* Override: before cmd_let calls eval_str_expr, check for INKEY$ */
/* We patch this by re-examining in the dispatcher — if the RHS token
   is INKEY$ we call display_inkey() directly. This is done in a thin
   wrapper. Because cmd_let is already defined, we instead add the check
   inside dispatch() for lines of form  VAR$ = INKEY$ . */

/* ================================================================
 * Main interpreter loop
 * ================================================================ */
static void run(void) {
    Interp ip = { .pc=0, .running=1 };
    while (ip.running && ip.pc<g_nlines) {
        int old_pc=ip.pc;
        int jumped=dispatch(&ip, g_lines[ip.pc].text);
        if (!jumped) ip.pc=old_pc+1;
    }
}

/* ================================================================
 * Loader — splits colon-separated statements into individual entries.
 * Each sub-statement gets the same line number as its parent line;
 * GOTO/GOSUB still work because find_line_idx returns the first entry
 * with that number, and the pc advances sequentially through the rest.
 *
 * Splitting rules:
 *   - Don't split inside quoted strings
 *   - If the statement (trimmed) starts with IF, don't split it further
 *     (IF owns its THEN/ELSE clauses including any colons inside them)
 *   - Stop splitting at REM or '
 * ================================================================ */
static void load(const char *filename) {
    FILE *f=fopen(filename,"r");
    if (!f) { perror(filename); exit(1); }
    char buf[MAX_LINE_LEN];
    while (fgets(buf,sizeof buf,f)) {
        buf[strcspn(buf,"\r\n")]='\0';
        char *p=buf;
        while (isspace((unsigned char)*p)) p++;
        if (!*p||!isdigit((unsigned char)*p)) continue;
        int num=(int)strtol(p,&p,10);
        while (isspace((unsigned char)*p)) p++;

        /* Check if the whole line starts with IF — don't split */
        const char *trimmed = p;
        int starts_with_if = (strncasecmp(trimmed,"IF",2)==0 &&
                              !isalnum((unsigned char)trimmed[2]) &&
                              trimmed[2]!='_');

        if (starts_with_if) {
            /* Store whole line unsplit */
            if (g_nlines>=MAX_LINES) { fprintf(stderr,"Too many lines\n"); exit(1); }
            g_lines[g_nlines].linenum=num;
            strncpy(g_lines[g_nlines].text,p,MAX_LINE_LEN-1);
            g_nlines++;
        } else {
            /* Split on unquoted colons, but stop at REM/' and don't split IF sub-stmts */
            char *seg = p;
            int in_str = 0;
            char *c;
            for (c = p; *c; c++) {
                if (*c == '"') { in_str = !in_str; continue; }
                if (in_str) continue;
                if (*c == ':') {
                    /* terminate this segment */
                    *c = '\0';
                    /* store segment */
                    if (g_nlines>=MAX_LINES) { fprintf(stderr,"Too many lines\n"); exit(1); }
                    g_lines[g_nlines].linenum=num;
                    strncpy(g_lines[g_nlines].text,seg,MAX_LINE_LEN-1);
                    g_nlines++;
                    /* advance to next segment */
                    seg = c+1;
                    while (isspace((unsigned char)*seg)) seg++;
                    /* if next segment is REM or ' or IF, stop splitting */
                    int is_rem = (strncasecmp(seg,"REM",3)==0 &&
                                  !isalnum((unsigned char)seg[3]) && seg[3]!='_');
                    int is_apos = (*seg == '\'');
                    int is_if  = (strncasecmp(seg,"IF",2)==0 &&
                                  !isalnum((unsigned char)seg[2]) && seg[2]!='_');
                    if (is_rem || is_apos || is_if) {
                        /* store rest as one segment and stop */
                        if (g_nlines>=MAX_LINES) { fprintf(stderr,"Too many lines\n"); exit(1); }
                        g_lines[g_nlines].linenum=num;
                        strncpy(g_lines[g_nlines].text,seg,MAX_LINE_LEN-1);
                        g_nlines++;
                        seg = NULL;
                        break;
                    }
                    c = seg - 1; /* resume scan from new seg */
                }
            }
            /* store final segment */
            if (seg && *seg) {
                if (g_nlines>=MAX_LINES) { fprintf(stderr,"Too many lines\n"); exit(1); }
                g_lines[g_nlines].linenum=num;
                strncpy(g_lines[g_nlines].text,seg,MAX_LINE_LEN-1);
                g_nlines++;
            }
        }
    }
    fclose(f);
    qsort(g_lines,g_nlines,sizeof(Line),line_cmp);
}

/* ================================================================
 * clear_program: reset all program state for a fresh load
 * ================================================================ */
static void clear_program(void) {
    g_nlines = 0;
    g_nvar = 0;
    g_ctrl_top = 0;
    g_data_count = 0;
    g_data_pos = 0;
    g_defn_count = 0;
}

/* ================================================================
 * save_program: write loaded lines back out as a .bas file
 * ================================================================ */
static void save_program(const char *filename) {
    /* add .bas extension if missing */
    char path[512];
    if (strchr(filename, '.'))
        snprintf(path, sizeof path, "%s", filename);
    else
        snprintf(path, sizeof path, "%s.bas", filename);

    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }
    int prev = -1;
    for (int i = 0; i < g_nlines; i++) {
        if (g_lines[i].linenum != prev) {
            if (i > 0) fprintf(f, "\n");
            fprintf(f, "%d %s", g_lines[i].linenum, g_lines[i].text);
            prev = g_lines[i].linenum;
        } else {
            fprintf(f, ": %s", g_lines[i].text);
        }
    }
    if (g_nlines > 0) fprintf(f, "\n");
    fclose(f);
    printf("Saved %s\n", path);
}

/* ================================================================
 * load_program: clear state and load a .bas file
 * ================================================================ */
static void load_program(const char *filename) {
    char path[512];
    if (strchr(filename, '.'))
        snprintf(path, sizeof path, "%s", filename);
    else
        snprintf(path, sizeof path, "%s.bas", filename);

    clear_program();
    load(path);
    prescan_data();
    printf("Loaded %s (%d lines)\n", path, g_nlines);
}

/* ================================================================
 * Entry point
 * ================================================================ */
int main(int argc, char **argv) {
    g_prec = DEFAULT_PREC;
    mpf_set_default_prec(g_prec);
    srand((unsigned)time(NULL));
    display_init();

    if (argc >= 2) {
        /* Direct run mode: ./basic program.bas */
        if (argc >= 3) g_prec = (mp_bitcnt_t)atoi(argv[2]);
        load(argv[1]);
        prescan_data();
        run();
        display_shutdown();
        return 0;
    }

    /* Interactive REPL mode */
    display_cls();
    display_print("BASIC Interpreter\n");
    display_print("Commands: LOAD \"file\", SAVE \"file\", RUN, FILES, EXIT\n\n");

    char line[512];
    for (;;) {
        display_print("Ok\n");
        display_cursor(1);
        display_getline(line, sizeof line);
        display_newline();

        /* trim */
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!*p) continue;

        if (strncasecmp(p, "EXIT", 4) == 0 || strncasecmp(p, "QUIT", 4) == 0 ||
            strncasecmp(p, "BYE",  3) == 0) {
            break;

        } else if (strncasecmp(p, "FILES", 5) == 0) {
            system("ls -1 *.bas 2>/dev/null || echo '(no .bas files found)'");

        } else if (strncasecmp(p, "LOAD", 4) == 0) {
            p = sk(p + 4);
            if (*p == '"') p++;
            char name[256]; int i = 0;
            while (*p && *p != '"' && !isspace((unsigned char)*p) && i < 255)
                name[i++] = *p++;
            name[i] = '\0';
            if (!*name) { display_print("Usage: LOAD \"filename\"\n"); continue; }
            load_program(name);

        } else if (strncasecmp(p, "SAVE", 4) == 0) {
            p = sk(p + 4);
            if (*p == '"') p++;
            char name[256]; int i = 0;
            while (*p && *p != '"' && !isspace((unsigned char)*p) && i < 255)
                name[i++] = *p++;
            name[i] = '\0';
            if (!*name) { display_print("Usage: SAVE \"filename\"\n"); continue; }
            save_program(name);

        } else if (strncasecmp(p, "RUN", 3) == 0) {
            if (g_nlines == 0) { display_print("No program loaded.\n"); continue; }
            /* reset runtime state but keep program */
            g_nvar = 0;
            g_ctrl_top = 0;
            g_data_pos = 0;
            run();

        } else {
            display_print("Unknown command. Try: LOAD \"file\", RUN, FILES, SAVE \"file\", EXIT\n");
        }
    }

    display_shutdown();
    return 0;
}
