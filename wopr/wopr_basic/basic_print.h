#ifndef BASIC_SUPP_H
#define BASIC_SUPP_H

#include <setjmp.h>
#include <SDL2/SDL.h>

#include "basic_ns.h"

BASIC_NS_BEGIN

/* ============================================================================
 * Shared I/O surfaces (Felix BASIC shim)
 * ========================================================================== */


/* Provided by the Felix/WOPR overlay */
void wopr_basic_push_line(char *line);
void wopr_basic_signal_done(void);
void wopr_basic_flush_partial(void);
void wopr_basic_cls(void);
void wopr_basic_color(int fg);

/* Single-key (INKEY$) buffer — fed by wopr_basic_post_key, read by display_inkey */
void wopr_basic_post_key(char c);
int  wopr_basic_get_key(void);   /* returns -1 if empty, else char */

/* Input state */
extern char     basic_input_buf[512];
extern int      basic_input_ready;
extern int      g_basic_game_over;
extern int      g_basic_waiting_input;
extern int      g_basic_suppress_newline;
extern SDL_sem *basic_input_sem;

/* Longjmp target set in basic_thread_fn before calling basic_main() */
extern jmp_buf  basic_exit_jmp;

/* Shim initialization */
void basic_shim_init(void);

/* Push a line of input into the BASIC interpreter */
void basic_shim_set_input(char *line);

/* Blocking fgets replacement used by BASIC */
char *basic_shim_fgets(char *buf, int n);

/* ============================================================================
 * more_output (line-buffered output)
 * ========================================================================== */

void basic_more_init(void);
void basic_more_output(char *fmt, ...);
void basic_more_input(void);

#if defined(WOPR) || defined(FELIX_BASIC)
int basic_main(void);
#endif

/* ============================================================================
 * Lifecycle
 * ========================================================================== */

/* Exit BASIC immediately (longjmp back to thread wrapper) */
void basic_exit_(void);

/* BASIC-compatible time function */
void basic_itime_(int *hrptr, int *minptr, int *secptr);

/* BASIC-compatible random function */
int basic_rnd_(int maxval);

/* BASIC printf override (line-buffered) */
int basic_printf(char *fmt, ...);

int basic_stderr(char *fmt, ...);


/* BASIC fgets override (calls basic_shim_fgets) */
char *basic_fgets(char *buf, int size, FILE *fp);


BASIC_NS_END

#endif /* BASIC_SUPP_H */

