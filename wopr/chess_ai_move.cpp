/*
 * chess_ai_engine.cpp - Shared AI move calculation engine
 * 
 * Implementation of AI move calculation using minimax with alpha-beta pruning.
 * This module is used by both the DOS/Allegro and GTK versions of BeatChess.
 * 
 * DEBUG OUTPUT:
 *   Define CHESS_AI_DEBUG or DEBUG before including chess_ai_engine.h to enable
 *   detailed debug output including move scores, candidate selection, etc.
 */

#include "chess_ai_move.h"
#include <stdlib.h>
#include <limits.h>

#ifdef ALLEGRO_H
    #include <allegro.h>
    #define AI_YIELD_IMPL() rest(500)
#elif defined(__GTK_H__)
    #include <glib.h>
    #define AI_YIELD_IMPL() g_usleep(50000)
#else
    #define AI_YIELD_IMPL() do {} while(0)  /* No-op */
#endif

/**
 * Internal structure to track scored moves for candidate selection
 */
typedef struct {
    ChessMove move;
    int score;
} ScoredMove;

/**
 * Debug macro - only prints if CHESS_AI_DEBUG is defined
 */
#ifdef CHESS_AI_DEBUG
    #define DEBUG_PRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
    #define DEBUG_MOVE(m) \
        printf("    %c%d -> %c%d", \
               'a' + (m).from_col, 8 - (m).from_row, \
               'a' + (m).to_col, 8 - (m).to_row)
#else
    #define DEBUG_PRINT(fmt, ...) do {} while(0)
    #define DEBUG_MOVE(m) do {} while(0)
#endif

/**
 * Get the default AI configuration
 */
ChessAIConfig chess_ai_get_default_config(void) {
    ChessAIConfig config;
#ifdef __linux__
    config.search_depth = 4;
#else
    config.search_depth = 3;
#endif
    config.threshold_centipawns = 25;
    config.use_randomization = true;
    return config;
}

/**
 * Compute the best AI move for the current position
 * 
 * Algorithm:
 * 1. Generate all legal moves
 * 2. For each move, use minimax to evaluate the resulting position
 * 3. Track the best move found
 * 4. If randomization is enabled, collect all moves within the threshold
 *    and randomly select from them
 * 5. Return the selected move along with evaluation stats
 */
