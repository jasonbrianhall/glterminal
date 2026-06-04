/*
 * chess_ai_engine.h - Shared AI move calculation engine
 * 
 * This module provides a shared AI move calculation function that can be used
 * by both the DOS/Allegro and GTK versions of BeatChess.
 * 
 * The AI uses minimax with alpha-beta pruning to evaluate positions and 
 * selects moves within a threshold of the best move for variety.
 * 
 * DEBUG MODE:
 *   Define DEBUG or CHESS_AI_DEBUG before including this file to enable
 *   detailed debug output. This will print move evaluations, candidate
 *   selection, and other AI decision-making information.
 */

#ifndef CHESS_AI_ENGINE_H
#define CHESS_AI_ENGINE_H

#include "beatchess.h"
#include <stdio.h>

/* Debug mode - define CHESS_AI_DEBUG to enable debug output */
#ifdef DEBUG
    #define CHESS_AI_DEBUG 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Configuration structure for AI move calculation
 * 
 * This allows flexibility in how the AI behaves without changing the core
 * algorithm implementation.
 */
typedef struct {
    int search_depth;           /* Depth of minimax search (typically 3-5) */
    int threshold_centipawns;   /* Moves within this threshold are considered (e.g., 25) */
    bool use_randomization;     /* If true, randomly pick from candidate moves */
    double min_think_ms;   // minimum delay in ms; 0 = as fast as possible

} ChessAIConfig;

/**
 * Result structure for AI move calculation
 * 
 * Contains the selected move and metadata about the search
 */
typedef struct {
    ChessMove move;             /* The move to make */
    int score;                  /* Evaluation score of the move */
    int total_moves_evaluated;  /* Total number of legal moves evaluated */
    int candidate_moves;        /* Number of moves within threshold */
    double min_think_ms;        // minimum delay in ms; 0 = as fast as possible

} ChessAIMoveResult;

/**
 * Compute the best AI move for the current position
 * 
 * Uses minimax search with alpha-beta pruning to evaluate all legal moves
 * and selects the best one. If use_randomization is enabled, will randomly
 * select from all moves within threshold_centipawns of the best move.
 * 
 * @param game              Pointer to the current game state
 * @param config            AI configuration (search depth, threshold, etc.)
 * @return                  ChessAIMoveResult containing the selected move and stats
 */
ChessAIMoveResult chess_ai_compute_move(ChessGameState *game, ChessAIConfig config);

/**
 * Get the default AI configuration
 * 
 * Provides reasonable defaults:
 * - search_depth: 3
 * - threshold_centipawns: 25
 * - use_randomization: true
 * 
 * @return Default ChessAIConfig structure
 */
ChessAIConfig chess_ai_get_default_config(void);

#ifdef __cplusplus
}
#endif

#endif /* CHESS_AI_ENGINE_H */
