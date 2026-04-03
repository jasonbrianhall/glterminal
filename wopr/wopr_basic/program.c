/*
 * program.c — Program store: line store, loader, saver, keyword normalizer,
 *             DATA prescan, and clear_program.
 */
#include "basic.h"

/* ================================================================
 * Global state
 * ================================================================ */
Line g_lines[MAX_LINES];
int  g_nlines = 0;

char *g_data[MAX_DATA_ITEMS];
int   g_data_count = 0;
int   g_data_pos   = 0;

CtrlFrame g_ctrl[CTRL_STACK_MAX];
int       g_ctrl_top = 0;

/* ================================================================
 * Label table — maps free-form label names to pseudo-line-numbers.
 * Populated by load() when processing unnumbered source files.
 * ================================================================ */
#define MAX_LABELS 512

typedef struct {
    char name[MAX_VARNAME];
    int  linenum;
} Label;

static Label g_labels[MAX_LABELS];
static int   g_nlabels = 0;

static void label_clear(void) { g_nlabels = 0; }

static void label_add(const char *name, int linenum) {
    if (g_nlabels >= MAX_LABELS) { fprintf(stderr, "Too many labels\n"); return; }
    strncpy(g_labels[g_nlabels].name, name, MAX_VARNAME - 1);
    g_labels[g_nlabels].name[MAX_VARNAME - 1] = '\0';
    g_labels[g_nlabels].linenum = linenum;
    g_nlabels++;
}

/* Resolve a label name → line index, or -1 if not found. */
int find_line_by_label(const char *name) {
    for (int i = 0; i < g_nlabels; i++)
        if (strcasecmp(g_labels[i].name, name) == 0)
            return find_line_idx(g_labels[i].linenum);
    return -1;
}

/* ================================================================
 * Program store helpers
 * ================================================================ */
int line_cmp(const void *a, const void *b) {
    return ((Line *)a)->linenum - ((Line *)b)->linenum;
}

int find_line_idx(int num) {
    for (int i = 0; i < g_nlines; i++)
        if (g_lines[i].linenum == num) return i;
    return -1;
}

/* ================================================================
 * Keyword normalizer — insert spaces after keywords that appear at a
 * word boundary and are immediately followed by an identifier char,
 * e.g. "FORI=1TON" → "FOR I=1 TO N", "NEXTI" → "NEXT I".
 * ================================================================ */
void normalize_kw(const char *src, char *dst, int dstsz) {
    static const char *kws[] = {
        "PRINT","INPUT","GOTO","GOSUB","RETURN","THEN","ELSE",
        "FOR","NEXT","WHILE","WEND","STEP","TO","LET","DIM","DATA",
        "READ","RESTORE","STOP","REM","ON","DEF",
        NULL
    };
    int di = 0;
    const char *s = src;
    int in_str = 0, prev_alnum = 0;
    while (*s && di < dstsz - 1) {
        if (*s == '"') {
            in_str = !in_str;
            dst[di++] = *s++;
            prev_alnum = 0;
            continue;
        }
        if (in_str) { dst[di++] = *s++; continue; }

        int matched = 0;
        if (!prev_alnum) {
            for (int k = 0; kws[k]; k++) {
                int len = (int)strlen(kws[k]);
                if (strncasecmp(s, kws[k], len) == 0) {
                    char after = s[len];
                    /* The character immediately after the keyword must be
                     * something that starts a new token (alnum/$), AND
                     * the character at s[len-1] must be the last char of
                     * the keyword — meaning s[len] continuing the same
                     * identifier would make this a longer word that is NOT
                     * the keyword itself.  Check: read the full identifier
                     * at s; if its length != len, this is a longer word
                     * like "TOP" matching "TO" — don't split. */
                    if (isalnum((unsigned char)after) || after == '$') {
                        /* measure the full identifier length at s */
                        int idlen = 0;
                        while (isalnum((unsigned char)s[idlen]) || s[idlen] == '_') idlen++;
                        if (idlen != len) continue;   /* longer word — skip */
                        for (int i = 0; i < len && di < dstsz - 2; i++)
                            dst[di++] = s[i];
                        dst[di++] = ' ';
                        s += len;
                        prev_alnum = 0;
                        matched = 1;
                        break;
                    }
                }
            }
        }
        if (!matched) {
            char c = *s++;
            dst[di++] = c;
            prev_alnum = isalnum((unsigned char)c) || c == '_' || c == '$';
        }
    }
    dst[di] = '\0';
}

/* ================================================================
 * is_label_def — returns 1 if the trimmed line is purely a label
 * definition of the form "Identifier:" (nothing after the colon).
 * ================================================================ */
static int is_label_def(const char *p, char *name_out) {
    const char *start = p;
    if (!isalpha((unsigned char)*p) && *p != '_') return 0;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    if (*p != ':') return 0;
    p++;
    /* nothing (or only whitespace/comment) may follow */
    while (isspace((unsigned char)*p)) p++;
    if (*p && *p != '\'') return 0;   /* junk after colon — not a bare label */
    int len = (int)(p - start) - 1;   /* exclude the colon */
    if (len <= 0 || len >= MAX_VARNAME) return 0;
    memcpy(name_out, start, len);
    name_out[len] = '\0';
    return 1;
}

