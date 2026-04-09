/*
 * expr.c — Utility helpers, string/numeric expression evaluators, DEF FN.
 */
#include "basic.h"
#include <time.h>

#include "basic_print.h"
#define printf(...) basic_printf(__VA_ARGS__)

/* ================================================================
 * Global state
 * ================================================================ */
mp_bitcnt_t g_prec       = DEFAULT_PREC;
int         g_option_base = 0;

volatile sig_atomic_t g_break  = 0;
int                   g_cont_pc = -1;

/* ON ERROR GOTO handler state */
char g_error_handler[MAX_VARNAME] = "";  /* label/line of handler, "" = none */
int  g_error_resume_pc = -1;             /* pc to RESUME to */

DefFn g_defn[MAX_DEF_FN];
int   g_defn_count = 0;

/* ================================================================
 * CONST table — named constants set by the CONST command.
 * Stored as strings so they can hold both numeric and string values;
 * the expression evaluator substitutes them before eval.
 * ================================================================ */
#define MAX_CONSTS 256
typedef struct { char name[MAX_VARNAME]; char value[MAX_LINE_LEN]; int is_str; } ConstEntry;
static ConstEntry g_consts[MAX_CONSTS];
static int        g_nconsts = 0;

void const_clear(void) { g_nconsts = 0; }

void const_set(const char *name, const char *value, int is_str) {
    /* update existing */
    for (int i = 0; i < g_nconsts; i++) {
        if (strcasecmp(g_consts[i].name, name) == 0) {
            strncpy(g_consts[i].value, value, MAX_LINE_LEN - 1);
            g_consts[i].is_str = is_str;
            return;
        }
    }
    if (g_nconsts >= MAX_CONSTS) { basic_stderr("Too many CONSTs\n"); return; }
    strncpy(g_consts[g_nconsts].name,  name,  MAX_VARNAME  - 1);
    strncpy(g_consts[g_nconsts].value, value, MAX_LINE_LEN - 1);
    g_consts[g_nconsts].is_str = is_str;
    g_nconsts++;
}

static ConstEntry *const_find(const char *name) {
    for (int i = 0; i < g_nconsts; i++)
        if (strcasecmp(g_consts[i].name, name) == 0) return &g_consts[i];
    return NULL;
}

/* ================================================================
 * Utility helpers
 * ================================================================ */
char *str_dup(const char *s) {
    char *d = malloc(strlen(s) + 1);
    if (!d) { basic_stderr("OOM\n"); exit(1); }
    strcpy(d, s);
    return d;
}

const char *sk(const char *p) {
    while (isspace((unsigned char)*p)) p++;
    return p;
}

const char *read_varname(const char *p, char *name) {
    int i = 0;
    while ((isalnum((unsigned char)*p) || *p == '_') && i < MAX_VARNAME - 2)
        name[i++] = (char)toupper((unsigned char)*p++);
    if (*p == '$' || *p == '#' || *p == '!' || *p == '%' || *p == '&')
        name[i++] = *p++;
    name[i] = '\0';
    return p;
}

int kw_match(const char *p, const char *kw) {
    size_t len = strlen(kw);
    if (strncasecmp(p, kw, len) != 0) return 0;
    char next = p[len];
    return !isalnum((unsigned char)next) && next != '_' && next != '$';
}

/* ================================================================
 * String expression evaluator
 * ================================================================ */
static const char *eval_str_primary(const char *p, char *buf, int bufsz);

