/*
 * expr.c — Utility helpers, string/numeric expression evaluators, DEF FN.
 */
#include "basic.h"

/* ================================================================
 * Global state
 * ================================================================ */
mp_bitcnt_t g_prec       = DEFAULT_PREC;
int         g_option_base = 0;

volatile sig_atomic_t g_break  = 0;
int                   g_cont_pc = -1;

DefFn g_defn[MAX_DEF_FN];
int   g_defn_count = 0;

/* ================================================================
 * Utility helpers
 * ================================================================ */
char *str_dup(const char *s) {
    char *d = malloc(strlen(s) + 1);
    if (!d) { fprintf(stderr, "OOM\n"); exit(1); }
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
    if (*p == '$') name[i++] = *p++;
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

    /* String variable (scalar or array element) */
    if (isalpha((unsigned char)*p)) {
        char vname[MAX_VARNAME];
        const char *after = read_varname(p, vname);
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
    if (isalpha((unsigned char)*p)) {
        char name[MAX_VARNAME];
        read_varname(p, name);
        if (var_is_str_name(name)) return 1;
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
            if (mpf_sgn(tmp) == 0) { fprintf(stderr, "Division by zero\n"); exit(1); }
            mpf_div(result, result, tmp);
        }
        skip_ws_p(ps);
    }
    mpf_clear(tmp);
}

static void parse_unary_p(Parser *ps, mpf_t result) {
    skip_ws_p(ps);
    if      (*ps->p == '-') { ps->p++; parse_power_p(ps, result); mpf_neg(result, result); }
    else if (*ps->p == '+') { ps->p++; parse_power_p(ps, result); }
    else                      parse_power_p(ps, result);
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

        while (kw_match(ps->p, "AND") || kw_match(ps->p, "OR")) {
            int is_and = kw_match(ps->p, "AND");
            ps->p += is_and ? 3 : 2;
            skip_ws_p(ps);
            mpf_t rhs; mpf_init2(rhs, g_prec);
            EVAL_CMP_TERM(rhs);
            long lv = mpf_get_si(result), rv = mpf_get_si(rhs);
            mpf_set_si(result, is_and ? (lv & rv) : (lv | rv));
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
        ps->p += 3; skip_ws_p(ps); if (*ps->p == '(') ps->p++;
        parse_expr_p(ps, result);
        mpf_set_d(result, sqrt(mpf_get_d(result)));
        skip_ws_p(ps); if (*ps->p == ')') ps->p++;
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

    /* User-defined FNx */
    if (toupper((unsigned char)ps->p[0]) == 'F' &&
        toupper((unsigned char)ps->p[1]) == 'N' &&
        isalnum((unsigned char)ps->p[2])) {
        if (try_eval_defn(ps, result)) return;
    }

    /* Variable or array element */
    if (isalpha((unsigned char)*ps->p)) {
        char name[MAX_VARNAME];
        ps->p = read_varname(ps->p, name);
        skip_ws_p(ps);
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

    fprintf(stderr, "Parse error near: \"%.20s\"\n", ps->p);
    exit(1);
}

const char *eval_expr(const char *s, mpf_t result) {
    Parser ps = { s };
    parse_expr_p(&ps, result);
    return ps.p;
}
