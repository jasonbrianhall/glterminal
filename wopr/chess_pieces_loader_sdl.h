/*
 * chess_pieces_loader_sdl.h - SDL2 chess piece loader using NanoSVG
 *
 * Rasterizes embedded SVG data from chess_pieces_svg.h at runtime.
 * Requires nanosvg.h and nanosvgrast.h in the project directory:
 *   https://github.com/memononen/nanosvg
 *
 * Also provides sdl_show_splashscreen() for the JPEG splash via stb_image.
 */

#ifndef CHESS_PIECES_LOADER_SDL_H
#define CHESS_PIECES_LOADER_SDL_H

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "beatchess.h"
#include "chess_pieces_svg.h"

/* NanoSVG — define implementations exactly once here */
#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

/* stb_image for JPEG splash */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

/* ============================================================================
 * SVG -> SDL_Texture
 * ============================================================================ */

static SDL_Texture *sdl_load_svg_from_memory(SDL_Renderer *renderer,
                                               const unsigned char *data,
                                               unsigned int len,
                                               int render_size) {
    /* NanoSVG needs a null-terminated mutable char buffer */
    char *buf = (char *)malloc(len + 1);
    if (!buf) return NULL;
    memcpy(buf, data, len);
    buf[len] = '\0';

    NSVGimage *image = nsvgParse(buf, "px", 96.0f);
    free(buf);
    if (!image) return NULL;

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(image); return NULL; }

    unsigned char *pixels = (unsigned char *)malloc(render_size * render_size * 4);
    if (!pixels) {
        nsvgDeleteRasterizer(rast);
        nsvgDelete(image);
        return NULL;
    }

    float scale = (float)render_size / (image->width > 0 ? image->width : 45.0f);
    nsvgRasterize(rast, image, 0, 0, scale, pixels, render_size, render_size,
                  render_size * 4);

    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    SDL_Texture *tex = SDL_CreateTexture(renderer,
                                          SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC,
                                          render_size, render_size);
    if (!tex) { free(pixels); return NULL; }

    SDL_UpdateTexture(tex, NULL, pixels, render_size * 4);
    free(pixels);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return tex;
}

/* ============================================================================
 * Piece texture storage
 * ============================================================================ */

#define SVG_RENDER_SIZE 128

typedef struct {
    SDL_Texture *white_king,   *white_queen,  *white_rook;
    SDL_Texture *white_bishop, *white_knight, *white_pawn;
    SDL_Texture *black_king,   *black_queen,  *black_rook;
    SDL_Texture *black_bishop, *black_knight, *black_pawn;
} SdlPieceTextures;

static SdlPieceTextures sdl_piece_textures;

static int sdl_load_chess_pieces(SDL_Renderer *renderer) {
    sdl_piece_textures.white_king   = sdl_load_svg_from_memory(renderer, wK_svg, wK_svg_len, SVG_RENDER_SIZE);
    sdl_piece_textures.white_queen  = sdl_load_svg_from_memory(renderer, wQ_svg, wQ_svg_len, SVG_RENDER_SIZE);
    sdl_piece_textures.white_rook   = sdl_load_svg_from_memory(renderer, wR_svg, wR_svg_len, SVG_RENDER_SIZE);
    sdl_piece_textures.white_bishop = sdl_load_svg_from_memory(renderer, wB_svg, wB_svg_len, SVG_RENDER_SIZE);
    sdl_piece_textures.white_knight = sdl_load_svg_from_memory(renderer, wN_svg, wN_svg_len, SVG_RENDER_SIZE);
    sdl_piece_textures.white_pawn   = sdl_load_svg_from_memory(renderer, wP_svg, wP_svg_len, SVG_RENDER_SIZE);
    sdl_piece_textures.black_king   = sdl_load_svg_from_memory(renderer, bK_svg, bK_svg_len, SVG_RENDER_SIZE);
    sdl_piece_textures.black_queen  = sdl_load_svg_from_memory(renderer, bQ_svg, bQ_svg_len, SVG_RENDER_SIZE);
    sdl_piece_textures.black_rook   = sdl_load_svg_from_memory(renderer, bR_svg, bR_svg_len, SVG_RENDER_SIZE);
    sdl_piece_textures.black_bishop = sdl_load_svg_from_memory(renderer, bB_svg, bB_svg_len, SVG_RENDER_SIZE);
    sdl_piece_textures.black_knight = sdl_load_svg_from_memory(renderer, bN_svg, bN_svg_len, SVG_RENDER_SIZE);
    sdl_piece_textures.black_pawn   = sdl_load_svg_from_memory(renderer, bP_svg, bP_svg_len, SVG_RENDER_SIZE);

    if (!sdl_piece_textures.white_king   || !sdl_piece_textures.white_queen  ||
        !sdl_piece_textures.white_rook   || !sdl_piece_textures.white_bishop ||
        !sdl_piece_textures.white_knight || !sdl_piece_textures.white_pawn   ||
        !sdl_piece_textures.black_king   || !sdl_piece_textures.black_queen  ||
        !sdl_piece_textures.black_rook   || !sdl_piece_textures.black_bishop ||
        !sdl_piece_textures.black_knight || !sdl_piece_textures.black_pawn) {
        fprintf(stderr, "Failed to load one or more piece SVGs\n");
        return -1;
    }
    return 0;
}

