#pragma once
/*
 * basic_ns.h — Namespace selector for the BASIC interpreter.
 *
 * The felix_basic build passes BOTH -DWOPR and -DFELIX_BASIC, so we
 * must test FELIX_BASIC first.
 *
 *   -DFELIX_BASIC            → namespace FelixBasic
 *   -DWOPR (without FELIX)   → namespace WoprBasic
 *   (standalone)             → namespace StandaloneBasic
 *
 * C-linkage bridge symbols for hosted builds:
 *   BASIC_BREAK_SYM    — unique name for the g_break C-linkage global
 *   BASIC_AUTOLOAD_SYM — unique name for the g_autoload_path C-linkage global
 *   BASIC_MAIN_CSYM    — unique name for the basic_main C-linkage function
 *
 * These use names that are NOT the same token as g_break / g_autoload_path
 * to avoid circular macro expansion.
 */

#if defined(FELIX_BASIC)
#  define BASIC_NS              FelixBasic
#  define BASIC_MAIN_CSYM       fb_basic_main
#  define BASIC_AUTOLOAD_SYM    fb_autoload_path
#  define BASIC_BREAK_SYM       fb_g_break
#elif defined(WOPR)
#  define BASIC_NS              WoprBasic
#  define BASIC_MAIN_CSYM       basic_main
#  define BASIC_AUTOLOAD_SYM    wopr_autoload_path
#  define BASIC_BREAK_SYM       wopr_g_break
#else
#  define BASIC_NS              StandaloneBasic
#endif

/* In hosted builds, all internal uses of g_break resolve to the
 * C-linkage global via ::BASIC_BREAK_SYM.  No circular expansion
 * because BASIC_BREAK_SYM tokens (fb_g_break / wopr_g_break) are
 * never themselves macros. */
#if defined(FELIX_BASIC) || defined(WOPR)
#  define g_break  ::BASIC_BREAK_SYM
#endif

#define BASIC_NS_BEGIN   namespace BASIC_NS {
#define BASIC_NS_END     } /* namespace BASIC_NS */
