#include "beatchess.h"
#include "visualization.h"
#include "chess_pieces.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <unistd.h>

// ============================================================================
// GLOBAL RENDERING MODE
// ============================================================================

bool use_sprites = false;  // Default to geometric shapes (now extern accessible)

void set_rendering_mode(bool sprites) {
    use_sprites = sprites;
}

void toggle_sprite_mode() {
    use_sprites = !use_sprites;
}

void draw_chess_board(BeatChessVisualization *chess, cairo_t *cr) {
    double cell = chess->cell_size;
    double ox = chess->board_offset_x;
    double oy = chess->board_offset_y;
    
    // Draw board squares
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            // Apply board flip transformation if enabled
            int draw_r = chess->board_flipped ? (BOARD_SIZE - 1 - r) : r;
            int draw_c = chess->board_flipped ? (BOARD_SIZE - 1 - c) : c;
            
            bool is_light = (r + c) % 2 == 0;
            
            if (is_light) {
                cairo_set_source_rgb(cr, 0.9, 0.9, 0.85);
            } else {
                cairo_set_source_rgb(cr, 0.4, 0.5, 0.4);
            }
            
            cairo_rectangle(cr, ox + draw_c * cell, oy + draw_r * cell, cell, cell);
            cairo_fill(cr);
        }
    }
    
    // Draw coordinates (flipped if needed)
    cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, cell * 0.2);
    
    for (int i = 0; i < 8; i++) {
        char label[2];
        
        if (chess->board_flipped) {
            // Flipped coordinates
            // Files (h-a instead of a-h)
            label[0] = 'h' - i;
            label[1] = '\0';
            cairo_move_to(cr, ox + i * cell + cell * 0.05, oy + 8 * cell - cell * 0.05);
            cairo_show_text(cr, label);
            
            // Ranks (1-8 instead of 8-1)
            label[0] = '1' + i;
            cairo_move_to(cr, ox + cell * 0.05, oy + i * cell + cell * 0.25);
            cairo_show_text(cr, label);
        } else {
            // Normal coordinates
            // Files (a-h)
            label[0] = 'a' + i;
            label[1] = '\0';
            cairo_move_to(cr, ox + i * cell + cell * 0.05, oy + 8 * cell - cell * 0.05);
            cairo_show_text(cr, label);
            
            // Ranks (8-1)
            label[0] = '8' - i;
            cairo_move_to(cr, ox + cell * 0.05, oy + i * cell + cell * 0.25);
            cairo_show_text(cr, label);
        }
    }
}

void draw_chess_pvsa_button(BeatChessVisualization *chess, cairo_t *cr, int width, int height) {
    // Button position and size - LEFT SIDE, below RESET button
    double button_width = 120;
    double button_height = 40;
    double button_x = 20;  // LEFT side, same as RESET
    double button_y = 70;  // Below RESET (20 + 40 + 10 spacing)
    
    // Store button position for hit detection
    chess->pvsa_button_x = button_x;
    chess->pvsa_button_y = button_y;
    chess->pvsa_button_width = button_width;
    chess->pvsa_button_height = button_height;
    
    // Background
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_rectangle(cr, button_x, button_y, button_width, button_height);
    cairo_fill(cr);
    
    // Glow effect if hovered
    if (chess->pvsa_button_hovered || chess->pvsa_button_glow > 0) {
        double glow_alpha = chess->pvsa_button_glow * 0.5;
        if (chess->pvsa_button_hovered) glow_alpha = 0.4;
        
        cairo_set_source_rgba(cr, 1.0, 0.7, 0.2, glow_alpha);
        cairo_rectangle(cr, button_x - 3, button_y - 3, button_width + 6, button_height + 6);
        cairo_stroke(cr);
    }
    
    // Border
    cairo_set_source_rgb(cr, chess->pvsa_button_hovered ? 1.0 : 0.7, 
                         chess->pvsa_button_hovered ? 0.7 : 0.5, 
                         chess->pvsa_button_hovered ? 0.2 : 0.3);
    cairo_set_line_width(cr, 2);
    cairo_rectangle(cr, button_x, button_y, button_width, button_height);
    cairo_stroke(cr);
    
    // Text - show current mode
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12);
    
    const char *button_text = chess->choosing_color ? "P vs AI?" :
                              (chess->player_vs_ai   ? "P vs AI"  : "AI vs AI");
    
    cairo_text_extents_t extents;
    cairo_text_extents(cr, button_text, &extents);
    
    double text_x = button_x + (button_width - extents.width) / 2;
    double text_y = button_y + (button_height + extents.height) / 2;
    
    cairo_set_source_rgb(cr, chess->pvsa_button_hovered ? 1.0 : 0.9, 
                         chess->pvsa_button_hovered ? 0.8 : 0.7, 
                         chess->pvsa_button_hovered ? 0.3 : 0.4);
    cairo_move_to(cr, text_x, text_y);
    cairo_show_text(cr, button_text);
}