/* ================================================================
 * load — read a .bas file into g_lines[].
 *
 * Two source formats are supported and auto-detected:
 *
 *  Numbered  : every non-blank, non-comment line starts with a digit.
 *              Loaded exactly as before; sorted by line number at end.
 *
 *  Free-form : QBasic/QuickBASIC style — no line numbers.
 *              Pseudo-line-numbers are assigned starting at 1.
 *              Lines of the form "LabelName:" (nothing else on the
 *              line) register the name in the label table pointing at
 *              the *next* real statement; they are not stored in
 *              g_lines themselves.
 *              The array is NOT sorted (file order is execution order).
 * ================================================================ */
void load(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) { perror(filename); exit(1); }

    label_clear();

    /* ---- detect format by scanning for the first code line ---- */
    int numbered = 0;
    char buf[MAX_LINE_LEN];
    while (fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        const char *p = buf;
        while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '\'' ) continue;   /* blank or comment */
        numbered = isdigit((unsigned char)*p);
        break;
    }
    rewind(f);

    /* ---- NUMBERED path ---- */
    if (numbered) {
        while (fgets(buf, sizeof buf, f)) {
            buf[strcspn(buf, "\r\n")] = '\0';
            char *p = buf;
            while (isspace((unsigned char)*p)) p++;
            if (!*p || !isdigit((unsigned char)*p)) continue;

            int num = (int)strtol(p, &p, 10);
            while (isspace((unsigned char)*p)) p++;

            char normbuf[MAX_LINE_LEN];
            normalize_kw(p, normbuf, sizeof normbuf);
            p = normbuf;

            const char *trimmed = p;
            int starts_with_if = (strncasecmp(trimmed, "IF", 2) == 0 &&
                                  !isalnum((unsigned char)trimmed[2]) &&
                                  trimmed[2] != '_');

            #define STORE_SEG(lnum, sp) do { \
                if (g_nlines >= MAX_LINES) { fprintf(stderr, "Too many lines\n"); exit(1); } \
                g_lines[g_nlines].linenum = (lnum); \
                const char *_sp = (sp); \
                while (isspace((unsigned char)*_sp)) _sp++; \
                if (*_sp == '?') { \
                    snprintf(g_lines[g_nlines].text, MAX_LINE_LEN, "PRINT %s", _sp + 1); \
                } else { \
                    strncpy(g_lines[g_nlines].text, _sp, MAX_LINE_LEN - 1); \
                    g_lines[g_nlines].text[MAX_LINE_LEN - 1] = '\0'; \
                } \
                g_nlines++; \
            } while(0)

            if (starts_with_if) {
                STORE_SEG(num, p);
            } else {
                char *seg = p;
                int in_str = 0;
                char *c;
                for (c = p; *c; c++) {
                    if (*c == '"') { in_str = !in_str; continue; }
                    if (in_str) continue;
                    if (*c == ':') {
                        *c = '\0';
                        STORE_SEG(num, seg);
                        seg = c + 1;
                        while (isspace((unsigned char)*seg)) seg++;
                        int is_rem = (strncasecmp(seg, "REM", 3) == 0 &&
                                      !isalnum((unsigned char)seg[3]) && seg[3] != '_');
                        int is_apos = (*seg == '\'');
                        int is_if   = (strncasecmp(seg, "IF", 2) == 0 &&
                                       !isalnum((unsigned char)seg[2]) && seg[2] != '_');
                        if (is_rem || is_apos || is_if) {
                            STORE_SEG(num, seg);
                            seg = NULL;
                            break;
                        }
                        c = seg - 1;
                    }
                }
                if (seg && *seg) STORE_SEG(num, seg);
            }
            #undef STORE_SEG
        }
        fclose(f);
        qsort(g_lines, g_nlines, sizeof(Line), line_cmp);
        return;
    }

    /* ---- FREE-FORM path ---- */
    /* Pseudo-line numbers: each source line that produces at least one
       statement gets the next integer.  Labels point at their successor. */
    int pseudo = 0;
    int inside_type = 0;   /* skip TYPE...END TYPE blocks */
    char pending_buf[MAX_VARNAME * 8];
    memset(pending_buf, 0, sizeof pending_buf);
    int pending_count = 0;

    while (fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        char *p = buf;
        while (isspace((unsigned char)*p)) p++;

        /* skip blank lines and full-line comments */
        if (!*p || *p == '\'') continue;

        /* skip QBasic meta-lines: '$DYNAMIC, '$STATIC, DECLARE … */
        if (*p == '\'') continue;   /* already caught above */
        if (*p == '$')  continue;
        if (strncasecmp(p, "DECLARE ", 8) == 0) continue;

        /* TYPE...END TYPE blocks: skip entirely (fields become variables at runtime) */
        if (strncasecmp(p, "END TYPE", 8) == 0) { inside_type = 0; continue; }
        if (strncasecmp(p, "TYPE ",    5) == 0) { inside_type = 1; continue; }
        if (inside_type) continue;

        /* bare label definition? */
        char lname[MAX_VARNAME];
        if (is_label_def(p, lname)) {
            /* stash — will be registered when the next real stmt appears */
            if (pending_count < 8) {
                strncpy(pending_buf + pending_count * MAX_VARNAME,
                        lname, MAX_VARNAME - 1);
                pending_count++;
            }
            continue;
        }

        /* normalise keyword spacing */
        char normbuf[MAX_LINE_LEN];
        normalize_kw(p, normbuf, sizeof normbuf);
        p = normbuf;

        /* Helper: store one statement and assign/register its pseudo-number. */
        #define STORE_FREE(sp) do { \
            if (g_nlines >= MAX_LINES) { fprintf(stderr, "Too many lines\n"); exit(1); } \
            pseudo++; \
            /* flush any pending labels to point here */ \
            for (int _li = 0; _li < pending_count; _li++) \
                label_add(pending_buf + _li * MAX_VARNAME, pseudo); \
            pending_count = 0; \
            g_lines[g_nlines].linenum = pseudo; \
            const char *_sp = (sp); \
            while (isspace((unsigned char)*_sp)) _sp++; \
            if (*_sp == '?') { \
                snprintf(g_lines[g_nlines].text, MAX_LINE_LEN, "PRINT %s", _sp + 1); \
            } else { \
                strncpy(g_lines[g_nlines].text, _sp, MAX_LINE_LEN - 1); \
                g_lines[g_nlines].text[MAX_LINE_LEN - 1] = '\0'; \
            } \
            g_nlines++; \
        } while(0)

        /* IF lines are never colon-split */
        int starts_with_if = (strncasecmp(p, "IF", 2) == 0 &&
                              !isalnum((unsigned char)p[2]) && p[2] != '_');
        if (starts_with_if) {
            STORE_FREE(p);
        } else {
            /* split on unquoted colons, same logic as numbered path */
            char *seg = p;
            int in_str = 0;
            char *c;
            for (c = p; *c; c++) {
                if (*c == '"') { in_str = !in_str; continue; }
                if (in_str) continue;
                if (*c == ':') {
                    /* peek ahead: is the colon immediately followed by a
                       label-continuation (alpha + eventual ':')? 
                       No — just treat it as a statement separator. */
                    *c = '\0';
                    if (*seg && !(strncasecmp(seg,"REM",3)==0 &&
                                  !isalnum((unsigned char)seg[3])) &&
                                 *seg != '\'') {
                        STORE_FREE(seg);
                    }
                    seg = c + 1;
                    while (isspace((unsigned char)*seg)) seg++;
                    int is_rem  = (strncasecmp(seg,"REM",3)==0 &&
                                   !isalnum((unsigned char)seg[3]) && seg[3]!='_');
                    int is_apos = (*seg == '\'');
                    int is_if   = (strncasecmp(seg,"IF",2)==0 &&
                                   !isalnum((unsigned char)seg[2]) && seg[2]!='_');
                    if (is_rem || is_apos || is_if) {
                        if (is_if) STORE_FREE(seg);
                        seg = NULL;
                        break;
                    }
                    c = seg - 1;
                }
            }
            if (seg && *seg) STORE_FREE(seg);
        }
        #undef STORE_FREE
    }
    fclose(f);
    /* Free-form: do NOT sort — insertion order is execution order. */
}

