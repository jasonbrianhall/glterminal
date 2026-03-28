/*
 * beatchess_dos_enhanced.cpp - BeatChess DOS/Allegro 4 with Menu System
 * Enhanced version with File menu and side buttons
 */

#include "beatchess.h"
#include "chess_pieces.h"
#include "chess_pieces_loader.h"
#include "chess_ai_move.h"
#include "splashscreen.h"
#include "pgn.h"
#include <allegro.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <time.h>

/* Platform detection and conditional includes */
#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
    #include <dirent.h>
    #include <sys/types.h>
    #define LINUX_BUILD 1
#else
    #include <dir.h>
    #define DOS_BUILD 1
#endif

/* Allegro keyboard support - normalize key codes across platforms */
#ifndef KEY_UP
    #define KEY_UP 0x48
#endif
#ifndef KEY_DOWN
    #define KEY_DOWN 0x50
#endif
#ifndef KEY_LEFT
    #define KEY_LEFT 0x4B
#endif
#ifndef KEY_RIGHT
    #define KEY_RIGHT 0x4D
#endif
#ifndef KEY_ESC
    #define KEY_ESC 0x01
#endif
#ifndef KEY_ENTER
    #define KEY_ENTER 0x1C
#endif
#ifndef KEY_BACKSPACE
    #define KEY_BACKSPACE 0x0E
#endif
#ifndef KEY_F5
    #define KEY_F5 0x3F
#endif
#ifndef KEY_F6
    #define KEY_F6 0x40
#endif
#ifndef KEY_F7
    #define KEY_F7 0x41
#endif
#ifndef KEY_F8
    #define KEY_F8 0x42
#endif

/* Keyboard handling - use Allegro's key[] array on Linux */
/* On Linux, Allegro provides key[] array directly - no extern needed */

#define BOARD_START_X 60
#define BOARD_START_Y 80
#define SQUARE_SIZE 50
#define BUTTON_PANEL_X 480
#define BUTTON_PANEL_Y 80
#define BUTTON_WIDTH 140
#define BUTTON_HEIGHT 30
#define BUTTON_SPACING 8

/* Menu bar */
#define MENU_BAR_HEIGHT 20
#define MENU_ITEM_WIDTH 80

/* File Browser UI */
#define MAX_FILES 100
#define MAX_FILENAME_LEN 256
#define LIST_VISIBLE_ITEMS 10
#define FILE_LIST_X 50
#define FILE_LIST_Y 80
#define FILE_LIST_WIDTH 540
#define FILE_LIST_ITEM_HEIGHT 25
#define FILE_LIST_HEIGHT (LIST_VISIBLE_ITEMS * FILE_LIST_ITEM_HEIGHT)

/* Define custom colors for chess board (green and cream/beige) */
#define LIGHT_SQUARE 15   /* Light beige/cream color */
#define DARK_SQUARE 46    /* Dark green color */

/* ============================================================================
 * Board Color Palettes
 * ============================================================================
 */

/* Dark (Black) square color palette */
int dark_color_palette[] = {
    46,    /* 0: Dark green (original) */
    0,     /* 1: Black */
    1,     /* 2: Dark blue */
    4,     /* 3: Dark red */
    6,     /* 4: Dark cyan */
    8,     /* 5: Dark gray */
};
#define DARK_PALETTE_SIZE (sizeof(dark_color_palette) / sizeof(dark_color_palette[0]))

/* Light (White) square color palette */
int light_color_palette[] = {
    15,    /* 0: Light beige/cream (original) */
    7,     /* 1: Light gray */
    11,    /* 2: Light cyan */
    14,    /* 3: Light yellow */
    13,    /* 4: Light magenta */
    10,    /* 5: Light green */
};
#define LIGHT_PALETTE_SIZE (sizeof(light_color_palette) / sizeof(light_color_palette[0]))



ChessGUI chess_gui;

/* Color palette indices */
int dark_color_idx = 0;    /* Index into dark_color_palette */
int light_color_idx = 0;   /* Index into light_color_palette */

/* Triple buffering setup */
#define NUM_BUFFERS 3
BITMAP *buffers[NUM_BUFFERS];
int current_buffer = 0;
BITMAP *active_buffer = NULL;

/* Dirty screen tracking */
bool screen_is_dirty = true;  /* Start dirty to force initial draw */
bool screen_needs_full_redraw = true;  /* Force full redraw flag */

/* Mouse position tracking */
int prev_mouse_x = -1;
int prev_mouse_y = -1;

/* Help menu dropdown state (separate from show_help screen) */
bool show_help_menu_dropdown = false;

/* Close button state (Linux only) */
volatile bool window_close_requested = false;


/* ============================================================================
 * File Browser Structures
 * ============================================================================ */

typedef struct {
    char filename[MAX_FILENAME_LEN];
    int is_directory;
} FileEntry;

typedef struct {
    FileEntry files[MAX_FILES];
    int file_count;
    int selected_index;
    int scroll_offset;
} FileList;

/**
 * Initialize triple buffering
 */
void init_triple_buffers() {
    for (int i = 0; i < NUM_BUFFERS; i++) {
        buffers[i] = create_bitmap(640, 480);
        if (!buffers[i]) {
            allegro_exit();
            fprintf(stderr, "Failed to allocate buffer %d\n", i);
            exit(1);
        }
    }
    current_buffer = 0;
    active_buffer = buffers[0];
}

/**
 * Swap to next buffer and return it for drawing
 * Only blits to screen if screen_is_dirty is true
 */
BITMAP* get_next_buffer_and_swap() {
    /* Only update screen if it's marked as dirty */
    if (screen_is_dirty) {
        vsync();  /* Wait for vertical sync */
        blit(buffers[current_buffer], screen, 0, 0, 0, 0, 640, 480);
        screen_is_dirty = false;  /* Mark screen as clean after update */
    }
    
    /* Advance to next buffer for drawing */
    current_buffer = (current_buffer + 1) % NUM_BUFFERS;
    active_buffer = buffers[current_buffer];
    
    return active_buffer;
}

/**
 * Cleanup triple buffers
 */
void cleanup_triple_buffers() {
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (buffers[i]) {
            destroy_bitmap(buffers[i]);
            buffers[i] = NULL;
        }
    }
}

/**
 * Mark the screen as dirty so it will be redrawn next frame
 */
void mark_screen_dirty() {
    screen_is_dirty = true;
    screen_needs_full_redraw = true;
}

/**
 * Mark the screen for a full redraw (clears buffer before drawing)
 */
void mark_screen_needs_full_redraw() {
    screen_needs_full_redraw = true;
    screen_is_dirty = true;
}

/**
 * Retrieve a game state from history at logical position.
 * Uses a straight buffer - no circular wrapping.
 */
#ifndef LINUX_BUILD
void printToSerial(const char *fmt, ...) {
    FILE *serial = fopen("COM1", "w");
    if (!serial) return;

    va_list args;
    va_start(args, fmt);
    vfprintf(serial, fmt, args);
    va_end(args);

    fclose(serial);
}
#endif

ChessGameState get_history_at_position(int position) {
    ChessGameState result;
    
    /* Clear result by zeroing out board and flags */
    for (int i = 0; i < BOARD_SIZE; i++) {
        for (int j = 0; j < BOARD_SIZE; j++) {
            result.board[i][j].type = EMPTY;
            result.board[i][j].color = NONE;
        }
    }
    result.turn = NONE;
    result.en_passant_col = -1;
    result.en_passant_row = -1;
    result.white_king_moved = false;
    result.black_king_moved = false;
    result.white_rook_a_moved = false;
    result.white_rook_h_moved = false;
    result.black_rook_a_moved = false;
    result.black_rook_h_moved = false;
    
    /* Validate history buffer is initialized before access */
    if (!chess_gui.history || chess_gui.history_capacity <= 0) {
        return result;  /* Return empty state if history not initialized */
    }
    
    /* Bounds check for straight buffer */
    if (position < 0 || position >= chess_gui.history_size || position >= chess_gui.history_capacity) {
        return result;
    }
    
    return chess_gui.history[position];
}

/* Side panel buttons */
Button side_buttons[] = {
    {BUTTON_PANEL_X, BUTTON_PANEL_Y + 0, BUTTON_WIDTH, BUTTON_HEIGHT, "New Game (N)", 'N', true},
    {BUTTON_PANEL_X, BUTTON_PANEL_Y + 38, BUTTON_WIDTH, BUTTON_HEIGHT, "Undo (U)", 'U', true},
    {BUTTON_PANEL_X, BUTTON_PANEL_Y + 76, BUTTON_WIDTH, BUTTON_HEIGHT, "Toggle AI (A)", 'A', true},
    {BUTTON_PANEL_X, BUTTON_PANEL_Y + 114, BUTTON_WIDTH, BUTTON_HEIGHT, "Swap Color (B)", 'B', true},
    {BUTTON_PANEL_X, BUTTON_PANEL_Y + 152, BUTTON_WIDTH, BUTTON_HEIGHT, "Help (?)", '?', true},
    {BUTTON_PANEL_X, BUTTON_PANEL_Y + 190, BUTTON_WIDTH, BUTTON_HEIGHT, "Quit (Q)", 'Q', true},
};

#define NUM_BUTTONS (sizeof(side_buttons) / sizeof(side_buttons[0]))

/* File menu items */
const char *file_menu_items[] = {
    "New Game     N",
    "Undo Move    U",
    "",  /* separator */
    "Save Game    S",
    "Load Game    L",
    "",  /* separator */
    "AI vs AI     A",
    "Swap Color   B",
    "",  /* separator */
    "Quit         Q"
};

#define NUM_FILE_MENU_ITEMS (sizeof(file_menu_items) / sizeof(file_menu_items[0]))

/* Help menu items */
const char *help_menu_items[] = {
    "Help         ?",
    "About..."
};

#define NUM_HELP_MENU_ITEMS (sizeof(help_menu_items) / sizeof(help_menu_items[0]))

/* ============================================================================
 * Helper functions
 * ============================================================================
 */

void draw_text(int x, int y, int color, const char *text) {
    textout_ex(screen, font, text, x, y, color, -1);
}

void draw_text_center(int x, int y, int color, const char *text) {
    int len = strlen(text) * 8;  /* Approximate width */
    textout_ex(screen, font, text, x - len/2, y, color, -1);
}

bool point_in_rect(int px, int py, int x, int y, int w, int h) {
    return (px >= x && px < x + w && py >= y && py < y + h);
}

void cleanup_chess_game() {
    chess_gui.history_size = 0;
    chess_gui.history_capacity = 0;
    chess_gui.move_history_count = 0;
    chess_gui.move_history_index = 0;
}

void init_chess_game() {
    if (chess_gui.ai_computing) {
        return;
    }

    /* Initialize board */
    chess_init_board(&chess_gui.game);

    /* Force Player vs AI, player is white */
    chess_gui.ai_vs_ai = false;
    chess_gui.player_is_white = true;

    /* Reset UI state */
    chess_gui.selected_row = -1;
    chess_gui.selected_col = -1;
    chess_gui.piece_selected_row = -1;
    chess_gui.piece_selected_col = -1;
    chess_gui.piece_selected = false;
    chess_gui.last_move_from_row = -1;
    chess_gui.last_move_from_col = -1;
    chess_gui.last_move_to_row = -1;
    chess_gui.last_move_to_col = -1;
    chess_gui.has_last_move = false;
    chess_gui.show_help = false;

    /* Reset AI state */
    chess_gui.ai_move_counter = 0;
    chess_gui.ai_move_delay = 15;
    chess_gui.ai_thinking = false;
    chess_gui.ai_computing = false;
    chess_gui.ai_total_moves = 0;
    chess_gui.ai_evaluated_moves = 0;
    chess_gui.ai_best_move.from_row = -1;
    chess_gui.ai_best_move.from_col = -1;
    chess_gui.ai_best_move.to_row = -1;
    chess_gui.ai_best_move.to_col = -1;
    chess_gui.ai_best_move.score = 0;

    /* Reset timers to use proper clock_t and milliseconds */
    chess_gui.white_time_milliseconds = 0;
    chess_gui.black_time_milliseconds = 0;
    chess_gui.timer_started = false;
    chess_gui.ai_thinking_start_time = clock();
    chess_gui.white_total_time = 0;
    chess_gui.black_total_time = 0;
    chess_gui.current_move_start_time = clock();
    chess_gui.last_move_end_time = clock();
    chess_gui.history_capacity = MAX_MOVE_HISTORY*2;
    /* Reset check/checkmate/stalemate flags */
    chess_gui.is_in_check = false;
    chess_gui.check_display_timer = 0;
    chess_gui.is_checkmate = false;
    chess_gui.is_stalemate = false;

    /* Clear history buffer */
    for (int i = 0; i < chess_gui.history_capacity; i++) {
        chess_gui.history[i].board[0][0].type = EMPTY;
        chess_gui.history[i].board[0][0].color = NONE;
        chess_gui.history[i].turn = NONE;
        chess_gui.move_history[i].game_state.board[0][0].type = EMPTY;
        chess_gui.move_history[i].game_state.board[0][0].color = NONE;
        chess_gui.move_history[i].time_elapsed = 0;
    }

    /* Reset history counters */
    chess_gui.history_size = 0;
    chess_gui.move_history_count = 0;
    chess_gui.move_history_index = 0;
    chess_gui.move_count = 0;

    /* Reset game state */
    chess_gui.game.turn = WHITE;
    chess_gui.game.white_king_moved = false;
    chess_gui.game.black_king_moved = false;
    chess_gui.game.white_rook_a_moved = false;
    chess_gui.game.white_rook_h_moved = false;
    chess_gui.game.black_rook_a_moved = false;
    chess_gui.game.black_rook_h_moved = false;
    chess_gui.game.en_passant_col = -1;
    chess_gui.game.en_passant_row = -1;

    /* Save initial position */
    save_position_to_history();
}


