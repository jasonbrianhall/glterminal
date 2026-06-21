/*
 * sound_null.cpp — Null sound implementation for text-only builds
 * Provides BEEP as console bell only
 */

#include "sound.h"
#include <cstdio>

BASIC_NS_BEGIN

void sound_init(void) {
    /* No initialization needed */
}

void sound_shutdown(void) {
    /* No cleanup needed */
}

void sound_beep(void) {
    /* DOS: just beep via console bell */
    putchar('\007');  /* ASCII bell */
    fflush(stdout);
}

void sound_tone(double freq, double duration_ms) {
    /* DOS: can't generate tones, just beep */
    (void)freq;
    (void)duration_ms;
    sound_beep();
}

void sound_play(char *mml) {
    /* DOS: can't play MML sequences, just beep */
    (void)mml;
    sound_beep();
}

void sound_set_volume(int percent) {
    /* Ignore - no volume control in text mode */
    (void)percent;
}

BASIC_NS_END

