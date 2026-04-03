#include <string.h>
#include <setjmp.h>
#include "basic.h"

// These must match your wrapper's externs
char    basic_input_buf[1024];
int     basic_input_ready = 0;
int     g_basic_game_over = 0;
jmp_buf basic_exit_jmp;

// Called once when BASIC session starts
void basic_shim_init(void)
{
    basic_input_buf[0] = '\0';
    basic_input_ready  = 0;
    g_basic_game_over  = 0;
}

// Called by WOPR wrapper when user presses Enter
void basic_shim_set_input(const char *line)
{
    strncpy(basic_input_buf, line, sizeof(basic_input_buf) - 1);
    basic_input_buf[sizeof(basic_input_buf) - 1] = '\0';
    basic_input_ready = 1;
}