/**
 * Save the current game position to history using a straight buffer.
 * Once the buffer is full, no more moves are saved (prevents overflow).
 * 
 * FIXED: Added defensive check to resync move_history_index if corrupted by undo,
 * and added move parameter to store the move that created this position
 */
void save_position_to_history() {
    save_position_to_history_with_move(0, 0, 0, 0);  /* Delegate to version with move info */
}

/**
 * Save position with the move that created it (for better undo/replay)
 */
void save_position_to_history_with_move(int from_r, int from_c, int to_r, int to_c) {
    //printToSerial("DEBUG: Entering save_position_to_history_with_move (from %d,%d to %d,%d)\n", from_r, from_c, to_r, to_c);

    /* Ensure move_history_index is in valid range (defensive against corruption) */
    if (chess_gui.move_history_index < 0 || chess_gui.move_history_index >= chess_gui.history_capacity) {
        //printToSerial("WARNING: move_history_index (%d) out of range [0..%d], resyncing to history_size (%d)\n", chess_gui.move_history_index, chess_gui.history_capacity - 1, chess_gui.history_size);
        chess_gui.move_history_index = chess_gui.history_size;
    }
    
    /* Don't save if buffer is full - prevent overflow */
    if (chess_gui.move_history_index >= chess_gui.history_capacity) {
        //printToSerial("ERROR: Buffer full, cannot save move. index=%d capacity=%d\n", chess_gui.move_history_index, chess_gui.history_capacity);
        return;
    }
    
    /* Calculate elapsed time since move started */
    clock_t current_time = clock();
    long elapsed_ms = (long)((current_time - chess_gui.current_move_start_time) * 1000.0 / CLOCKS_PER_SEC);
    double elapsed_seconds = elapsed_ms / 1000.0;
    
    /* Store at current write position */
    //printToSerial("DEBUG: Saving move at index %d\n", chess_gui.move_history_index);
    chess_gui.history[chess_gui.move_history_index] = chess_gui.game;
    chess_gui.move_history[chess_gui.move_history_index].game_state = chess_gui.game;
    chess_gui.move_history[chess_gui.move_history_index].move.from_row = from_r;
    chess_gui.move_history[chess_gui.move_history_index].move.from_col = from_c;
    chess_gui.move_history[chess_gui.move_history_index].move.to_row = to_r;
    chess_gui.move_history[chess_gui.move_history_index].move.to_col = to_c;
    chess_gui.move_history[chess_gui.move_history_index].time_elapsed = elapsed_seconds;  /* Save actual elapsed time! */
    
    /* Increment counters together - keep them in sync */
    chess_gui.move_history_index++;
    chess_gui.history_size = chess_gui.move_history_index;  /* Keep synchronized */
    chess_gui.move_history_count = chess_gui.history_size;
    //printToSerial("DEBUG: Counters updated -> move_history_index=%d history_size=%d move_history_count=%d\n", chess_gui.move_history_index, chess_gui.history_size, chess_gui.move_history_count);
    
    /* Start timer after first move is saved (when we have 2+ positions) */
    if (chess_gui.history_size == 2) {
        chess_gui.timer_started = true;
        //printToSerial("DEBUG: Timer started (history_size=%d)\n", chess_gui.history_size);
    }

    //printToSerial("DEBUG: Exiting save_position_to_history_with_move\n");
}

/**
 * Recalculate elapsed times based on move history
 * Used when loading games or undoing moves to sync timers with position
 */
void recalculate_timers_from_history() {
    chess_gui.white_total_time = 0;
    chess_gui.black_total_time = 0;
    
    /* Sum up elapsed times from each move in history */
    for (int i = 0; i < chess_gui.move_history_count && i < MAX_MOVE_HISTORY * 2; i++) {
        if (chess_gui.move_history[i].time_elapsed > 0) {
            /* Determine whose move it was (alternating: white, black, white, ...) */
            if (i % 2 == 0) {
                /* Even index = white's move */
                chess_gui.white_total_time += chess_gui.move_history[i].time_elapsed;
            } else {
                /* Odd index = black's move */
                chess_gui.black_total_time += chess_gui.move_history[i].time_elapsed;
            }
        }
    }
    
    /* Reset current move timer to now - this starts the timer for the next move to be made */
    /* Do NOT update the display values here - they will be updated in the main loop */
    clock_t now = clock();
    chess_gui.current_move_start_time = now;
    chess_gui.last_move_end_time = now;
    
    /* Sync display to show the accumulated times only (not including current move yet) */
    chess_gui.white_time_milliseconds = (long)(chess_gui.white_total_time * 1000.0);
    chess_gui.black_time_milliseconds = (long)(chess_gui.black_total_time * 1000.0);
}

void undo_move() {
    //printToSerial("\n\nIn Undo Move\n");

    /* Don't allow undo while AI is computing */
    if (chess_gui.ai_computing) { 
        //printToSerial("AI computing, abort undo\n");
        return;
    }
    
    /* Safety check - make sure we have history initialized */
    /*if (!chess_gui.history || !chess_gui.move_history || chess_gui.history_capacity <= 0) {
        //printToSerial("exit 1: history not initialized\n");
        return;
    }*/
    
    /* Can't undo if we only have the initial position (need at least 2 positions) */
    if (chess_gui.history_size < 2) {
        //printToSerial("exit 2: not enough history %i\n", chess_gui.history_size);
        return;
    }
    
    /* Determine how many moves to undo */
    int moves_to_undo = chess_gui.ai_vs_ai ? 1 : 2;
    //printToSerial("moves_to_undo = %d\n", moves_to_undo);
    
    /* Don't undo more moves than we have (must keep initial position at index 0) */
    if (chess_gui.history_size - moves_to_undo < 1) {
        moves_to_undo = chess_gui.history_size - 1;
        //printToSerial("adjusted moves_to_undo = %d\n", moves_to_undo);
    }
    
    /* Safety check: can't undo if we'd go past the beginning */
    if (moves_to_undo <= 0) {
        //printToSerial("exit 3: moves_to_undo <= 0\n");
        return;
    }
    
    /* CRITICAL: Calculate indices BEFORE modifying history_size */
    int restore_index = chess_gui.history_size - moves_to_undo - 1;
    int last_move_index = restore_index;
    //printToSerial("restore_index = %d, last_move_index = %d\n", restore_index, last_move_index);
    
    /* Validate restore_index with full bounds checking */
    if (restore_index < 0) {
        //printToSerial("restore_index < 0, resetting\n");
        restore_index = 0;
        last_move_index = -1;
    }
    if (restore_index >= chess_gui.history_capacity) {
        ////printToSerial("restore_index >= capacity, resetting\n");
        chess_gui.history_size = 1;
        restore_index = 0;
        last_move_index = -1;
    }
    
    /* Now it's safe to update history_size */
    chess_gui.history_size -= moves_to_undo;
    ////printToSerial("new history_size = %d\n", chess_gui.history_size);
    
    /* Extra safety: ensure we didn't underflow */
    if (chess_gui.history_size < 1) {
        //printToSerial("history_size underflow, resetting\n");
        chess_gui.history_size = 1;
        restore_index = 0;
        last_move_index = -1;
    }
    
    /* Restore game state from the valid restore_index */
    if (restore_index >= 0 && restore_index < chess_gui.history_capacity) {
        //printToSerial("Restoring game state from index %d\n", restore_index);
        chess_gui.game = chess_gui.history[restore_index];
    } else {
        //printToSerial("Invalid restore_index, reinitializing board\n");
        chess_init_board(&chess_gui.game);
        last_move_index = -1;
    }
    
    chess_gui.move_history_count = chess_gui.history_size;
    chess_gui.move_history_index = chess_gui.history_size;
    chess_gui.move_count = chess_gui.history_size;  /* Keep move counter synchronized with history */
    
    /* Restore the last move display */
    if (last_move_index > 0 && last_move_index < chess_gui.history_capacity) {
        int previous_move_index = last_move_index - 1;
        //printToSerial("last_move_index = %d, previous_move_index = %d\n", last_move_index, previous_move_index);
        if (previous_move_index >= 0 && previous_move_index < chess_gui.history_capacity) {
            ChessMove prev_move = chess_gui.move_history[previous_move_index].move;
            if (prev_move.from_row >= 0) {
                //printToSerial("Restoring last move display\n");
                chess_gui.last_move_from_row = prev_move.from_row;
                chess_gui.last_move_from_col = prev_move.from_col;
                chess_gui.last_move_to_row = prev_move.to_row;
                chess_gui.last_move_to_col = prev_move.to_col;
                chess_gui.has_last_move = true;
            } else {
                //printToSerial("Invalid prev_move, clearing last move\n");
                chess_gui.has_last_move = false;
            }
        } else {
            //printToSerial("previous_move_index out of bounds\n");
            chess_gui.has_last_move = false;
        }
    } else {
        //printToSerial("No last move to show\n");
        chess_gui.has_last_move = false;
    }
    
    /* Clear piece selection after undo */
    //printToSerial("Clearing piece selection\n");
    chess_gui.piece_selected = false;
    chess_gui.piece_selected_row = -1;
    chess_gui.piece_selected_col = -1;
    chess_gui.selected_row = -1;
    chess_gui.selected_col = -1;
    
    /* Recalculate timers based on the restored position */
    recalculate_timers_from_history();
    
    /* Clear AI state */
    ////printToSerial("Resetting AI state\n");
    chess_gui.ai_thinking = false;
    chess_gui.ai_computing = false;
    chess_gui.ai_move_counter = 0;
}

/* Global variables removed as they are now managed by chess_ai_move module:
 * - ai_eval_counter: Interruptibility handled by shared module
 * - ScoredMove struct: Internal to shared module
 * - Move collection logic: Handled by shared module
 */


/* Display splash screen and wait for keypress or timeout */
void show_splash_screen(BITMAP *backbuffer) {
    /* Load the splash screen - need to handle 8-bit indexed BMPs differently */
    const unsigned char *data = splashscreen_bmp;
    unsigned int len = splashscreen_bmp_len;
    BITMAP *splash = NULL;
    
    /* Verify BMP signature */
    if (len >= 54 && data[0] == 'B' && data[1] == 'M') {
        int data_offset = data[10] | (data[11] << 8) | (data[12] << 16) | (data[13] << 24);
        int width = data[18] | (data[19] << 8) | (data[20] << 16) | (data[21] << 24);
        int height = data[22] | (data[23] << 8) | (data[24] << 16) | (data[25] << 24);
        int bpp = data[28] | (data[29] << 8);
        
        if (height < 0) height = -height;
        
        if (width > 0 && height > 0 && width <= 2048 && height <= 2048) {
            splash = create_bitmap(width, height);
            
            if (splash) {
                if (bpp == 8) {
                    /* 8-bit indexed color BMP */
                    const unsigned char *palette = data + 54;
                    const unsigned char *pixel_data = data + data_offset;
                    int bytes_per_row = ((width + 3) / 4) * 4;
                    
                    for (int row = 0; row < height; row++) {
                        const unsigned char *row_data = pixel_data + (height - 1 - row) * bytes_per_row;
                        for (int col = 0; col < width; col++) {
                            int palette_index = row_data[col];
                            int b = palette[palette_index * 4 + 0];
                            int g = palette[palette_index * 4 + 1];
                            int r = palette[palette_index * 4 + 2];
                            putpixel(splash, col, row, makecol(r, g, b));
                        }
                    }
                } else if (bpp == 24) {
                    /* 24-bit RGB BMP */
                    const unsigned char *pixel_data = data + data_offset;
                    int bytes_per_row = ((width * 3 + 3) / 4) * 4;
                    
                    for (int row = 0; row < height; row++) {
                        const unsigned char *row_data = pixel_data + (height - 1 - row) * bytes_per_row;
                        for (int col = 0; col < width; col++) {
                            int b = row_data[col * 3 + 0];
                            int g = row_data[col * 3 + 1];
                            int r = row_data[col * 3 + 2];
                            putpixel(splash, col, row, makecol(r, g, b));
                        }
                    }
                }
            }
        }
    }
    
    if (splash) {
        /* Clear screen to black */
        clear_to_color(backbuffer, COLOR_BLACK);
        
        /* Center the splash screen */
        int x = (640 - splash->w) / 2;
        int y = (480 - splash->h) / 2;
        
        /* Draw splash to backbuffer */
        blit(splash, backbuffer, 0, 0, x, y, splash->w, splash->h);
        
        /* Display "Press any key..." message */
        textout_centre_ex(backbuffer, font, "Press any key or click to continue...", 
                         320, 450, COLOR_WHITE, -1);
        
        /* Show the splash screen */
        scare_mouse();
        blit(backbuffer, screen, 0, 0, 0, 0, 640, 480);
        unscare_mouse();
        
        /* Clear any pending keypresses and mouse clicks first */
        clear_keybuf();
        
        /* Wait for keypress, mouse click, or timeout (10 seconds) */
        int timeout = 0;
        int prev_mouse_b = 0;
        while (timeout < 1000) {  /* 1000 frames * 10ms = 10 seconds */
            poll_mouse();
            
            /* Check for keypress */
            if (keypressed()) {
                readkey();  /* Consume the key */
                break;
            }
            
            /* Check for mouse click */
            if ((mouse_b & 1) && !(prev_mouse_b & 1)) {
                break;  /* Left click detected */
            }
            prev_mouse_b = mouse_b;
            
            rest(10);
            timeout++;
        }
        
        /* Cleanup */
        destroy_bitmap(splash);
    } else {
        printf("Warning: Could not load splash screen\n");
        rest(500);  /* Brief pause so user sees the message */
    }
}

