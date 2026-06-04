#include <gtk/gtk.h>
#include <cairo.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <math.h>
#include "beatchess.h"
#include "visualization.h"
#include "chess_ai_move.h"  /* INTEGRATED: Shared AI module */

// Include the chess engine header
extern "C" {
    #include "beatchess.h"
}

#ifndef MAX_MOVES_BEFORE_DRAW
#define MAX_MOVES_BEFORE_DRAW 150
#endif

typedef struct {
    GtkWidget *window;
    GtkWidget *drawing_area;
    GtkWidget *status_label;
    GtkWidget *render_mode_item;  // Menu item for render mode
    
    ChessGameState game;
    ChessGameStatus status;
    
    int selected_row;
    int selected_col;
    bool has_selection;
    
    int last_from_row, last_from_col;
    int last_to_row, last_to_col;
    
    double cell_size;
    double board_offset_x;
    double board_offset_y;
    
    bool player_is_white;
    bool zero_players;  // AI vs AI mode
    bool flip_board;    // Display board flipped
    
    int move_count;
    double ai_think_time;  // Time AI has been thinking
    
    // Move history for undo
    ChessGameState *move_history;
    int history_size;
    int history_capacity;
} ChessGUI;

// Forward declarations
void draw_piece(cairo_t *cr, PieceType type, ChessColor color, double x, double y, double size, double dance_offset);
gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer data);
gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, gpointer data);
void update_status_text(ChessGUI *gui);
void make_ai_move(ChessGUI *gui);
gboolean ai_move_timeout(gpointer data);
void on_menu_help(GtkMenuItem *menuitem, gpointer user_data);  /* Defined in help.cpp */
void on_flip_board(GtkWidget *widget, gpointer data);
void on_undo_move(GtkWidget *widget, gpointer data);
void on_toggle_player_color(GtkWidget *widget, gpointer data);
void save_position_to_history(ChessGUI *gui);
void on_toggle_render_mode(GtkWidget *widget, gpointer data);
void update_render_mode_label(GtkWidget *menu_item);

// External functions from beatchess_draw.cpp
extern void init_sprite_cache();
extern void cleanup_sprite_cache();
extern void set_rendering_mode(bool sprites);
extern bool get_rendering_mode();

// External chess engine functions
extern int chess_minimax(ChessGameState *game, int depth, int alpha, int beta, bool maximizing);
extern int chess_get_all_moves(ChessGameState *game, ChessColor color, ChessMove *moves);

gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    ChessGUI *gui = (ChessGUI*)data;
    
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    
    gui->cell_size = fmin(width / 8.5, height / 8.5);
    gui->board_offset_x = (width - gui->cell_size * 8) / 2;
    gui->board_offset_y = (height - gui->cell_size * 8) / 2;
    
    // Draw background
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.25);
    cairo_paint(cr);
    
    // Draw board
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            bool is_light = (r + c) % 2 == 0;
            
            int draw_row = gui->flip_board ? (7 - r) : r;
            int draw_col = gui->flip_board ? (7 - c) : c;
            
            if (is_light) {
                cairo_set_source_rgb(cr, 0.9, 0.9, 0.85);
            } else {
                cairo_set_source_rgb(cr, 0.4, 0.5, 0.4);
            }
            
            cairo_rectangle(cr, gui->board_offset_x + draw_col * gui->cell_size,
                          gui->board_offset_y + draw_row * gui->cell_size,
                          gui->cell_size, gui->cell_size);
            cairo_fill(cr);
        }
    }
    
    // Highlight selected square
    if (gui->has_selection) {
        int draw_row = gui->flip_board ? (7 - gui->selected_row) : gui->selected_row;
        int draw_col = gui->flip_board ? (7 - gui->selected_col) : gui->selected_col;
        cairo_set_source_rgba(cr, 1.0, 1.0, 0.0, 0.5);
        cairo_rectangle(cr, gui->board_offset_x + draw_col * gui->cell_size,
                       gui->board_offset_y + draw_row * gui->cell_size,
                       gui->cell_size, gui->cell_size);
        cairo_fill(cr);
    }
    
    // Highlight last move
    if (gui->last_from_row >= 0) {
        int draw_from_row = gui->flip_board ? (7 - gui->last_from_row) : gui->last_from_row;
        int draw_from_col = gui->flip_board ? (7 - gui->last_from_col) : gui->last_from_col;
        int draw_to_row = gui->flip_board ? (7 - gui->last_to_row) : gui->last_to_row;
        int draw_to_col = gui->flip_board ? (7 - gui->last_to_col) : gui->last_to_col;
        
        cairo_set_source_rgba(cr, 0.5, 0.8, 1.0, 0.3);
        cairo_rectangle(cr, gui->board_offset_x + draw_from_col * gui->cell_size,
                       gui->board_offset_y + draw_from_row * gui->cell_size,
                       gui->cell_size, gui->cell_size);
        cairo_fill(cr);
        cairo_rectangle(cr, gui->board_offset_x + draw_to_col * gui->cell_size,
                       gui->board_offset_y + draw_to_row * gui->cell_size,
                       gui->cell_size, gui->cell_size);
        cairo_fill(cr);
    }
    
    // Draw coordinates
    cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, gui->cell_size * 0.2);
    
    for (int i = 0; i < 8; i++) {
        char label[2];
        label[0] = 'a' + i;
        label[1] = '\0';
        cairo_move_to(cr, gui->board_offset_x + i * gui->cell_size + gui->cell_size * 0.05,
                     gui->board_offset_y + 8 * gui->cell_size - gui->cell_size * 0.05);
        cairo_show_text(cr, label);
        
        label[0] = '8' - i;
        cairo_move_to(cr, gui->board_offset_x + gui->cell_size * 0.05,
                     gui->board_offset_y + i * gui->cell_size + gui->cell_size * 0.25);
        cairo_show_text(cr, label);
    }
    
    // Draw pieces
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            ChessPiece piece = gui->game.board[r][c];
            if (piece.type != EMPTY) {
                int draw_row = gui->flip_board ? (7 - r) : r;
                int draw_col = gui->flip_board ? (7 - c) : c;
                double x = gui->board_offset_x + draw_col * gui->cell_size;
                double y = gui->board_offset_y + draw_row * gui->cell_size;
                
                cairo_save(cr);
                cairo_translate(cr, 2, 2);
                cairo_set_source_rgba(cr, 0, 0, 0, 0.3);
                draw_piece(cr, piece.type, piece.color, x, y, gui->cell_size, 0);
                cairo_restore(cr);
                
                draw_piece(cr, piece.type, piece.color, x, y, gui->cell_size, 0);
            }
        }
    }
    
    return FALSE;
}

void update_status_text(ChessGUI *gui) {
    char status[256];
    
    if (gui->status == CHESS_CHECKMATE_WHITE) {
        snprintf(status, sizeof(status), "Checkmate! White wins! (Move %d)", gui->move_count);
    } else if (gui->status == CHESS_CHECKMATE_BLACK) {
        snprintf(status, sizeof(status), "Checkmate! Black wins! (Move %d)", gui->move_count);
    } else if (gui->status == CHESS_STALEMATE) {
        snprintf(status, sizeof(status), "Stalemate! Draw! (Move %d)", gui->move_count);
    } else if (gui->move_count >= MAX_MOVES_BEFORE_DRAW) {
        snprintf(status, sizeof(status), "Draw by move limit! (Move %d)", gui->move_count);
    } else {
        if (chess_is_in_check(&gui->game, gui->game.turn)) {
            snprintf(status, sizeof(status), "Move %d - %s to move (CHECK!)",
                    gui->move_count, gui->game.turn == WHITE ? "White" : "Black");
        } else {
            snprintf(status, sizeof(status), "Move %d - %s to move",
                    gui->move_count, gui->game.turn == WHITE ? "White" : "Black");
        }
    }
    
    gtk_label_set_text(GTK_LABEL(gui->status_label), status);
}

/**
 * Make an AI move using the shared chess_ai_move module
 * 
 * INTEGRATED VERSION: This function now uses chess_ai_compute_move()
 * from the shared module instead of inline minimax logic.
 * This ensures both GTK and DOS versions use identical AI.
 */