void draw_chess_reset_button(BeatChessVisualization *chess, cairo_t *cr, int width, int height) {
    // Button position and size - LEFT SIDE
    double button_width = 120;
    double button_height = 40;
    double button_x = 20;  // LEFT side, 20px from edge
    double button_y = 20;
    
    // Store button position for hit detection
    chess->reset_button_x = button_x;
    chess->reset_button_y = button_y;
    chess->reset_button_width = button_width;
    chess->reset_button_height = button_height;
    
    // Background
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_rectangle(cr, button_x, button_y, button_width, button_height);
    cairo_fill(cr);
    
    // Glow effect if hovered
    if (chess->reset_button_hovered || chess->reset_button_glow > 0) {
        double glow_alpha = chess->reset_button_glow * 0.5;
        if (chess->reset_button_hovered) glow_alpha = 0.4;
        
        cairo_set_source_rgba(cr, 1.0, 0.7, 0.2, glow_alpha);
        cairo_rectangle(cr, button_x - 3, button_y - 3, button_width + 6, button_height + 6);
        cairo_stroke(cr);
    }
    
    // Border
    cairo_set_source_rgb(cr, chess->reset_button_hovered ? 1.0 : 0.7, 
                         chess->reset_button_hovered ? 0.7 : 0.5, 
                         chess->reset_button_hovered ? 0.2 : 0.3);
    cairo_set_line_width(cr, 2);
    cairo_rectangle(cr, button_x, button_y, button_width, button_height);
    cairo_stroke(cr);
    
    // Text
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 14);
    
    cairo_text_extents_t extents;
    cairo_text_extents(cr, "RESET", &extents);
    
    double text_x = button_x + (button_width - extents.width) / 2;
    double text_y = button_y + (button_height + extents.height) / 2;
    
    cairo_set_source_rgb(cr, chess->reset_button_hovered ? 1.0 : 0.9, 
                         chess->reset_button_hovered ? 0.8 : 0.7, 
                         chess->reset_button_hovered ? 0.3 : 0.4);
    cairo_move_to(cr, text_x, text_y);
    cairo_show_text(cr, "RESET");
}

void draw_chess_status(BeatChessVisualization *chess, cairo_t *cr, int width, int height) {
    // Status text
    cairo_select_font_face(cr, "Monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 16);
    
    cairo_text_extents_t extents;
    cairo_text_extents(cr, chess->status_text, &extents);
    
    double text_x = (width - extents.width) / 2;
    double text_y = chess->board_offset_y - 20;
    
    // Flash background if timer active
    if (chess->status_flash_timer > 0) {
        double alpha = chess->status_flash_timer * 0.3;
        cairo_set_source_rgba(cr, 
                            chess->status_flash_color[0],
                            chess->status_flash_color[1],
                            chess->status_flash_color[2],
                            alpha);
        cairo_rectangle(cr, text_x - 10, text_y - extents.height - 5, 
                       extents.width + 20, extents.height + 10);
        cairo_fill(cr);
    }
    
    // Text
    if (chess->status_flash_timer > 0) {
        cairo_set_source_rgb(cr,
                           chess->status_flash_color[0],
                           chess->status_flash_color[1],
                           chess->status_flash_color[2]);
    } else {
        cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
    }
    cairo_move_to(cr, text_x, text_y);
    cairo_show_text(cr, chess->status_text);
    
    // Move counter
    char move_text[64];
    snprintf(move_text, sizeof(move_text), "Move: %d", chess->move_count);
    cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    cairo_set_font_size(cr, 14);
    cairo_text_extents(cr, move_text, &extents);
    cairo_move_to(cr, (width - extents.width) / 2, 
                  chess->board_offset_y + chess->cell_size * 8 + 30);
    cairo_show_text(cr, move_text);
    
    // Time display (only in Player vs AI mode)
    if (chess->player_vs_ai) {
        char time_text[256];
        
        // Show current turn's elapsed time prominently
        double current_time = (chess->game.turn == WHITE) ? chess->current_move_start_time : chess->time_thinking;
        const char *current_player = (chess->game.turn == WHITE) ? "Your" : "AI";
        
        snprintf(time_text, sizeof(time_text), "%s turn: %.1fs | Total - White: %.1fs | Black: %.1fs",
                current_player, current_time,
                chess->white_total_time, chess->black_total_time);
        
        cairo_set_source_rgb(cr, 1.0, 1.0, 0.0);  // Bright yellow for visibility
        cairo_set_font_size(cr, 14);  // Larger font
        cairo_text_extents(cr, time_text, &extents);
        cairo_move_to(cr, (width - extents.width) / 2, 
                      chess->board_offset_y + chess->cell_size * 8 + 55);
        cairo_show_text(cr, time_text);
    }
}

void draw_chess_eval_bar(BeatChessVisualization *chess, cairo_t *cr, int width, int height) {
    double bar_width = 30;
    double bar_height = chess->cell_size * 8;
    double bar_x = chess->board_offset_x + chess->cell_size * 8 + 20;
    double bar_y = chess->board_offset_y;
    
    // Background
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_rectangle(cr, bar_x, bar_y, bar_width, bar_height);
    cairo_fill(cr);
    
    // Center line
    cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, bar_x, bar_y + bar_height / 2);
    cairo_line_to(cr, bar_x + bar_width, bar_y + bar_height / 2);
    cairo_stroke(cr);
    
    // Evaluation position (-1 to 1)
    double eval_pos = chess->eval_bar_position;
    double fill_y = bar_y + bar_height / 2 - (eval_pos * bar_height / 2);
    double fill_height = eval_pos * bar_height / 2;
    
    if (eval_pos > 0) {
        // White advantage
        cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
        cairo_rectangle(cr, bar_x, fill_y, bar_width, fill_height);
    } else {
        // Black advantage
        cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        cairo_rectangle(cr, bar_x, bar_y + bar_height / 2, bar_width, -fill_height);
    }
    cairo_fill(cr);
    
    // Border
    cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    cairo_set_line_width(cr, 2);
    cairo_rectangle(cr, bar_x, bar_y, bar_width, bar_height);
    cairo_stroke(cr);
}