const char *eval_str_expr(const char *s, char *buf, int bufsz) {
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

    /* UCASE$(str$) */
    if (kw_match(p, "UCASE$")) {
        p = sk(p + 6); if (*p == '(') p++;
        char src[1024];
        p = sk(eval_str_expr(sk(p), src, sizeof src));
        if (*sk(p) == ')') p = sk(p) + 1;
        int i = 0;
        for (; src[i] && i < bufsz - 1; i++) buf[i] = (char)toupper((unsigned char)src[i]);
        buf[i] = '\0';
        return p;
    }
    /* LCASE$(str$) */
    if (kw_match(p, "LCASE$")) {
        p = sk(p + 6); if (*p == '(') p++;
        char src[1024];
        p = sk(eval_str_expr(sk(p), src, sizeof src));
        if (*sk(p) == ')') p = sk(p) + 1;
        int i = 0;
        for (; src[i] && i < bufsz - 1; i++) buf[i] = (char)tolower((unsigned char)src[i]);
        buf[i] = '\0';
        return p;
    }
    /* LTRIM$(str$) */
    if (kw_match(p, "LTRIM$")) {
        p = sk(p + 6); if (*p == '(') p++;
        char src[1024];
        p = sk(eval_str_expr(sk(p), src, sizeof src));
        if (*sk(p) == ')') p = sk(p) + 1;
        const char *s = src;
        while (*s == ' ') s++;
        strncpy(buf, s, bufsz - 1); buf[bufsz - 1] = '\0';
        return p;
    }
    /* RTRIM$(str$) */
    if (kw_match(p, "RTRIM$")) {
        p = sk(p + 6); if (*p == '(') p++;
        char src[1024];
        p = sk(eval_str_expr(sk(p), src, sizeof src));
        if (*sk(p) == ')') p = sk(p) + 1;
        strncpy(buf, src, bufsz - 1); buf[bufsz - 1] = '\0';
        int len = (int)strlen(buf);
        while (len > 0 && buf[len - 1] == ' ') buf[--len] = '\0';
        return p;
    }
    /* HEX$(n) */
    if (kw_match(p, "HEX$")) {
        p = sk(p + 4); if (*p == '(') p++;
        mpf_t n; mpf_init2(n, g_prec);
        p = eval_expr(sk(p), n);
        snprintf(buf, bufsz, "%lX", (unsigned long)mpf_get_si(n));
        mpf_clear(n);
        if (*sk(p) == ')') p = sk(p) + 1;
        return p;
    }
    /* OCT$(n) */
    if (kw_match(p, "OCT$")) {
        p = sk(p + 4); if (*p == '(') p++;
        mpf_t n; mpf_init2(n, g_prec);
        p = eval_expr(sk(p), n);
        snprintf(buf, bufsz, "%lo", (unsigned long)mpf_get_si(n));
        mpf_clear(n);
        if (*sk(p) == ')') p = sk(p) + 1;
        return p;
    }

    /* SPC(n) */
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

    /* LEFT$(str_expr, n) */
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

    /* RIGHT$(str_expr, n) */
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

    /* INKEY$ */
    if (kw_match(p, "INKEY$")) {
        int ch = display_inkey();
        buf[0] = ch ? (char)ch : '\0';
        buf[1] = '\0';
        return p + 6;
    }

    /* TIME$ */
    if (kw_match(p, "TIME$")) {
        time_t t = time(NULL); struct tm *tm = localtime(&t);
        strftime(buf, bufsz, "%H:%M:%S", tm);
        return p + 5;
    }

    /* DATE$ */
    if (kw_match(p, "DATE$")) {
        time_t t = time(NULL); struct tm *tm = localtime(&t);
        strftime(buf, bufsz, "%m-%d-%Y", tm);
        return p + 5;
    }

    /* ENV$("VAR") */
    if (kw_match(p, "ENV$")) {
        p = sk(p + 4);
        if (*p == '(') p = sk(p + 1);
        char varname[256];
        p = sk(eval_str_expr(p, varname, sizeof varname));
        if (*p == ')') p++;
        const char *val = getenv(varname);
        strncpy(buf, val ? val : "", bufsz - 1);
        buf[bufsz - 1] = '\0';
        return p;
    }

    /* String literal */
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < bufsz - 1) buf[i++] = *p++;
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
        if (*sk(p) == ')') p = sk(p) + 1;
        return p;
    }

    /* STRING$(n, c) */
    if (kw_match(p, "STRING$")) {
        p = sk(p + 7);
        if (*p == '(') p++;
        mpf_t n; mpf_init2(n, g_prec);
        p = sk(eval_expr(sk(p), n));
        int count = (int)mpf_get_si(n); mpf_clear(n);
        if (*p == ',') p = sk(p + 1);
        int ch;
        if (*p == '"') { p++; ch = (unsigned char)*p; while (*p && *p != '"') p++; if (*p == '"') p++; }
        else { mpf_t c2; mpf_init2(c2, g_prec); p = eval_expr(p, c2); ch = (int)mpf_get_si(c2); mpf_clear(c2); }
        if (count < 0) count = 0;
        if (count >= bufsz) count = bufsz - 1;
        memset(buf, ch, count); buf[count] = '\0';
        if (*sk(p) == ')') p = sk(p) + 1;
        return p;
    }

    /* MID$(str_expr, start [,len]) */
    if (kw_match(p, "MID$")) {
        p = sk(p + 4);
        if (*p == '(') p++;
        char src_buf[1024];
        p = sk(eval_str_expr(sk(p), src_buf, sizeof src_buf));
        const char *src = src_buf;
        if (*p == ',') p = sk(p + 1);
        mpf_t st; mpf_init2(st, g_prec);
        p = sk(eval_expr(sk(p), st));
        int start = (int)mpf_get_si(st) - 1; mpf_clear(st);
        int len = -1;
        if (*p == ',') {
            p = sk(p + 1);
            mpf_t ln; mpf_init2(ln, g_prec);
            p = sk(eval_expr(sk(p), ln));
            len = (int)mpf_get_si(ln); mpf_clear(ln);
        }
        if (*sk(p) == ')') p = sk(p) + 1;
        int srclen = (int)strlen(src);
        if (start < 0) start = 0;
        if (start >= srclen) { buf[0] = '\0'; return p; }
        if (len < 0 || start + len > srclen) len = srclen - start;
        if (len >= bufsz) len = bufsz - 1;
        memcpy(buf, src + start, len); buf[len] = '\0';
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
        if (*sk(p) == ')') p = sk(p) + 1;
        return p;
    }

    /* SPACE$(n) */
    if (kw_match(p, "SPACE$")) {
        p = sk(p + 6);
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

    /* INPUT$(n [,#fh]) — read n chars from keyboard or file */
    if (kw_match(p, "INPUT$")) {
        p = sk(p + 6);
        if (*p == '(') p++;
        mpf_t n; mpf_init2(n, g_prec);
        p = sk(eval_expr(sk(p), n));
        int count = (int)mpf_get_si(n); mpf_clear(n);
        FILE *src_fp = stdin;
        if (*p == ',') {
            p = sk(p + 1);
            if (*p == '#') p = sk(p + 1);
            mpf_t fh; mpf_init2(fh, g_prec);
            p = sk(eval_expr(p, fh));
            int fn = (int)mpf_get_si(fh); mpf_clear(fh);
            FileHandle *f = fh_get(fn);
            if (f->fp) src_fp = f->fp;
        }
        if (*sk(p) == ')') p = sk(p) + 1;
        if (count < 0) count = 0;
        if (count >= bufsz) count = bufsz - 1;
        int i = 0;
        while (i < count) {
            int ch = fgetc(src_fp);
            if (ch == EOF) break;
            buf[i++] = (char)ch;
        }
        buf[i] = '\0';
        return p;
    }

    /* Struct field string read: name.field$ or name(idx).field$ */
    if (isalpha((unsigned char)*p)) {
        const char *save2 = p;
        char base2[MAX_VARNAME]; int bi2 = 0;
        while ((isalnum((unsigned char)*p) || *p == '_') && bi2 < MAX_VARNAME - 1)
            base2[bi2++] = (char)toupper((unsigned char)*p++);
        base2[bi2] = '\0';
        p = sk(p);
        char idx2_str[32] = "";
        if (*p == '(') {
            p = sk(p + 1);
            mpf_t vi; mpf_init2(vi, g_prec);
            p = sk(eval_expr(p, vi));
            int id1 = (int)mpf_get_si(vi); mpf_clear(vi);
            snprintf(idx2_str, sizeof idx2_str, "%d", id1);
            if (*p == ',') {
                p = sk(p + 1);
                mpf_t vi2; mpf_init2(vi2, g_prec);
                p = sk(eval_expr(p, vi2));
                int id2 = (int)mpf_get_si(vi2); mpf_clear(vi2);
                char t2[16]; snprintf(t2, sizeof t2, ",%d", id2);
                strncat(idx2_str, t2, sizeof idx2_str - strlen(idx2_str) - 1);
            }
            if (*p == ')') p++;
            p = sk(p);
        }
        if (*p == '.') {
            p = sk(p + 1);
            char field2[MAX_VARNAME]; int fi2 = 0;
            while ((isalnum((unsigned char)*p) || *p == '_') && fi2 < MAX_VARNAME - 1)
                field2[fi2++] = (char)toupper((unsigned char)*p++);
            field2[fi2] = '\0';
            if (fi2) {
                /* Try string flat var: BASE.IDX.FIELD$ or BASE.FIELD$ */
                char flatname2[MAX_VARNAME], sname2[MAX_VARNAME];
                if (idx2_str[0])
                    snprintf(flatname2, sizeof flatname2, "%s.%s.%s", base2, idx2_str, field2);
                else
                    snprintf(flatname2, sizeof flatname2, "%s.%s", base2, field2);
                snprintf(sname2, sizeof sname2, "%s$", flatname2);
                Var *vs2 = var_find(sname2);
                if (vs2) { strncpy(buf, vs2->str ? vs2->str : "", bufsz-1); buf[bufsz-1]='\0'; return p; }
                /* numeric field in string context */
                Var *vn2 = var_find(flatname2);
                if (vn2) { snprintf(buf, bufsz, "%g", mpf_get_d(vn2->num)); return p; }
                buf[0] = '\0'; return p;
            }
        }
        p = save2; /* not a field access, fall through */
    }

    /* String variable (scalar or array element) — check CONST table first */
    if (isalpha((unsigned char)*p)) {
        char vname[MAX_VARNAME];
        const char *after = read_varname(p, vname);

        /* CONST string lookup */
        ConstEntry *ce = const_find(vname);
        if (ce && ce->is_str) {
            strncpy(buf, ce->value, bufsz - 1); buf[bufsz - 1] = '\0';
            return after;
        }

        if (var_is_str_name(vname)) {
            Var *v = var_get(vname);
            p = after;
            /* array element: name$(i) or name$(i,j) */
            if (*p == '(') {
                p = sk(p + 1);
                mpf_t i1; mpf_init2(i1, g_prec);
                p = sk(eval_expr(p, i1)); int ai = (int)mpf_get_si(i1); mpf_clear(i1);
                int aj = 1;
                if (*p == ',') { p=sk(p+1); mpf_t i2; mpf_init2(i2,g_prec); p=sk(eval_expr(p,i2)); aj=(int)mpf_get_si(i2); mpf_clear(i2); }
                if (*p == ')') p++;
                if (v->kind == VAR_ARRAY_STR) {
                    char **slot = arr_str_elem(v, ai, aj);
                    strncpy(buf, *slot ? *slot : "", bufsz - 1);
                } else {
                    strncpy(buf, v->str ? v->str : "", bufsz - 1);
                }
            } else {
                strncpy(buf, v->str ? v->str : "", bufsz - 1);
            }
            buf[bufsz - 1] = '\0';
            return p;
        }
    }

    return p;
}