void make_ai_move(ChessGUI *gui) {
    /* Get default AI configuration */
    ChessAIConfig config = chess_ai_get_default_config();
    
    /* Optional: Override defaults for GTK version
     * config.search_depth = 5;              // Deeper search for stronger play
     * config.threshold_centipawns = 25;     // Moves within 25cp of best
     * config.use_randomization = true;      // Enable move variety
     */
    
    /* Call the shared AI engine */
    ChessAIMoveResult result = chess_ai_compute_move(&gui->game, config);
    
    /* Check if AI found a valid move */
    if (result.move.from_row < 0 || result.move.from_col < 0) {
        /* No valid move - game over */
        gui->status = chess_check_game_status(&gui->game);
        update_status_text(gui);
        gtk_widget_queue_draw(gui->drawing_area);
        return;
    }
    
    /* Record the move for highlighting */
    gui->last_from_row = result.move.from_row;
    gui->last_from_col = result.move.from_col;
    gui->last_to_row = result.move.to_row;
    gui->last_to_col = result.move.to_col;
    
    /* Save position before making move */
    save_position_to_history(gui);
    
    /* Execute the move */
    chess_make_move(&gui->game, result.move);
    gui->move_count++;
    
    /* Update game status */
    gui->status = chess_check_game_status(&gui->game);
    
    /* Update UI */
    update_status_text(gui);
    gtk_widget_queue_draw(gui->drawing_area);
}

gboolean ai_move_timeout(gpointer data) {
    ChessGUI *gui = (ChessGUI*)data;
    
    // Check if we should make an AI move
    bool is_player_turn = (gui->game.turn == WHITE) == gui->player_is_white;
    
    if (!is_player_turn && gui->status == CHESS_PLAYING) {
        make_ai_move(gui);
    }
    
    return TRUE;  // Keep timer running
}

gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    ChessGUI *gui = (ChessGUI*)data;
    
    // Only allow moves if it's the player's turn
    bool is_player_turn = (gui->game.turn == WHITE) == gui->player_is_white;
    if (!is_player_turn || gui->status != CHESS_PLAYING) {
        return FALSE;
    }
    
    double x = event->x;
    double y = event->y;
    
    // Convert pixel coordinates to board coordinates
    int col = (x - gui->board_offset_x) / gui->cell_size;
    int row = (y - gui->board_offset_y) / gui->cell_size;
    
    // Handle board flipping
    if (gui->flip_board) {
        row = 7 - row;
        col = 7 - col;
    }
    
    // Bounds check
    if (row < 0 || row >= 8 || col < 0 || col >= 8) {
        return FALSE;
    }
    
    // If no piece is selected, try to select one
    if (!gui->has_selection) {
        ChessPiece piece = gui->game.board[row][col];
        if (piece.type != EMPTY && piece.color == gui->game.turn) {
            gui->selected_row = row;
            gui->selected_col = col;
            gui->has_selection = true;
            gtk_widget_queue_draw(gui->drawing_area);
            return TRUE;
        }
        return FALSE;
    }
    
    // A piece is selected - try to move it
    if (chess_is_valid_move(&gui->game, gui->selected_row, gui->selected_col, row, col)) {
        // Verify move doesn't leave king in check
        ChessGameState temp = gui->game;
        ChessMove move = {gui->selected_row, gui->selected_col, row, col, 0};
        chess_make_move(&temp, move);
        
        if (!chess_is_in_check(&temp, gui->game.turn)) {
            // Valid move - execute it
            gui->last_from_row = gui->selected_row;
            gui->last_from_col = gui->selected_col;
            gui->last_to_row = row;
            gui->last_to_col = col;
            
            save_position_to_history(gui);
            chess_make_move(&gui->game, move);
            gui->move_count++;
            
            gui->has_selection = false;
            gui->status = chess_check_game_status(&gui->game);
            update_status_text(gui);
            gtk_widget_queue_draw(gui->drawing_area);
            
            return TRUE;
        }
    }
    
    // Move failed - deselect
    gui->has_selection = false;
    gtk_widget_queue_draw(gui->drawing_area);
    return FALSE;
}