/**
 * Compute the best AI move using the shared AI engine
 * 
 * INTEGRATED VERSION: This function now delegates to chess_ai_compute_move()
 * from the shared chess_ai_move module. This ensures that both the DOS/Allegro
 * and GTK versions use identical AI logic, eliminating code duplication and
 * reducing maintenance burden.
 * 
 * The shared module handles:
 * - Move generation and evaluation
 * - Minimax with alpha-beta pruning
 * - Candidate move collection within threshold
 * - Random move selection for variety
 */
ChessMove compute_ai_move() {
    ChessMove best_move = {-1, -1, -1, -1, 0};
    
    /* Get default AI configuration from shared module */
    ChessAIConfig config = chess_ai_get_default_config();
    
    /* Optional: Override defaults if needed
     * config.search_depth = 4;              // Deeper search for stronger play
     * config.threshold_centipawns = 25;     // Moves within 25cp of best
     * config.use_randomization = true;      // Enable move variety
     */
    
    /* Call the shared AI engine - this performs all move evaluation */
    ChessAIMoveResult result = chess_ai_compute_move(&chess_gui.game, config);
    
    /* Update GUI state with AI evaluation metadata */
    chess_gui.ai_total_moves = result.total_moves_evaluated;
    chess_gui.ai_evaluated_moves = result.total_moves_evaluated;
    
    /* Return the selected move */
    return result.move;
}

/* ============================================================================
 * Drawing functions
 * ============================================================================
 */

void draw_menu_bar() {
    /* Menu bar background */
    rectfill(screen, 0, 0, 640, MENU_BAR_HEIGHT, COLOR_BLUE);
    
    /* File menu button */
    textout_ex(screen, font, "File", 5, 5, COLOR_WHITE, -1);
    
    /* Help menu button */
    textout_ex(screen, font, "Help", 50, 5, COLOR_WHITE, -1);
    
    /* Title */
#ifdef LINUX_BUILD
    textout_ex(screen, font, "BeatChess - Allegro Edition", 200, 5, COLOR_YELLOW, -1);
#else
    textout_ex(screen, font, "BeatChess - DOS Edition", 200, 5, COLOR_YELLOW, -1);
#endif
    
    /* Draw File dropdown menu if active */
    if (chess_gui.show_menu) {
        int menu_x = 0;
        int menu_y = MENU_BAR_HEIGHT;
        int menu_w = 200;
        int item_h = 20;
        int menu_h = NUM_FILE_MENU_ITEMS * item_h;
        
        /* Menu background with border */
        rectfill(screen, menu_x, menu_y, menu_x + menu_w, menu_y + menu_h, COLOR_BLUE);
        rect(screen, menu_x, menu_y, menu_x + menu_w, menu_y + menu_h, COLOR_WHITE);
        
        /* Menu items */
        for (int i = 0; i < NUM_FILE_MENU_ITEMS; i++) {
            int item_y = menu_y + i * item_h;
            
            /* Separator */
            if (strlen(file_menu_items[i]) == 0) {
                hline(screen, menu_x + 5, item_y + item_h/2, menu_x + menu_w - 5, COLOR_GRAY);
                continue;
            }
            
            /* Highlight selected */
            if (i == chess_gui.menu_selected) {
                rectfill(screen, menu_x + 2, item_y + 2, 
                        menu_x + menu_w - 2, item_y + item_h - 2, COLOR_CYAN);
            }
            
            /* Draw menu item text */
            int text_color = (i == chess_gui.menu_selected) ? COLOR_BLACK : COLOR_WHITE;
            textout_ex(screen, font, file_menu_items[i], menu_x + 10, item_y + 5, text_color, -1);
        }
    }
    
    /* Draw Help dropdown menu if active */
    if (show_help_menu_dropdown) {
        int menu_x = 50;
        int menu_y = MENU_BAR_HEIGHT;
        int menu_w = 150;
        int item_h = 20;
        int menu_h = NUM_HELP_MENU_ITEMS * item_h;
        
        /* Menu background with border */
        rectfill(screen, menu_x, menu_y, menu_x + menu_w, menu_y + menu_h, COLOR_BLUE);
        rect(screen, menu_x, menu_y, menu_x + menu_w, menu_y + menu_h, COLOR_WHITE);
        
        /* Menu items */
        for (int i = 0; i < NUM_HELP_MENU_ITEMS; i++) {
            int item_y = menu_y + i * item_h;
            
            /* Highlight selected */
            if (i == chess_gui.menu_selected) {
                rectfill(screen, menu_x + 2, item_y + 2, 
                        menu_x + menu_w - 2, item_y + item_h - 2, COLOR_CYAN);
            }
            
            /* Draw menu item text */
            int text_color = (i == chess_gui.menu_selected) ? COLOR_BLACK : COLOR_WHITE;
            textout_ex(screen, font, help_menu_items[i], menu_x + 10, item_y + 5, text_color, -1);
        }
    }
    
    /* Draw Help dropdown menu if active */
    /* (Help menu disabled - not in ChessGUI struct) */
}

void draw_button(Button *btn, bool hover) {
    int bg_color = btn->enabled ? (hover ? COLOR_CYAN : COLOR_BLUE) : COLOR_GRAY;
    int text_color = btn->enabled ? (hover ? COLOR_BLACK : COLOR_WHITE) : COLOR_BLACK;
    
    /* Button background */
    rectfill(screen, btn->x, btn->y, btn->x + btn->w, btn->y + btn->h, bg_color);
    
    /* Button border */
    rect(screen, btn->x, btn->y, btn->x + btn->w, btn->y + btn->h, COLOR_WHITE);
    
    /* Button text - centered */
    int text_w = strlen(btn->label) * 8;
    int text_x = btn->x + (btn->w - text_w) / 2;
    int text_y = btn->y + (btn->h - 8) / 2;
    textout_ex(screen, font, btn->label, text_x, text_y, text_color, -1);
}

void draw_side_panel() {
    /* Panel background - don't draw over the board */
    rectfill(screen, BUTTON_PANEL_X - 10, MENU_BAR_HEIGHT, 
             640, 480, COLOR_BLACK);
    
    /* Panel title */
    textout_ex(screen, font, "Controls", BUTTON_PANEL_X + 35, MENU_BAR_HEIGHT + 5, 
               COLOR_YELLOW, -1);
    
    /* Draw buttons */
    for (int i = 0; i < NUM_BUTTONS; i++) {
        bool hover = point_in_rect(mouse_x, mouse_y, 
                                   side_buttons[i].x, side_buttons[i].y,
                                   side_buttons[i].w, side_buttons[i].h);
        draw_button(&side_buttons[i], hover);
    }
    
    /* Game info panel */
    int info_y = BUTTON_PANEL_Y + 250;
    textout_ex(screen, font, "Game Info:", BUTTON_PANEL_X, info_y, COLOR_YELLOW, -1);
    
    char buf[64];
    snprintf(buf, 64, "Move: %d", chess_gui.move_count);
    textout_ex(screen, font, buf, BUTTON_PANEL_X, info_y + 20, COLOR_WHITE, -1);
    
    const char *turn_str = (chess_gui.game.turn == WHITE) ? "White" : "Black";
    snprintf(buf, 64, "Turn: %s", turn_str);
    textout_ex(screen, font, buf, BUTTON_PANEL_X, info_y + 35, COLOR_WHITE, -1);
    
    const char *mode_str = chess_gui.ai_vs_ai ? "AI vs AI" : 
                          (chess_gui.player_is_white ? "Player vs AI" : "AI vs Player");
    snprintf(buf, 64, "Mode: %s", mode_str);
    textout_ex(screen, font, buf, BUTTON_PANEL_X, info_y + 50, COLOR_WHITE, -1);
    
    if (!chess_gui.ai_vs_ai) {
        const char *player_color = chess_gui.player_is_white ? "White" : "Black";
        snprintf(buf, 64, "You: %s", player_color);
        textout_ex(screen, font, buf, BUTTON_PANEL_X, info_y + 65, COLOR_GREEN, -1);
    }
    
    /* Display timers */
    int timer_y = info_y + 85;
    textout_ex(screen, font, "Time Elapsed:", BUTTON_PANEL_X, timer_y, COLOR_YELLOW, -1);
    
    /* White time - display as MM:SS.mmm (milliseconds with 3 decimal places) */
    long white_total_ms = chess_gui.white_time_milliseconds;
    int white_mins = white_total_ms / 60000;
    int white_secs = (white_total_ms % 60000) / 1000;
    int white_ms = white_total_ms % 1000;
    snprintf(buf, 64, "White: %d:%02d.%03d", white_mins, white_secs, white_ms);
    textout_ex(screen, font, buf, BUTTON_PANEL_X, timer_y + 15, COLOR_WHITE, -1);
    
    /* Black time - display as MM:SS.mmm (milliseconds with 3 decimal places) */
    long black_total_ms = chess_gui.black_time_milliseconds;
    int black_mins = black_total_ms / 60000;
    int black_secs = (black_total_ms % 60000) / 1000;
    int black_ms = black_total_ms % 1000;
    snprintf(buf, 64, "Black: %d:%02d.%03d", black_mins, black_secs, black_ms);
    textout_ex(screen, font, buf, BUTTON_PANEL_X, timer_y + 30, COLOR_WHITE, -1);
    
    /* AI thinking indicator */
    if (chess_gui.ai_thinking) {
        snprintf(buf, 64, "AI thinking...");
        textout_ex(screen, font, buf, BUTTON_PANEL_X, timer_y + 50, COLOR_MAGENTA, -1);
        snprintf(buf, 64, "Depth: %d", chess_gui.ai_search_depth);
        textout_ex(screen, font, buf, BUTTON_PANEL_X, timer_y + 65, COLOR_MAGENTA, -1);
        if (chess_gui.ai_total_moves > 0) {
            snprintf(buf, 64, "Move: %d/%d", chess_gui.ai_evaluated_moves, chess_gui.ai_total_moves);
            textout_ex(screen, font, buf, BUTTON_PANEL_X, timer_y + 80, COLOR_MAGENTA, -1);
        }
    }
}