int is_str_token(const char *p) {
    p = sk(p);
    if (*p == '"') return 1;
    if (kw_match(p, "INKEY$"))  return 1;
    if (kw_match(p, "TIME$"))   return 1;
    if (kw_match(p, "DATE$"))   return 1;
    if (kw_match(p, "ENV$"))    return 1;
    if (kw_match(p, "CHR$"))    return 1;
    if (kw_match(p, "STRING$")) return 1;
    if (kw_match(p, "MID$"))    return 1;
    if (kw_match(p, "STR$"))    return 1;
    if (kw_match(p, "SPC"))     return 1;
    if (kw_match(p, "SPACE$"))  return 1;
    if (kw_match(p, "TAB"))     return 1;
    if (kw_match(p, "LEFT$"))   return 1;
    if (kw_match(p, "RIGHT$"))  return 1;
    if (kw_match(p, "INPUT$"))  return 1;
    if (kw_match(p, "UCASE$"))  return 1;
    if (kw_match(p, "LCASE$"))  return 1;
    if (kw_match(p, "LTRIM$"))  return 1;
    if (kw_match(p, "RTRIM$"))  return 1;
    if (kw_match(p, "HEX$"))    return 1;
    if (kw_match(p, "OCT$"))    return 1;
    if (isalpha((unsigned char)*p)) {
        char name[MAX_VARNAME];
        const char *after = read_varname(p, name);
        if (var_is_str_name(name)) return 1;
        /* check CONST table for string constants */
        ConstEntry *ce = const_find(name);
        if (ce && ce->is_str) return 1;
        /* struct field: scan for (idx).field$ or .field$ */
        after = sk(after);
        if (*after == '(') {
            int depth = 1; after++;
            while (*after && depth > 0) {
                if (*after == '(') depth++;
                else if (*after == ')') depth--;
                after++;
            }
            after = sk(after);
        }
        if (*after == '.') {
            after = sk(after + 1);
            char field[MAX_VARNAME]; int fi = 0;
            while ((isalnum((unsigned char)*after) || *after == '_') && fi < MAX_VARNAME - 1)
                field[fi++] = (char)toupper((unsigned char)*after++);
            field[fi] = '\0';
            /* Build flat name and check if the string variant exists */
            char flatname[MAX_VARNAME], sname[MAX_VARNAME];
            snprintf(flatname, sizeof flatname, "%s.%s", name, field);
            snprintf(sname, sizeof sname, "%s$", flatname);
            if (var_find(sname)) return 1;
            /* Check typedef to see if field is a string */
            /* Walk all typedefs */
            for (int ti = 0; ti < g_ntypedefs; ti++) {
                TypeDef *td = &g_typedefs[ti];
                for (int tfi = 0; tfi < td->nfields; tfi++) {
                    if (strcasecmp(td->fields[tfi].name, field) == 0)
                        return td->fields[tfi].is_str;
                }
            }
        }
        return 0;
    }
    return 0;
}