void draw_chess_pieces(BeatChessVisualization *chess, cairo_t *cr) {
    double cell = chess->cell_size;
    double ox = chess->board_offset_x;
    double oy = chess->board_offset_y;
    
    // Draw selection highlight if a piece is selected
    if (chess->has_selected_piece && chess->selected_piece_row >= 0) {
        int sel_r = chess->board_flipped ? (BOARD_SIZE - 1 - chess->selected_piece_row) : chess->selected_piece_row;
        int sel_c = chess->board_flipped ? (BOARD_SIZE - 1 - chess->selected_piece_col) : chess->selected_piece_col;
        
        cairo_set_source_rgba(cr, 0.0, 1.0, 1.0, 0.3);  // Cyan highlight
        cairo_rectangle(cr, 
                       ox + sel_c * cell, 
                       oy + sel_r * cell, 
                       cell, cell);
        cairo_fill(cr);
        
        // Border around selected piece
        cairo_set_source_rgb(cr, 0.0, 1.0, 1.0);
        cairo_set_line_width(cr, 3);
        cairo_rectangle(cr, 
                       ox + sel_c * cell, 
                       oy + sel_r * cell, 
                       cell, cell);
        cairo_stroke(cr);
    }
    
    // Get volume level from the parent visualizer structure
    // We need to pass this through from draw_beat_chess
    Visualizer *vis = (Visualizer*)((char*)chess - offsetof(Visualizer, beat_chess));
    double volume = vis->volume_level;
    
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            ChessPiece piece = chess->game.board[r][c];
            
            // Skip if animating this piece
            if (chess->is_animating && 
                r == chess->animating_from_row && 
                c == chess->animating_from_col) {
                continue;
            }
            
            if (piece.type != EMPTY) {
                // Apply board flip transformation
                int draw_r = chess->board_flipped ? (BOARD_SIZE - 1 - r) : r;
                int draw_c = chess->board_flipped ? (BOARD_SIZE - 1 - c) : c;
                
                double x = ox + draw_c * cell;
                double y = oy + draw_r * cell;
                
                // Calculate dance offset based on music volume and position
                double phase = (r * 0.5 + c * 0.3) * 3.14159;  // Different phase for each square
                double time_wave = sin(chess->time_since_last_move * 10.0 + phase);
                // Scale dance by volume - pieces bounce more with louder music
                double dance_amount = time_wave * volume * cell * 0.2;
                
                // Draw shadow - dark for all pieces
                cairo_save(cr);
                cairo_translate(cr, 3, 3);
                cairo_set_source_rgba(cr, 0, 0, 0, 0.4);
                draw_piece(cr, piece.type, piece.color, x, y, cell, dance_amount);
                cairo_restore(cr);
                
                // Draw piece
                draw_piece(cr, piece.type, piece.color, x, y, cell, dance_amount);
            }
        }
    }
    
    // Draw animating piece
    if (chess->is_animating) {
        int fr = chess->animating_from_row;
        int fc = chess->animating_from_col;
        int tr = chess->animating_to_row;
        int tc = chess->animating_to_col;
        
        // Apply board flip transformation
        int draw_fr = chess->board_flipped ? (BOARD_SIZE - 1 - fr) : fr;
        int draw_fc = chess->board_flipped ? (BOARD_SIZE - 1 - fc) : fc;
        int draw_tr = chess->board_flipped ? (BOARD_SIZE - 1 - tr) : tr;
        int draw_tc = chess->board_flipped ? (BOARD_SIZE - 1 - tc) : tc;
        
        ChessPiece piece = chess->game.board[tr][tc];
        
        // Smooth interpolation
        double t = chess->animation_progress;
        t = t * t * (3.0 - 2.0 * t); // Smoothstep
        
        double x = ox + (draw_fc + t * (draw_tc - draw_fc)) * cell;
        double y = oy + (draw_fr + t * (draw_tr - draw_fr)) * cell;
        
        // Animating piece dances even more to the music
        double dance_amount = sin(chess->time_since_last_move * 15.0) * volume * cell * 0.3;
        
        // Draw shadow - dark for all pieces
        cairo_save(cr);
        cairo_translate(cr, 3, 3);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.4);
        draw_piece(cr, piece.type, piece.color, x, y, cell, dance_amount);
        cairo_restore(cr);
        
        // Draw piece with slight glow
        cairo_save(cr);
        if (piece.color == WHITE) {
            cairo_set_source_rgb(cr, 1.0, 1.0, 0.9);
        } else {
            // Brighter gold glow for animating gold pieces
            cairo_set_source_rgb(cr, 0.95, 0.75, 0.2);
        }
        draw_piece(cr, piece.type, piece.color, x, y, cell, dance_amount);
        cairo_restore(cr);
    }
}

void draw_chess_last_move_highlight(BeatChessVisualization *chess, cairo_t *cr) {
    if (chess->last_from_row < 0 || chess->last_move_glow <= 0) return;
    
    double cell = chess->cell_size;
    double ox = chess->board_offset_x;
    double oy = chess->board_offset_y;
    
    double alpha = chess->last_move_glow * 0.5;
    
    // Transform coordinates if board is flipped
    int from_row = chess->board_flipped ? (BOARD_SIZE - 1 - chess->last_from_row) : chess->last_from_row;
    int from_col = chess->board_flipped ? (BOARD_SIZE - 1 - chess->last_from_col) : chess->last_from_col;
    int to_row = chess->board_flipped ? (BOARD_SIZE - 1 - chess->last_to_row) : chess->last_to_row;
    int to_col = chess->board_flipped ? (BOARD_SIZE - 1 - chess->last_to_col) : chess->last_to_col;
    
    // Highlight from square
    cairo_set_source_rgba(cr, 1.0, 1.0, 0.0, alpha);
    cairo_rectangle(cr, 
                    ox + from_col * cell, 
                    oy + from_row * cell, 
                    cell, cell);
    cairo_fill(cr);
    
    // Highlight to square
    cairo_set_source_rgba(cr, 1.0, 1.0, 0.0, alpha);
    cairo_rectangle(cr, 
                    ox + to_col * cell, 
                    oy + to_row * cell, 
                    cell, cell);
    cairo_fill(cr);
}

// ============================================================================
// SPRITE RENDERING FUNCTIONS
// ============================================================================

