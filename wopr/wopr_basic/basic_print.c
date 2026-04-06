#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

int basic_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    /* Scan for %f-like formats */
    const char *p = fmt;
    va_list ap2;
    va_copy(ap2, ap);

    while (*p) {
        if (*p == '%') {
            p++;
            while (*p && strchr(" +-0#123456789.", *p)) p++;
            if (*p == 'f' || *p == 'F' || *p == 'g' || *p == 'G' ||
                *p == 'e' || *p == 'E') {

                double d = va_arg(ap2, double);

                if (isinf(d)) {
                    va_end(ap2);
                    va_end(ap);
                    return printf(d > 0 ? "1.#INF" : "-1.#INF");
                }
                if (isnan(d)) {
                    va_end(ap2);
                    va_end(ap);
                    return printf("1.#IND");
                }
            }
        }
        p++;
    }
    va_end(ap2);

    /* Safe path */
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}


int basic_stderr(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    int r = vfprintf(stderr, fmt, ap);

    va_end(ap);
    return r;
}