static SDL_Texture *sdl_get_piece_texture(PieceType type, ChessColor color) {
    if (color == WHITE) {
        switch (type) {
            case KING:   return sdl_piece_textures.white_king;
            case QUEEN:  return sdl_piece_textures.white_queen;
            case ROOK:   return sdl_piece_textures.white_rook;
            case BISHOP: return sdl_piece_textures.white_bishop;
            case KNIGHT: return sdl_piece_textures.white_knight;
            case PAWN:   return sdl_piece_textures.white_pawn;
            default:     return NULL;
        }
    } else {
        switch (type) {
            case KING:   return sdl_piece_textures.black_king;
            case QUEEN:  return sdl_piece_textures.black_queen;
            case ROOK:   return sdl_piece_textures.black_rook;
            case BISHOP: return sdl_piece_textures.black_bishop;
            case KNIGHT: return sdl_piece_textures.black_knight;
            case PAWN:   return sdl_piece_textures.black_pawn;
            default:     return NULL;
        }
    }
}

static void sdl_draw_piece(SDL_Renderer *renderer, SDL_Texture *tex,
                            int cx, int cy, int size) {
    if (!tex) return;
    SDL_Rect dst = { cx - size/2, cy - size/2, size, size };
    SDL_RenderCopy(renderer, tex, NULL, &dst);
}

static void sdl_destroy_chess_pieces(void) {
    SDL_Texture **all[] = {
        &sdl_piece_textures.white_king,   &sdl_piece_textures.white_queen,
        &sdl_piece_textures.white_rook,   &sdl_piece_textures.white_bishop,
        &sdl_piece_textures.white_knight, &sdl_piece_textures.white_pawn,
        &sdl_piece_textures.black_king,   &sdl_piece_textures.black_queen,
        &sdl_piece_textures.black_rook,   &sdl_piece_textures.black_bishop,
        &sdl_piece_textures.black_knight, &sdl_piece_textures.black_pawn,
    };
    for (int i = 0; i < 12; i++) {
        if (*all[i]) { SDL_DestroyTexture(*all[i]); *all[i] = NULL; }
    }
}

/* ============================================================================
 * Splash screen — JPEG via stb_image
 * ============================================================================ */

#include "splashscreen.h"

static void sdl_show_splashscreen(SDL_Renderer *renderer, int logical_w, int logical_h) {
    int width, height, channels;
    unsigned char *pixels = stbi_load_from_memory(
        splashscreen_jpg, (int)splashscreen_jpg_len,
        &width, &height, &channels, 4);

    if (!pixels) {
        fprintf(stderr, "Warning: could not decode splash screen\n");
        SDL_Delay(500);
        return;
    }

    SDL_Texture *tex = SDL_CreateTexture(renderer,
                                          SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC,
                                          width, height);
    if (!tex) { stbi_image_free(pixels); SDL_Delay(500); return; }

    SDL_UpdateTexture(tex, NULL, pixels, width * 4);
    stbi_image_free(pixels);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);

    /* Scale to fit while preserving aspect ratio */
    float scale = (float)logical_w / width;
    if ((int)(height * scale) > logical_h)
        scale = (float)logical_h / height;
    SDL_Rect dst;
    dst.w = (int)(width  * scale);
    dst.h = (int)(height * scale);
    dst.x = (logical_w - dst.w) / 2;
    dst.y = (logical_h - dst.h) / 2;

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, tex, NULL, &dst);
    SDL_RenderPresent(renderer);
    SDL_DestroyTexture(tex);

    /* Wait up to 10s — SDL_Delay(16) keeps CPU idle between polls */
    Uint32 deadline = SDL_GetTicks() + 1500;
    SDL_Event ev;
    while (SDL_GetTicks() < deadline) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_KEYDOWN ||
                ev.type == SDL_MOUSEBUTTONDOWN ||
                ev.type == SDL_QUIT) return;
        }
        SDL_Delay(16);
    }
}

#endif /* CHESS_PIECES_LOADER_SDL_H */