void draw_board() {
    int x, y, color;
    char buf[64];
    
    /* Get current colors from palettes */
    int current_light = light_color_palette[light_color_idx % LIGHT_PALETTE_SIZE];
    int current_dark = dark_color_palette[dark_color_idx % DARK_PALETTE_SIZE];
    
    /* Draw chess board - 50 pixels per square */
    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            int screen_x = BOARD_START_X + x * SQUARE_SIZE;
            int screen_y = BOARD_START_Y + y * SQUARE_SIZE;
            
            /* Alternate colors - light beige on even squares, dark green on odd */
            if ((x + y) % 2 == 0) {
                color = current_light;  /* Light color */
            } else {
                color = current_dark;   /* Dark color */
            }
            
            /* Highlight last move (from and to squares) */
            if (chess_gui.has_last_move) {
                if ((y == chess_gui.last_move_from_row && x == chess_gui.last_move_from_col) ||
                    (y == chess_gui.last_move_to_row && x == chess_gui.last_move_to_col)) {
                    color = COLOR_CYAN;  /* Cyan highlight for last move */
                }
            }
            
            /* Highlight selected square (overrides last move highlight) */
            /* Ensure selected_row and selected_col are in valid range (0-7) */
            if (chess_gui.selected_row >= 0 && chess_gui.selected_row < 8 &&
                chess_gui.selected_col >= 0 && chess_gui.selected_col < 8) {
                
                if (chess_gui.piece_selected && chess_gui.selected_row == y && chess_gui.selected_col == x) {
                    color = COLOR_YELLOW;  /* Yellow highlight for selected piece */
                }
                
                /* Highlight cursor position for keyboard navigation */
                if (!chess_gui.piece_selected && chess_gui.selected_row == y && chess_gui.selected_col == x) {
                    color = COLOR_MAGENTA;  /* Magenta highlight for cursor position */
                }
            }
            
            rectfill(screen, screen_x, screen_y, screen_x + SQUARE_SIZE - 1, screen_y + SQUARE_SIZE - 1, color);
            
            /* Draw subtle border */
            int border_color = ((x + y) % 2 == 0) ? current_dark : current_light;
            rect(screen, screen_x, screen_y, screen_x + SQUARE_SIZE - 1, screen_y + SQUARE_SIZE - 1, border_color);
        }
    }
    
    /* Draw file labels (A-H) */
    const char *files = "ABCDEFGH";
    for (x = 0; x < 8; x++) {
        char buf[2] = {files[x], '\0'};
        textout_ex(screen, font, buf, BOARD_START_X + 18 + x * SQUARE_SIZE, BOARD_START_Y - 20, COLOR_WHITE, -1);
    }
    
    /* Draw rank labels (8-1) */
    for (y = 0; y < 8; y++) {
        snprintf(buf, 64, "%d", 8 - y);
        textout_ex(screen, font, buf, BOARD_START_X - 20, BOARD_START_Y + 15 + y * SQUARE_SIZE, COLOR_WHITE, -1);
    }
    
    /* Draw board border */
    rect(screen, BOARD_START_X - 1, BOARD_START_Y - 1, 
         BOARD_START_X + 8 * SQUARE_SIZE, BOARD_START_Y + 8 * SQUARE_SIZE, COLOR_WHITE);
}

void draw_piece_at_square(int x, int y, ChessPiece piece) {
    if (piece.type == EMPTY) return;
    
    int screen_x = BOARD_START_X + x * SQUARE_SIZE + SQUARE_SIZE / 2;
    int screen_y = BOARD_START_Y + y * SQUARE_SIZE + SQUARE_SIZE / 2;
    int piece_size = SQUARE_SIZE - 4;  // 46 pixels for 50px squares
    
    BITMAP *sprite = get_piece_sprite(piece.type, piece.color);
    if (sprite) {
        draw_piece_sprite(sprite, screen_x, screen_y, piece_size);
    }
}

void draw_pieces() {
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            ChessPiece piece = chess_gui.game.board[y][x];
            draw_piece_at_square(x, y, piece);
        }
    }
}

void draw_check_status() {
    /* Only draw if something needs to be displayed */
    bool should_draw = (chess_gui.check_display_timer > 0) || 
                       chess_gui.is_checkmate || 
                       chess_gui.is_stalemate;
    
    if (!should_draw) return;
    
    /* Determine what to display and its color */
    const char *text = NULL;
    int text_color = COLOR_WHITE;
    int box_color = COLOR_WHITE;
    
    if (chess_gui.is_checkmate) {
        text = "CHECKMATE";
        text_color = COLOR_YELLOW;  /* Gold-ish yellow */
        box_color = COLOR_YELLOW;
    } else if (chess_gui.is_stalemate) {
        text = "STALEMATE";
        text_color = COLOR_GRAY;
        box_color = COLOR_GRAY;
    } else if (chess_gui.check_display_timer > 0) {
        text = "CHECK";
        text_color = COLOR_RED;
        box_color = COLOR_RED;
    }
    
    if (!text) return;
    
    /* Calculate center of board */
    int board_center_x = BOARD_START_X + (8 * SQUARE_SIZE) / 2;
    int board_center_y = BOARD_START_Y + (8 * SQUARE_SIZE) / 2;
    
    /* Calculate text box size */
    int text_width = strlen(text) * 8;  /* Rough estimate for 8-pixel wide chars */
    int box_width = text_width + 40;
    int box_height = 60;
    int box_x = board_center_x - box_width / 2;
    int box_y = board_center_y - box_height / 2;
    
    /* Draw semi-transparent background box */
    /* Fill background */
    rectfill(screen, box_x, box_y, box_x + box_width, box_y + box_height, COLOR_BLACK);
    
    /* Draw border with status color */
    rect(screen, box_x, box_y, box_x + box_width, box_y + box_height, box_color);
    rect(screen, box_x + 1, box_y + 1, box_x + box_width - 1, box_y + box_height - 1, box_color);
    
    /* Draw text centered in box */
    textout_centre_ex(screen, font, text, board_center_x, board_center_y - 8,
                      text_color, -1);
}

void draw_help_screen() {
    int y = 50;
    
    rectfill(screen, 0, 0, 640, 480, COLOR_BLACK);
    
    draw_text_center(320, y, COLOR_YELLOW, "BeatChess - Help");
    y += 30;
    
    draw_text(100, y, COLOR_CYAN, "KEYBOARD SHORTCUTS:"); y += 20;
    draw_text(100, y, COLOR_WHITE, "N - New Game"); y += 15;
    draw_text(100, y, COLOR_WHITE, "U - Undo Move"); y += 15;
    draw_text(100, y, COLOR_WHITE, "A - Toggle AI Mode (Player vs AI / AI vs AI)"); y += 15;
    draw_text(100, y, COLOR_WHITE, "B - Swap Player Color (White/Black)"); y += 15;
    draw_text(100, y, COLOR_WHITE, "? - Show This Help"); y += 15;
    draw_text(100, y, COLOR_WHITE, "Q or ESC - Quit Game"); y += 25;
    
    draw_text(100, y, COLOR_CYAN, "MOUSE CONTROLS:"); y += 20;
    draw_text(100, y, COLOR_WHITE, "Click on a piece to select it"); y += 15;
    draw_text(100, y, COLOR_WHITE, "Click on a square to move the piece there"); y += 15;
    draw_text(100, y, COLOR_WHITE, "Click side buttons for quick actions"); y += 15;
    draw_text(100, y, COLOR_WHITE, "Click 'File' menu for game options"); y += 25;
    
    draw_text(100, y, COLOR_CYAN, "GAME MODES:"); y += 20;
    draw_text(100, y, COLOR_WHITE, "Player vs AI - Play against the computer"); y += 15;
    draw_text(100, y, COLOR_WHITE, "AI vs AI - Watch two AI players compete"); y += 25;
    
    draw_text_center(320, y + 20, COLOR_GREEN, "Press any key to continue...");
}

void draw_about_screen() {
    int y = 80;
    
    rectfill(screen, 0, 0, 640, 480, COLOR_BLACK);
    
    draw_text_center(320, y, COLOR_YELLOW, "BeatChess");
    y += 20;
    draw_text_center(320, y, COLOR_WHITE, "DOS Edition");
    y += 40;
    
    draw_text_center(320, y, COLOR_CYAN, "Copyright (c) 2025 Jason Brian Hall");
    y += 30;
    
    draw_text_center(320, y, COLOR_GREEN, "MIT License");
    y += 30;
    
    draw_text(80, y, COLOR_WHITE, "Permission is hereby granted, free of charge, to any person"); y += 15;
    draw_text(80, y, COLOR_WHITE, "obtaining a copy of this software and associated documentation"); y += 15;
    draw_text(80, y, COLOR_WHITE, "files (the \"Software\"), to deal in the Software without"); y += 15;
    draw_text(80, y, COLOR_WHITE, "restriction, including without limitation the rights to use,"); y += 15;
    draw_text(80, y, COLOR_WHITE, "copy, modify, merge, publish, distribute, sublicense, and/or"); y += 15;
    draw_text(80, y, COLOR_WHITE, "sell copies of the Software, and to permit persons to whom the"); y += 15;
    draw_text(80, y, COLOR_WHITE, "Software is furnished to do so, subject to the following"); y += 15;
    draw_text(80, y, COLOR_WHITE, "conditions:"); y += 25;
    
    draw_text(80, y, COLOR_WHITE, "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND."); y += 30;
    
    draw_text_center(320, y + 20, COLOR_GREEN, "Press any key to continue...");
}

/* ============================================================================
 * PGN Save/Load Functions (requires pgn.h and pgn.c to be included)
 * ============================================================================
 */

/* ============================================================================
 * File Browser Helper Functions
 * ============================================================================ */

/**
 * Scan directory for .sav files
 */
int scan_files(const char *pattern, FileList *file_list) {
    int count = 0;
    
    file_list->file_count = 0;
    file_list->selected_index = 0;
    file_list->scroll_offset = 0;

#ifdef DOS_BUILD
    /* DOS implementation using findfirst/findnext */
    struct ffblk ff;
    int done;
    
    done = findfirst(pattern, &ff, FA_ARCH);
    
    while (!done && count < MAX_FILES) {
        if (!(ff.ff_attrib & FA_DIREC)) {
            strncpy(file_list->files[count].filename, ff.ff_name, MAX_FILENAME_LEN - 1);
            file_list->files[count].filename[MAX_FILENAME_LEN - 1] = '\0';
            file_list->files[count].is_directory = 0;
            count++;
        }
        done = findnext(&ff);
    }

#elif defined(LINUX_BUILD)
    /* Linux/Unix implementation using opendir/readdir */
    DIR *dir;
    struct dirent *entry;
    char *dot;
    const char *ext = ".sav"; /* Default to .sav files */
    
    dir = opendir(".");
    if (!dir) {
        return 0;
    }
    
    while ((entry = readdir(dir)) != NULL && count < MAX_FILES) {
        /* Skip directories */
        if (entry->d_type == DT_DIR) {
            continue;
        }
        
        /* Check if file ends with .sav extension */
        dot = strrchr(entry->d_name, '.');
        if (dot && strcmp(dot, ext) == 0) {
            strncpy(file_list->files[count].filename, entry->d_name, MAX_FILENAME_LEN - 1);
            file_list->files[count].filename[MAX_FILENAME_LEN - 1] = '\0';
            file_list->files[count].is_directory = 0;
            count++;
        }
    }
    
    closedir(dir);

#endif
    
    file_list->file_count = count;
    return count;
}

/**
 * Draw the file list on screen
 */
void draw_file_list(FileList *file_list, int is_save_mode) {
    int y = FILE_LIST_Y;
    int i;
    const char *title = is_save_mode ? "SAVE GAME - Select file or create new" 
                                      : "LOAD GAME - Select a saved game";
    
    textout_centre_ex(screen, font, title, 320, 40, COLOR_YELLOW, -1);
    
    rect(screen, FILE_LIST_X - 2, FILE_LIST_Y - 2, 
         FILE_LIST_X + FILE_LIST_WIDTH + 2, FILE_LIST_Y + FILE_LIST_HEIGHT + 2, 
         COLOR_WHITE);
    
    rectfill(screen, FILE_LIST_X, FILE_LIST_Y - 20, 
             FILE_LIST_X + FILE_LIST_WIDTH, FILE_LIST_Y - 1, COLOR_CYAN);
    textout_ex(screen, font, "FILENAME", FILE_LIST_X + 10, FILE_LIST_Y - 18, 
               COLOR_BLACK, -1);
    
    for (i = 0; i < LIST_VISIBLE_ITEMS && (i + file_list->scroll_offset) < file_list->file_count; i++) {
        int file_idx = i + file_list->scroll_offset;
        int text_y = y + i * FILE_LIST_ITEM_HEIGHT;
        
        int bg_color = (file_idx == file_list->selected_index) ? COLOR_CYAN : COLOR_BLACK;
        int text_color = (file_idx == file_list->selected_index) ? COLOR_BLACK : COLOR_WHITE;
        
        rectfill(screen, FILE_LIST_X, text_y, 
                 FILE_LIST_X + FILE_LIST_WIDTH, text_y + FILE_LIST_ITEM_HEIGHT - 1, 
                 bg_color);
        
        char display_name[50];
        strncpy(display_name, file_list->files[file_idx].filename, 49);
        display_name[49] = '\0';
        
        textout_ex(screen, font, display_name, FILE_LIST_X + 10, text_y + 5, 
                   text_color, -1);
    }
    
    if (file_list->file_count == 0) {
        textout_centre_ex(screen, font, "(No saved games found)", 
                          FILE_LIST_X + FILE_LIST_WIDTH/2, 
                          FILE_LIST_Y + FILE_LIST_HEIGHT/2 - 10, 
                          COLOR_GRAY, -1);
        
        if (is_save_mode) {
            textout_centre_ex(screen, font, "Enter a filename to create a new save", 
                              FILE_LIST_X + FILE_LIST_WIDTH/2, 
                              FILE_LIST_Y + FILE_LIST_HEIGHT/2 + 10, 
                              COLOR_GRAY, -1);
        }
    }
    
    if (file_list->file_count > LIST_VISIBLE_ITEMS) {
        char scroll_msg[64];
        snprintf(scroll_msg, sizeof(scroll_msg), 
                 "File %d of %d (use UP/DOWN arrows)", 
                 file_list->selected_index + 1, 
                 file_list->file_count);
        textout_centre_ex(screen, font, scroll_msg, 320, 
                          FILE_LIST_Y + FILE_LIST_HEIGHT + 20, 
                          COLOR_CYAN, -1);
    }
}

