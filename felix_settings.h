#pragma once

// Load settings from registry (Windows) or ~/.config/FelixTerminal/settings.ini (Linux).
// Applies values directly to g_font_size, g_theme_idx, and audio enabled state.
void settings_load(void);

// Save current g_font_size, g_theme_idx, and audio enabled state.
void settings_save(void);
