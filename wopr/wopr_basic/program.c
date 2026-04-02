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
                    if (isalnum((unsigned char)after) || after == '$') {
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
 * load — read a .bas file into g_lines[], splitting colon-separated
 * statements into separate entries (same line number).
 * ================================================================ */
void load(const char *filename) {
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
