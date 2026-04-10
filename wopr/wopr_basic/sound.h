#pragma once

#include "basic_ns.h"

BASIC_NS_BEGIN
/*
 * sound.h — Audio backend interface for the BASIC interpreter.
 *
 * Two implementations exist:
 *   sound_sdl.c  — real audio via SDL2 (square-wave synth + MML PLAY)
 *   sound_null.c — silent stubs used when SDL2 is not available
 *
 * commands.c calls only the functions declared here; it never touches
 * SDL or any platform audio API directly.
 */

/* Initialise audio subsystem.  Called once from display_init() path.
 * Safe to call even if no audio hardware is present. */
void sound_init(void);

/* Shut down audio subsystem.  Called once at exit. */
void sound_shutdown(void);

/* Block until the note queue is fully drained (all queued SOUND/PLAY
 * events have finished playing).  Called automatically by sound_shutdown,
 * but also callable directly when you need to wait before exit. */
void sound_drain(void);

/* BEEP — 800 Hz for ~0.25 s (same as IBM PC BASIC). */
void sound_beep(void);

/* SOUND freq, duration
 *   freq     : frequency in Hz (37–32767)
 *   duration : clock ticks at 18.2 ticks/sec (matches GW-BASIC) */
void sound_tone(double freq, double duration_ticks);

/* Stop all audio immediately, clearing the queue without waiting. */
void sound_stop(void);

/* PLAY "mml-string"
 * Implements the GW-BASIC Music Macro Language subset:
 *   Oc        set octave 0-6
 *   A-G[#+-]  play note (sharp=#or+, flat=-)
 *   Ln        set default duration (1=whole … 64=sixty-fourth)
 *   Tn        set tempo in BPM (32-255, default 120)
 *   Pn        rest for duration n (or current L if n omitted)
 *   .         dot current note/rest (×1.5)
 *   MN/ML/MS  normal / legato / staccato articulation
 *   MF/MB     foreground (blocking) / background (non-blocking)
 *   > <       octave up / octave down
 *   Xvar$;    execute string variable (passed in as pre-expanded string)
 */
void sound_play(const char *mml);

BASIC_NS_END
