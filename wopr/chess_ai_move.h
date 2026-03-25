/*
 * chess_ai_engine.h - Shared AI move calculation engine
 */

#ifndef CHESS_AI_ENGINE_H
#define CHESS_AI_ENGINE_H

#include "beatchess.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int search_depth;
    int threshold_centipawns;
    bool use_randomization;
} ChessAIConfig;

typedef struct {
    ChessMove move;
    int score;
    int total_moves_evaluated;
    int candidate_moves;
} ChessAIMoveResult;

ChessAIMoveResult chess_ai_compute_move(ChessGameState *game, ChessAIConfig config);
ChessAIConfig     chess_ai_get_default_config(void);

#ifdef __cplusplus
}
#endif

#endif /* CHESS_AI_ENGINE_H */