void on_new_game(GtkWidget *widget, gpointer data) {
    ChessGUI *gui = (ChessGUI*)data;
    
    chess_init_board(&gui->game);
    gui->status = CHESS_PLAYING;
    gui->move_count = 0;
    gui->has_selection = false;
    gui->last_from_row = -1;
    gui->ai_think_time = 0;
    
    // Reset history
    gui->history_size = 1;
    gui->move_history[0] = gui->game;
    
    update_status_text(gui);
    gtk_widget_queue_draw(gui->drawing_area);
}

void on_toggle_zero_players(GtkWidget *widget, gpointer data) {
    ChessGUI *gui = (ChessGUI*)data;
    
    gui->zero_players = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(widget));
    
    // Restart game
    on_new_game(widget, data);
}

void save_position_to_history(ChessGUI *gui) {
    // Grow history if needed
    if (gui->history_size >= gui->history_capacity) {
        gui->history_capacity *= 2;
        gui->move_history = (ChessGameState*)realloc(gui->move_history,
                                                     gui->history_capacity * sizeof(ChessGameState));
    }
    
    // Save current position
    gui->move_history[gui->history_size] = gui->game;
    gui->history_size++;
}

void on_flip_board(GtkWidget *widget, gpointer data) {
    ChessGUI *gui = (ChessGUI*)data;
    gui->flip_board = !gui->flip_board;
    gtk_widget_queue_draw(gui->drawing_area);
}

void on_toggle_player_color(GtkWidget *widget, gpointer data) {
    ChessGUI *gui = (ChessGUI*)data;
    gui->player_is_white = !gui->player_is_white;
    
    // Restart the game with new color
    on_new_game(widget, data);
}

void on_undo_move(GtkWidget *widget, gpointer data) {
    ChessGUI *gui = (ChessGUI*)data;
    
    // Can only undo in single-player mode, not AI vs AI
    if (gui->zero_players) return;
    
    // Need at least 2 positions to undo (one AI move + one player move)
    if (gui->history_size < 2) {
        gtk_label_set_text(GTK_LABEL(gui->status_label), "Nothing to undo!");
        return;
    }
    
    // Stop AI thinking
    
    // Go back 2 moves: undo AI move and player move
    gui->history_size -= 2;
    gui->game = gui->move_history[gui->history_size];
    gui->move_count -= 2;
    
    gui->has_selection = false;
    gui->last_from_row = -1;
    
    gui->status = chess_check_game_status(&gui->game);
    
    // Restart AI thinking for AI's turn
    
    update_status_text(gui);
    gtk_widget_queue_draw(gui->drawing_area);
}

void update_render_mode_label(GtkWidget *menu_item) {
    bool using_sprites = get_rendering_mode();
    if (using_sprites) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(menu_item), "Render Mode: Sprites");
    } else {
        gtk_menu_item_set_label(GTK_MENU_ITEM(menu_item), "Render Mode: Geometric");
    }
}

void on_toggle_render_mode(GtkWidget *widget, gpointer data) {
    ChessGUI *gui = (ChessGUI*)data;
    
    bool current_mode = get_rendering_mode();
    set_rendering_mode(!current_mode);
    
    update_render_mode_label(gui->render_mode_item);
    gtk_widget_queue_draw(gui->drawing_area);
}