/**
 * Draw input field for filename
 */
void draw_filename_input(const char *prompt, const char *input, int cursor_pos) {
    int input_y = FILE_LIST_Y + FILE_LIST_HEIGHT + 60;
    
    textout_ex(screen, font, prompt, FILE_LIST_X, input_y, COLOR_WHITE, -1);
    
    rect(screen, FILE_LIST_X, input_y + 20, 
         FILE_LIST_X + 400, input_y + 40, COLOR_WHITE);
    
    char display_input[50];
    strncpy(display_input, input, 49);
    display_input[49] = '\0';
    textout_ex(screen, font, display_input, FILE_LIST_X + 5, input_y + 22, 
               COLOR_YELLOW, -1);
    
    if (cursor_pos <= (int)strlen(input)) {
        textout_ex(screen, font, "_", FILE_LIST_X + 5 + cursor_pos * 8, input_y + 22, 
                   COLOR_CYAN, -1);
    }
}

/**
 * Draw help text
 */
void draw_dialog_help(int is_save_mode) {
    int help_y = FILE_LIST_Y + FILE_LIST_HEIGHT + 120;
    
    textout_ex(screen, font, "UP/DOWN - Browse files | CLICK - Select file", FILE_LIST_X, help_y, 
               COLOR_GREEN, -1);
    
    if (is_save_mode) {
        textout_ex(screen, font, "ENTER - Save with selected/new name", FILE_LIST_X, help_y + 15, 
                   COLOR_GREEN, -1);
    } else {
        textout_ex(screen, font, "ENTER - Load | DOUBLE-CLICK - Quick load", FILE_LIST_X, help_y + 15, 
                   COLOR_GREEN, -1);
    }
    
    textout_ex(screen, font, "ESC - Cancel", FILE_LIST_X, help_y + 30, 
               COLOR_GREEN, -1);
}

/**
 * Handle file list navigation
 */
void handle_file_list_input(FileList *file_list, int key) {
    switch (key) {
        case KEY_UP:
            if (file_list->selected_index > 0) {
                file_list->selected_index--;
                if (file_list->selected_index < file_list->scroll_offset) {
                    file_list->scroll_offset = file_list->selected_index;
                }
            }
            break;
            
        case KEY_DOWN:
            if (file_list->selected_index < file_list->file_count - 1) {
                file_list->selected_index++;
                if (file_list->selected_index >= file_list->scroll_offset + LIST_VISIBLE_ITEMS) {
                    file_list->scroll_offset = file_list->selected_index - LIST_VISIBLE_ITEMS + 1;
                }
            }
            break;
    }
}

/**
 * Enhanced Save Game Dialog with Mouse Click Support
 */
void dos_save_game_dialog() {
    FileList file_list;
    char custom_filename[MAX_FILENAME_LEN] = "";
    int input_pos = 0;
    int result = -1;
    bool dialog_dirty = true;
    int prev_mouse_b_dialog = 0;
    
    scan_files("*.sav", &file_list);
    
    while (1) {
        /* Update mouse position and button state */
        poll_mouse();
        
        /* Only redraw if needed */
        if (dialog_dirty) {
            clear_to_color(screen, COLOR_BLACK);
            
            draw_file_list(&file_list, 1);
            draw_filename_input("Or enter new filename (.sav will be added):", 
                               custom_filename, input_pos);
            draw_dialog_help(1);
            
            vsync();
            dialog_dirty = false;
        }
        
        /* Handle mouse clicks on file list */
        if ((mouse_b & 1) && !(prev_mouse_b_dialog & 1)) {  /* Left button just pressed */
            int mx = mouse_x;
            int my = mouse_y;
            
            /* Validate bounds */
            if (mx < 0) mx = 0;
            if (mx >= 640) mx = 639;
            if (my < 0) my = 0;
            if (my >= 480) my = 479;
            
            /* Check if clicking on file list items */
            if (mx >= FILE_LIST_X && mx < FILE_LIST_X + FILE_LIST_WIDTH &&
                my >= FILE_LIST_Y && my < FILE_LIST_Y + FILE_LIST_HEIGHT) {
                
                /* Calculate which file item was clicked */
                int item_index = (my - FILE_LIST_Y) / FILE_LIST_ITEM_HEIGHT;
                int file_index = item_index + file_list.scroll_offset;
                
                /* Validate the index */
                if (file_index >= 0 && file_index < file_list.file_count) {
                    file_list.selected_index = file_index;
                    memset(custom_filename, 0, sizeof(custom_filename));
                    input_pos = 0;
                    dialog_dirty = true;
                }
            }
        }
        
        prev_mouse_b_dialog = mouse_b;
        
        if (keypressed()) {
            int key = readkey();
            int key_code = key & 0xFF;
            int key_scan = key >> 8;
            
            if (key_scan == KEY_UP || key_scan == KEY_DOWN) {
                handle_file_list_input(&file_list, key_scan);
                memset(custom_filename, 0, sizeof(custom_filename));
                input_pos = 0;
                dialog_dirty = true;
                continue;
            }
            
            if (key_scan == KEY_ESC) {
                result = -1;
                break;
            }
            
            if (key_scan == KEY_ENTER || key_code == '\r') {
                if (strlen(custom_filename) > 0) {
                    result = -2;
                } else if (file_list.file_count > 0 && file_list.selected_index < file_list.file_count) {
                    result = file_list.selected_index;
                } else {
                    continue;
                }
                break;
            }
            
            if (key_scan == KEY_BACKSPACE && input_pos > 0) {
                input_pos--;
                custom_filename[input_pos] = '\0';
                dialog_dirty = true;
                continue;
            }
            
            if (key_code >= 32 && key_code <= 126 && input_pos < MAX_FILENAME_LEN - 5) {
                custom_filename[input_pos] = tolower(key_code);
                input_pos++;
                custom_filename[input_pos] = '\0';
                dialog_dirty = true;
                continue;
            }
        }
        
        rest(10);
    }
    
    if (result == -1) {
        mark_screen_needs_full_redraw();
        return;
    }
    
    char filename[MAX_FILENAME_LEN];
    if (result == -2) {
        snprintf(filename, sizeof(filename), "%s", custom_filename);
        if (!strstr(filename, ".sav")) {
            strcat(filename, ".sav");
        }
    } else {
        strncpy(filename, file_list.files[result].filename, sizeof(filename) - 1);
    }
    
    /* Show saving message */
    clear_to_color(screen, COLOR_BLACK);
    textout_centre_ex(screen, font, "Saving game...", 320, 200, COLOR_YELLOW, -1);
    vsync();
    rest(500);  /* Brief pause to show message */
    
    BeatChessVisualization chess_vis;
    chess_vis.game = chess_gui.game;
    chess_vis.move_history_count = chess_gui.move_history_count;
    chess_vis.move_count = chess_gui.move_count;
    
    for (int i = 0; i < chess_gui.move_history_count && i < MAX_MOVE_HISTORY * 2; i++) {
        chess_vis.move_history[i] = chess_gui.move_history[i];
    }
    
    clear_to_color(screen, COLOR_BLACK);
    if (pgn_export_game(&chess_vis, filename, "Human", "BeatChess AI")) {
        textout_centre_ex(screen, font, "Game saved successfully!", 320, 200, COLOR_GREEN, -1);
        char msg[256];
        snprintf(msg, sizeof(msg), "File: %s", filename);
        textout_centre_ex(screen, font, msg, 320, 220, COLOR_WHITE, -1);
    } else {
        textout_centre_ex(screen, font, "Error: Failed to save game", 320, 200, COLOR_RED, -1);
    }
    
    textout_centre_ex(screen, font, "Press any key or click to continue...", 320, 280, COLOR_YELLOW, -1);
    vsync();
    
    /* Wait for either key press or mouse click */
    int continue_pressed = 0;
    int prev_mouse_b_continue = 0;
    while (!continue_pressed) {
        poll_mouse();
        
        /* Check for mouse click */
        if ((mouse_b & 1) && !(prev_mouse_b_continue & 1)) {
            continue_pressed = 1;
        }
        prev_mouse_b_continue = mouse_b;
        
        /* Check for key press */
        if (keypressed()) {
            readkey();
            continue_pressed = 1;
        }
        
        rest(10);
    }
    mark_screen_needs_full_redraw();
}

/**
 * Enhanced Load Game Dialog with Mouse Click Support
 */
void dos_load_game_dialog() {
    FileList file_list;
    bool dialog_dirty = true;
    int prev_mouse_b_dialog = 0;
    
    scan_files("*.sav", &file_list);
    
    if (file_list.file_count == 0) {
        clear_to_color(screen, COLOR_BLACK);
        textout_centre_ex(screen, font, "LOAD GAME", 320, 50, COLOR_YELLOW, -1);
        textout_centre_ex(screen, font, "No saved games found", 320, 200, COLOR_WHITE, -1);
        textout_centre_ex(screen, font, "Press any key or click to continue...", 320, 280, COLOR_GREEN, -1);
        vsync();
        
        /* Wait for either key press or mouse click */
        int continue_pressed = 0;
        int prev_mouse_b_continue = 0;
        while (!continue_pressed) {
            poll_mouse();
            
            /* Check for mouse click */
            if ((mouse_b & 1) && !(prev_mouse_b_continue & 1)) {
                continue_pressed = 1;
            }
            prev_mouse_b_continue = mouse_b;
            
            /* Check for key press */
            if (keypressed()) {
                readkey();
                continue_pressed = 1;
            }
            
            rest(10);
        }
        mark_screen_needs_full_redraw();
        return;
    }
    
    while (1) {
        /* Update mouse position and button state */
        poll_mouse();
        
        /* Only redraw if needed */
        if (dialog_dirty) {
            clear_to_color(screen, COLOR_BLACK);
            
            draw_file_list(&file_list, 0);
            draw_dialog_help(0);
            
            vsync();
            dialog_dirty = false;
        }
        
        /* Handle mouse clicks on file list */
        if ((mouse_b & 1) && !(prev_mouse_b_dialog & 1)) {  /* Left button just pressed */
            int mx = mouse_x;
            int my = mouse_y;
            
            /* Validate bounds */
            if (mx < 0) mx = 0;
            if (mx >= 640) mx = 639;
            if (my < 0) my = 0;
            if (my >= 480) my = 479;
            
            /* Check if clicking on file list items */
            if (mx >= FILE_LIST_X && mx < FILE_LIST_X + FILE_LIST_WIDTH &&
                my >= FILE_LIST_Y && my < FILE_LIST_Y + FILE_LIST_HEIGHT) {
                
                /* Calculate which file item was clicked */
                int item_index = (my - FILE_LIST_Y) / FILE_LIST_ITEM_HEIGHT;
                int file_index = item_index + file_list.scroll_offset;
                
                /* Validate the index */
                if (file_index >= 0 && file_index < file_list.file_count) {
                    file_list.selected_index = file_index;
                    dialog_dirty = true;
                    /* Double-click detection: if same file clicked twice quickly, load it */
                    static int last_selected = -1;
                    static int double_click_counter = 0;
                    
                    if (last_selected == file_index) {
                        double_click_counter++;
                        if (double_click_counter >= 1) {
                            /* Double-click detected, load the file */
                            break;
                        }
                    } else {
                        last_selected = file_index;
                        double_click_counter = 0;
                    }
                }
            }
        }
        
        prev_mouse_b_dialog = mouse_b;
        
        if (keypressed()) {
            int key = readkey();
            int key_code = key & 0xFF;
            int key_scan = key >> 8;
            
            if (key_scan == KEY_UP || key_scan == KEY_DOWN) {
                handle_file_list_input(&file_list, key_scan);
                dialog_dirty = true;
                continue;
            }
            
            if (key_scan == KEY_ESC) {
                mark_screen_needs_full_redraw();
                return;
            }
            
            if (key_scan == KEY_ENTER || key_code == '\r') {
                if (file_list.selected_index < file_list.file_count) {
                    break;
                }
                continue;
            }
        }
        
        rest(10);
    }
    
    char filename[MAX_FILENAME_LEN];
    strncpy(filename, file_list.files[file_list.selected_index].filename, 
            sizeof(filename) - 1);
    
    /* Show loading message */
    clear_to_color(screen, COLOR_BLACK);
    textout_centre_ex(screen, font, "Loading game...", 320, 200, COLOR_YELLOW, -1);
    vsync();
    rest(500);  /* Brief pause to show message */
    
    BeatChessVisualization chess_vis;
    chess_vis.game = chess_gui.game;
    chess_vis.move_history_count = 0;
    chess_vis.move_count = 0;
    
    clear_to_color(screen, COLOR_BLACK);
    
    if (pgn_import_game(&chess_vis, filename)) {
        chess_gui.game = chess_vis.game;
        chess_gui.move_history_count = chess_vis.move_history_count;
        chess_gui.move_count = chess_vis.move_count;
        chess_gui.history_size = chess_vis.move_history_count + 1;
        chess_gui.move_history_index = chess_gui.history_size;
        
        for (int i = 0; i < chess_vis.move_history_count && i < MAX_MOVE_HISTORY * 2; i++) {
            chess_gui.move_history[i] = chess_vis.move_history[i];
        }
        
        /* Initialize and recalculate timers based on loaded game */
        recalculate_timers_from_history();
        
        textout_centre_ex(screen, font, "Game loaded successfully!", 320, 200, COLOR_GREEN, -1);
        char msg[256];
        snprintf(msg, sizeof(msg), "Loaded %d moves from %s", 
                 chess_gui.move_history_count, filename);
        textout_centre_ex(screen, font, msg, 320, 220, COLOR_WHITE, -1);
    } else {
        textout_centre_ex(screen, font, "Error: Failed to load game", 320, 200, COLOR_RED, -1);
        textout_centre_ex(screen, font, "Check filename and format", 320, 220, COLOR_RED, -1);
    }
    
    textout_centre_ex(screen, font, "Press any key or click to continue...", 320, 280, COLOR_YELLOW, -1);
    vsync();
    
    /* Wait for either key press or mouse click */
    int continue_pressed = 0;
    int prev_mouse_b_continue = 0;
    while (!continue_pressed) {
        poll_mouse();
        
        /* Check for mouse click */
        if ((mouse_b & 1) && !(prev_mouse_b_continue & 1)) {
            continue_pressed = 1;
        }
        prev_mouse_b_continue = mouse_b;
        
        /* Check for key press */
        if (keypressed()) {
            readkey();
            continue_pressed = 1;
        }
        
        rest(10);
    }
    
    mark_screen_needs_full_redraw();
}