// Helper function to load BMP data into a cairo surface
static cairo_surface_t* create_surface_from_bmp_data(const unsigned char* bmp_data, size_t data_size) {
    // Create a surface from the BMP data
    // BMP files start with 0x424D ("BM")
    if (data_size < 54 || bmp_data[0] != 0x42 || bmp_data[1] != 0x4D) {
        return NULL;
    }
    
    // Parse BMP header
    uint32_t offset = *(uint32_t*)(bmp_data + 10);  // Pixel data offset
    int32_t width = *(int32_t*)(bmp_data + 18);
    int32_t height = *(int32_t*)(bmp_data + 22);
    uint16_t bits_per_pixel = *(uint16_t*)(bmp_data + 28);
    
    if (width <= 0 || height <= 0 || (bits_per_pixel != 24 && bits_per_pixel != 32)) {
        return NULL;
    }
    
    // Create image surface (RGBA)
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, abs(height));
    if (!surface) {
        return NULL;
    }
    
    unsigned char* surface_data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    
    // Convert BMP to RGBA
    // BMP is BGR bottom-up, Cairo is ARGB top-down
    int bytes_per_pixel_bmp = bits_per_pixel / 8;
    int scan_line_size = ((width * bits_per_pixel + 31) / 32) * 4;  // BMP scanlines are padded to 4-byte boundary
    
    // Process each pixel
    for (int y = 0; y < abs(height); y++) {
        for (int x = 0; x < width; x++) {
            // BMP coordinates (bottom-up)
            int bmp_y = (height < 0) ? y : (abs(height) - 1 - y);
            const unsigned char* bmp_pixel = bmp_data + offset + (bmp_y * scan_line_size) + (x * bytes_per_pixel_bmp);
            unsigned char* cairo_pixel = surface_data + (y * stride) + (x * 4);
            
            unsigned char b = bmp_pixel[0];
            unsigned char g = bmp_pixel[1];
            unsigned char r = bmp_pixel[2];
            unsigned char a = 0xFF;
            
            // Detect green pixels as transparent (like DOS version)
            // Green channel significantly higher than red and blue, and g > 200
            if (g > r + 30 && g > b + 30 && g > 200) {
                // Transparent pixel - green background
                a = 0x00;
                r = 0;
                g = 0;
                b = 0;
            }
            
            // Store in ARGB format (pre-multiplied alpha for Cairo)
            if (a == 0x00) {
                // Fully transparent - pre-multiply gives 0
                cairo_pixel[0] = 0;  // B
                cairo_pixel[1] = 0;  // G
                cairo_pixel[2] = 0;  // R
                cairo_pixel[3] = 0;  // A
            } else {
                // Opaque pixel
                cairo_pixel[0] = b;  // B
                cairo_pixel[1] = g;  // G
                cairo_pixel[2] = r;  // R
                cairo_pixel[3] = a;  // A
            }
        }
    }
    
    cairo_surface_mark_dirty(surface);
    return surface;
}

// Cache for sprite surfaces
static struct {
    cairo_surface_t* surfaces[12];  // 6 piece types × 2 colors
    bool initialized;
} sprite_cache = { {NULL}, false };



// Get sprite surface for a piece
static cairo_surface_t* get_sprite_surface(PieceType type, ChessColor color) {
    if (!sprite_cache.initialized) {
        return NULL;
    }
    
    // Index mapping: piece_bmps array has BLACK first, WHITE second for each type
    // So: index = (type - 1) * 2 + (color == WHITE ? 1 : 0)
    int index = (type - 1) * 2 + (color == WHITE ? 1 : 0);
    if (index < 0 || index >= 12) {
        return NULL;
    }
    
    return sprite_cache.surfaces[index];
}

// Initialize sprite cache
void init_sprite_cache() {
    // Map piece types to their BMP data
    const struct {
        const unsigned char* data;
        size_t size;
    } piece_bmps[] = {
        {black_pawn_bmp, sizeof(black_pawn_bmp)},
        {white_pawn_bmp, sizeof(white_pawn_bmp)},
        {black_knight_bmp, sizeof(black_knight_bmp)},
        {white_knight_bmp, sizeof(white_knight_bmp)},
        {black_bishop_bmp, sizeof(black_bishop_bmp)},
        {white_bishop_bmp, sizeof(white_bishop_bmp)},
        {black_rook_bmp, sizeof(black_rook_bmp)},
        {white_rook_bmp, sizeof(white_rook_bmp)},
        {black_queen_bmp, sizeof(black_queen_bmp)},
        {white_queen_bmp, sizeof(white_queen_bmp)},
        {black_king_bmp, sizeof(black_king_bmp)},
        {white_king_bmp, sizeof(white_king_bmp)},
    };
    
    for (int i = 0; i < 12; i++) {
        sprite_cache.surfaces[i] = create_surface_from_bmp_data(
            piece_bmps[i].data,
            piece_bmps[i].size
        );
    }
    
    sprite_cache.initialized = true;
}

// Clean up sprite cache
void cleanup_sprite_cache() {
    for (int i = 0; i < 12; i++) {
        if (sprite_cache.surfaces[i]) {
            cairo_surface_destroy(sprite_cache.surfaces[i]);
            sprite_cache.surfaces[i] = NULL;
        }
    }
    sprite_cache.initialized = false;
}

// Draw sprite for a piece
static void draw_sprite_piece(cairo_t *cr, PieceType type, ChessColor color, 
                             double x, double y, double size) {
    cairo_surface_t* surface = get_sprite_surface(type, color);
    if (!surface) {
        return;
    }
    
    int sprite_width = cairo_image_surface_get_width(surface);
    int sprite_height = cairo_image_surface_get_height(surface);
    
    // Scale to 75% of cell size to leave some margin
    double scale = (size * 0.75) / sprite_width;
    
    cairo_save(cr);
    cairo_translate(cr, x + size / 2, y + size / 2);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -sprite_width / 2, -sprite_height / 2);
    
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_paint(cr);
    
    cairo_restore(cr);
}