const char *eval_str_or_inkey(const char *p, char *buf, int bufsz) {
    p = sk(p);
    if (kw_match(p, "INKEY$")) {
        int ch = display_inkey();
        buf[0] = ch ? (char)ch : '\0';
        buf[1] = '\0';
        return p + 6;
    }
    return eval_str_expr(p, buf, bufsz);
}

/* ================================================================
 * Numeric expression evaluator (recursive descent)
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
    mpf_t rhs; mpf_init2(rhs, g_prec);

    /* -----------------------------------------
     * 1. Parse left additive expression
     * ----------------------------------------- */
    parse_term_p(ps, result);
    skip_ws_p(ps);
    while (*ps->p == '+' || *ps->p == '-') {
        char op = *ps->p++;
        parse_term_p(ps, tmp);
        if (op == '+') mpf_add(result, result, tmp);
        else           mpf_sub(result, result, tmp);
        skip_ws_p(ps);
    }

    /* -----------------------------------------
     * 2. STRING comparison (must run BEFORE numeric)
     * ----------------------------------------- */
    skip_ws_p(ps);
    if (is_str_token(ps->p)) {
        char lhs[1024], rhs_s[1024];

        /* parse left string */
        ps->p = sk(eval_str_expr(ps->p, lhs, sizeof lhs));
        skip_ws_p(ps);

        /* detect operator */
        char op[3] = { ps->p[0], ps->p[1], '\0' };
        int oplen = 0;

        if (!strcmp(op,"<>") || !strcmp(op,"><") ||
            !strcmp(op,"<=") || !strcmp(op,"=<") ||
            !strcmp(op,">=") || !strcmp(op,"=>")) {
            oplen = 2;
        } else if (ps->p[0]=='<' || ps->p[0]=='>' || ps->p[0]=='=') {
            op[1] = '\0';
            oplen = 1;
        }

        if (oplen > 0) {
            ps->p += oplen;
            skip_ws_p(ps);

            /* parse right string */
            ps->p = sk(eval_str_expr(ps->p, rhs_s, sizeof rhs_s));

            int c = strcmp(lhs, rhs_s);
            int cmp;

            if (!strcmp(op,"<>") || !strcmp(op,"><"))      cmp = (c != 0);
            else if (!strcmp(op,"<=") || !strcmp(op,"=<")) cmp = (c <= 0);
            else if (!strcmp(op,">=") || !strcmp(op,"=>")) cmp = (c >= 0);
            else if (op[0]=='<')                           cmp = (c < 0);
            else if (op[0]=='>')                           cmp = (c > 0);
            else                                           cmp = (c == 0);

            mpf_set_si(result, cmp ? -1 : 0);
            skip_ws_p(ps);
        }
    }
    else {
        /* -----------------------------------------
         * 3. NUMERIC comparison
         * ----------------------------------------- */
        skip_ws_p(ps);
        char op[3] = { ps->p[0], ps->p[1], '\0' };
        int oplen = 0;

        if (!strcmp(op,"<>") || !strcmp(op,"><") ||
            !strcmp(op,"<=") || !strcmp(op,"=<") ||
            !strcmp(op,">=") || !strcmp(op,"=>")) {
            oplen = 2;
        } else if (ps->p[0]=='<' || ps->p[0]=='>' || ps->p[0]=='=') {
            op[1] = '\0';
            oplen = 1;
        }

        if (oplen > 0) {
            ps->p += oplen;
            skip_ws_p(ps);

            /* parse RHS additive */
            parse_term_p(ps, rhs);
            skip_ws_p(ps);
            while (*ps->p=='+' || *ps->p=='-') {
                char aop = *ps->p++;
                parse_term_p(ps, tmp);
                if (aop=='+') mpf_add(rhs,rhs,tmp);
                else          mpf_sub(rhs,rhs,tmp);
                skip_ws_p(ps);
            }

            int c = mpf_cmp(result, rhs);
            int cmp;

            if (!strcmp(op,"<>") || !strcmp(op,"><"))      cmp = (c != 0);
            else if (!strcmp(op,"<=") || !strcmp(op,"=<")) cmp = (c <= 0);
            else if (!strcmp(op,">=") || !strcmp(op,"=>")) cmp = (c >= 0);
            else if (op[0]=='<')                           cmp = (c < 0);
            else if (op[0]=='>')                           cmp = (c > 0);
            else                                           cmp = (c == 0);

            mpf_set_si(result, cmp ? -1 : 0);
            skip_ws_p(ps);
        }
    }

    /* -----------------------------------------
     * 4. Boolean chaining (AND / OR / XOR)
     * ----------------------------------------- */
    skip_ws_p(ps);
    while (kw_match(ps->p,"AND") || kw_match(ps->p,"OR") || kw_match(ps->p,"XOR")) {
        int is_and = kw_match(ps->p,"AND");
        int is_xor = kw_match(ps->p,"XOR");

        ps->p += is_and ? 3 : (is_xor ? 3 : 2);
        skip_ws_p(ps);

        parse_expr_p(ps, rhs);

        long lv = mpf_get_si(result);
        long rv = mpf_get_si(rhs);

        if (is_and)      mpf_set_si(result, lv & rv);
        else if (is_xor) mpf_set_si(result, lv ^ rv);
        else             mpf_set_si(result, lv | rv);

        skip_ws_p(ps);
    }

    mpf_clear(tmp);
    mpf_clear(rhs);
}