/* ============================================================================
 * Menu and button handling
 * ============================================================================
 */

int execute_menu_action(int menu_type, int index) {
    if (menu_type == 0) {  /* File menu */
        switch (index) {
            case 0:  /* New Game */
                init_chess_game();
                chess_gui.ai_move_counter = 0;
                return 1;  /* Continue */
                
            case 1:  /* Undo Move */
                undo_move();
                chess_gui.ai_move_counter = 0;
                return 1;  /* Continue */
                
            case 3:  /* Save Game */
                dos_save_game_dialog();
                return 1;  /* Continue */
                
            case 4:  /* Load Game */
                dos_load_game_dialog();
                return 1;  /* Continue */
                
            case 6:  /* AI vs AI */
                chess_gui.ai_vs_ai = !chess_gui.ai_vs_ai;
                chess_gui.ai_move_counter = 0;
                return 1;  /* Continue */
                
            case 7:  /* Swap Color */
                chess_gui.player_is_white = !chess_gui.player_is_white;
                chess_gui.ai_move_counter = 0;
                return 1;  /* Continue */
                
            case 9:  /* Quit */
                return 0;  /* Signal quit */
        }
    } else if (menu_type == 1) {  /* Help menu */
        switch (index) {
            case 0:  /* Help */
                chess_gui.show_help = true;
                return 1;  /* Continue */
                
            case 1:  /* About */
                chess_gui.show_about = true;
                return 1;  /* Continue */
        }
    }
    return 1;  /* Continue by default */
}

int handle_menu_click(int mx, int my) {
    /* Check if clicking menu bar */
    if (my < MENU_BAR_HEIGHT) {
        /* File menu button (at x=0-50) */
        if (mx < 50) {
            chess_gui.show_menu = !chess_gui.show_menu;
            show_help_menu_dropdown = false;  /* Close Help menu if open */
            chess_gui.menu_selected = -1;
            mark_screen_dirty();  /* Mark dirty when menu opens/closes */
            return 1;  /* Continue */
        }
        /* Help menu button (at x=50-100) */
        if (mx >= 50 && mx < 100) {
            show_help_menu_dropdown = !show_help_menu_dropdown;
            chess_gui.show_menu = false;  /* Close File menu if open */
            chess_gui.menu_selected = -1;
            mark_screen_dirty();  /* Mark dirty when menu opens/closes */
            return 1;  /* Continue */
        }
        /* Clicked elsewhere on menu bar - close menus if open */
        if (chess_gui.show_menu || show_help_menu_dropdown) {
            chess_gui.show_menu = false;
            show_help_menu_dropdown = false;
            chess_gui.menu_selected = -1;
            mark_screen_dirty();  /* Mark dirty when menu closes */
        }
        return 1;  /* Continue */
    }
    
    /* Check if clicking File menu items */
    if (chess_gui.show_menu) {
        int menu_x = 0;
        int menu_y = MENU_BAR_HEIGHT;
        int menu_w = 200;  
        int item_h = 20;
        
        if (mx >= menu_x && mx < menu_x + menu_w &&
            my >= menu_y && my < menu_y + NUM_FILE_MENU_ITEMS * item_h) {
            int item = (my - menu_y) / item_h;
            /* Bounds check */
            if (item >= 0 && item < NUM_FILE_MENU_ITEMS) {
                /* Check if it's not a separator */
                if (strlen(file_menu_items[item]) > 0) {
                    int result = execute_menu_action(0, item);
                    chess_gui.show_menu = false;
                    chess_gui.menu_selected = -1;
                    mark_screen_dirty();  /* Mark dirty after menu action */
                    return result;  /* Return the result (0=quit, 1=continue) */
                }
            }
        } else {
            /* Clicked outside menu - close it */
            chess_gui.show_menu = false;
            chess_gui.menu_selected = -1;
            mark_screen_dirty();  /* Mark dirty when menu closes */
            return 1;  /* Continue */
        }
    }
    
    /* Check if clicking Help menu items */
    if (show_help_menu_dropdown) {
        int menu_x = 50;
        int menu_y = MENU_BAR_HEIGHT;
        int menu_w = 150;  
        int item_h = 20;
        
        if (mx >= menu_x && mx < menu_x + menu_w &&
            my >= menu_y && my < menu_y + NUM_HELP_MENU_ITEMS * item_h) {
            int item = (my - menu_y) / item_h;
            /* Bounds check */
            if (item >= 0 && item < NUM_HELP_MENU_ITEMS) {
                int result = execute_menu_action(1, item);
                show_help_menu_dropdown = false;
                chess_gui.menu_selected = -1;
                mark_screen_dirty();  /* Mark dirty after menu action */
                return result;
            }
        } else {
            /* Clicked outside menu - close it */
            show_help_menu_dropdown = false;
            chess_gui.menu_selected = -1;
            mark_screen_dirty();  /* Mark dirty when menu closes */
            return 1;  /* Continue */
        }
    }
    
    return 1;  /* Continue by default */
}

bool handle_button_click(int mx, int my) {
    for (int i = 0; i < NUM_BUTTONS; i++) {
        if (side_buttons[i].enabled && 
            point_in_rect(mx, my, side_buttons[i].x, side_buttons[i].y,
                         side_buttons[i].w, side_buttons[i].h)) {
            
            /* Simulate keypress for the button's hotkey */
            int key = side_buttons[i].hotkey;
            switch (key) {
                case 'N': 
                    init_chess_game(); 
                    chess_gui.ai_move_counter = 0; 
                    mark_screen_needs_full_redraw();
                    break;
                case 'U': 
                    undo_move(); 
                    chess_gui.ai_move_counter = 0; 
                    mark_screen_dirty();
                    break;
                case 'A': 
                    chess_gui.ai_vs_ai = !chess_gui.ai_vs_ai; 
                    chess_gui.ai_move_counter = 0; 
                    mark_screen_dirty();
                    break;
                case 'B': 
                    chess_gui.player_is_white = !chess_gui.player_is_white; 
                    chess_gui.ai_move_counter = 0; 
                    mark_screen_dirty();
                    break;
                case '?': 
                    chess_gui.show_help = true; 
                    mark_screen_needs_full_redraw();
                    break;
                case 'Q': return false;  /* Signal to quit */
            }
            return true;
        }
    }
    return true;
}

void update_menu_selection(int my) {
    int prev_menu_selected = chess_gui.menu_selected;  /* Track previous selection */
    
    /* Safety check input */
    if (my < 0 || my >= 480) {
        chess_gui.menu_selected = -1;
    } else if (chess_gui.show_menu) {
        int menu_y = MENU_BAR_HEIGHT;
        int item_h = 20;
        int max_menu_y = menu_y + NUM_FILE_MENU_ITEMS * item_h;
        
        /* Bounds check */
        if (my >= menu_y && my < max_menu_y) {
            int item = (my - menu_y) / item_h;
            /* Double bounds check and skip separators */
            if (item >= 0 && item < NUM_FILE_MENU_ITEMS) {
                if (strlen(file_menu_items[item]) > 0) {
                    chess_gui.menu_selected = item;
                } else {
                    chess_gui.menu_selected = -1;
                }
            } else {
                chess_gui.menu_selected = -1;
            }
        } else {
            chess_gui.menu_selected = -1;
        }
    } else if (show_help_menu_dropdown) {
        int menu_y = MENU_BAR_HEIGHT;
        int item_h = 20;
        int max_menu_y = menu_y + NUM_HELP_MENU_ITEMS * item_h;
        
        /* Bounds check */
        if (my >= menu_y && my < max_menu_y) {
            int item = (my - menu_y) / item_h;
            if (item >= 0 && item < NUM_HELP_MENU_ITEMS) {
                chess_gui.menu_selected = item;
            } else {
                chess_gui.menu_selected = -1;
            }
        } else {
            chess_gui.menu_selected = -1;
        }
    } else {
        chess_gui.menu_selected = -1;
    }
    
    /* Mark screen dirty if menu selection changed (for hover highlighting) */
    if (chess_gui.menu_selected != prev_menu_selected) {
        mark_screen_dirty();
    }
}

/* ============================================================================
 * Close Button Callback (Linux only)
 * ============================================================================
 */
void close_button_callback(void) {
    window_close_requested = true;
}

/* ============================================================================
 * Main function
 * ============================================================================
 */