/* ================================================================
 * prescan_data — walk all DATA lines and build g_data[]
 * ================================================================ */
void prescan_data(void) {
    for (int li = 0; li < g_nlines; li++) {
        const char *p = sk(g_lines[li].text);
        if (!kw_match(p, "DATA")) continue;
        p = sk(p + 4);
        while (*p) {
            p = sk(p);
            char item[512]; int i = 0;
            if (*p == '"') {
                p++;
                while (*p && *p != '"' && i < (int)sizeof(item) - 1) item[i++] = *p++;
                if (*p == '"') p++;
            } else {
                while (*p && *p != ',' && i < (int)sizeof(item) - 1) item[i++] = *p++;
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

/* ================================================================
 * clear_program — reset all program and interpreter state
 * ================================================================ */
void clear_program(void) {
    g_nlines      = 0;
    g_nvar        = 0;
    g_ctrl_top    = 0;
    g_data_count  = 0;
    g_data_pos    = 0;
    g_defn_count  = 0;
    g_option_base = 0;
    g_cont_pc     = -1;
    label_clear();
    const_clear();
    for (int i = 1; i <= MAX_FILE_HANDLES; i++)
        if (g_files[i].fp) { fclose(g_files[i].fp); g_files[i].fp = NULL; g_files[i].mode = 0; }
}

/* ================================================================
 * save_program — write g_lines[] back out as a numbered .bas file
 * ================================================================ */
void save_program(const char *filename) {
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
 * load_program — clear state and load a .bas file
 * ================================================================ */
void load_program(const char *filename) {
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
