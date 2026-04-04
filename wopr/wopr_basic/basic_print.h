#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern int g_wopr_active;      // You toggle this when entering/exiting WOPR mode
void display_print(const char *s);
void display_newline(void);

int basic_printf(const char *fmt, ...);
