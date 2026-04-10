#pragma once
/*
 * wopr_basic_compat.h — Maps the flat C names that wopr_basic.cpp uses
 * to the namespaced / renamed symbols produced by the WOPR BASIC build.
 */

#include "basic_ns.h"
#include <signal.h>

/* g_break is already handled by the macro in basic_ns.h:
 *   #define g_break  ::wopr_g_break
 * We just need to extern-declare the underlying symbol. */
extern volatile sig_atomic_t wopr_g_break;

/* g_autoload_path — defined in main.cpp as wopr_autoload_path[512].
 * Declared here as a reference so wopr_basic.cpp can use the plain name. */
extern char wopr_autoload_path[512];
static char (&g_autoload_path)[512] = wopr_autoload_path;

/* sound_init / sound_shutdown — bring WoprBasic:: versions into scope. */
using WoprBasic::sound_init;
using WoprBasic::sound_shutdown;