static void parse_term_p(Parser *ps, mpf_t result) {
    mpf_t tmp; mpf_init2(tmp, g_prec);
    parse_unary_p(ps, result);
    skip_ws_p(ps);
    while (*ps->p == '*' || *ps->p == '/'
           || (*ps->p == '\\')
           || kw_match(ps->p, "MOD")) {
        int is_mod  = kw_match(ps->p, "MOD");
        int is_idiv = (*ps->p == '\\');
        if (is_mod)       ps->p += 3;
        else if (is_idiv) ps->p++;
        else              { char op = *ps->p++; skip_ws_p(ps);
                            parse_unary_p(ps, tmp);
                            if (op == '*') mpf_mul(result, result, tmp);
                            else {
                                if (mpf_sgn(tmp) == 0) { basic_stderr("Division by zero\n"); exit(1); }
                                mpf_div(result, result, tmp);
                            }
                            skip_ws_p(ps); continue; }
        skip_ws_p(ps);
        parse_unary_p(ps, tmp);
        long lv = (long)mpf_get_d(result);
        long rv = (long)mpf_get_d(tmp);
        if (rv == 0) { basic_stderr("Division by zero\n"); exit(1); }
        mpf_set_si(result, is_mod ? (lv % rv) : (lv / rv));
        skip_ws_p(ps);
    }
    mpf_clear(tmp);
}

static void parse_unary_p(Parser *ps, mpf_t result) {
    skip_ws_p(ps);
    if      (*ps->p == '-')         { ps->p++; parse_power_p(ps, result); mpf_neg(result, result); }
    else if (*ps->p == '+')         { ps->p++; parse_power_p(ps, result); }
    else if (kw_match(ps->p,"NOT")) { ps->p += 3; skip_ws_p(ps);
                                      parse_unary_p(ps, result);   /* recursive: handles NOT -1 */
                                      mpf_set_si(result, ~mpf_get_si(result)); }
    else                              parse_power_p(ps, result);
}

static void parse_power_p(Parser *ps, mpf_t result) {
    parse_primary_p(ps, result);
    skip_ws_p(ps);
    if (*ps->p == '^') {
        ps->p++;
        mpf_t exp; mpf_init2(exp, g_prec);
        parse_unary_p(ps, exp);
        mpf_set_d(result, pow(mpf_get_d(result), mpf_get_d(exp)));
        mpf_clear(exp);
    }
}

/* ----------------------------------------------------------------
 * DEF FN evaluation — called from parse_primary_p
 * ---------------------------------------------------------------- */