// ============================================================================
// GEOMETRIC DRAWING FUNCTIONS (Original implementation)
// ============================================================================

static void draw_geometric_piece(cairo_t *cr, PieceType type, ChessColor color, double x, double y, double size, double dance_offset) {
    double cx = x + size / 2;
    double cy = y + size / 2;
    double s = size * 0.4;  // Scale factor
    
    // Apply dance offset (vertical bounce)
    cy += dance_offset;
    
    // Set colors
    if (color == WHITE) {
        cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    } else {
        // Gold color for black pieces
        cairo_set_source_rgb(cr, 0.85, 0.65, 0.13);
    }
    
    switch (type) {
        case PAWN:
            // Circle on small rectangle
            cairo_arc(cr, cx, cy - s * 0.15, s * 0.25, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_rectangle(cr, cx - s * 0.2, cy + s * 0.1, s * 0.4, s * 0.3);
            cairo_fill(cr);
            break;
            
        case KNIGHT:
            // Crude horse head - blocky and angular
            // Base/neck
            cairo_rectangle(cr, cx - s * 0.15, cy, s * 0.3, s * 0.4);
            cairo_fill(cr);
            // Head (off-center rectangle)
            cairo_rectangle(cr, cx - s * 0.1, cy - s * 0.4, s * 0.35, s * 0.4);
            cairo_fill(cr);
            // Snout (small rectangle sticking out)
            cairo_rectangle(cr, cx + s * 0.15, cy - s * 0.25, s * 0.2, s * 0.15);
            cairo_fill(cr);
            // Ear (triangle on top)
            cairo_move_to(cr, cx + s * 0.05, cy - s * 0.4);
            cairo_line_to(cr, cx + s * 0.15, cy - s * 0.55);
            cairo_line_to(cr, cx + s * 0.2, cy - s * 0.35);
            cairo_fill(cr);
            break;
            
        case BISHOP:
            // Triangle with circle on top
            cairo_move_to(cr, cx, cy - s * 0.5);
            cairo_line_to(cr, cx - s * 0.25, cy + s * 0.4);
            cairo_line_to(cr, cx + s * 0.25, cy + s * 0.4);
            cairo_close_path(cr);
            cairo_fill(cr);
            cairo_arc(cr, cx, cy - s * 0.5, s * 0.12, 0, 2 * M_PI);
            cairo_fill(cr);
            break;
            
        case ROOK:
            // Castle tower
            cairo_rectangle(cr, cx - s * 0.3, cy - s * 0.1, s * 0.6, s * 0.5);
            cairo_fill(cr);
            // Crenellations
            cairo_rectangle(cr, cx - s * 0.3, cy - s * 0.5, s * 0.15, s * 0.35);
            cairo_fill(cr);
            cairo_rectangle(cr, cx - s * 0.05, cy - s * 0.5, s * 0.1, s * 0.35);
            cairo_fill(cr);
            cairo_rectangle(cr, cx + s * 0.15, cy - s * 0.5, s * 0.15, s * 0.35);
            cairo_fill(cr);
            break;
            
        case QUEEN:
            // Crown with multiple points
            cairo_move_to(cr, cx, cy - s * 0.5);
            cairo_line_to(cr, cx - s * 0.15, cy - s * 0.2);
            cairo_line_to(cr, cx - s * 0.3, cy - s * 0.4);
            cairo_line_to(cr, cx - s * 0.3, cy + s * 0.4);
            cairo_line_to(cr, cx + s * 0.3, cy + s * 0.4);
            cairo_line_to(cr, cx + s * 0.3, cy - s * 0.4);
            cairo_line_to(cr, cx + s * 0.15, cy - s * 0.2);
            cairo_close_path(cr);
            cairo_fill(cr);
            // Center ball
            cairo_arc(cr, cx, cy - s * 0.5, s * 0.1, 0, 2 * M_PI);
            cairo_fill(cr);
            break;
            
        case KING:
            // Crown with cross
            cairo_rectangle(cr, cx - s * 0.3, cy - s * 0.1, s * 0.6, s * 0.5);
            cairo_fill(cr);
            // Cross on top
            cairo_rectangle(cr, cx - s * 0.05, cy - s * 0.6, s * 0.1, s * 0.5);
            cairo_fill(cr);
            cairo_rectangle(cr, cx - s * 0.25, cy - s * 0.45, s * 0.5, s * 0.1);
            cairo_fill(cr);
            break;
            
        default:
            break;
    }
    
    // Outline for all pieces
    if (type != EMPTY) {
        if (color == WHITE) {
            cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
        } else {
            // Darker gold outline for gold pieces
            cairo_set_source_rgb(cr, 0.5, 0.35, 0.05);
        }
        cairo_set_line_width(cr, 1.5);
        
        switch (type) {
            case PAWN:
                cairo_arc(cr, cx, cy - s * 0.15, s * 0.25, 0, 2 * M_PI);
                cairo_stroke(cr);
                cairo_rectangle(cr, cx - s * 0.2, cy + s * 0.1, s * 0.4, s * 0.3);
                cairo_stroke(cr);
                break;
            case KNIGHT:
                cairo_rectangle(cr, cx - s * 0.15, cy, s * 0.3, s * 0.4);
                cairo_stroke(cr);
                cairo_rectangle(cr, cx - s * 0.1, cy - s * 0.4, s * 0.35, s * 0.4);
                cairo_stroke(cr);
                cairo_rectangle(cr, cx + s * 0.15, cy - s * 0.25, s * 0.2, s * 0.15);
                cairo_stroke(cr);
                cairo_move_to(cr, cx + s * 0.05, cy - s * 0.4);
                cairo_line_to(cr, cx + s * 0.15, cy - s * 0.55);
                cairo_line_to(cr, cx + s * 0.2, cy - s * 0.35);
                cairo_stroke(cr);
                break;
            case BISHOP:
                cairo_move_to(cr, cx, cy - s * 0.5);
                cairo_line_to(cr, cx - s * 0.25, cy + s * 0.4);
                cairo_line_to(cr, cx + s * 0.25, cy + s * 0.4);
                cairo_close_path(cr);
                cairo_stroke(cr);
                cairo_arc(cr, cx, cy - s * 0.5, s * 0.12, 0, 2 * M_PI);
                cairo_stroke(cr);
                break;
            case ROOK:
                cairo_rectangle(cr, cx - s * 0.3, cy - s * 0.1, s * 0.6, s * 0.5);
                cairo_stroke(cr);
                cairo_rectangle(cr, cx - s * 0.3, cy - s * 0.5, s * 0.15, s * 0.35);
                cairo_stroke(cr);
                cairo_rectangle(cr, cx - s * 0.05, cy - s * 0.5, s * 0.1, s * 0.35);
                cairo_stroke(cr);
                cairo_rectangle(cr, cx + s * 0.15, cy - s * 0.5, s * 0.15, s * 0.35);
                cairo_stroke(cr);
                break;
            case QUEEN:
                cairo_move_to(cr, cx, cy - s * 0.5);
                cairo_line_to(cr, cx - s * 0.15, cy - s * 0.2);
                cairo_line_to(cr, cx - s * 0.3, cy - s * 0.4);
                cairo_line_to(cr, cx - s * 0.3, cy + s * 0.4);
                cairo_line_to(cr, cx + s * 0.3, cy + s * 0.4);
                cairo_line_to(cr, cx + s * 0.3, cy - s * 0.4);
                cairo_line_to(cr, cx + s * 0.15, cy - s * 0.2);
                cairo_close_path(cr);
                cairo_stroke(cr);
                cairo_arc(cr, cx, cy - s * 0.5, s * 0.1, 0, 2 * M_PI);
                cairo_stroke(cr);
                break;
            case KING:
                cairo_rectangle(cr, cx - s * 0.3, cy - s * 0.1, s * 0.6, s * 0.5);
                cairo_stroke(cr);
                cairo_rectangle(cr, cx - s * 0.05, cy - s * 0.6, s * 0.1, s * 0.5);
                cairo_stroke(cr);
                cairo_rectangle(cr, cx - s * 0.25, cy - s * 0.45, s * 0.5, s * 0.1);
                cairo_stroke(cr);
                break;
            default:
                break;
        }
    }
}

void draw_chess_render_mode_button(BeatChessVisualization *chess, cairo_t *cr, int width, int height) {
    // Button position and size - LEFT SIDE, below UNDO button
    double button_width = 120;
    double button_height = 40;
    double button_x = 20;  // LEFT side, same as other buttons
    double button_y = 220;  // Below UNDO button (170 + 40 + 10 spacing)
    
    // Store button position for hit detection
    chess->render_mode_button_x = button_x;
    chess->render_mode_button_y = button_y;
    chess->render_mode_button_width = button_width;
    chess->render_mode_button_height = button_height;
    
    // Background
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_rectangle(cr, button_x, button_y, button_width, button_height);
    cairo_fill(cr);
    
    // Glow effect if hovered
    if (chess->render_mode_button_hovered || chess->render_mode_button_glow > 0) {
        double glow_alpha = chess->render_mode_button_glow * 0.5;
        if (chess->render_mode_button_hovered) glow_alpha = 0.4;
        
        cairo_set_source_rgba(cr, 1.0, 0.7, 0.2, glow_alpha);
        cairo_rectangle(cr, button_x - 3, button_y - 3, button_width + 6, button_height + 6);
        cairo_stroke(cr);
    }
    
    // Border - SAME AS RESET BUTTON
    cairo_set_source_rgb(cr, chess->render_mode_button_hovered ? 1.0 : 0.7, 
                         chess->render_mode_button_hovered ? 0.7 : 0.5, 
                         chess->render_mode_button_hovered ? 0.2 : 0.3);
    cairo_set_line_width(cr, 2);
    cairo_rectangle(cr, button_x, button_y, button_width, button_height);
    cairo_stroke(cr);
    
    // Text - show current rendering mode
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 14);
    
    const char *button_text = use_sprites ? "SPRITE" : "GEOMETRIC";
    
    cairo_text_extents_t extents;
    cairo_text_extents(cr, button_text, &extents);
    
    double text_x = button_x + (button_width - extents.width) / 2;
    double text_y = button_y + (button_height + extents.height) / 2;
    
    cairo_set_source_rgb(cr, chess->render_mode_button_hovered ? 1.0 : 0.9, 
                         chess->render_mode_button_hovered ? 0.8 : 0.7, 
                         chess->render_mode_button_hovered ? 0.3 : 0.4);
    cairo_move_to(cr, text_x, text_y);
    cairo_show_text(cr, button_text);
}

