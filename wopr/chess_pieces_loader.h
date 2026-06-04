/*
 * chess_pieces_loader.h - Helper functions to load embedded BMP pieces
 * 
 * Include this AFTER chess_pieces.h
 * Compatible with DJGPP and Allegro 4 (no pack_fopen_mem dependency)
 */

#ifndef CHESS_PIECES_LOADER_H
#define CHESS_PIECES_LOADER_H

#include <allegro.h>
#include "chess_pieces.h"

/* Structure to hold all loaded piece bitmaps */
typedef struct {
    BITMAP *white_king;
    BITMAP *white_queen;
    BITMAP *white_rook;
    BITMAP *white_bishop;
    BITMAP *white_knight;
    BITMAP *white_pawn;
    BITMAP *black_king;
    BITMAP *black_queen;
    BITMAP *black_rook;
    BITMAP *black_bishop;
    BITMAP *black_knight;
    BITMAP *black_pawn;
} ChessPieceSprites;

/* Global sprite storage */
static ChessPieceSprites piece_sprites;

/* Simple BMP loader from memory - works with 24-bit uncompressed BMPs */
static BITMAP* load_bmp_from_memory(const unsigned char *data, unsigned int len) {
    BITMAP *bmp;
    int width, height;
    int data_offset;
    int row, col;
    const unsigned char *pixel_data;
    int bytes_per_row;
    int mask_color;
    
    /* Verify BMP signature */
    if (len < 54 || data[0] != 'B' || data[1] != 'M') {
        return NULL;
    }
    
    /* Read BMP header (little endian) */
    data_offset = data[10] | (data[11] << 8) | (data[12] << 16) | (data[13] << 24);
    width = data[18] | (data[19] << 8) | (data[20] << 16) | (data[21] << 24);
    height = data[22] | (data[23] << 8) | (data[24] << 16) | (data[25] << 24);
    
    /* Only support positive height (bottom-up BMP) */
    if (height < 0) height = -height;
    
    if (width <= 0 || height <= 0 || width > 2048 || height > 2048) {
        return NULL;
    }
    
    /* Create bitmap */
    bmp = create_bitmap(width, height);
    if (!bmp) {
        return NULL;
    }
    
    /* Get the correct mask color for this bitmap */
    mask_color = bitmap_mask_color(bmp);
    
    /* Calculate bytes per row (must be multiple of 4) */
    bytes_per_row = ((width * 3 + 3) / 4) * 4;
    
    /* Get pointer to pixel data */
    pixel_data = data + data_offset;
    
    /* Read pixel data (BMPs are stored bottom-up) */
    for (row = 0; row < height; row++) {
        const unsigned char *row_data = pixel_data + (height - 1 - row) * bytes_per_row;
        
        for (col = 0; col < width; col++) {
            unsigned char b = row_data[col * 3 + 0];
            unsigned char g = row_data[col * 3 + 1];
            unsigned char r = row_data[col * 3 + 2];
            
            /* Treat greenish pixels as transparency (green > red AND green > blue) */
            /* This catches pure green (0,255,0) and antialiased edges */
            if (g > r + 30 && g > b + 30 && g > 200) {
                /* Use the bitmap's mask color */
                putpixel(bmp, col, row, mask_color);
            } else {
                putpixel(bmp, col, row, makecol(r, g, b));
            }
        }
    }
    
    return bmp;
}