int main(void) {

    /* Initialize Allegro */
    if (allegro_init() != 0) {
        printf("Failed to initialize Allegro\n");
        return 1;
    }
    
    install_keyboard();
    
    install_mouse();
    install_timer();
    init_chess_game();
    /* Seed random number generator with current time for variety in AI moves */
    
    /* Set graphics mode */
    int gfx_result = -1;
    
    #ifdef __linux__
    /* On Linux, try windowed mode first */
    gfx_result = set_gfx_mode(GFX_AUTODETECT_WINDOWED, 640, 480, 0, 0);
    if (gfx_result != 0) {
        /* Fallback to autodetect if windowed fails */
        gfx_result = set_gfx_mode(GFX_AUTODETECT, 640, 480, 0, 0);
    }
    #else
    /* On DOS, use fullscreen autodetect */
    gfx_result = set_gfx_mode(GFX_AUTODETECT, 640, 480, 0, 0);
    #endif
    
    if (gfx_result != 0) {
        printf("Error setting graphics mode\n");
        return 1;
    }
    
    /* Register close button callback (Linux only) */
    #ifdef LINUX_BUILD
    set_close_button_callback(close_button_callback);
    #endif
    
    /* Set up custom palette colors for better board appearance */
    RGB light_square, dark_square;
    light_square.r = 58;  /* Beige/cream - light square */
    light_square.g = 54;
    light_square.b = 47;
    dark_square.r = 35;   /* Dark green - dark square */
    dark_square.g = 45;
    dark_square.b = 35;
    
    set_color(15, &light_square);  /* Set color 15 to light beige */
    set_color(46, &dark_square);   /* Set color 46 to dark green */
    
    /* Show mouse cursor on screen */
    show_mouse(screen);
    
    /* Create backbuffer for double buffering */
    /* Initialize triple buffering */
    init_triple_buffers();
    
    /* Keep backbuffer reference for compatibility with splash screen */
    BITMAP *backbuffer = buffers[0];
    if (!backbuffer) {
        printf("Error creating backbuffer\n");
        allegro_exit();
        return 1;
    }
    
    /* Load chess piece sprites from embedded data */
    printf("Loading chess piece sprites...\n");
    if (load_chess_pieces() != 0) {
        printf("Error loading chess piece sprites!\n");
        destroy_bitmap(backbuffer);
        allegro_exit();
        return 1;
    }
    printf("Chess pieces loaded successfully!\n");
    
    /* Display splash screen */
    printf("Displaying splash screen...\n");
    show_splash_screen(backbuffer);
    
    bool running = true;
    int prev_mouse_b = 0;  /* Track previous mouse button state */
    
    /* Game loop */
    while (running) {
        srand((unsigned int)time(NULL));

        /* Check for window close button (Linux only) */
        #ifdef LINUX_BUILD
        if (window_close_requested) {
            running = false;
        }
        #endif
        
        /* Update check/checkmate/stalemate display */
        
        /* Detect check (only during actual gameplay) */
        bool in_check = chess_is_in_check(&chess_gui.game, chess_gui.game.turn);
        if (in_check && !chess_gui.is_in_check) {
            /* Transition from not-in-check to in-check */
            chess_gui.is_in_check = true;
            chess_gui.check_display_timer = 1.0;  /* Display "CHECK" for 1 second */
        } else if (!in_check) {
            chess_gui.is_in_check = false;
            chess_gui.check_display_timer = 0;  /* Hide the display */
        }
        
        /* Count down the check display timer */
        if (chess_gui.check_display_timer > 0) {
            chess_gui.check_display_timer -= 0.01;  /* ~10ms per frame */
            if (chess_gui.check_display_timer < 0) {
                chess_gui.check_display_timer = 0;
            }
        }
        
        /* Check if it's AI's turn */
        bool ai_should_move = false;
        if (chess_gui.ai_vs_ai) {
            ai_should_move = true;
        } else {
            /* Player vs AI mode */
            ChessColor player_color = chess_gui.player_is_white ? WHITE : BLACK;
            ai_should_move = (chess_gui.game.turn != player_color);
        }
        
        /* AI move logic */
        if (ai_should_move && !chess_gui.ai_thinking) {
            chess_gui.ai_move_counter++;
            
            if (chess_gui.ai_move_counter >= chess_gui.ai_move_delay) {
                chess_gui.ai_thinking = true;
                chess_gui.ai_computing = true;  /* Flag that we're computing */
                chess_gui.ai_move_counter = 0;
                
                /* Record retrace count before AI starts (for accurate timing even when blocking) */
                int start_retrace = retrace_count;
                
                /* Make a COPY of the game state for AI to analyze */
                /* This prevents race conditions with drawing/display code */
                ChessGameState game_copy = chess_gui.game;
                
                /* In DOS, AI computation will block, but this is necessary for strong play.
                 * The AI evaluates thousands of positions via minimax search.
                 * On modern CPUs via DOSBox, this typically takes 1-2 seconds. */
                clock_t ai_start_time = clock();
                ChessMove ai_move = compute_ai_move();
                clock_t ai_end_time = clock();
                
                chess_gui.ai_computing = false;  /* Done computing */
                
                /* Calculate elapsed time during AI thinking using system clock */
                if (chess_gui.timer_started) {
                    /* Convert clock ticks to milliseconds */
                    long elapsed_ms = (long)((ai_end_time - ai_start_time) * 1000.0 / CLOCKS_PER_SEC);
                    
                    /* Add elapsed time to the appropriate player's time */
                    if (chess_gui.game.turn == WHITE) {
                        chess_gui.white_time_milliseconds += elapsed_ms;
                    } else {
                        chess_gui.black_time_milliseconds += elapsed_ms;
                    }
                }
                
                /* Validate and make move */
                if (chess_is_valid_move(&chess_gui.game, 
                                       ai_move.from_row, ai_move.from_col,
                                       ai_move.to_row, ai_move.to_col)) {
                    /* Verify move doesn't leave king in check */
                    ChessGameState temp = chess_gui.game;
                    chess_make_move(&temp, ai_move);
                    
                    if (!chess_is_in_check(&temp, chess_gui.game.turn)) {
                        /* Record this move for highlighting */
                        chess_gui.last_move_from_row = ai_move.from_row;
                        chess_gui.last_move_from_col = ai_move.from_col;
                        chess_gui.last_move_to_row = ai_move.to_row;
                        chess_gui.last_move_to_col = ai_move.to_col;
                        chess_gui.has_last_move = true;
                        
                        chess_make_move(&chess_gui.game, ai_move);
                        save_position_to_history_with_move(ai_move.from_row, ai_move.from_col, 
                                                            ai_move.to_row, ai_move.to_col);
                        chess_gui.move_count++;  /* Increment move counter after move is committed */
                        
                        /* Update total time for the player who just moved */
                        if (chess_gui.game.turn == WHITE) {
                            /* Black just moved - save black's total time and reset timer for white */
                            clock_t current_time = clock();
                            long elapsed_ms = (long)((current_time - chess_gui.current_move_start_time) * 1000.0 / CLOCKS_PER_SEC);
                            chess_gui.black_total_time += (elapsed_ms / 1000.0);  /* Convert ms to seconds before adding */
                            chess_gui.current_move_start_time = current_time;
                        } else {
                            /* White just moved - save white's total time and reset timer for black */
                            clock_t current_time = clock();
                            long elapsed_ms = (long)((current_time - chess_gui.current_move_start_time) * 1000.0 / CLOCKS_PER_SEC);
                            chess_gui.white_total_time += (elapsed_ms / 1000.0);  /* Convert ms to seconds before adding */
                            chess_gui.current_move_start_time = current_time;
                        }
                        
                        mark_screen_dirty();  /* Mark screen dirty after AI move */
                        
                        /* Check for checkmate or stalemate */
                        if (!chess_is_in_check(&chess_gui.game, chess_gui.game.turn)) {
                            /* Current player is not in check - check if they have any legal moves */
                            ChessMove test_moves[256];
                            int num_moves = chess_get_all_moves(&chess_gui.game, chess_gui.game.turn, test_moves);
                            
                            /* Check if all moves are illegal (leave king in check) */
                            bool has_legal_move = false;
                            for (int i = 0; i < num_moves; i++) {
                                ChessGameState temp = chess_gui.game;
                                chess_make_move(&temp, test_moves[i]);
                                if (!chess_is_in_check(&temp, chess_gui.game.turn)) {
                                    has_legal_move = true;
                                    break;
                                }
                            }
                            
                            if (!has_legal_move) {
                                /* Stalemate */
                                chess_gui.is_stalemate = true;
                            }
                        } else {
                            /* Current player is in check - check if they have any legal moves */
                            ChessMove test_moves[256];
                            int num_moves = chess_get_all_moves(&chess_gui.game, chess_gui.game.turn, test_moves);
                            
                            /* Check if all moves are illegal (leave king in check) */
                            bool has_legal_move = false;
                            for (int i = 0; i < num_moves; i++) {
                                ChessGameState temp = chess_gui.game;
                                chess_make_move(&temp, test_moves[i]);
                                if (!chess_is_in_check(&temp, chess_gui.game.turn)) {
                                    has_legal_move = true;
                                    break;
                                }
                            }
                            
                            if (!has_legal_move) {
                                /* Checkmate */
                                chess_gui.is_checkmate = true;
                            }
                        }
                    }
                }
                
                chess_gui.ai_thinking = false;
                chess_gui.piece_selected = false;
            }
        }
        
        /* Update mouse position for menu highlighting */
        poll_mouse();
        
        /* Mark screen dirty if mouse moved */
        if (mouse_x != prev_mouse_x || mouse_y != prev_mouse_y) {
            mark_screen_dirty();
            prev_mouse_x = mouse_x;
            prev_mouse_y = mouse_y;
        }
        
        /* Bounds check mouse position before using it */
        int safe_mouse_y = mouse_y;
        if (safe_mouse_y < 0) safe_mouse_y = 0;
        if (safe_mouse_y >= 480) safe_mouse_y = 479;
        
        /* Only update menu selection, not board cursor (board cursor updates on click) */
        update_menu_selection(safe_mouse_y);
        
        /* Update timers - accumulate elapsed time since last move */
        if (chess_gui.timer_started) {
            clock_t current_time = clock();
            long elapsed_ms = (long)((current_time - chess_gui.current_move_start_time) * 1000.0 / CLOCKS_PER_SEC);
            
            /* Add elapsed time to the current player's time */
            if (chess_gui.game.turn == WHITE) {
                /* Convert white_total_time from seconds to milliseconds and add elapsed */
                chess_gui.white_time_milliseconds = (long)(chess_gui.white_total_time * 1000.0) + elapsed_ms;
            } else {
                /* Convert black_total_time from seconds to milliseconds and add elapsed */
                chess_gui.black_time_milliseconds = (long)(chess_gui.black_total_time * 1000.0) + elapsed_ms;
            }
        }
        
        /* Draw to active buffer (mouse cursor is not drawn to buffer) */
        BITMAP *prev_target = screen;
        screen = active_buffer;
        
        /* Only redraw if screen is dirty or needs full redraw */
        if (screen_is_dirty || screen_needs_full_redraw) {
            /* Clear active buffer only if needed */
            if (screen_needs_full_redraw) {
                clear_to_color(active_buffer, COLOR_BLACK);
                screen_needs_full_redraw = false;
            } else {
                clear_to_color(active_buffer, COLOR_BLACK);
            }
            
            /* Draw game (no mouse cursor interference) */
            if (chess_gui.show_help) {
                draw_help_screen();
            } else if (chess_gui.show_about) {
                draw_about_screen();
            } else {
                draw_board();
                draw_pieces();
                draw_check_status();  /* Draw check/checkmate/stalemate overlay */
                draw_side_panel();
                draw_menu_bar();  /* Draw menu last so it appears on top */
            }
        }
        
        /* Restore screen target */
        screen = prev_target;
        
        /* Triple buffer: swap to next buffer with vsync (only blits if dirty) */
        scare_mouse();  /* Temporarily disable mouse drawing */
        get_next_buffer_and_swap();  /* Handles vsync and buffer rotation */
        unscare_mouse();  /* Re-enable mouse drawing */
        
        /* Handle input */
        #ifdef LINUX_BUILD
        /* On Linux, poll keyboard */
        poll_keyboard();
        
        /* Try both keypressed() AND key[] array as fallback */
        int key_input = 0;
        int key_code = 0;
        int key_scancode = 0;
        bool has_key = false;
        
        /* Check keypressed() first */
        if (keypressed()) {
            key_input = readkey();
            key_code = key_input & 0xFF;
            key_scancode = key_input >> 8;
            has_key = true;
        } else {
            /* Fallback: check key[] array directly */
            if (key[KEY_UP] || key[KEY_DOWN] || key[KEY_LEFT] || key[KEY_RIGHT] ||
                key[KEY_ESC] || key[KEY_ENTER] || key[KEY_BACKSPACE]) {
                
                if (key[KEY_UP]) key_scancode = KEY_UP;
                else if (key[KEY_DOWN]) key_scancode = KEY_DOWN;
                else if (key[KEY_LEFT]) key_scancode = KEY_LEFT;
                else if (key[KEY_RIGHT]) key_scancode = KEY_RIGHT;
                else if (key[KEY_ESC]) key_scancode = KEY_ESC;
                else if (key[KEY_ENTER]) key_scancode = KEY_ENTER;
                else if (key[KEY_BACKSPACE]) key_scancode = KEY_BACKSPACE;
                has_key = true;
            } else {
                /* Check ASCII keys */
                for (int i = 32; i < 127; i++) {
                    if (key[i]) {
                        key_code = i;
                        has_key = true;
                        break;
                    }
                }
            }
        }
        
        if (has_key) {
        #else
        /* On DOS, use traditional keypressed()/readkey() */
        if (keypressed()) {
            int key_input = readkey();
            int key_code = key_input & 0xFF;
            int key_scancode = key_input >> 8;
        #endif
            
            /* If showing help, any key returns to game */
            if (chess_gui.show_help) {
                chess_gui.show_help = false;
                mark_screen_needs_full_redraw();
                continue;
            }
            
            /* If showing about, any key returns to game */
            if (chess_gui.show_about) {
                chess_gui.show_about = false;
                mark_screen_needs_full_redraw();
                continue;
            }
            
            /* Handle board navigation with arrow keys and WASD */
            if (!ai_should_move) {
                /* Arrow keys and WASD for movement */
                bool is_movement = false;
                switch (key_scancode) {
                    case KEY_UP:
                        if (chess_gui.selected_row > 0) {
                            chess_gui.selected_row--;
                        } else {
                            chess_gui.selected_row = 0;
                        }
                        is_movement = true;
                        break;
                        
                    case KEY_DOWN:
                        if (chess_gui.selected_row < 7) {
                            chess_gui.selected_row++;
                        } else {
                            chess_gui.selected_row = 7;
                        }
                        is_movement = true;
                        break;
                        
                    case KEY_LEFT:
                        if (chess_gui.selected_col > 0) {
                            chess_gui.selected_col--;
                        } else {
                            chess_gui.selected_col = 0;
                        }
                        is_movement = true;
                        break;
                        
                    case KEY_RIGHT:
                        if (chess_gui.selected_col < 7) {
                            chess_gui.selected_col++;
                        } else {
                            chess_gui.selected_col = 7;
                        }
                        is_movement = true;
                        break;
                }
                
                /* WASD movement (check ASCII for W/A/S/D) */
                if (!is_movement) {
                    switch (key_code) {
                        case 'w':
                        case 'W':
                            if (chess_gui.selected_row > 0) {
                                chess_gui.selected_row--;
                            } else {
                                chess_gui.selected_row = 0;
                            }
                            is_movement = true;
                            break;
                            
                        case 's':
                        case 'S':
                            if (chess_gui.selected_row < 7) {
                                chess_gui.selected_row++;
                            } else {
                                chess_gui.selected_row = 7;
                            }
                            is_movement = true;
                            break;
                            
                        case 'a':
                        case 'A':
                            if (chess_gui.selected_col > 0) {
                                chess_gui.selected_col--;
                            } else {
                                chess_gui.selected_col = 0;
                            }
                            is_movement = true;
                            break;
                            
                        case 'd':
                        case 'D':
                            if (chess_gui.selected_col < 7) {
                                chess_gui.selected_col++;
                            } else {
                                chess_gui.selected_col = 7;
                            }
                            is_movement = true;
                            break;
                    }
                }
                
                if (is_movement) {
                    mark_screen_dirty();
                    continue;  /* Skip further key processing */
                }
            }
            
            /* Initialize cursor position on first movement key */
            if (chess_gui.selected_row < 0 && (key_scancode == KEY_UP || key_scancode == KEY_DOWN ||
                                                key_scancode == KEY_LEFT || key_scancode == KEY_RIGHT ||
                                                key_code == 'w' || key_code == 'W' ||
                                                key_code == 'a' || key_code == 'A' ||
                                                key_code == 's' || key_code == 'S' ||
                                                key_code == 'd' || key_code == 'D')) {
                chess_gui.selected_row = 0;
                chess_gui.selected_col = 0;
                continue;
            }
            
            /* Handle Enter key for selection/movement */
            if (key_code == 13) {  /* Enter key */
                if (!ai_should_move && chess_gui.selected_row >= 0) {
                    ChessPiece piece = chess_gui.game.board[chess_gui.selected_row][chess_gui.selected_col];
                    
                    if (!chess_gui.piece_selected && piece.type != EMPTY && 
                        piece.color == chess_gui.game.turn) {
                        /* Select piece - remember where it was selected from */
                        chess_gui.piece_selected_row = chess_gui.selected_row;
                        chess_gui.piece_selected_col = chess_gui.selected_col;
                        chess_gui.piece_selected = true;
                    } else if (chess_gui.piece_selected) {
                        /* Try to move piece from selected position to current position */
                        if (chess_is_valid_move(&chess_gui.game,
                                               chess_gui.piece_selected_row,
                                               chess_gui.piece_selected_col,
                                               chess_gui.selected_row,
                                               chess_gui.selected_col)) {
                            
                            ChessGameState temp = chess_gui.game;
                            ChessMove move = {chess_gui.piece_selected_row,
                                             chess_gui.piece_selected_col,
                                             chess_gui.selected_row,
                                             chess_gui.selected_col, 0};
                            chess_make_move(&temp, move);
                            
                            if (!chess_is_in_check(&temp, chess_gui.game.turn)) {
                                chess_gui.last_move_from_row = chess_gui.piece_selected_row;
                                chess_gui.last_move_from_col = chess_gui.piece_selected_col;
                                chess_gui.last_move_to_row = chess_gui.selected_row;
                                chess_gui.last_move_to_col = chess_gui.selected_col;
                                chess_gui.has_last_move = true;
                                
                                chess_make_move(&chess_gui.game, move);
                                save_position_to_history_with_move(chess_gui.piece_selected_row, 
                                                                    chess_gui.piece_selected_col,
                                                                    chess_gui.selected_row, 
                                                                    chess_gui.selected_col);
                                chess_gui.ai_move_counter = 0;
                                
                                /* Check for checkmate or stalemate */
                                if (!chess_is_in_check(&chess_gui.game, chess_gui.game.turn)) {
                                    /* Current player is not in check - check if they have any legal moves */
                                    ChessMove test_moves[256];
                                    int num_moves = chess_get_all_moves(&chess_gui.game, chess_gui.game.turn, test_moves);
                                    
                                    /* Check if all moves are illegal (leave king in check) */
                                    bool has_legal_move = false;
                                    for (int i = 0; i < num_moves; i++) {
                                        ChessGameState temp = chess_gui.game;
                                        chess_make_move(&temp, test_moves[i]);
                                        if (!chess_is_in_check(&temp, chess_gui.game.turn)) {
                                            has_legal_move = true;
                                            break;
                                        }
                                    }
                                    
                                    if (!has_legal_move) {
                                        /* Stalemate */
                                        chess_gui.is_stalemate = true;
                                    }
                                } else {
                                    /* Current player is in check - check if they have any legal moves */
                                    ChessMove test_moves[256];
                                    int num_moves = chess_get_all_moves(&chess_gui.game, chess_gui.game.turn, test_moves);
                                    
                                    /* Check if all moves are illegal (leave king in check) */
                                    bool has_legal_move = false;
                                    for (int i = 0; i < num_moves; i++) {
                                        ChessGameState temp = chess_gui.game;
                                        chess_make_move(&temp, test_moves[i]);
                                        if (!chess_is_in_check(&temp, chess_gui.game.turn)) {
                                            has_legal_move = true;
                                            break;
                                        }
                                    }
                                    
                                    if (!has_legal_move) {
                                        /* Checkmate */
                                        chess_gui.is_checkmate = true;
                                    }
                                }
                            }
                        }
                        
                        chess_gui.piece_selected = false;
                        chess_gui.piece_selected_row = -1;
                        chess_gui.piece_selected_col = -1;
                    }
                }
                continue;
            }
            
            /* Handle ESC to deselect piece or cancel */
            if (key_code == 27) {  /* ESC */
                chess_gui.piece_selected = false;
                chess_gui.piece_selected_row = -1;
                chess_gui.piece_selected_col = -1;
                mark_screen_dirty();
                continue;
            }
            
            switch (key_code) {
                case 'q':
                case 'Q':
                    running = false;
                    break;
                    
                case 'n':
                case 'N':
                    init_chess_game();
                    chess_gui.ai_move_counter = 0;
                    mark_screen_needs_full_redraw();
                    break;
                    
                case 'u':
                case 'U':
                    undo_move();
                    chess_gui.ai_move_counter = 0;
                    mark_screen_dirty();
                    break;
                    
                case 'b':
                case 'B':
                    chess_gui.player_is_white = !chess_gui.player_is_white;
                    chess_gui.ai_move_counter = 0;
                    mark_screen_dirty();
                    break;
                    
                case '?':
                    chess_gui.show_help = true;
                    mark_screen_needs_full_redraw();
                    break;
                    
                default:
                    /* Check for function keys for board colors */
                    switch (key_scancode) {
                        case KEY_F5:
                            /* F5: Black color next */
                            dark_color_idx = (dark_color_idx + 1) % DARK_PALETTE_SIZE;
                            mark_screen_dirty();
                            break;
                            
                        case KEY_F6:
                            /* F6: Black color previous */
                            dark_color_idx = (dark_color_idx - 1 + DARK_PALETTE_SIZE) % DARK_PALETTE_SIZE;
                            mark_screen_dirty();
                            break;
                            
                        case KEY_F7:
                            /* F7: White color next */
                            light_color_idx = (light_color_idx + 1) % LIGHT_PALETTE_SIZE;
                            mark_screen_dirty();
                            break;
                            
                        case KEY_F8:
                            /* F8: White color previous */
                            light_color_idx = (light_color_idx - 1 + LIGHT_PALETTE_SIZE) % LIGHT_PALETTE_SIZE;
                            mark_screen_dirty();
                            break;
                    }
                    break;
            }
        }
        
        /* Handle mouse clicks */
        if ((mouse_b & 1) && !(prev_mouse_b & 1)) {  /* Left button just pressed */
            mark_screen_dirty();  /* Mark dirty on any mouse click */
            
            /* CRITICAL: Bounds check mouse coordinates FIRST */
            int mx = mouse_x;
            int my = mouse_y;
            
            /* Validate mouse coordinates are within screen bounds */
            if (mx < 0) mx = 0;
            if (mx >= 640) mx = 639;
            if (my < 0) my = 0;
            if (my >= 480) my = 479;
            
            /* If showing help, click returns to game */
            if (chess_gui.show_help) {
                chess_gui.show_help = false;
                mark_screen_needs_full_redraw();
                prev_mouse_b = mouse_b;
                continue;
            }
            
            /* If showing about, click returns to game */
            if (chess_gui.show_about) {
                chess_gui.show_about = false;
                mark_screen_needs_full_redraw();
                prev_mouse_b = mouse_b;
                continue;
            }
            
            /* Check menu clicks first */
            int menu_result = handle_menu_click(mx, my);
            if (menu_result == 0) {
                running = false;  /* Quit was selected */
                prev_mouse_b = mouse_b;
                continue;
            }
            
            /* If menu was clicked (even if just opened/closed), don't process other clicks */
            if (chess_gui.show_menu || show_help_menu_dropdown || my < MENU_BAR_HEIGHT) {
                prev_mouse_b = mouse_b;
                continue;
            }
            
            /* Check button clicks */
            if (!handle_button_click(mx, my)) {
                running = false;  /* Quit button was pressed */
                prev_mouse_b = mouse_b;
                continue;
            }
            
            /* Handle board clicks - only if it's player's turn */
            if (!ai_should_move) {
                /* Convert to board coordinates */
                if (mx >= BOARD_START_X && mx < BOARD_START_X + 400 && 
                    my >= BOARD_START_Y && my < BOARD_START_Y + 400) {
                    
                    int col = (mx - BOARD_START_X) / SQUARE_SIZE;
                    int row = (my - BOARD_START_Y) / SQUARE_SIZE;
                    
                    /* Extra bounds check for board coordinates */
                    if (col >= 0 && col < 8 && row >= 0 && row < 8) {
                        ChessPiece piece = chess_gui.game.board[row][col];
                        
                        if (!chess_gui.piece_selected && piece.type != EMPTY && 
                            piece.color == chess_gui.game.turn) {
                            /* Select piece */
                            chess_gui.selected_row = row;
                            chess_gui.selected_col = col;
                            chess_gui.piece_selected = true;
                            mark_screen_dirty();
                        } else if (chess_gui.piece_selected) {
                            /* Try to move piece */
                            if (chess_is_valid_move(&chess_gui.game,
                                                   chess_gui.selected_row,
                                                   chess_gui.selected_col,
                                                   row, col)) {
                                /* Verify move doesn't leave king in check */
                                ChessGameState temp = chess_gui.game;
                                ChessMove move = {chess_gui.selected_row, chess_gui.selected_col, 
                                                 row, col, 0};
                                chess_make_move(&temp, move);
                                
                                if (!chess_is_in_check(&temp, chess_gui.game.turn)) {
                                    /* Record this move for highlighting */
                                    chess_gui.last_move_from_row = chess_gui.selected_row;
                                    chess_gui.last_move_from_col = chess_gui.selected_col;
                                    chess_gui.last_move_to_row = row;
                                    chess_gui.last_move_to_col = col;
                                    chess_gui.has_last_move = true;
                                    
                                    chess_make_move(&chess_gui.game, move);
                                    save_position_to_history_with_move(chess_gui.selected_row, 
                                                                        chess_gui.selected_col,
                                                                        row, col);
                                    chess_gui.move_count++;  /* Increment move counter after move is committed */
                                    
                                    /* Update total time for the player who just moved (white) */
                                    clock_t current_time = clock();
                                    long elapsed_ms = (long)((current_time - chess_gui.current_move_start_time) * 1000.0 / CLOCKS_PER_SEC);
                                    chess_gui.white_total_time += (elapsed_ms / 1000.0);  /* Convert ms to seconds before adding */
                                    chess_gui.current_move_start_time = current_time;
                                    
                                    /* Turn is switched by chess_make_move */
                                    chess_gui.ai_move_counter = 0;  /* Reset AI timer */
                                    mark_screen_dirty();  /* Mark screen dirty after move */
                                }
                            }
                            chess_gui.piece_selected = false;
                            mark_screen_dirty();  /* Mark dirty when deselecting piece */
                        }
                    }
                }
            }
        }
        
        prev_mouse_b = mouse_b;  /* Remember current state for next frame */
        
        /* Mark screen dirty every second to ensure timer updates display continuously */
        static clock_t last_periodic_dirty_mark = 0;
        clock_t current_time_check = clock();
        long elapsed_since_mark_ms = (long)((current_time_check - last_periodic_dirty_mark) * 1000.0 / CLOCKS_PER_SEC);
        
        if (elapsed_since_mark_ms >= 1000) {  /* Every 1000ms (1 second) */
            mark_screen_dirty();
            last_periodic_dirty_mark = current_time_check;
        }
        
        /* Small delay to prevent CPU spinning */
        rest(10);  /* 10ms delay */
    }
    
    /* Cleanup */
    destroy_chess_pieces();  /* Free sprite bitmaps */
    cleanup_triple_buffers();
    cleanup_chess_game();
    allegro_exit();
    
    return 0;
}

END_OF_MAIN()
