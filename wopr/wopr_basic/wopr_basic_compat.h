#pragma once
/*
 * wopr_basic_compat.h — Maps the flat C names that wopr_basic.cpp uses
 * to the namespaced / renamed symbols produced by the WOPR BASIC build.
 *
 * Include this in wopr_basic.cpp (and any other host file that references
 * the interpreter directly) instead of reaching into the interpreter headers.
 *
 * WOPR interpreter symbols live in namespace WoprBasic::
 * C-linkage bridge symbols use the wopr_* prefix (defined in main.cpp).
 */

#include "basic_ns.h"   /* for WoprBasic namespace and BASIC_BREAK_SYM etc. */

/* ----------------------------------------------------------------
 * g_break — the interpreter's break flag.
 * Defined as extern "C" wopr_g_break in main.cpp.
 * ---------------------------------------------------------------- */
#include <signal.h>
extern "C" volatile sig_atomic_t wopr_g_break;
static volatile sig_atomic_t &g_break = wopr_g_break;

/* ----------------------------------------------------------------
 * g_autoload_path — path to auto-load on start.
 * Defined as extern "C" wopr_autoload_path[512] in main.cpp.
 * ---------------------------------------------------------------- */
extern "C" char wopr_autoload_path[512];
static char (&g_autoload_path)[512] = wopr_autoload_path;

/* ----------------------------------------------------------------
 * sound_init / sound_shutdown — bring WoprBasic:: versions into scope.
 * ---------------------------------------------------------------- */
using WoprBasic::sound_init;
using WoprBasic::sound_shutdown;