/* Initialize all chess piece sprites */
static int load_chess_pieces(void) {
    piece_sprites.white_king = load_bmp_from_memory(white_king_bmp, white_king_bmp_len);
    piece_sprites.white_queen = load_bmp_from_memory(white_queen_bmp, white_queen_bmp_len);
    piece_sprites.white_rook = load_bmp_from_memory(white_rook_bmp, white_rook_bmp_len);
    piece_sprites.white_bishop = load_bmp_from_memory(white_bishop_bmp, white_bishop_bmp_len);
    piece_sprites.white_knight = load_bmp_from_memory(white_knight_bmp, white_knight_bmp_len);
    piece_sprites.white_pawn = load_bmp_from_memory(white_pawn_bmp, white_pawn_bmp_len);
    
    piece_sprites.black_king = load_bmp_from_memory(black_king_bmp, black_king_bmp_len);
    piece_sprites.black_queen = load_bmp_from_memory(black_queen_bmp, black_queen_bmp_len);
    piece_sprites.black_rook = load_bmp_from_memory(black_rook_bmp, black_rook_bmp_len);
    piece_sprites.black_bishop = load_bmp_from_memory(black_bishop_bmp, black_bishop_bmp_len);
    piece_sprites.black_knight = load_bmp_from_memory(black_knight_bmp, black_knight_bmp_len);
    piece_sprites.black_pawn = load_bmp_from_memory(black_pawn_bmp, black_pawn_bmp_len);
    
    /* Check if all loaded successfully */
    if (!piece_sprites.white_king || !piece_sprites.white_queen || 
        !piece_sprites.white_rook || !piece_sprites.white_bishop ||
        !piece_sprites.white_knight || !piece_sprites.white_pawn ||
        !piece_sprites.black_king || !piece_sprites.black_queen ||
        !piece_sprites.black_rook || !piece_sprites.black_bishop ||
        !piece_sprites.black_knight || !piece_sprites.black_pawn) {
        return -1;  /* Failed to load */
    }
    
    return 0;  /* Success */
}

/* Get sprite for a specific piece */
static BITMAP* get_piece_sprite(PieceType type, ChessColor color) {
    if (color == WHITE) {
        switch (type) {
            case KING:   return piece_sprites.white_king;
            case QUEEN:  return piece_sprites.white_queen;
            case ROOK:   return piece_sprites.white_rook;
            case BISHOP: return piece_sprites.white_bishop;
            case KNIGHT: return piece_sprites.white_knight;
            case PAWN:   return piece_sprites.white_pawn;
            default:     return NULL;
        }
    } else if (color == BLACK) {
        switch (type) {
            case KING:   return piece_sprites.black_king;
            case QUEEN:  return piece_sprites.black_queen;
            case ROOK:   return piece_sprites.black_rook;
            case BISHOP: return piece_sprites.black_bishop;
            case KNIGHT: return piece_sprites.black_knight;
            case PAWN:   return piece_sprites.black_pawn;
            default:     return NULL;
        }
    }
    return NULL;
}

/* Cleanup all piece sprites */
static void destroy_chess_pieces(void) {
    if (piece_sprites.white_king) destroy_bitmap(piece_sprites.white_king);
    if (piece_sprites.white_queen) destroy_bitmap(piece_sprites.white_queen);
    if (piece_sprites.white_rook) destroy_bitmap(piece_sprites.white_rook);
    if (piece_sprites.white_bishop) destroy_bitmap(piece_sprites.white_bishop);
    if (piece_sprites.white_knight) destroy_bitmap(piece_sprites.white_knight);
    if (piece_sprites.white_pawn) destroy_bitmap(piece_sprites.white_pawn);
    
    if (piece_sprites.black_king) destroy_bitmap(piece_sprites.black_king);
    if (piece_sprites.black_queen) destroy_bitmap(piece_sprites.black_queen);
    if (piece_sprites.black_rook) destroy_bitmap(piece_sprites.black_rook);
    if (piece_sprites.black_bishop) destroy_bitmap(piece_sprites.black_bishop);
    if (piece_sprites.black_knight) destroy_bitmap(piece_sprites.black_knight);
    if (piece_sprites.black_pawn) destroy_bitmap(piece_sprites.black_pawn);
}

/* Draw piece sprite at screen coordinates (center position) */
static void draw_piece_sprite(BITMAP *sprite, int cx, int cy, int size) {
    if (!sprite) return;
    
    /* Calculate top-left position from center */
    int x = cx - size / 2;
    int y = cy - size / 2;
    
    /* If sprite is already correct size, draw it with transparency */
    if (sprite->w == size && sprite->h == size) {
        /* draw_sprite automatically uses the bitmap's mask color as transparent */
        draw_sprite(screen, sprite, x, y);
    } else {
        /* Need to scale the sprite */
        BITMAP *scaled = create_bitmap(size, size);
        if (scaled) {
            /* Clear with the mask color (transparent) */
            clear_to_color(scaled, bitmap_mask_color(scaled));
            
            /* Scale the sprite with transparency preserved */
            masked_stretch_blit(sprite, scaled, 0, 0, sprite->w, sprite->h, 0, 0, size, size);
            
            /* Draw with transparency */
            draw_sprite(screen, scaled, x, y);
            
            destroy_bitmap(scaled);
        }
    }
}

#endif /* CHESS_PIECES_LOADER_H */
