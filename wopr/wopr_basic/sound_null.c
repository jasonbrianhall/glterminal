/*
 * sound_null.c — Silent audio stubs.
 *
 * Compiled when SDL2 is not available.  Every call is a no-op so
 * BEEP / SOUND / PLAY in BASIC programs silently do nothing rather
 * than crashing or producing a compile error.
 */
#include "sound.h"

void sound_init(void)                         { }
void sound_shutdown(void)                     { }
void sound_beep(void)                         { }
void sound_tone(double f, double d)           { (void)f; (void)d; }
void sound_play(const char *mml)              { (void)mml; }
