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

#include "basic_ns.h"

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

BASIC_NS_BEGIN

/* ================================================================
 * Configuration constants
 * ================================================================ */
#define DEFAULT_PREC     128
#define DEFAULT_BUFFER  4096
#define PRINT_DIGITS      60
#define MAX_VARS         512
#define MAX_VARNAME       64
#define MAX_LINES       8192
#define MAX_LINE_LEN     512
#define CTRL_STACK_MAX    16384
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
/* g_break: in hosted builds it is a macro (defined in basic_ns.h) that
 * expands to ::BASIC_BREAK_SYM.  In standalone builds it is a normal extern. */
#if !defined(WOPR) && !defined(FELIX_BASIC)
extern volatile sig_atomic_t g_break;
#endif
extern int              g_cont_pc;
extern char             g_error_handler[MAX_VARNAME];
extern int              g_error_resume_pc;
extern int              g_err;   /* last error code (ERR) */
extern int              g_erl;   /* line number of last error (ERL) */
extern int              g_tron;  /* TRON trace flag */

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

int     var_is_str_name(char *name);
Var    *var_find(char *name);
Var    *var_create(char *name);
Var    *var_get(char *name);
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
int  find_line_by_label(char *name);
void normalize_kw(char *src, char *dst, int dstsz);
void load(char *filename);
void save_program(char *filename);
void load_program(char *filename);
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
 * TYPE / struct definitions
 *
 * TYPE PlayerData
 *   PNam AS STRING * 17
 *   XCoor AS DOUBLE
 * END TYPE
 *
 * We store each TYPE definition as a list of field names (in order).
 * At runtime, TYPE variables are "flat": PDat(1).PNam is stored as
 * the variable "PDAT.1.PNAM" (for array element) or "PDAT.PNAM"
 * (for scalar).  This avoids a full struct runtime.
 * ================================================================ */
#define MAX_TYPE_DEFS   32
#define MAX_TYPE_FIELDS 64

typedef struct {
    char name[MAX_VARNAME];
    int  is_str;   /* 1 = string field, 0 = numeric */
} TypeField;

typedef struct {
    char      name[MAX_VARNAME];
    TypeField fields[MAX_TYPE_FIELDS];
    int       nfields;
} TypeDef;

extern TypeDef g_typedefs[MAX_TYPE_DEFS];
extern int     g_ntypedefs;

TypeDef *typedef_find(char *name);

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
char        *str_dup(char *s);
char  *sk(char *p);
char  *read_varname(char *p, char *name);
int          kw_match(char *p, char *kw);
int          is_str_token(char *p);

/* ================================================================
 * CONST table (expr.c)
 * ================================================================ */
void const_clear(void);
void const_set(char *name, char *value, int is_str);
char *eval_expr(char *s, mpf_t result);
char *eval_str_expr(char *s, char *buf, int bufsz);
char *eval_str_or_inkey(char *p, char *buf, int bufsz);

/* ================================================================
 * Command dispatch (commands.c)
 * ================================================================ */
typedef int (*CmdFn)(Interp *ip, char *args);
typedef struct { char *keyword; CmdFn fn; } Command;

extern const Command commands[];

int dispatch_one(Interp *ip, char *stmt, char *full_line);
int dispatch(Interp *ip, char *line);
int dispatch_multi(Interp *ip, char *clause);

/* Individual command handlers needed by other modules */
int cmd_goto(Interp *ip, char *args);
int cmd_gosub(Interp *ip, char *args);

/* ================================================================
 * Main interpreter loop (main.c)
 * ================================================================ */
void run(void);
void run_from(int start_pc);

#if defined(WOPR) || defined(FELIX_BASIC)
int basic_main(void);
#else
int basic_main(int argc, char **argv);
#endif

BASIC_NS_END