ChessAIMoveResult chess_ai_compute_move(ChessGameState *game, ChessAIConfig config) {
    ChessAIMoveResult result;
    
    /* Initialize result */
    result.move.from_row = -1;
    result.move.from_col = -1;
    result.move.to_row = -1;
    result.move.to_col = -1;
    result.score = 0;
    result.total_moves_evaluated = 0;
    result.candidate_moves = 0;
    
    /* Get all legal moves */
    ChessMove moves[256];
    int num_moves = chess_get_all_moves(game, game->turn, moves);
    DEBUG_PRINT("\n=== CHESS_AI_COMPUTE_MOVE ===\n");
    DEBUG_PRINT("Current turn: %s\n", game->turn == WHITE ? "WHITE" : "BLACK");
    DEBUG_PRINT("Total pseudo-legal moves: %d\n", num_moves);
    DEBUG_PRINT("Search depth: %d\n", config.search_depth);
    DEBUG_PRINT("Threshold: %d centipawns\n", config.threshold_centipawns);
    DEBUG_PRINT("Randomization: %s\n", config.use_randomization ? "ON" : "OFF");
    
    if (num_moves == 0) {
        DEBUG_PRINT("ERROR: No moves available!\n");
        return result;  /* No moves available */
    }
    
    /* Determine if we're maximizing or minimizing
     * White maximizes (wants positive scores)
     * Black minimizes (wants negative scores) */
    bool we_are_white = (game->turn == WHITE);
    int best_score = we_are_white ? INT_MIN : INT_MAX;
    ChessMove best_move = {-1, -1, -1, -1, 0};
    
    /* Store all evaluated moves to avoid re-evaluation */
    ScoredMove all_evaluated_moves[256];
    int valid_move_count = 0;
    
    DEBUG_PRINT("\nPHASE 1: Finding best move (evaluating each position)...\n");
    
    /* Phase 1: Find the best move using minimax and store results */
    for (int i = 0; i < num_moves; i++) {
        AI_YIELD_IMPL();
        ChessGameState temp = *game;
        chess_make_move(&temp, moves[i]);
        
        /* Skip if move leaves king in check */
        if (chess_is_in_check(&temp, game->turn)) {
            DEBUG_PRINT("Move %d: ", i);
            DEBUG_MOVE(moves[i]);
            DEBUG_PRINT(" - ILLEGAL (leaves king in check)\n");
            continue;
        }
        
        /* Evaluate this move using minimax from opponent's perspective */
        bool opponent_is_white = (temp.turn == WHITE);
        int score = chess_minimax(&temp, config.search_depth - 1, INT_MIN, INT_MAX, opponent_is_white);
        
        result.total_moves_evaluated++;
        
        /* Store the evaluated move */
        all_evaluated_moves[valid_move_count].move = moves[i];
        all_evaluated_moves[valid_move_count].score = score;
        valid_move_count++;
        
        DEBUG_PRINT("Move %d: ", i);
        DEBUG_MOVE(moves[i]);
        DEBUG_PRINT(" score=%d", score);
        
        /* Update best move based on whether we're maximizing or minimizing */
        if (we_are_white) {
            /* White maximizes - wants the highest score */
            if (score > best_score) {
                best_score = score;
                best_move = moves[i];
                DEBUG_PRINT(" <- NEW BEST (White maximizing)\n");
            } else {
                DEBUG_PRINT("\n");
            }
        } else {
            /* Black minimizes - wants the lowest score */
            if (score < best_score) {
                best_score = score;
                best_move = moves[i];
                DEBUG_PRINT(" <- NEW BEST (Black minimizing)\n");
            } else {
                DEBUG_PRINT("\n");
            }
        }
    }
    
    DEBUG_PRINT("\nBest move found: ");
    DEBUG_MOVE(best_move);
    DEBUG_PRINT(" with score %d\n", best_score);
    DEBUG_PRINT("Total legal moves evaluated: %d\n", result.total_moves_evaluated);
    
    /* If randomization is disabled, return the best move */
    if (!config.use_randomization) {
        result.move = best_move;
        result.score = best_score;
        result.candidate_moves = 1;
        DEBUG_PRINT("\nRandomization OFF - returning best move\n");
        DEBUG_PRINT("=== FINAL DECISION ===\n");
        DEBUG_PRINT("Selected: ");
        DEBUG_MOVE(result.move);
        DEBUG_PRINT(" (score: %d)\n\n", result.score);
        return result;
    }
    
    /* Phase 2: If randomization is enabled, collect all moves within threshold */
    DEBUG_PRINT("\nPHASE 2: Collecting candidate moves within threshold...\n");
    DEBUG_PRINT("Threshold range: ");
    if (we_are_white) {
        DEBUG_PRINT("[%d, %d] (White maximizing - score >= %d)\n", 
                   best_score - config.threshold_centipawns, best_score,
                   best_score - config.threshold_centipawns);
    } else {
        DEBUG_PRINT("[%d, %d] (Black minimizing - score <= %d)\n", 
                   best_score, best_score + config.threshold_centipawns,
                   best_score + config.threshold_centipawns);
    }
    
    ScoredMove candidate_moves[256];
    int candidate_count = 0;
    
    /* Reuse stored evaluations from PHASE 1 */
    for (int i = 0; i < valid_move_count; i++) {
        int score = all_evaluated_moves[i].score;
        
        /* Check if within threshold */
        bool is_within_threshold = false;
        if (we_are_white) {
            /* White: move is good if score >= best_score - threshold */
            is_within_threshold = (score >= best_score - config.threshold_centipawns);
        } else {
            /* Black: move is good if score <= best_score + threshold */
            is_within_threshold = (score <= best_score + config.threshold_centipawns);
        }
        
        if (is_within_threshold) {
            candidate_moves[candidate_count].move = all_evaluated_moves[i].move;
            candidate_moves[candidate_count].score = score;
            candidate_count++;
            DEBUG_PRINT("  Candidate %d: ", candidate_count);
            DEBUG_MOVE(all_evaluated_moves[i].move);
            DEBUG_PRINT(" (score: %d)\n", score);
        }
    }
    
    DEBUG_PRINT("\nTotal candidates within threshold: %d\n", candidate_count);
    
    /* Phase 3: Randomly select from candidates */
    if (candidate_count > 0) {
        int choice = rand() % candidate_count;
        result.move = candidate_moves[choice].move;
        result.score = candidate_moves[choice].score;
        result.candidate_moves = candidate_count;
        DEBUG_PRINT("\nPHASE 3: Randomly selected candidate #%d of %d\n", choice + 1, candidate_count);
    } else {
        /* Shouldn't happen, but fallback to best move */
        DEBUG_PRINT("\nWARNING: No candidates within threshold! Using best move as fallback.\n");
        result.move = best_move;
        result.score = best_score;
        result.candidate_moves = 1;
    }
    
    DEBUG_PRINT("\n=== FINAL DECISION ===\n");
    DEBUG_PRINT("Selected: ");
    DEBUG_MOVE(result.move);
    DEBUG_PRINT(" (score: %d, chosen from %d candidates)\n\n", result.score, result.candidate_moves);
    AI_YIELD_IMPL();

    return result;
}
