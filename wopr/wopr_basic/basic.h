#pragma once
/*
 * basic.h — Shared types, constants, and extern declarations for the
 *            BASIC interpreter.  Every .c file in this project includes
 *            this header and nothing else (beyond standard library headers).
 *
 * Build:
 *   gcc -O2 -o basic main.c vars.c expr.c program.c commands.c display_ansi.c \
 *           -lgmp -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <gmp.h>

#include "display.h"
#include "sound.h"

/* ================================================================
 * Configuration constants
 * ================================================================ */
#define DEFAULT_PREC     512
#define PRINT_DIGITS      60
#define MAX_VARS         512
#define MAX_VARNAME       64
#define MAX_LINES       8192
#define MAX_LINE_LEN     512
#define CTRL_STACK_MAX    64
#define MAX_ARRAY_DIMS     2
#define MAX_ARRAY_SIZE  4096
#define MAX_FILE_HANDLES  16
#define MAX_DEF_FN        32
#define MAX_DATA_ITEMS  4096
#define MAX_STMTS         32

/* ================================================================
 * Global interpreter settings
 * ================================================================ */
extern mp_bitcnt_t      g_prec;
extern int              g_option_base;
extern volatile sig_atomic_t g_break;
extern int              g_cont_pc;

/* ================================================================
 * File handle table
 * ================================================================ */
typedef struct {
    FILE *fp;
    char  mode;   /* 'I'=input, 'O'=output, 'A'=append, 0=closed */
} FileHandle;

extern FileHandle g_files[MAX_FILE_HANDLES + 1];  /* 1-based */

FileHandle *fh_get(int n);

/* ================================================================
 * Variable store
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

extern Var  g_vars[MAX_VARS];
extern int  g_nvar;

int     var_is_str_name(const char *name);
Var    *var_find(const char *name);
Var    *var_create(const char *name);
Var    *var_get(const char *name);
mpf_t  *arr_num_elem(Var *v, int i, int j);
char  **arr_str_elem(Var *v, int i, int j);

/* ================================================================
 * Program store
 * ================================================================ */
typedef struct {
    int  linenum;
    char text[MAX_LINE_LEN];
} Line;

extern Line g_lines[MAX_LINES];
extern int  g_nlines;

int  line_cmp(const void *a, const void *b);
int  find_line_idx(int num);
int  find_line_by_label(const char *name);
void normalize_kw(const char *src, char *dst, int dstsz);
void load(const char *filename);
void save_program(const char *filename);
void load_program(const char *filename);
void clear_program(void);

/* ================================================================
 * DATA / READ / RESTORE
 * ================================================================ */
extern char *g_data[MAX_DATA_ITEMS];
extern int   g_data_count;
extern int   g_data_pos;

void prescan_data(void);

/* ================================================================
 * Control stack — FOR loops and GOSUB frames
 * ================================================================ */
typedef struct {
    char  varname[MAX_VARNAME];  /* "\x01GOSUB" for subroutine frames */
    mpf_t limit, step;
    int   line_idx;              /* FOR: loop start; GOSUB: return address */
} CtrlFrame;

extern CtrlFrame g_ctrl[CTRL_STACK_MAX];
extern int       g_ctrl_top;

/* ================================================================
 * DEF FN store
 * ================================================================ */
typedef struct {
    char name[MAX_VARNAME];
    char param[MAX_VARNAME];
    char body[MAX_LINE_LEN];
} DefFn;

extern DefFn g_defn[MAX_DEF_FN];
extern int   g_defn_count;

/* ================================================================
 * Interpreter state
 * ================================================================ */
typedef struct {
    int pc;
    int running;
} Interp;

/* ================================================================
 * Utility helpers (defined in expr.c, used everywhere)
 * ================================================================ */
char        *str_dup(const char *s);
const char  *sk(const char *p);
const char  *read_varname(const char *p, char *name);
int          kw_match(const char *p, const char *kw);
int          is_str_token(const char *p);

/* ================================================================
 * CONST table (expr.c)
 * ================================================================ */
void const_clear(void);
void const_set(const char *name, const char *value, int is_str);
const char *eval_expr(const char *s, mpf_t result);
const char *eval_str_expr(const char *s, char *buf, int bufsz);
const char *eval_str_or_inkey(const char *p, char *buf, int bufsz);

/* ================================================================
 * Command dispatch (commands.c)
 * ================================================================ */
typedef int (*CmdFn)(Interp *ip, const char *args);
typedef struct { const char *keyword; CmdFn fn; } Command;

extern const Command commands[];

int dispatch_one(Interp *ip, const char *stmt, const char *full_line);
int dispatch(Interp *ip, const char *line);
int dispatch_multi(Interp *ip, const char *clause);

/* Individual command handlers needed by other modules */
int cmd_goto(Interp *ip, const char *args);
int cmd_gosub(Interp *ip, const char *args);

/* ================================================================
 * TYPE / END TYPE — user-defined struct types
 * ================================================================ */
#define MAX_TYPE_DEFS    32
#define MAX_TYPE_FIELDS  64
#define MAX_TYPE_NAMELEN 64

typedef struct {
    char name[MAX_TYPE_NAMELEN];
    int  is_str;    /* 1 = string field, 0 = numeric */
} TypeField;

typedef struct {
    char      name[MAX_TYPE_NAMELEN];
    TypeField fields[MAX_TYPE_FIELDS];
    int       nfields;
} TypeDef;

extern TypeDef g_typedefs[MAX_TYPE_DEFS];
extern int     g_ntypedefs;

TypeDef *type_find(const char *name);
void     prescan_types(void);

/* Mangle "varbase(idx).field" into a flat variable name.
 * idx < 0 means scalar (no subscript).  Result stored in out (size >= MAX_VARNAME). */
void type_mangle(const char *base, int idx, const char *field, char *out, int outsz);

/* ================================================================
 * Main interpreter loop (main.c)
 * ================================================================ */
void run(void);
void run_from(int start_pc);
