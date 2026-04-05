/*
 * vars.c — Variable store: scalar and array, numeric (mpf) and string.
 */
#include "basic.h"

#include "basic_print.h"
#define printf(...) basic_printf(__VA_ARGS__)


/* ================================================================
 * Global state
 * ================================================================ */
Var  g_vars[MAX_VARS];
int  g_nvar = 0;

FileHandle g_files[MAX_FILE_HANDLES + 1];  /* 1-based */

/* ================================================================
 * File handle
 * ================================================================ */
FileHandle *fh_get(int n) {
    if (n < 1 || n > MAX_FILE_HANDLES) {
        basic_stderr("Bad file number: %d\n", n); exit(1);
    }
    return &g_files[n];
}

/* ================================================================
 * Variable helpers
 * ================================================================ */
int var_is_str_name(const char *name) {
    return name[strlen(name) - 1] == '$';
}

Var *var_find(const char *name) {
    for (int i = 0; i < g_nvar; i++)
        if (strcasecmp(g_vars[i].name, name) == 0) return &g_vars[i];
    return NULL;
}

Var *var_create(const char *name) {
    if (g_nvar >= MAX_VARS) { basic_stderr("Too many variables\n"); exit(1); }
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

Var *var_get(const char *name) {
    Var *v = var_find(name);
    return v ? v : var_create(name);
}

/* ================================================================
 * Array element access (1-based or option-base-based indices)
 * ================================================================ */
mpf_t *arr_num_elem(Var *v, int i, int j) {
    int oi  = i - g_option_base;
    int oj  = j - g_option_base;
    int idx = (v->ndim == 2) ? (oi * v->dim[1] + oj) : oi;
    int total = v->dim[0] * (v->ndim == 2 ? v->dim[1] : 1);
    if (idx < 0 || idx >= total) {
        basic_stderr("Array out of bounds: index %d (size %d) -- clamping\n", idx, total);
        idx = (idx < 0) ? 0 : total - 1;
    }
    return &v->arr_num[idx];
}

char **arr_str_elem(Var *v, int i, int j) {
    int oi  = i - g_option_base;
    int oj  = j - g_option_base;
    int idx = (v->ndim == 2) ? (oi * v->dim[1] + oj) : oi;
    int total = v->dim[0] * (v->ndim == 2 ? v->dim[1] : 1);
    if (idx < 0 || idx >= total) {
        basic_stderr("Array out of bounds: index %d (size %d) -- clamping\n", idx, total);
        idx = (idx < 0) ? 0 : total - 1;
    }
    return &v->arr_str[idx];
}