void draw_beat_chess(void *vis_ptr, cairo_t *cr) {
    Visualizer *vis = (Visualizer*)vis_ptr;
    BeatChessVisualization *chess = &vis->beat_chess;
    
    // Calculate board layout
    int width = vis->width;
    int height = vis->height;
    
    double available_width = width * 0.8;
    double available_height = height * 0.8;
    
    chess->cell_size = fmin(available_width / 8, available_height / 8);
    chess->board_offset_x = (width - chess->cell_size * 8) / 2;
    chess->board_offset_y = (height - chess->cell_size * 8) / 2;
    
    // Draw components
    draw_chess_board(chess, cr);
    draw_chess_last_move_highlight(chess, cr);
    draw_chess_pieces(chess, cr);
    draw_chess_eval_bar(chess, cr, width, height);
    draw_chess_status(chess, cr, width, height);
    // Draw buttons
    draw_chess_reset_button(chess, cr, width, height);
    draw_chess_pvsa_button(chess, cr, width, height);
    draw_chess_flip_button(chess, cr, width, height);
    draw_chess_undo_button(chess, cr, width, height);
    draw_chess_render_mode_button(chess, cr, width, height);

    // ===== COLOR SELECTION OVERLAY =====
    if (chess->choosing_color) {
        // Semi-transparent dark backdrop
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.72);
        cairo_rectangle(cr, 0, 0, width, height);
        cairo_fill(cr);

        // Panel
        double pw = 340, ph = 220;
        double px = (width - pw) / 2.0;
        double py = (height - ph) / 2.0;

        cairo_set_source_rgba(cr, 0.12, 0.12, 0.18, 0.97);
        cairo_rectangle(cr, px, py, pw, ph);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.55, 0.55, 0.7);
        cairo_set_line_width(cr, 2.0);
        cairo_rectangle(cr, px, py, pw, ph);
        cairo_stroke(cr);

        // Title
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 20);
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        const char *title = "Play as...";
        cairo_text_extents_t te;
        cairo_text_extents(cr, title, &te);
        cairo_move_to(cr, px + (pw - te.width) / 2.0, py + 48);
        cairo_show_text(cr, title);

        double bw = 120, bh = 56;
        double gap = 24;
        double btotal = bw * 2 + gap;
        double bx_start = px + (pw - btotal) / 2.0;
        double by = py + 90;

        // --- White button ---
        double wx = bx_start;
        chess->choose_white_button_x = wx;
        chess->choose_white_button_y = by;
        chess->choose_white_button_width = bw;
        chess->choose_white_button_height = bh;

        bool wh = chess->choose_white_button_hovered;
        // Shadow
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.3);
        cairo_rectangle(cr, wx + 3, by + 3, bw, bh);
        cairo_fill(cr);
        // Fill
        cairo_set_source_rgb(cr, wh ? 1.0 : 0.88, wh ? 1.0 : 0.88, wh ? 1.0 : 0.88);
        cairo_rectangle(cr, wx, by, bw, bh);
        cairo_fill(cr);
        // Border
        cairo_set_source_rgb(cr, wh ? 0.6 : 0.5, wh ? 0.6 : 0.5, wh ? 0.6 : 0.5);
        cairo_set_line_width(cr, wh ? 2.5 : 1.5);
        cairo_rectangle(cr, wx, by, bw, bh);
        cairo_stroke(cr);
        // Label
        cairo_set_font_size(cr, 16);
        cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        cairo_text_extents(cr, "White", &te);
        cairo_move_to(cr, wx + (bw - te.width) / 2.0, by + bh / 2.0 + te.height / 2.0);
        cairo_show_text(cr, "White");

        // --- Black button ---
        double bkx = bx_start + bw + gap;
        chess->choose_black_button_x = bkx;
        chess->choose_black_button_y = by;
        chess->choose_black_button_width = bw;
        chess->choose_black_button_height = bh;

        bool bkh = chess->choose_black_button_hovered;
        // Shadow
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.4);
        cairo_rectangle(cr, bkx + 3, by + 3, bw, bh);
        cairo_fill(cr);
        // Fill
        cairo_set_source_rgb(cr, bkh ? 0.28 : 0.18, bkh ? 0.28 : 0.18, bkh ? 0.28 : 0.18);
        cairo_rectangle(cr, bkx, by, bw, bh);
        cairo_fill(cr);
        // Border
        cairo_set_source_rgb(cr, bkh ? 0.8 : 0.5, bkh ? 0.8 : 0.5, bkh ? 0.8 : 0.5);
        cairo_set_line_width(cr, bkh ? 2.5 : 1.5);
        cairo_rectangle(cr, bkx, by, bw, bh);
        cairo_stroke(cr);
        // Label
        cairo_set_font_size(cr, 16);
        cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
        cairo_text_extents(cr, "Black", &te);
        cairo_move_to(cr, bkx + (bw - te.width) / 2.0, by + bh / 2.0 + te.height / 2.0);
        cairo_show_text(cr, "Black");

        // Cancel hint
        cairo_set_font_size(cr, 11);
        cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.9);
        const char *hint = "Press Player vs AI again to cancel";
        cairo_text_extents(cr, hint, &te);
        cairo_move_to(cr, px + (pw - te.width) / 2.0, py + ph - 16);
        cairo_show_text(cr, hint);
    }
    // ====================================
}

