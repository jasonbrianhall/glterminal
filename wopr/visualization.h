#ifndef VISUALIZATION_H
#define VISUALIZATION_H

#include <stdbool.h>
#include <math.h>
#include <string.h>
#include "beatchess.h"

typedef struct {
    int width, height;
    double volume_level;
    BeatChessVisualization beat_chess;
    int mouse_x;
    int mouse_y;
    bool mouse_left_pressed;
} Visualizer;

bool chess_is_in_bounds(int r, int c);
bool chess_is_path_clear(ChessGameState *game, int fr, int fc, int tr, int tc);
ChessGameStatus chess_check_game_status(ChessGameState *game);
void update_beat_chess(void *vis_ptr, double dt);

bool chess_is_in_bounds(int r, int c);
bool chess_is_path_clear(ChessGameState *game, int fr, int fc, int tr, int tc);
void chess_init_board(ChessGameState *game);
bool chess_is_valid_move(ChessGameState *game, int fr, int fc, int tr, int tc);
bool chess_is_in_check(ChessGameState *game, ChessColor color);
void chess_make_move(ChessGameState *game, ChessMove move);
int chess_evaluate_position(ChessGameState *game);
int chess_get_all_moves(ChessGameState *game, ChessColor color, ChessMove *moves);
int chess_minimax(ChessGameState *game, int depth, int alpha, int beta, bool maximizing);
ChessGameStatus chess_check_game_status(ChessGameState *game);

void chess_init_thinking_state(ChessThinkingState *ts);
void chess_start_thinking(ChessThinkingState *ts, ChessGameState *game);
ChessMove chess_get_best_move_now(ChessThinkingState *ts);
void chess_stop_thinking(ChessThinkingState *ts);
void* chess_think_continuously(void* arg);

#endif