static int try_eval_defn(Parser *ps, mpf_t result) {
    if (!(toupper((unsigned char)ps->p[0]) == 'F' &&
          toupper((unsigned char)ps->p[1]) == 'N' &&
          isalnum((unsigned char)ps->p[2]))) return 0;
    const char *start = ps->p;
    const char *p = ps->p + 2;
    char fname[MAX_VARNAME]; int i = 0;
    fname[i++] = 'F'; fname[i++] = 'N';
    while (isalnum((unsigned char)*p) && i < MAX_VARNAME - 1) fname[i++] = (char)toupper(*p++);
    fname[i] = '\0';
    DefFn *fn = NULL;
    for (int k = 0; k < g_defn_count; k++)
        if (strcasecmp(g_defn[k].name, fname) == 0) { fn = &g_defn[k]; break; }
    if (!fn) { ps->p = start; return 0; }
    ps->p = p; skip_ws_p(ps);
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
    Var *pv = NULL; double old_val = 0;
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

        while (kw_match(ps->p, "AND") || kw_match(ps->p, "OR") || kw_match(ps->p, "XOR")) {
            int is_and = kw_match(ps->p, "AND");
            int is_xor = kw_match(ps->p, "XOR");
            ps->p += is_and ? 3 : (is_xor ? 3 : 2);
            skip_ws_p(ps);
            mpf_t rhs; mpf_init2(rhs, g_prec);
            EVAL_CMP_TERM(rhs);
            long lv = mpf_get_si(result), rv = mpf_get_si(rhs);
            mpf_set_si(result, is_and ? (lv & rv) : (is_xor ? (lv ^ rv) : (lv | rv)));
            mpf_clear(rhs);
            skip_ws_p(ps);
        }
        #undef EVAL_CMP_TERM

        if (*ps->p == ')') ps->p++;
        return;
    }

    /* Hex literal &H */
    if (*ps->p == '&' && (ps->p[1] == 'H' || ps->p[1] == 'h')) {
        ps->p += 2;
        long v = strtol(ps->p, (char **)&ps->p, 16);
        mpf_set_si(result, v);
        return;
    }

    /* Numeric literal */
    if (isdigit((unsigned char)*ps->p) || *ps->p == '.') {
        char buf[64]; int i = 0;
        while (isdigit((unsigned char)*ps->p) || *ps->p == '.' || *ps->p == 'E' || *ps->p == 'e'
               || (i > 0 && (ps->p[-1] == 'E' || ps->p[-1] == 'e') && (*ps->p == '+' || *ps->p == '-')))
            buf[i++] = *ps->p++;
        if (*ps->p == '!' || *ps->p == '#' || *ps->p == '%') ps->p++;
        buf[i] = '\0';
        mpf_set_str(result, buf, 10);
        return;
    }

    /* PEEK(addr) — stub */
    if (kw_match(ps->p, "PEEK")) {
        ps->p += 4; skip_ws_p(ps); if (*ps->p == '(') ps->p++;
        mpf_t addr; mpf_init2(addr, g_prec);
        parse_expr_p(ps, addr);
        long a = mpf_get_si(addr); mpf_clear(addr);
        mpf_set_si(result, (a == 0x410) ? 0x00 : 0);
        skip_ws_p(ps); if (*ps->p == ')') ps->p++;
        return;
    }

    /* INT(x) */
    if (kw_match(ps->p, "INT")) {
        ps->p += 3; skip_ws_p(ps); if (*ps->p == '(') ps->p++;
        parse_expr_p(ps, result);
        mpf_set_d(result, floor(mpf_get_d(result)));
        skip_ws_p(ps); if (*ps->p == ')') ps->p++;
        return;
    }

    /* ABS(x) */
    if (kw_match(ps->p, "ABS")) {
        ps->p += 3; skip_ws_p(ps); if (*ps->p == '(') ps->p++;
        parse_expr_p(ps, result);
        mpf_abs(result, result);
        skip_ws_p(ps); if (*ps->p == ')') ps->p++;
        return;
    }

    /* SQR(x) */
    if (kw_match(ps->p, "SQR")) {
        ps->p += 3; skip_ws_p(ps); 
        if (*ps->p == '(') ps->p++;

        parse_expr_p(ps, result);

        double d = mpf_get_d(result);

        if (d < 0) {
            /* BASIC domain error behavior */
            mpf_set_si(result, 0);   /* or print error, or return -1 */
        } else {
            mpf_set_d(result, sqrt(d));
        }

        skip_ws_p(ps); 
        if (*ps->p == ')') ps->p++;
        return;
    }

    /* VAL(str$) */
    if (kw_match(ps->p, "VAL")) {
        ps->p += 3; skip_ws_p(ps); if (*ps->p == '(') ps->p++;
        char sbuf[256];
        ps->p = eval_str_expr(ps->p, sbuf, sizeof sbuf);
        mpf_set_d(result, atof(sbuf));
        skip_ws_p(ps); if (*ps->p == ')') ps->p++;
        return;
    }

    /* ASC(str$) */
    if (kw_match(ps->p, "ASC")) {
        ps->p += 3; skip_ws_p(ps); if (*ps->p == '(') ps->p++;
        char sbuf[256];
        ps->p = eval_str_expr(ps->p, sbuf, sizeof sbuf);
        mpf_set_si(result, (unsigned char)sbuf[0]);
        skip_ws_p(ps); if (*ps->p == ')') ps->p++;
        return;
    }

    /* LEN(str$) */
    if (kw_match(ps->p, "LEN")) {
        ps->p += 3; skip_ws_p(ps); if (*ps->p == '(') ps->p++;
        char sbuf[256];
        ps->p = eval_str_expr(ps->p, sbuf, sizeof sbuf);
        mpf_set_si(result, (long)strlen(sbuf));
        skip_ws_p(ps); if (*ps->p == ')') ps->p++;
        return;
    }

    /* EOF(n) */
    if (kw_match(ps->p, "EOF")) {
        ps->p += 3; skip_ws_p(ps);
        if (*ps->p == '(') ps->p++;
        if (*ps->p == '#') ps->p++;
        mpf_t fn; mpf_init2(fn, g_prec);
        parse_expr_p(ps, fn);
        int n = (int)mpf_get_si(fn); mpf_clear(fn);
        skip_ws_p(ps); if (*ps->p == ')') ps->p++;
        FileHandle *fh = (n >= 1 && n <= MAX_FILE_HANDLES) ? &g_files[n] : NULL;
        mpf_set_si(result, (!fh || !fh->fp || feof(fh->fp)) ? -1 : 0);
        return;
    }

    /* RND(x) */
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

    /* INSTR([start,] haystack$, needle$) — returns 1-based position or 0 */
    if (kw_match(ps->p, "INSTR")) {
        ps->p += 5; skip_ws_p(ps); if (*ps->p == '(') ps->p++;
        int start = 1;
        /* if first token is numeric, it's the start offset */
        if (!is_str_token(ps->p)) {
            const char *save = ps->p;
            mpf_t s; mpf_init2(s, g_prec);
            const char *after = eval_expr(ps->p, s);
            const char *q = after; while (isspace((unsigned char)*q)) q++;
            if (*q == ',') { start = (int)mpf_get_si(s); ps->p = (char*)(q + 1); }
            else             ps->p = save;
            mpf_clear(s);
        }
        char hay[1024], needle[256];
        ps->p = (char*)sk(eval_str_expr(ps->p, hay, sizeof hay));
        if (*ps->p == ',') ps->p++;
        ps->p = (char*)sk(eval_str_expr(sk(ps->p), needle, sizeof needle));
        skip_ws_p(ps); if (*ps->p == ')') ps->p++;
        if (start < 1) start = 1;
        if (start > (int)strlen(hay)) { mpf_set_si(result, 0); return; }
        const char *found = strstr(hay + start - 1, needle);
        mpf_set_si(result, found ? (long)(found - hay + 1) : 0);
        return;
    }

    /* SIN(x) */
    if (kw_match(ps->p, "SIN")) {
        ps->p += 3; skip_ws_p(ps); if (*ps->p == '(') ps->p++;
        parse_expr_p(ps, result);
        mpf_set_d(result, sin(mpf_get_d(result)));
        skip_ws_p(ps); if (*ps->p == ')') ps->p++;
        return;
    }
    /* COS(x) */
    if (kw_match(ps->p, "COS")) {
        ps->p += 3; skip_ws_p(ps); if (*ps->p == '(') ps->p++;
        parse_expr_p(ps, result);
        mpf_set_d(result, cos(mpf_get_d(result)));
        skip_ws_p(ps); if (*ps->p == ')') ps->p++;
        return;
    }
    /* TAN(x) */
    if (kw_match(ps->p, "TAN")) {
        ps->p += 3; skip_ws_p(ps); if (*ps->p == '(') ps->p++;
        parse_expr_p(ps, result);
        mpf_set_d(result, tan(mpf_get_d(result)));
        skip_ws_p(ps); if (*ps->p == ')') ps->p++;
        return;
    }
    /* ATN(x) */
    if (kw_match(ps->p, "ATN")) {
        ps->p += 3; skip_ws_p(ps); if (*ps->p == '(') ps->p++;
        parse_expr_p(ps, result);
        mpf_set_d(result, atan(mpf_get_d(result)));
        skip_ws_p(ps); if (*ps->p == ')') ps->p++;
        return;
    }
    /* LOG(x) */
    if (kw_match(ps->p, "LOG")) {
        ps->p += 3; skip_ws_p(ps); 
        if (*ps->p == '(') ps->p++;

        parse_expr_p(ps, result);

        double d = mpf_get_d(result);

        if (d <= 0) {
            /* BASIC domain error behavior */
            mpf_set_si(result, 0);   /* or -1, or print error */
        } else {
            mpf_set_d(result, log(d));
        }

        skip_ws_p(ps); 
        if (*ps->p == ')') ps->p++;
        return;
    }

    /* EXP(x) */
    if (kw_match(ps->p, "EXP")) {
        ps->p += 3;
        skip_ws_p(ps);
        if (*ps->p == '(') ps->p++;

        parse_expr_p(ps, result);

        double d = mpf_get_d(result);
        const double EXP_OVERFLOW_LIMIT  = 709.782712893384;
        const double EXP_UNDERFLOW_LIMIT = -745.0;

        double e;
        if (d > EXP_OVERFLOW_LIMIT) {
            e = -1.0;
        } else if (d < EXP_UNDERFLOW_LIMIT) {
            e = 0.0;
        } else {
            e = exp(d);
        }
        mpf_set_d(result, e);

        skip_ws_p(ps);
        if (*ps->p == ')') ps->p++;
        return;
    }

    /* SGN(x) */
    if (kw_match(ps->p, "SGN")) {
        ps->p += 3; skip_ws_p(ps); if (*ps->p == '(') ps->p++;
        parse_expr_p(ps, result);
        int s = mpf_sgn(result);
        mpf_set_si(result, s > 0 ? 1 : s < 0 ? -1 : 0);
        skip_ws_p(ps); if (*ps->p == ')') ps->p++;
        return;
    }
    /* FIX(x) — truncate toward zero */
    if (kw_match(ps->p, "FIX")) {
        ps->p += 3; skip_ws_p(ps); if (*ps->p == '(') ps->p++;
        parse_expr_p(ps, result);
        double d = mpf_get_d(result);
        mpf_set_d(result, d >= 0 ? floor(d) : ceil(d));
        skip_ws_p(ps); if (*ps->p == ')') ps->p++;
        return;
    }
    /* CINT(x) — round to nearest integer */
    if (kw_match(ps->p, "CINT")) {
        ps->p += 4; skip_ws_p(ps); if (*ps->p == '(') ps->p++;
        parse_expr_p(ps, result);
        mpf_set_d(result, floor(mpf_get_d(result) + 0.5));
        skip_ws_p(ps); if (*ps->p == ')') ps->p++;
        return;
    }
    /* CLNG(x) — same as CINT for our purposes */
    if (kw_match(ps->p, "CLNG")) {
        ps->p += 4; skip_ws_p(ps); if (*ps->p == '(') ps->p++;
        parse_expr_p(ps, result);
        mpf_set_d(result, floor(mpf_get_d(result) + 0.5));
        skip_ws_p(ps); if (*ps->p == ')') ps->p++;
        return;
    }
    /* CSNG / CDBL — type-cast stubs, just evaluate */
    if (kw_match(ps->p, "CSNG") || kw_match(ps->p, "CDBL")) {
        ps->p += 4; skip_ws_p(ps); if (*ps->p == '(') ps->p++;
        parse_expr_p(ps, result);
        skip_ws_p(ps); if (*ps->p == ')') ps->p++;
        return;
    }
    /* TIMER — seconds since midnight as a float */
    if (kw_match(ps->p, "TIMER")) {
        ps->p += 5;
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        struct tm *t = localtime(&ts.tv_sec);
        double secs = t->tm_hour * 3600.0 + t->tm_min * 60.0 + t->tm_sec
                      + ts.tv_nsec / 1e9;
        mpf_set_d(result, secs);
        return;
    }

    /* User-defined FNx */
    if (toupper((unsigned char)ps->p[0]) == 'F' &&
        toupper((unsigned char)ps->p[1]) == 'N' &&
        isalnum((unsigned char)ps->p[2])) {
        if (try_eval_defn(ps, result)) return;
    }

    /* Struct field read: name.field or name(idx).field
     * Flat encoding: "BASE.IDX.FIELD" (numeric) or "BASE.IDX.FIELD$" (string)
     * We detect this by peeking ahead for a dot after the name or closing paren */
    if (isalpha((unsigned char)*ps->p)) {
        const char *save = ps->p;
        char base[MAX_VARNAME]; int bi = 0;
        while ((isalnum((unsigned char)*ps->p) || *ps->p == '_') && bi < MAX_VARNAME - 1)
            base[bi++] = (char)toupper((unsigned char)*ps->p++);
        base[bi] = '\0';
        skip_ws_p(ps);

        /* optional array index */
        char idx_str[32] = "";
        if (*ps->p == '(') {
            ps->p++; skip_ws_p(ps);
            mpf_t v; mpf_init2(v, g_prec);
            parse_expr_p(ps, v);
            int idx1 = (int)mpf_get_si(v); mpf_clear(v);
            snprintf(idx_str, sizeof idx_str, "%d", idx1);
            skip_ws_p(ps);
            if (*ps->p == ',') {
                ps->p++; skip_ws_p(ps);
                mpf_t v2; mpf_init2(v2, g_prec);
                parse_expr_p(ps, v2);
                int idx2 = (int)mpf_get_si(v2); mpf_clear(v2);
                char tmp2[16]; snprintf(tmp2, sizeof tmp2, ",%d", idx2);
                strncat(idx_str, tmp2, sizeof idx_str - strlen(idx_str) - 1);
                skip_ws_p(ps);
            }
            if (*ps->p == ')') ps->p++;
            skip_ws_p(ps);
        }

        if (*ps->p == '.') {
            ps->p++; skip_ws_p(ps);
            char field[MAX_VARNAME]; int fi = 0;
            while ((isalnum((unsigned char)*ps->p) || *ps->p == '_') && fi < MAX_VARNAME - 1)
                field[fi++] = (char)toupper((unsigned char)*ps->p++);
            field[fi] = '\0';
            if (fi) {
                char flatname[MAX_VARNAME];
                if (idx_str[0])
                    snprintf(flatname, sizeof flatname, "%s.%s.%s", base, idx_str, field);
                else
                    snprintf(flatname, sizeof flatname, "%s.%s", base, field);
                /* try numeric flat var first, then string */
                Var *v = var_find(flatname);
                if (v) { mpf_set(result, v->num); return; }
                /* try string variant */
                char sname[MAX_VARNAME];
                snprintf(sname, sizeof sname, "%s$", flatname);
                Var *vs = var_find(sname);
                if (vs) { mpf_set_ui(result, 0); return; } /* string in numeric context = 0 */
                /* not found: return 0 */
                mpf_set_ui(result, 0); return;
            }
        }
        /* not a dot-field expression: restore and fall through */
        ps->p = save;
    }

    /* Variable or array element — check CONST table first */
    if (isalpha((unsigned char)*ps->p)) {
        char name[MAX_VARNAME];
        const char *name_start = ps->p;
        ps->p = read_varname(ps->p, name);
        skip_ws_p(ps);

        /* CONST lookup (numeric) — before treating as a variable */
        ConstEntry *ce = const_find(name);
        if (ce && !ce->is_str) {
            /* evaluate the stored expression (handles NOT TRUE etc.) */
            eval_expr(ce->value, result);
            return;
        }

        if (var_is_str_name(name) || strcasecmp(name, "SPC") == 0) {
            if (*ps->p == '(') {
                int depth = 1; ps->p++;
                while (*ps->p && depth > 0) {
                    if (*ps->p == '(') depth++;
                    else if (*ps->p == ')') depth--;
                    ps->p++;
                }
            }
            mpf_set_ui(result, 0);
            return;
        }
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
            if (v->kind != VAR_ARRAY_NUM) { mpf_set_ui(result, 0); return; }
            mpf_set(result, *arr_num_elem(v, i1, i2));
            return;
        }
        Var *v = var_get(name);
        mpf_set(result, v->num);
        return;
    }

    basic_stderr("Parse error near: \"%.20s\"\n", ps->p);
    exit(1);
}

const char *eval_expr(const char *s, mpf_t result) {
    Parser ps = { s };
    parse_expr_p(&ps, result);
    return ps.p;
}
