/*
 * type.c — User-defined TYPE / END TYPE support.
 *
 * TYPE blocks are pre-scanned from the program store at RUN time (like DATA).
 * Field access uses variable-name mangling so the existing Var store needs no
 * changes:
 *
 *   DIM PDat(5) AS PlayerData
 *   PDat(2).PNam$ = "Alice"
 *   X = PDat(2).XCoor
 *
 * is stored internally as variables "__PDAT_2__PNAM$" and "__PDAT_2__XCOOR".
 *
 * Scalar struct instances (DIM P AS PlayerData) use index -1:
 *   "__P__XCOOR"
 */
#include "basic.h"
#include "basic_print.h"
#define printf(...) basic_printf(__VA_ARGS__)

TypeDef g_typedefs[MAX_TYPE_DEFS];
int     g_ntypedefs = 0;

TypeDef *type_find(const char *name) {
    for (int i = 0; i < g_ntypedefs; i++)
        if (strcasecmp(g_typedefs[i].name, name) == 0) return &g_typedefs[i];
    return NULL;
}

/* Pre-scan all TYPE ... END TYPE blocks in the program store. */
void prescan_types(void) {
    g_ntypedefs = 0;
    for (int i = 0; i < g_nlines; i++) {
        const char *t = sk(g_lines[i].text);
        if (!kw_match(t, "TYPE")) continue;
        t = sk(t + 4);
        /* read type name */
        if (g_ntypedefs >= MAX_TYPE_DEFS) break;
        TypeDef *td = &g_typedefs[g_ntypedefs++];
        td->nfields = 0;
        int ni = 0;
        while ((isalnum((unsigned char)*t) || *t == '_') && ni < MAX_TYPE_NAMELEN - 1)
            td->name[ni++] = (char)toupper((unsigned char)*t++);
        td->name[ni] = '\0';
        /* scan subsequent lines for fields until END TYPE */
        for (int j = i + 1; j < g_nlines; j++) {
            const char *fl = sk(g_lines[j].text);
            if (kw_match(fl, "END")) {
                const char *rest = sk(fl + 3);
                if (kw_match(rest, "TYPE")) break;
            }
            /* Field line: fieldname AS typename  or  fieldname AS STRING * n */
            if (!isalpha((unsigned char)*fl)) continue;
            if (td->nfields >= MAX_TYPE_FIELDS) continue;
            TypeField *f = &td->fields[td->nfields++];
            int fi = 0;
            while ((isalnum((unsigned char)*fl) || *fl == '_') && fi < MAX_TYPE_NAMELEN - 1)
                f->name[fi++] = (char)toupper((unsigned char)*fl++);
            /* preserve $ sigil if present */
            if (*fl == '$' && fi < MAX_TYPE_NAMELEN - 1) { f->name[fi++] = '$'; fl++; }
            f->name[fi] = '\0';
            fl = sk(fl);
            /* determine if string: look for AS STRING or field name ending in $ */
            f->is_str = (f->name[fi - 1] == '$');
            if (kw_match(fl, "AS")) {
                fl = sk(fl + 2);
                if (kw_match(fl, "STRING")) f->is_str = 1;
            }
        }
    }
}

/* Build the mangled variable name for base(idx).field or base.field (idx<0). */
void type_mangle(const char *base, int idx, const char *field, char *out, int outsz) {
    if (idx >= 0)
        snprintf(out, outsz, "__%s_%d__%s", base, idx, field);
    else
        snprintf(out, outsz, "__%s__%s", base, field);
    /* uppercase the whole thing */
    for (int i = 0; out[i]; i++) out[i] = (char)toupper((unsigned char)out[i]);
}