int main(int argc, char *argv[]) {
    srand(time(NULL));
    
    gtk_init(&argc, &argv);
    
    // Initialize sprite cache
    init_sprite_cache();
    set_rendering_mode(true);
    ChessGUI gui;
    
    gui.player_is_white = true;
    gui.zero_players = false;
    gui.last_from_row = -1;
    gui.ai_think_time = 0;
    gui.flip_board = false;
    gui.move_history = NULL;
    gui.history_size = 0;
    gui.history_capacity = 0;
    
    // Parse command line args
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--black") == 0 || strcmp(argv[i], "-b") == 0) {
            gui.player_is_white = false;
        }
        if (strcmp(argv[i], "--zero-players") == 0 || strcmp(argv[i], "-z") == 0) {
            gui.zero_players = true;
        }
    }
    
    // Initialize game
    chess_init_board(&gui.game);
    gui.status = CHESS_PLAYING;
    gui.move_count = 0;
    
    // Initialize move history
    gui.move_history = (ChessGameState*)malloc(10 * sizeof(ChessGameState));
    gui.history_capacity = 10;
    gui.history_size = 1;
    gui.move_history[0] = gui.game;  // Save initial position
    
    // Create window
    gui.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(gui.window), "Chess");
    gtk_window_set_default_size(GTK_WINDOW(gui.window), 600, 650);
    g_signal_connect(gui.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    
    // Create main vbox
    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(gui.window), main_vbox);
    
    // Create menu bar
    GtkWidget *menu_bar = gtk_menu_bar_new();
    
    // File menu
    GtkWidget *file_menu = gtk_menu_new();
    GtkWidget *file_item = gtk_menu_item_new_with_label("File");
    
    GtkWidget *restart_item = gtk_menu_item_new_with_label("Restart");
    g_signal_connect(restart_item, "activate", G_CALLBACK(on_new_game), &gui);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), restart_item);
    
    GtkWidget *play_as_black_item = gtk_check_menu_item_new_with_label("Play as Black");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(play_as_black_item), !gui.player_is_white);
    g_signal_connect(play_as_black_item, "activate", G_CALLBACK(on_toggle_player_color), &gui);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), play_as_black_item);
    
    GtkWidget *flip_item = gtk_menu_item_new_with_label("Flip Board");
    g_signal_connect(flip_item, "activate", G_CALLBACK(on_flip_board), &gui);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), flip_item);
    
    GtkWidget *undo_item = gtk_menu_item_new_with_label("Undo Move");
    g_signal_connect(undo_item, "activate", G_CALLBACK(on_undo_move), &gui);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), undo_item);
    
    GtkWidget *zero_players_item = gtk_check_menu_item_new_with_label("AI vs AI (Zero Players)");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(zero_players_item), gui.zero_players);
    g_signal_connect(zero_players_item, "activate", G_CALLBACK(on_toggle_zero_players), &gui);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), zero_players_item);
    
    GtkWidget *separator = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), separator);
    
    GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(quit_item, "activate", G_CALLBACK(gtk_main_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), quit_item);
    
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_item), file_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), file_item);
    
    // View menu (new)
    GtkWidget *view_menu = gtk_menu_new();
    GtkWidget *view_item = gtk_menu_item_new_with_label("View");
    
    gui.render_mode_item = gtk_menu_item_new_with_label("Render Mode: Sprite");
    g_signal_connect(gui.render_mode_item, "activate", G_CALLBACK(on_toggle_render_mode), &gui);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), gui.render_mode_item);
    
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view_item), view_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), view_item);
    
    // Help menu
    GtkWidget *help_menu = gtk_menu_new();
    GtkWidget *help_item = gtk_menu_item_new_with_label("Help");
    
    GtkWidget *help_contents = gtk_menu_item_new_with_label("Help Contents");
    g_signal_connect(help_contents, "activate", G_CALLBACK(on_menu_help), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(help_menu), help_contents);
    
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(help_item), help_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), help_item);
    
    gtk_box_pack_start(GTK_BOX(main_vbox), menu_bar, FALSE, FALSE, 0);
    
    // Create vbox for game content
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_box_pack_start(GTK_BOX(main_vbox), vbox, TRUE, TRUE, 0);
    
    // Status label
    gui.status_label = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(vbox), gui.status_label, FALSE, FALSE, 5);
    update_status_text(&gui);
    
    // Drawing area
    gui.drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(gui.drawing_area, 600, 600);
    gtk_widget_add_events(gui.drawing_area, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(gui.drawing_area, "draw", G_CALLBACK(on_draw), &gui);
    g_signal_connect(gui.drawing_area, "button-press-event", G_CALLBACK(on_button_press), &gui);
    gtk_box_pack_start(GTK_BOX(vbox), gui.drawing_area, TRUE, TRUE, 0);
    
    gtk_widget_show_all(gui.window);
    
    // Start AI move timer (1 second delay between moves)
    g_timeout_add(1000, ai_move_timeout, &gui);
    
    gtk_main();
    
    // Cleanup
    cleanup_sprite_cache();
    if (gui.move_history) {
        free(gui.move_history);
    }
    
    return 0;
}