void draw_chess_flip_button(BeatChessVisualization *chess, cairo_t *cr, int width, int height) {
    // Only show flip button in Player vs AI mode
    if (!chess->player_vs_ai) return;
    
    // Button position and size - LEFT SIDE, below PvsA button
    double button_width = 120;
    double button_height = 40;
    double button_x = 20;  // LEFT side, same as other buttons
    double button_y = 120;  // Below PvsA button (70 + 40 + 10 spacing)
    
    // Store button position for hit detection
    chess->flip_button_x = button_x;
    chess->flip_button_y = button_y;
    chess->flip_button_width = button_width;
    chess->flip_button_height = button_height;
    
    // Background
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_rectangle(cr, button_x, button_y, button_width, button_height);
    cairo_fill(cr);
    
    // Glow effect if hovered
    if (chess->flip_button_hovered || chess->flip_button_glow > 0) {
        double glow_alpha = chess->flip_button_glow * 0.5;
        if (chess->flip_button_hovered) glow_alpha = 0.4;
        
        cairo_set_source_rgba(cr, 0.2, 0.7, 1.0, glow_alpha);  // Blue glow
        cairo_rectangle(cr, button_x - 3, button_y - 3, button_width + 6, button_height + 6);
        cairo_stroke(cr);
    }
    
    // Border - highlight if board is flipped
    cairo_set_source_rgb(cr, chess->flip_button_hovered ? 0.3 : (chess->board_flipped ? 0.4 : 0.5), 
                         chess->flip_button_hovered ? 0.9 : (chess->board_flipped ? 0.9 : 0.7), 
                         chess->flip_button_hovered ? 1.0 : (chess->board_flipped ? 1.0 : 0.6));
    cairo_set_line_width(cr, chess->board_flipped ? 3 : 2);
    cairo_rectangle(cr, button_x, button_y, button_width, button_height);
    cairo_stroke(cr);
    
    // Text
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12);
    
    const char *button_text = "FLIP BOARD";
    
    cairo_text_extents_t extents;
    cairo_text_extents(cr, button_text, &extents);
    
    double text_x = button_x + (button_width - extents.width) / 2;
    double text_y = button_y + (button_height + extents.height) / 2;
    
    cairo_set_source_rgb(cr, chess->flip_button_hovered ? 0.3 : (chess->board_flipped ? 0.4 : 0.8), 
                         chess->flip_button_hovered ? 0.9 : (chess->board_flipped ? 0.9 : 0.6), 
                         chess->flip_button_hovered ? 1.0 : (chess->board_flipped ? 1.0 : 0.2));
    cairo_move_to(cr, text_x, text_y);
    cairo_show_text(cr, button_text);
}

