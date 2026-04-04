#include <stdarg.h>
#include <stdio.h>

int basic_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    int r = vprintf(fmt, ap);

    va_end(ap);
    return r;
}