void draw_chess_undo_button(BeatChessVisualization *chess, cairo_t *cr, int width, int height) {
    // Only show undo button in Player vs AI mode
    if (!chess->player_vs_ai) return;
    
    // Button position and size - LEFT SIDE, below FLIP button
    double button_width = 120;
    double button_height = 40;
    double button_x = 20;  // LEFT side, same as other buttons
    double button_y = 170;  // Below FLIP button (120 + 40 + 10 spacing)
    
    // Store button position for hit detection
    chess->undo_button_x = button_x;
    chess->undo_button_y = button_y;
    chess->undo_button_width = button_width;
    chess->undo_button_height = button_height;
    
    // Disable button if can't undo
    bool can_undo = chess_can_undo(chess);
    
    // Background
    cairo_set_source_rgb(cr, can_undo ? 0.15 : 0.08, can_undo ? 0.15 : 0.08, can_undo ? 0.15 : 0.08);
    cairo_rectangle(cr, button_x, button_y, button_width, button_height);
    cairo_fill(cr);
    
    // Glow effect if hovered and enabled
    if ((chess->undo_button_hovered || chess->undo_button_glow > 0) && can_undo) {
        double glow_alpha = chess->undo_button_glow * 0.5;
        if (chess->undo_button_hovered) glow_alpha = 0.4;
        
        cairo_set_source_rgba(cr, 1.0, 0.4, 0.2, glow_alpha);
        cairo_rectangle(cr, button_x - 3, button_y - 3, button_width + 6, button_height + 6);
        cairo_stroke(cr);
    }
    
    // Border
    if (can_undo) {
        cairo_set_source_rgb(cr, chess->undo_button_hovered ? 1.0 : 0.6, 
                             chess->undo_button_hovered ? 0.4 : 0.3, 
                             chess->undo_button_hovered ? 0.2 : 0.2);
    } else {
        cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    }
    cairo_set_line_width(cr, 2);
    cairo_rectangle(cr, button_x, button_y, button_width, button_height);
    cairo_stroke(cr);
    
    // Text
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 14);
    
    cairo_text_extents_t extents;
    cairo_text_extents(cr, "UNDO", &extents);
    
    double text_x = button_x + (button_width - extents.width) / 2;
    double text_y = button_y + (button_height + extents.height) / 2;
    
    if (can_undo) {
        cairo_set_source_rgb(cr, chess->undo_button_hovered ? 1.0 : 0.8, 
                             chess->undo_button_hovered ? 0.6 : 0.4, 
                             chess->undo_button_hovered ? 0.3 : 0.2);
    } else {
        cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
    }
    cairo_move_to(cr, text_x, text_y);
    cairo_show_text(cr, "UNDO");
}

// ============================================================================
// SPRITE MODE CONTROL
// ============================================================================

bool is_render_mode_button_clicked(BeatChessVisualization *chess, double x, double y) {
    return (x >= chess->render_mode_button_x && x < chess->render_mode_button_x + chess->render_mode_button_width &&
            y >= chess->render_mode_button_y && y < chess->render_mode_button_y + chess->render_mode_button_height);
}

bool get_sprite_mode(void) {
    return use_sprites;
}

// Call this from your mouse event handler
void update_render_mode_button_click(BeatChessVisualization *chess, double x, double y, bool mouse_down) {
    bool is_over_button = is_render_mode_button_clicked(chess, x, y);
    
    // Update hover state
    chess->render_mode_button_hovered = is_over_button;
    
    // Handle click-release (button was pressed and is now released)
    if (is_over_button && mouse_down && !chess->render_mode_button_was_pressed) {
        chess->render_mode_button_was_pressed = true;
    } else if (!mouse_down && chess->render_mode_button_was_pressed) {
        // Mouse released - trigger toggle
        toggle_sprite_mode();
        chess->render_mode_button_was_pressed = false;
    } else if (!is_over_button) {
        chess->render_mode_button_was_pressed = false;
    }
}

// ============================================================================
// PUBLIC DRAWING FUNCTION (with toggle support)
// ============================================================================

void draw_piece(cairo_t *cr, PieceType type, ChessColor color, double x, double y, double size, double dance_offset) {
    if (type == EMPTY) {
        return;
    }
    
    if (use_sprites) {
        // Try to draw sprite, fall back to geometric if not available
        draw_sprite_piece(cr, type, color, x, y, size);
    } else {
        // Draw geometric piece
        draw_geometric_piece(cr, type, color, x, y, size, dance_offset);
    }
}

bool get_rendering_mode() {
    return use_sprites;
}
