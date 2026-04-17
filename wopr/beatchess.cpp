#include "beatchess.h"
#include "visualization.h"
#include "chess_pieces.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#ifdef MSDOS
#include "math_compat.h"
#endif

// ============================================================================
// CORE CHESS ENGINE
// ============================================================================

#ifndef MAX_DEPTH
#define MAX_DEPTH 10
#endif
/* ============================================================================
 * OPTIMIZATION: ZOBRIST HASHING
 * ============================================================================
 */

static ZobristTable global_zobrist = {0};
static bool zobrist_initialized = false;

void chess_init_zobrist(void) {
    if (zobrist_initialized) return;
    srand(12345);
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            for (int p = 0; p < 7; p++) {
                global_zobrist.zobrist_board[r][c][p] = ((uint64_t)rand() << 32) | rand();
            }
        }
    }
    global_zobrist.zobrist_side_to_move = ((uint64_t)rand() << 32) | rand();
    for (int i = 0; i < 8; i++) {
        global_zobrist.zobrist_en_passant[i] = ((uint64_t)rand() << 32) | rand();
    }
    for (int i = 0; i < 4; i++) {
        global_zobrist.zobrist_castling[i] = ((uint64_t)rand() << 32) | rand();
    }
    zobrist_initialized = true;
}

uint64_t chess_zobrist_hash(ChessGameState *game) {
    uint64_t hash = 0;
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            if (game->board[r][c].type != EMPTY) {
                int piece_type = game->board[r][c].type;
                if (game->board[r][c].color == BLACK) piece_type += 6;
                if (piece_type < 7) hash ^= global_zobrist.zobrist_board[r][c][piece_type];
            }
        }
    }
    if (game->turn == BLACK) hash ^= global_zobrist.zobrist_side_to_move;
    if (game->en_passant_col >= 0 && game->en_passant_col < 8) {
        hash ^= global_zobrist.zobrist_en_passant[game->en_passant_col];
    }
    int castling_idx = 0;
    if (!game->white_king_moved && !game->white_rook_h_moved) castling_idx |= 1;
    if (!game->white_king_moved && !game->white_rook_a_moved) castling_idx |= 2;
    if (!game->black_king_moved && !game->black_rook_h_moved) castling_idx |= 4;
    if (!game->black_king_moved && !game->black_rook_a_moved) castling_idx |= 8;
    for (int i = 0; i < 4; i++) {
        if (castling_idx & (1 << i)) hash ^= global_zobrist.zobrist_castling[i];
    }
    return hash;
}

/* ============================================================================
 * OPTIMIZATION: TRANSPOSITION TABLE
 * ============================================================================
 */

static TranspositionEntry global_tt[TRANSPOSITION_TABLE_SIZE];

void chess_clear_transposition_table(void) {
    memset(global_tt, 0, sizeof(global_tt));
}

void chess_store_tt(uint64_t hash, int depth, int value, TTFlag flag) {
    uint32_t idx = hash & TT_MASK;
    TranspositionEntry *entry = &global_tt[idx];
    if (entry->hash != hash || depth >= entry->depth) {
        entry->hash = hash;
        entry->value = value;
        entry->depth = depth;
        entry->flag = flag;
    }
}

bool chess_probe_tt(uint64_t hash, int depth, int alpha, int beta, int *out_value) {
    uint32_t idx = hash & TT_MASK;
    TranspositionEntry *entry = &global_tt[idx];
    if (entry->hash != hash || entry->depth < depth) return false;
    if (entry->flag == TT_EXACT) {
        *out_value = entry->value;
        return true;
    } else if (entry->flag == TT_LOWER) {
        if (entry->value >= beta) {
            *out_value = entry->value;
            return true;
        }
    } else if (entry->flag == TT_UPPER) {
        if (entry->value <= alpha) {
            *out_value = entry->value;
            return true;
        }
    }
    return false;
}

/* ============================================================================
 * OPTIMIZATION: KILLER MOVES
 * ============================================================================
 */

static KillerMoveTable global_killers = {0};

void chess_clear_killers(KillerMoveTable *killers) {
    memset(killers, 0, sizeof(KillerMoveTable));
}

static bool moves_equal(ChessMove a, ChessMove b) {
    return a.from_row == b.from_row && a.from_col == b.from_col &&
           a.to_row == b.to_row && a.to_col == b.to_col;
}

void chess_update_killer_move(KillerMoveTable *killers, ChessMove move, int depth) {
    if (depth > MAX_CHESS_DEPTH) return;
    if (killers->killer_count[depth] > 0 && moves_equal(move, killers->killer_moves[depth][0])) return;
    if (killers->killer_count[depth] < MAX_KILLERS_PER_DEPTH) {
        for (int i = killers->killer_count[depth]; i > 0; i--) {
            killers->killer_moves[depth][i] = killers->killer_moves[depth][i-1];
        }
        killers->killer_count[depth]++;
    } else {
        for (int i = MAX_KILLERS_PER_DEPTH - 1; i > 0; i--) {
            killers->killer_moves[depth][i] = killers->killer_moves[depth][i-1];
        }
    }
    killers->killer_moves[depth][0] = move;
}

static void chess_reorder_moves_with_killers(ChessGameState *game, ChessMove *moves, int *move_count, KillerMoveTable *killers, int depth) {
    static int piece_values[] = {0, 100, 320, 330, 500, 900, 20000};
    typedef struct { ChessMove move; int score; } ScoredMove;
    ScoredMove scored[256];
    
    for (int i = 0; i < *move_count; i++) {
        scored[i].move = moves[i];
        scored[i].score = 0;
        ChessPiece target = game->board[moves[i].to_row][moves[i].to_col];
        ChessPiece attacker = game->board[moves[i].from_row][moves[i].from_col];
        
        if (killers && killers->killer_count[depth] > 0) {
            for (int k = 0; k < killers->killer_count[depth]; k++) {
                if (moves_equal(moves[i], killers->killer_moves[depth][k])) {
                    scored[i].score = 5000 - (k * 100);
                    break;
                }
            }
        }
        if (scored[i].score > 0) continue;
        
        if (target.type != EMPTY) {
            scored[i].score = (piece_values[target.type] * 10) - piece_values[attacker.type];
        }
        if (target.type == EMPTY) {
            ChessGameState temp = *game;
            chess_make_move(&temp, moves[i]);
            if (chess_is_in_check(&temp, game->turn == WHITE ? BLACK : WHITE)) {
                scored[i].score += 300;
            }
        }
    }
    
    for (int i = 0; i < *move_count - 1; i++) {
        for (int j = 0; j < *move_count - i - 1; j++) {
            if (scored[j].score < scored[j+1].score) {
                ScoredMove tmp = scored[j];
                scored[j] = scored[j+1];
                scored[j+1] = tmp;
            }
        }
    }
    for (int i = 0; i < *move_count; i++) {
        moves[i] = scored[i].move;
    }
}

/* ============================================================================
 * OPTIMIZATION: ENHANCED MINIMAX
 * ============================================================================
 */


/* Forward declaration */
static int chess_aggressive_filter_moves(ChessGameState *game, ChessMove *moves, 
                                        int count, int depth, int initial_depth);

int chess_minimax_enhanced(ChessGameState *game, int depth, int initial_depth, int alpha, int beta, bool maximizing, KillerMoveTable *killers) {
    uint64_t hash = chess_zobrist_hash(game);
    int tt_value = 0;
    if (chess_probe_tt(hash, depth, alpha, beta, &tt_value)) return tt_value;
    
    ChessMove moves[256];
    int move_count = chess_get_all_moves(game, game->turn, moves);
    
    if (move_count == 0) {
        if (chess_is_in_check(game, game->turn)) {
            int value = maximizing ? (-1000000 + depth) : (1000000 - depth);
            chess_store_tt(hash, depth, value, TT_EXACT);
            return value;
        }
        chess_store_tt(hash, depth, 0, TT_EXACT);
        return 0;
    }
    
    if (depth == 0) {
        int eval = chess_evaluate_position(game);
        chess_store_tt(hash, depth, eval, TT_EXACT);
        return eval;
    }
    
    if (depth <= 3 && depth > 0) {
        int eval = chess_evaluate_position(game);
        int margin = 300 * depth;
        if (maximizing && eval + margin < alpha) {
            chess_store_tt(hash, depth, eval, TT_UPPER);
            return eval;
        }
        if (!maximizing && eval - margin > beta) {
            chess_store_tt(hash, depth, eval, TT_UPPER);
            return eval;
        }
    }
    
    /*move_count = chess_aggressive_filter_moves(game, moves, move_count, depth, initial_depth);
    if (move_count == 0) {
        int eval = chess_evaluate_position(game);
        chess_store_tt(hash, depth, eval, TT_EXACT);
        return eval;
    }*/
    
    chess_reorder_moves_with_killers(game, moves, &move_count, killers, depth);
    
    int best_value = maximizing ? INT_MIN : INT_MAX;
    TTFlag flag = TT_UPPER;
    
    if (maximizing) {
        for (int i = 0; i < move_count; i++) {
            ChessPiece target = game->board[moves[i].to_row][moves[i].to_col];
            ChessGameState temp = *game;
            chess_make_move(&temp, moves[i]);
            int eval = chess_minimax_enhanced(&temp, depth - 1, initial_depth, alpha, beta, false, killers);
            best_value = (eval > best_value) ? eval : best_value;
            alpha = (alpha > best_value) ? alpha : best_value;
            if (beta <= alpha) {
                if (killers && target.type == EMPTY) chess_update_killer_move(killers, moves[i], depth);
                flag = TT_LOWER;
                break;
            }
        }
        if (best_value > alpha && best_value < beta) flag = TT_EXACT;
    } else {
        for (int i = 0; i < move_count; i++) {
            ChessPiece target = game->board[moves[i].to_row][moves[i].to_col];
            ChessGameState temp = *game;
            chess_make_move(&temp, moves[i]);
            int eval = chess_minimax_enhanced(&temp, depth - 1, initial_depth, alpha, beta, true, killers);
            best_value = (eval < best_value) ? eval : best_value;
            beta = (beta < best_value) ? beta : best_value;
            if (beta <= alpha) {
                if (killers && target.type == EMPTY) chess_update_killer_move(killers, moves[i], depth);
                flag = TT_UPPER;
                break;
            }
        }
        if (best_value > alpha && best_value < beta) flag = TT_EXACT;
    }
    
    chess_store_tt(hash, depth, best_value, flag);
    return best_value;
}

#define MAX_KILLERS_PER_DEPTH 2

typedef struct {
    ChessMove move;
    int score;
} ScoredMove;

static bool chess_should_eliminate_move(ChessGameState *game, ChessMove move, int depth, int initial_depth) {
    // Validate move bounds
    if (!chess_is_in_bounds(move.from_row, move.from_col) ||
        !chess_is_in_bounds(move.to_row, move.to_col)) {
        return true;  // Out of bounds
    }
    
    // Get the piece being moved
    ChessPiece attacker = game->board[move.from_row][move.from_col];
    ChessPiece target = game->board[move.to_row][move.to_col];
    
    // Can't move empty squares or opponent pieces
    if (attacker.type == EMPTY || attacker.color != game->turn) {
        return true;  // Illegal move
    }
    
    // Can't capture own pieces
    if (target.color == game->turn) {
        return true;  // Illegal move
    }
    
    // Check if move leaves king in check (illegal for any piece)
    ChessGameState temp = *game;
    chess_make_move(&temp, move);
    
    // After moving, check if the side that just moved is in check.
    // The turn has already switched in temp, so we check the moving side's original color.
    ChessColor moving_color = game->turn;
    if (chess_is_in_check(&temp, moving_color)) {
        return true;  // Moving piece leaves king in check - ILLEGAL
    }
    
    return false;  // Move is legal
}

bool chess_is_in_bounds(int r, int c) {
    return r >= 0 && r < BOARD_SIZE && c >= 0 && c < BOARD_SIZE;
}

bool chess_is_path_clear(ChessGameState *game, int fr, int fc, int tr, int tc) {
    int dr = (tr > fr) ? 1 : (tr < fr) ? -1 : 0;
    int dc = (tc > fc) ? 1 : (tc < fc) ? -1 : 0;
    
    int r = fr + dr;
    int c = fc + dc;
    
    while (r != tr || c != tc) {
        if (game->board[r][c].type != EMPTY) return false;
        r += dr;
        c += dc;
    }
    return true;
}

void chess_init_board(ChessGameState *game) {
    // Clear board
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            game->board[r][c].type = EMPTY;
            game->board[r][c].color = NONE;
        }
    }
    
    // Set up pieces
    PieceType back_row[] = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK};
    
    for (int c = 0; c < BOARD_SIZE; c++) {
        game->board[0][c].type = back_row[c];
        game->board[0][c].color = BLACK;
        game->board[1][c].type = PAWN;
        game->board[1][c].color = BLACK;
        
        game->board[6][c].type = PAWN;
        game->board[6][c].color = WHITE;
        game->board[7][c].type = back_row[c];
        game->board[7][c].color = WHITE;
    }
    
    game->turn = WHITE;
    game->white_king_moved = false;
    game->black_king_moved = false;
    game->white_rook_a_moved = false;
    game->white_rook_h_moved = false;
    game->black_rook_a_moved = false;
    game->black_rook_h_moved = false;
    game->en_passant_col = -1;
    game->en_passant_row = -1;
}

bool chess_is_valid_move(ChessGameState *game, int fr, int fc, int tr, int tc) {
    if (!chess_is_in_bounds(fr, fc) || !chess_is_in_bounds(tr, tc)) return false;
    if (fr == tr && fc == tc) return false;

    ChessPiece piece  = game->board[fr][fc];
    ChessPiece target = game->board[tr][tc];

    if (piece.type == EMPTY || piece.color != game->turn) return false;
    if (target.color == piece.color) return false;

    int dr = tr - fr;
    int dc = tc - fc;

    bool ok = false;

    switch (piece.type) {
        case PAWN: {
            int direction = (piece.color == WHITE) ? -1 : 1;
            int start_row = (piece.color == WHITE) ? 6 : 1;

            // Normal pawn moves
            if (dc == 0 && target.type == EMPTY) {
                if (dr == direction) {
                    ok = true;
                } else if (fr == start_row && dr == 2 * direction &&
                           game->board[fr + direction][fc].type == EMPTY) {
                    ok = true;
                }
            }
            // Regular captures
            else if (abs(dc) == 1 && dr == direction && target.type != EMPTY) {
                ok = true;
            }
            // En passant capture
            else if (abs(dc) == 1 && dr == direction && target.type == EMPTY) {
                if (game->en_passant_col >= 0 && game->en_passant_col < BOARD_SIZE &&
                    game->en_passant_row >= 0 && game->en_passant_row < BOARD_SIZE &&
                    game->en_passant_col == tc && game->en_passant_row == tr) {
                    ok = true;
                }
            }
            break;
        }

        case KNIGHT:
            ok = (abs(dr) == 2 && abs(dc) == 1) || (abs(dr) == 1 && abs(dc) == 2);
            break;

        case BISHOP:
            ok = (abs(dr) == abs(dc) && abs(dr) > 0 &&
                  chess_is_path_clear(game, fr, fc, tr, tc));
            break;

        case ROOK:
            ok = ((dr == 0 || dc == 0) && (dr != 0 || dc != 0) &&
                  chess_is_path_clear(game, fr, fc, tr, tc));
            break;

        case QUEEN:
            ok = (((dr == 0 || dc == 0) || (abs(dr) == abs(dc))) &&
                  (dr != 0 || dc != 0) &&
                  chess_is_path_clear(game, fr, fc, tr, tc));
            break;

        case KING: {
            // Normal king move
            if (abs(dr) <= 1 && abs(dc) <= 1 && (dr != 0 || dc != 0)) {
                ok = true;
            } else if (dr == 0 && abs(dc) == 2) {
                bool castle_ok = true;

                // Must not have moved king
                if (piece.color == WHITE && game->white_king_moved) castle_ok = false;
                if (piece.color == BLACK && game->black_king_moved) castle_ok = false;

                if (castle_ok && dc == 2) {
                    // Kingside castling
                    if (piece.color == WHITE && game->white_rook_h_moved) castle_ok = false;
                    if (piece.color == BLACK && game->black_rook_h_moved) castle_ok = false;

                    if (castle_ok) {
                        ChessPiece rook = game->board[fr][7];
                        if (rook.type != ROOK || rook.color != piece.color) castle_ok = false;
                    }
                    if (castle_ok && game->board[fr][5].type != EMPTY) castle_ok = false;
                    if (castle_ok && game->board[fr][6].type != EMPTY) castle_ok = false;

                    if (castle_ok) {
                        if (chess_is_in_check(game, piece.color)) castle_ok = false;
                    }
                    if (castle_ok) {
                        ChessGameState temp = *game;
                        temp.board[fr][5] = piece;
                        temp.board[fr][4].type  = EMPTY;
                        temp.board[fr][4].color = NONE;
                        if (chess_is_in_check(&temp, piece.color)) castle_ok = false;
                    }
                    if (castle_ok) {
                        ChessGameState temp = *game;
                        temp.board[fr][6] = piece;
                        temp.board[fr][4].type  = EMPTY;
                        temp.board[fr][4].color = NONE;
                        if (chess_is_in_check(&temp, piece.color)) castle_ok = false;
                    }

                    if (castle_ok) ok = true;
                } else if (castle_ok && dc == -2) {
                    // Queenside castling
                    if (piece.color == WHITE && game->white_rook_a_moved) castle_ok = false;
                    if (piece.color == BLACK && game->black_rook_a_moved) castle_ok = false;

                    if (castle_ok) {
                        ChessPiece rook = game->board[fr][0];
                        if (rook.type != ROOK || rook.color != piece.color) castle_ok = false;
                    }
                    if (castle_ok && game->board[fr][1].type != EMPTY) castle_ok = false;
                    if (castle_ok && game->board[fr][2].type != EMPTY) castle_ok = false;
                    if (castle_ok && game->board[fr][3].type != EMPTY) castle_ok = false;

                    if (castle_ok) {
                        if (chess_is_in_check(game, piece.color)) castle_ok = false;
                    }
                    if (castle_ok) {
                        ChessGameState temp = *game;
                        temp.board[fr][3] = piece;
                        temp.board[fr][4].type  = EMPTY;
                        temp.board[fr][4].color = NONE;
                        if (chess_is_in_check(&temp, piece.color)) castle_ok = false;
                    }
                    if (castle_ok) {
                        ChessGameState temp = *game;
                        temp.board[fr][2] = piece;
                        temp.board[fr][4].type  = EMPTY;
                        temp.board[fr][4].color = NONE;
                        if (chess_is_in_check(&temp, piece.color)) castle_ok = false;
                    }

                    if (castle_ok) ok = true;
                }
            }
            break;
        }

        default:
            ok = false;
            break;
    }

    if (!ok) return false;

    // FINAL LEGALITY CHECK: move must not leave own king in check
    ChessGameState temp = *game;
    ChessMove mv;
    mv.from_row = fr;
    mv.from_col = fc;
    mv.to_row   = tr;
    mv.to_col   = tc;
    mv.score    = 0;

    chess_make_move(&temp, mv);

    if (chess_is_in_check(&temp, piece.color)) {
        return false;
    }

    return true;
}



bool chess_is_in_check(ChessGameState *game, ChessColor color) {
    int king_r = -1, king_c = -1;
    
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (game->board[r][c].type == KING && game->board[r][c].color == color) {
                king_r = r;
                king_c = c;
                break;
            }
        }
        if (king_r != -1) break;
    }
    
    if (king_r == -1) return false;
    
    ChessColor opponent = (color == WHITE) ? BLACK : WHITE;
    
    // Check each opponent piece to see if it can attack the king
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            ChessPiece piece = game->board[r][c];
            if (piece.color != opponent) continue;
            
            // Temporarily set turn to opponent to check if move is valid
            ChessColor saved_turn = game->turn;
            game->turn = opponent;
            bool can_attack = chess_is_valid_move(game, r, c, king_r, king_c);
            game->turn = saved_turn;
            
            if (can_attack) {
                return true;
            }
        }
    }
    
    return false;
}

void chess_make_move(ChessGameState *game, ChessMove move) {
    ChessPiece piece = game->board[move.from_row][move.from_col];
    
    // Clear en passant state before making move
    game->en_passant_col = -1;
    game->en_passant_row = -1;
    
    // Handle en passant capture
    if (piece.type == PAWN && move.to_col != move.from_col && 
        game->board[move.to_row][move.to_col].type == EMPTY) {
        // This is an en passant capture - remove the captured pawn
        int captured_pawn_row = (piece.color == WHITE) ? move.to_row + 1 : move.to_row - 1;
        game->board[captured_pawn_row][move.to_col].type = EMPTY;
        game->board[captured_pawn_row][move.to_col].color = NONE;
    }
    
    // Move the piece
    game->board[move.to_row][move.to_col] = piece;
    game->board[move.from_row][move.from_col].type = EMPTY;
    game->board[move.from_row][move.from_col].color = NONE;
    
    // Set en passant state if pawn moved two squares
    if (piece.type == PAWN && abs(move.to_row - move.from_row) == 2) {
        game->en_passant_col = move.to_col;
        game->en_passant_row = (move.from_row + move.to_row) / 2;
    }
    
    // Handle castling - move the rook too
    if (piece.type == KING && abs(move.to_col - move.from_col) == 2) {
        if (move.to_col > move.from_col) {
            // Kingside castle - move rook from h to f
            ChessPiece rook = game->board[move.from_row][7];
            game->board[move.from_row][5] = rook;
            game->board[move.from_row][7].type = EMPTY;
            game->board[move.from_row][7].color = NONE;
        } else {
            // Queenside castle - move rook from a to d
            ChessPiece rook = game->board[move.from_row][0];
            game->board[move.from_row][3] = rook;
            game->board[move.from_row][0].type = EMPTY;
            game->board[move.from_row][0].color = NONE;
        }
    }
    
    // Pawn promotion
    if (piece.type == PAWN) {
        if ((piece.color == WHITE && move.to_row == 0) || 
            (piece.color == BLACK && move.to_row == 7)) {
            // Promote to queen (90% of the time) or knight (10% for variety)
            //game->board[move.to_row][move.to_col].type = (rand() % 10 == 0) ? KNIGHT : QUEEN;
            game->board[move.to_row][move.to_col].type = QUEEN;
        }
    }
    
    if (piece.type == KING) {
        if (piece.color == WHITE) game->white_king_moved = true;
        else game->black_king_moved = true;
    }
    if (piece.type == ROOK) {
        if (piece.color == WHITE) {
            if (move.from_col == 0) game->white_rook_a_moved = true;
            if (move.from_col == 7) game->white_rook_h_moved = true;
        } else {
            if (move.from_col == 0) game->black_rook_a_moved = true;
            if (move.from_col == 7) game->black_rook_h_moved = true;
        }
    }
    
    // Also mark rook as moved if it's being captured
    ChessPiece target = game->board[move.to_row][move.to_col];
    if (target.type == ROOK) {
        if (target.color == WHITE) {
            if (move.to_col == 0) game->white_rook_a_moved = true;
            if (move.to_col == 7) game->white_rook_h_moved = true;
        } else {
            if (move.to_col == 0) game->black_rook_a_moved = true;
            if (move.to_col == 7) game->black_rook_h_moved = true;
        }
    }
    
    game->turn = (game->turn == WHITE) ? BLACK : WHITE;
}

int chess_evaluate_position(ChessGameState *game) {
    int piece_values[] = {0, 100, 320, 330, 500, 900, 20000};
    int score = 0;
    
    // Material count (dominates everything)
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            ChessPiece p = game->board[r][c];
            if (p.type != EMPTY) {
                score += (p.color == WHITE) ? piece_values[p.type] : -piece_values[p.type];
            }
        }
    }
    
    // KING SAFETY - Discourage unnecessary king moves
    // Apply penalty to kings that have moved away from starting positions
    // Starting positions: White king at e1 (row 7, col 4), Black king at e8 (row 0, col 4)
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            ChessPiece p = game->board[r][c];
            if (p.type == KING) {
                int distance_penalty = 0;
                if (p.color == WHITE) {
                    // White king starting position: row 7, col 4
                    int dist_from_start = abs(r - 7) + abs(c - 4);  // Manhattan distance
                    distance_penalty = dist_from_start * 30;  // 30 points per square away from start
                    score -= distance_penalty;
                } else {
                    // Black king starting position: row 0, col 4
                    int dist_from_start = abs(r - 0) + abs(c - 4);  // Manhattan distance
                    distance_penalty = dist_from_start * 30;  // 30 points per square away from start
                    score += distance_penalty;
                }
            }
        }
    }
    
    // PAWN PROMOTION BONUS - reward queens on promotion rank
    // Queens on back rank (row 0 for white, row 7 for black) are newly promoted
    // This gives a huge score boost to make promotion moves highest priority
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            ChessPiece p = game->board[r][c];
            if (p.type == QUEEN) {
                // Check if queen just promoted (on back rank)
                if ((p.color == WHITE && r == 0) || (p.color == BLACK && r == 7)) {
                    // Bonus = net gain from pawn promotion (900 - 100 = 800)
                    int promotion_bonus = 800;
                    score += (p.color == WHITE) ? promotion_bonus : -promotion_bonus;
                }
            }
        }
    }
    
    // DEFENSE CHECK - pieces with friendly neighbors
    // 15 centipawns per defender - meaningful but not overwhelming
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            ChessPiece p = game->board[r][c];
            if (p.type == EMPTY || p.type == KING || p.type == PAWN) continue;
            
            // Count friendly pieces adjacent to this piece
            int defenders = 0;
            for (int dr = -1; dr <= 1; dr++) {
                for (int dc = -1; dc <= 1; dc++) {
                    if (dr == 0 && dc == 0) continue;
                    int nr = r + dr;
                    int nc = c + dc;
                    if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) continue;
                    
                    ChessPiece neighbor = game->board[nr][nc];
                    if (neighbor.type != EMPTY && neighbor.color == p.color) {
                        defenders++;
                    }
                }
            }
            
            // 15 centipawns per defender
            if (defenders > 0) {
                int defense_bonus = defenders * 15;
                score += (p.color == WHITE) ? defense_bonus : -defense_bonus;
            }
        }
    }
    
    // Pawn advancement bonus
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            ChessPiece p = game->board[r][c];
            if (p.type == PAWN) {
                // White pawns get bonus for advancing (lower row number)
                if (p.color == WHITE) {
                    int advance_bonus = (6 - r) * 5;
                    score += advance_bonus;
                } else {
                    // Black pawns get bonus for advancing (higher row number)
                    int advance_bonus = (r - 1) * 5;
                    score -= advance_bonus;
                }
            }
        }
    }
    
    // ATTACKING BONUS - encourage pieces to attack
    // Simple check: bonus for pieces that have enemy pieces in their "line of attack"
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            ChessPiece p = game->board[r][c];
            if (p.type == EMPTY || p.type == KING || p.type == PAWN) continue;
            
            // Quick attack check - just count if enemy pieces are reachable
            int attacking = 0;
            int highest_target_value = 0;
            bool is_threatened = false;
            
            for (int tr = 0; tr < 8; tr++) {
                for (int tc = 0; tc < 8; tc++) {
                    ChessPiece target = game->board[tr][tc];
                    if (target.type == EMPTY || target.color == p.color) continue;
                    
                    bool can_attack = false;
                    
                    // Knight: can attack anything 2-1 away
                    if (p.type == KNIGHT) {
                        int dr = abs(tr - r);
                        int dc = abs(tc - c);
                        if ((dr == 2 && dc == 1) || (dr == 1 && dc == 2)) {
                            can_attack = true;
                        }
                    }
                    // Rook: can attack along rank or file
                    else if (p.type == ROOK) {
                        if (r == tr || c == tc) {
                            can_attack = true;
                        }
                    }
                    // Bishop: can attack along diagonal
                    else if (p.type == BISHOP) {
                        if (abs(tr - r) == abs(tc - c)) {
                            can_attack = true;
                        }
                    }
                    // Queen: rook + bishop
                    else if (p.type == QUEEN) {
                        if (r == tr || c == tc || abs(tr - r) == abs(tc - c)) {
                            can_attack = true;
                        }
                    }
                    
                    if (can_attack) {
                        attacking++;
                        if (piece_values[target.type] > highest_target_value) {
                            highest_target_value = piece_values[target.type];
                        }
                        
                        // Check if target can attack back
                        bool target_attacks_back = false;
                        if (target.type == KNIGHT) {
                            int dr = abs(r - tr);
                            int dc = abs(c - tc);
                            if ((dr == 2 && dc == 1) || (dr == 1 && dc == 2)) {
                                target_attacks_back = true;
                            }
                        } else if (target.type == ROOK) {
                            if (tr == r || tc == c) {
                                target_attacks_back = true;
                            }
                        } else if (target.type == BISHOP) {
                            if (abs(r - tr) == abs(c - tc)) {
                                target_attacks_back = true;
                            }
                        } else if (target.type == QUEEN) {
                            if (tr == r || tc == c || abs(r - tr) == abs(c - tc)) {
                                target_attacks_back = true;
                            }
                        } else if (target.type == PAWN) {
                            // Pawn attacks diagonally
                            int pawn_dir = (target.color == WHITE) ? -1 : 1;
                            if (tr + pawn_dir == r && abs(tc - c) == 1) {
                                target_attacks_back = true;
                            }
                        }
                        
                        if (target_attacks_back) {
                            is_threatened = true;
                        }
                    }
                }
            }
            
            // Bonus for attacking (5 points per attacked piece, 15 extra for attacking high-value piece)
            if (attacking > 0) {
                int attack_bonus = attacking * 5;
                if (highest_target_value >= 500) {  // Attacking rook or queen
                    attack_bonus += 15;
                }
                
                // PENALTY if this piece is threatened by what it attacks
                // Don't attack if you'll get taken (unless favorable trade)
                if (is_threatened) {
                    int threat_penalty = piece_values[p.type] / 3;  // Lose 1/3 of piece value if attacked back
                    attack_bonus -= threat_penalty;
                }
                
                score += (p.color == WHITE) ? attack_bonus : -attack_bonus;
            }
        }
    }
    
    // ROOK ACTIVITY - rooks should stay put until endgame
    // Count material to determine game phase
    int material_count = 0;
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            ChessPiece p = game->board[r][c];
            if (p.type != EMPTY && p.type != KING && p.type != PAWN) {
                material_count += 1;
            }
        }
    }
    
    // If still in opening/mid-game (lots of pieces), penalize rooks not on back rank
    if (material_count > 8) {  // Endgame when few pieces left
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 8; c++) {
                ChessPiece p = game->board[r][c];
                if (p.type == ROOK) {
                    // Rook should be on back rank (row 7 for white, row 0 for black)
                    bool on_back_rank = (p.color == WHITE && r == 7) || (p.color == BLACK && r == 0);
                    
                    if (!on_back_rank) {
                        int rook_penalty = 20;  // 20 centipawn penalty for moving rook early
                        score += (p.color == WHITE) ? -rook_penalty : rook_penalty;
                    }
                }
            }
        }
    }
    
    // KING SAFETY - penalize moving king when not in check
    // Kings should stay safe, not wander around
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            ChessPiece p = game->board[r][c];
            if (p.type == KING) {
                // Check if king is in check
                bool in_check = chess_is_in_check(game, p.color);
                
                if (!in_check) {
                    // King not in check - check if it's on a safe square
                    // Safe squares: castled position (g1, c1, g8, c8) or center-ish position
                    bool safe_square = false;
                    
                    // White king safe positions
                    if (p.color == WHITE) {
                        if ((r == 7 && (c == 6 || c == 2)) ||  // Castled positions
                            (r == 6 && (c >= 1 && c <= 6))) {   // Back rank area
                            safe_square = true;
                        }
                    } else {
                        // Black king safe positions
                        if ((r == 0 && (c == 6 || c == 2)) ||  // Castled positions
                            (r == 1 && (c >= 1 && c <= 6))) {   // Back rank area
                            safe_square = true;
                        }
                    }
                    
                    // Penalty for king in exposed position
                    if (!safe_square) {
                        int exposure_penalty = 30;  // 30 centipawn penalty
                        score += (p.color == WHITE) ? -exposure_penalty : exposure_penalty;
                    }
                }
            }
        }
    }
    
    return score;
}

int chess_get_all_moves(ChessGameState *game, ChessColor color, ChessMove *moves) {
    int count = 0;
    
    for (int fr = 0; fr < BOARD_SIZE; fr++) {
        for (int fc = 0; fc < BOARD_SIZE; fc++) {
            if (game->board[fr][fc].color == color) {
                for (int tr = 0; tr < BOARD_SIZE; tr++) {
                    for (int tc = 0; tc < BOARD_SIZE; tc++) {
                        if (chess_is_valid_move(game, fr, fc, tr, tc)) {
                            ChessGameState temp = *game;
                            ChessMove m = {fr, fc, tr, tc, 0};
                            chess_make_move(&temp, m);
                            
                            if (!chess_is_in_check(&temp, color)) {
                                moves[count++] = m;
                            }
                        }
                    }
                }
            }
        }
    }
    
    return count;
}

int chess_quick_eval_move(ChessGameState *game, ChessMove move, bool maximizing) {
    int score = 0;
    
    // Most valuable victim / Least valuable attacker heuristic (MVV-LVA)
    int piece_values[] = {0, 100, 320, 330, 500, 900, 20000};
    
    ChessPiece attacking_piece = game->board[move.from_row][move.from_col];
    ChessPiece target_piece = game->board[move.to_row][move.to_col];
    
    // Captures: prioritize valuable targets attacked with cheap pieces
    if (target_piece.type != EMPTY) {
        score = (piece_values[target_piece.type] * 10) - piece_values[attacking_piece.type];
    }
    
    // Check moves: high priority
    ChessGameState temp = *game;
    chess_make_move(&temp, move);
    if (chess_is_in_check(&temp, game->turn == WHITE ? BLACK : WHITE)) {
        score += 500;
    }
    
    // Pawn promotion: extremely high priority
    if (attacking_piece.type == PAWN) {
        if ((game->turn == WHITE && move.to_row == 0) ||
            (game->turn == BLACK && move.to_row == 7)) {
            score += 800;
        }
    }
    
    // Castling: decent priority (safety improvement)
    if (attacking_piece.type == KING && abs(move.to_col - move.from_col) == 2) {
        score += 300;
    }
    
    return score;
}

// Sort moves using quick evaluation for better search ordering
void chess_sort_moves_by_heuristic(ChessGameState *game, ChessMove *moves, int move_count,
                                   KillerMoveTable *killers, HistoryHeuristic *history, int depth) {
    ScoredMove scored[256];
    
    for (int i = 0; i < move_count; i++) {
        scored[i].move = moves[i];
        scored[i].score = 0;
        
        // Killer move bonus
        if (killers) {
            for (int k = 0; k < killers->killer_count[depth]; k++) {
                if (moves[i].from_row == killers->killer_moves[depth][k].from_row &&
                    moves[i].from_col == killers->killer_moves[depth][k].from_col &&
                    moves[i].to_row == killers->killer_moves[depth][k].to_row &&
                    moves[i].to_col == killers->killer_moves[depth][k].to_col) {
                    scored[i].score += 400 - (k * 100);  // First killer better than second
                    break;
                }
            }
        }
        
        // History heuristic bonus
        if (history) {
            scored[i].score += history->history[moves[i].from_row][moves[i].from_col]
                                               [moves[i].to_row][moves[i].to_col] / 10;
        }
        
        // Quick evaluation of move
        scored[i].score += chess_quick_eval_move(game, moves[i], true);
    }
    
    // Simple insertion sort (good for small arrays)
    for (int i = 1; i < move_count; i++) {
        ScoredMove key = scored[i];
        int j = i - 1;
        
        while (j >= 0 && scored[j].score < key.score) {
            scored[j + 1] = scored[j];
            j--;
        }
        scored[j + 1] = key;
    }
    
    // Copy back sorted moves
    for (int i = 0; i < move_count; i++) {
        moves[i] = scored[i].move;
    }
}

// Path elimination: quickly filter out obviously bad moves
int chess_eliminate_poor_moves(ChessGameState *game, ChessMove *moves, int move_count,
                               int piece_values[]) {
    int good_move_count = 0;
    ChessMove good_moves[256];
    
    for (int i = 0; i < move_count; i++) {
        ChessPiece moving_piece = game->board[moves[i].from_row][moves[i].from_col];
        ChessPiece target = game->board[moves[i].to_row][moves[i].to_col];
        
        // ELIMINATION RULE 1: Avoid captures that lose material (SEE check)
        // Simplified Static Exchange Evaluation
        if (target.type != EMPTY) {
            int target_value = piece_values[target.type];
            int moving_value = piece_values[moving_piece.type];
            
            // Only eliminate if it's clearly bad (we lose much more than we gain)
            // Allow modest unfavorable exchanges as they may have tactical benefits
            if (moving_value > target_value * 3) {
                continue;  // Skip obviously bad capture
            }
        }
        
        // ELIMINATION RULE 2: Avoid moving into capture unless there's compensation
        ChessGameState temp = *game;
        chess_make_move(&temp, moves[i]);
        ChessPiece moved_piece = temp.board[moves[i].to_row][moves[i].to_col];
        
        // Check if piece is hanging (undefended and can be captured)
        bool can_be_captured = false;
        
        // Simple check: is there an enemy piece that can capture this?
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 8; c++) {
                ChessPiece p = temp.board[r][c];
                if (p.type != EMPTY && p.color != moved_piece.color) {
                    // Check if this piece can capture our moved piece
                    if (chess_is_valid_move(&temp, r, c, moves[i].to_row, moves[i].to_col)) {
                        can_be_captured = true;
                        // Loss would be this piece's value
                        if (piece_values[moved_piece.type] < piece_values[p.type] * 2) {
                            break;
                        }
                    }
                }
            }
            if (can_be_captured) break;
        }
        
        // ELIMINATION RULE 3: Filter check moves separately (always valuable)
        bool gives_check = chess_is_in_check(&temp, game->turn == WHITE ? BLACK : WHITE);
        if (gives_check) {
            good_moves[good_move_count++] = moves[i];
            continue;
        }
        
        // ELIMINATION RULE 4: Never allow moving into checkmate (king moves only)
        if (moving_piece.type == KING && chess_is_in_check(&temp, game->turn)) {
            continue;  // This move leaves us in check
        }
        
        // Keep the move if it passes checks
        good_moves[good_move_count++] = moves[i];
    }
    
    // Copy good moves back
    for (int i = 0; i < good_move_count; i++) {
        moves[i] = good_moves[i];
    }
    
    return good_move_count;
}

static HistoryHeuristic global_history = {0};

// Clear global tables when starting new search
void chess_clear_search_tables() {
    memset(&global_killers, 0, sizeof(global_killers));
    memset(&global_history, 0, sizeof(global_history));
}

int chess_aggressive_filter_moves(ChessGameState *game, ChessMove *moves, 
                                        int count, int depth, int initial_depth) {
    int good_count = 0;
    ChessMove good_moves[256];
    
    // Separate moves into categories
    int captures = 0, checks = 0, quiet = 0;
    ChessMove capture_moves[256];
    ChessMove check_moves[256];
    ChessMove quiet_moves[256];
    
    for (int i = 0; i < count; i++) {
        // Skip moves that should be eliminated
        if (chess_should_eliminate_move(game, moves[i], depth, initial_depth)) {
            continue;
        }
        
        // Categorize keeper moves
        ChessPiece target = game->board[moves[i].to_row][moves[i].to_col];
        
        if (target.type != EMPTY) {
            // Capture move
            capture_moves[captures++] = moves[i];
        } else {
            // Check if it's a check
            ChessGameState temp = *game;
            chess_make_move(&temp, moves[i]);
            if (chess_is_in_check(&temp, game->turn == WHITE ? BLACK : WHITE)) {
                check_moves[checks++] = moves[i];
            } else {
                quiet_moves[quiet++] = moves[i];
            }
        }
    }
    
    // Copy moves in priority order: captures, checks, quiet
    // This helps alpha-beta pruning (best moves first)
    for (int i = 0; i < captures; i++) {
        good_moves[good_count++] = capture_moves[i];
    }
    for (int i = 0; i < checks; i++) {
        good_moves[good_count++] = check_moves[i];
    }
    for (int i = 0; i < quiet; i++) {
        good_moves[good_count++] = quiet_moves[i];
    }
    
    // Copy back
    for (int i = 0; i < good_count; i++) {
        moves[i] = good_moves[i];
    }
    
    return good_count;
}

// ============================================================================
// FAST MOVE ORDERING (for moves that passed filtering)
// ============================================================================

static int chess_score_move_for_order(ChessGameState *game, ChessMove move) {
    static int piece_values[] = {0, 100, 320, 330, 500, 900, 20000};
    
    int score = 0;
    ChessPiece target = game->board[move.to_row][move.to_col];
    ChessPiece attacker = game->board[move.from_row][move.from_col];
    
    // MVV-LVA: capture value minus attacker value
    if (target.type != EMPTY) {
        score = (piece_values[target.type] * 10) - piece_values[attacker.type];
    }
    
    // Quick check detection (expensive but important)
    ChessGameState temp = *game;
    chess_make_move(&temp, move);
    if (chess_is_in_check(&temp, game->turn == WHITE ? BLACK : WHITE)) {
        score += 300;
    }
    
    return score;
}

// Sort moves by score for better pruning

// ============================================================================
// ENHANCED MINIMAX WITH AGGRESSIVE PATH ELIMINATION
// ============================================================================

/* ============================================================================
 * MINIMAX WRAPPER - BACKWARD COMPATIBLE
 * ============================================================================
 */

int chess_minimax(ChessGameState *game, int depth, int alpha, int beta, bool maximizing) {
    return chess_minimax_enhanced(game, depth, depth, alpha, beta, maximizing, &global_killers);
}

// ============================================================================
// THINKING STATE MANAGEMENT
// ============================================================================

void* chess_think_continuously(void* arg) {
    ChessThinkingState *ts = (ChessThinkingState*)arg;
    
    while (true) {
#if BEATCHESS_HAS_PTHREAD
        pthread_mutex_lock(&ts->lock);
#endif
        if (!ts->thinking) {
#if BEATCHESS_HAS_PTHREAD
            pthread_mutex_unlock(&ts->lock);
#endif
            usleep(10000);
            continue;
        }
        
        ChessGameState game_copy = ts->game;
#if BEATCHESS_HAS_PTHREAD
        pthread_mutex_unlock(&ts->lock);
#endif
        
        ChessMove moves[256];
        int move_count = chess_get_all_moves(&game_copy, game_copy.turn, moves);
        
        if (move_count == 0) {
#if BEATCHESS_HAS_PTHREAD
            pthread_mutex_lock(&ts->lock);
#endif
            ts->has_move = false;
            ts->thinking = false;
#if BEATCHESS_HAS_PTHREAD
            pthread_mutex_unlock(&ts->lock);
#endif
            //printf("THINK: No legal moves found!\n");
            continue;
        }
        
        //printf("THINK: Starting search for %s, %d legal moves\n",  game_copy.turn == WHITE ? "WHITE" : "BLACK", move_count);
        
        // Iterative deepening - uses MAX_CHESS_DEPTH for configurable search depth
        for (int depth = 1; depth <= MAX_CHESS_DEPTH; depth++) {
            ChessMove best_moves[256];
            int best_move_count = 0;
            int best_score = (game_copy.turn == WHITE) ? INT_MIN : INT_MAX;
              
            bool depth_completed = true;
            for (int i = 0; i < move_count; i++) {
#if BEATCHESS_HAS_PTHREAD
                pthread_mutex_lock(&ts->lock);
                bool should_stop = !ts->thinking;
                pthread_mutex_unlock(&ts->lock);
#else
                bool should_stop = !ts->thinking;
#endif
                
                if (should_stop) {
                    depth_completed = false;
                    break;
                }
                
                ChessGameState temp = game_copy;
                chess_make_move(&temp, moves[i]);
                int score = chess_minimax(&temp, depth - 1, INT_MIN, INT_MAX, 
                                         game_copy.turn == BLACK);
                
                if (game_copy.turn == WHITE) {
                    if (score > best_score) {
                        best_score = score;
                        best_moves[0] = moves[i];
                        best_move_count = 1;
                    } else if (score == best_score) {
                        best_moves[best_move_count++] = moves[i];
                    }
                } else {
                    if (score < best_score) {
                        best_score = score;
                        best_moves[0] = moves[i];
                        best_move_count = 1;
                    } else if (score == best_score) {
                        best_moves[best_move_count++] = moves[i];
                    }
                }
            }
            
            // Update if we completed this depth
#if BEATCHESS_HAS_PTHREAD
            pthread_mutex_lock(&ts->lock);
#endif
            if (depth_completed && ts->thinking && best_move_count > 0) {
                // Only update if we don't have a move yet, or if this is a significantly deeper search
                // This prevents shallower moves from being overwritten by equally-good deeper moves
                bool should_update = !ts->has_move || (depth > ts->current_depth);
                
                if (should_update) {
                    // ========== RANDOMNESS: Consider near-best moves ==========
                    // Collect all moves within a threshold of the best score
                    ScoredMove candidate_moves[256];
                    int candidate_count = 0;
                    int threshold = 250;  // Consider moves within 25 centipawns of best
                    
                    // Use the already-calculated scores from the main loop (lines 1079-1115)
                    // Don't recalculate - just look at which moves are near-best
                    for (int i = 0; i < move_count; i++) {
                        ChessGameState temp = game_copy;
                        chess_make_move(&temp, moves[i]);
                        int score = chess_minimax(&temp, depth - 1, INT_MIN, INT_MAX, 
                                                 game_copy.turn == BLACK);
                        bool is_close = false;
                        if (game_copy.turn == WHITE) {
                            is_close = (score >= best_score - threshold);
                        } else {
                            is_close = (score <= best_score + threshold);
                        }
                        
                        if (is_close) {
                            candidate_moves[candidate_count].move = moves[i];
                            candidate_moves[candidate_count].score = score;
                            candidate_count++;
                        }
                    }

                    // Randomly select from candidate moves
                    // This gives higher-scoring moves more chances to be picked
                    // but lower-scoring moves still have a chance
                    if (candidate_count > 0) {
                        int choice = rand() % candidate_count;
                        ts->best_move = candidate_moves[choice].move;
                    } else {
                        ts->best_move = best_moves[rand() % best_move_count];
                    }
                    
                    ts->best_score = best_score;
                    ts->current_depth = depth;
                    ts->has_move = true;
                }
            }
#if BEATCHESS_HAS_PTHREAD
            pthread_mutex_unlock(&ts->lock);
#endif
            

        }
        
#if BEATCHESS_HAS_PTHREAD
        pthread_mutex_lock(&ts->lock);
#endif
        ts->thinking = false;
#if BEATCHESS_HAS_PTHREAD
        pthread_mutex_unlock(&ts->lock);
#endif
    }
    
    return NULL;
}

void chess_init_thinking_state(ChessThinkingState *ts) {
    ts->thinking = false;
    ts->has_move = false;
    ts->current_depth = 0;
    ts->best_score = 0;
    chess_init_zobrist();
    chess_clear_transposition_table();
    chess_clear_killers(&global_killers);
#if BEATCHESS_HAS_PTHREAD
    pthread_mutex_init(&ts->lock, NULL);
    pthread_create(&ts->thread, NULL, chess_think_continuously, ts);
#endif
}

void chess_start_thinking(ChessThinkingState *ts, ChessGameState *game) {
#if BEATCHESS_HAS_PTHREAD
    pthread_mutex_lock(&ts->lock);
#endif
    ts->game = *game;
    ts->thinking = true;
    ts->has_move = false;
    ts->current_depth = 0;
#if BEATCHESS_HAS_PTHREAD
    pthread_mutex_unlock(&ts->lock);
#endif
}

ChessMove chess_get_best_move_now(ChessThinkingState *ts) {
#if BEATCHESS_HAS_PTHREAD
    pthread_mutex_lock(&ts->lock);
#endif
    ChessMove move = ts->best_move;
    bool has_move = ts->has_move;
    int depth = ts->current_depth;
    ts->thinking = false; // Stop thinking
#if BEATCHESS_HAS_PTHREAD
    pthread_mutex_unlock(&ts->lock);
#endif
    
    if (has_move) {
    }
    
    if (!has_move) {
        // No move found yet - pick random legal move as fallback
        ChessMove moves[256];
        int count = chess_get_all_moves(&ts->game, ts->game.turn, moves);
        if (count > 0) {
            move = moves[rand() % count];
        }
    }
    
    // Return the best move found so far (stored in ts->best_move)
    return move;
}

void chess_stop_thinking(ChessThinkingState *ts) {
#if BEATCHESS_HAS_PTHREAD
    pthread_mutex_lock(&ts->lock);
#endif
    ts->thinking = false;
#if BEATCHESS_HAS_PTHREAD
    pthread_mutex_unlock(&ts->lock);
#endif
}

// ============================================================================
// GAME STATUS
// ============================================================================

ChessGameStatus chess_check_game_status(ChessGameState *game) {
    ChessMove moves[256];
    int move_count = chess_get_all_moves(game, game->turn, moves);
    
    if (move_count == 0) {
        if (chess_is_in_check(game, game->turn)) {
            return (game->turn == WHITE) ? CHESS_CHECKMATE_BLACK : CHESS_CHECKMATE_WHITE;
        }
        return CHESS_STALEMATE;
    }
    
    return CHESS_PLAYING;
}

// ============================================================================
// UNDO FUNCTIONALITY
// ============================================================================

void chess_save_move_history(BeatChessVisualization *chess, ChessMove move, double time_spent) {
    // Don't save if buffer is full - prevent overflow
    if (chess->move_history_count >= MAX_MOVE_HISTORY) {
        return;
    }
    
    // Store move at current position (straight buffer, no wrapping)
    chess->move_history[chess->move_history_count].game_state = chess->game;
    chess->move_history[chess->move_history_count].move = move;
    chess->move_history[chess->move_history_count].time_elapsed = time_spent;
    
    // Increment move count (linear, no wrapping)
    chess->move_history_count++;
}

bool chess_can_undo(BeatChessVisualization *chess) {
    // Can undo if:
    // 1. In Player vs AI mode
    // 2. It's the player's turn (WHITE in normal mode, BLACK in flipped mode)
    // 3. There's at least one move in history (a move was just made by AI)
    
    ChessColor player_color = chess->board_flipped ? BLACK : WHITE;
    
    return chess->player_vs_ai && 
           chess->game.turn == player_color && 
           chess->move_history_count > 0;
}

void chess_undo_last_move(BeatChessVisualization *chess) {
    if (!chess_can_undo(chess)) return;
    
    // Determine actual player and AI colors based on board state
    ChessColor player_color = chess->board_flipped ? BLACK : WHITE;
    ChessColor ai_color = chess->board_flipped ? WHITE : BLACK;
    
    // When player clicks undo, it's currently the player's turn
    // This means: Player move -> AI move -> [NOW]
    // We want to undo the AI's move AND the player's move before it
    // So we go back 2 moves in history
    
    if (chess->move_history_count >= 2) {
        // Get the last two moves using straight buffer indexing
        int player_move_idx = chess->move_history_count - 2;
        int ai_move_idx = chess->move_history_count - 1;
        
        if (player_move_idx >= 0 && player_move_idx < MAX_MOVE_HISTORY &&
            ai_move_idx >= 0 && ai_move_idx < MAX_MOVE_HISTORY) {
            
            MoveHistory *player_move = &chess->move_history[player_move_idx];
            MoveHistory *ai_move = &chess->move_history[ai_move_idx];
            
            // Restore to the state BEFORE the player's move
            // We need to go back 3 moves worth: to before the previous AI move
            if (chess->move_history_count >= 3) {
                int restore_idx = chess->move_history_count - 3;
                if (restore_idx >= 0 && restore_idx < MAX_MOVE_HISTORY) {
                    chess->game = chess->move_history[restore_idx].game_state;
                } else {
                    chess_init_board(&chess->game);
                }
            } else {
                // Only 2 moves in history - this was the first exchange
                // Go back to starting position
                chess_init_board(&chess->game);
            }
            
            // Subtract times from totals (using correct colors)
            if (player_color == WHITE) {
                chess->white_total_time -= player_move->time_elapsed;
            } else {
                chess->black_total_time -= player_move->time_elapsed;
            }
            
            if (ai_color == WHITE) {
                chess->white_total_time -= ai_move->time_elapsed;
            } else {
                chess->black_total_time -= ai_move->time_elapsed;
            }
            
            if (chess->white_total_time < 0) chess->white_total_time = 0;
            if (chess->black_total_time < 0) chess->black_total_time = 0;
            
            // Remove the 2 moves from history count
            chess->move_history_count -= 2;
            
            snprintf(chess->status_text, sizeof(chess->status_text),
                    "Moves undone - your turn to play again");
            chess->status_flash_color[0] = 0.2;
            chess->status_flash_color[1] = 0.8;
            chess->status_flash_color[2] = 1.0;
        }
    } else if (chess->move_history_count == 1) {
        // Only player's opening move - undo it
        chess_init_board(&chess->game);
        
        // Subtract time for player's move (using correct color)
        MoveHistory *first_move = &chess->move_history[0];
        if (player_color == WHITE) {
            chess->white_total_time -= first_move->time_elapsed;
        } else {
            chess->black_total_time -= first_move->time_elapsed;
        }
        
        if (chess->white_total_time < 0) chess->white_total_time = 0;
        if (chess->black_total_time < 0) chess->black_total_time = 0;
        
        chess->move_history_count = 0;
        
        snprintf(chess->status_text, sizeof(chess->status_text),
                "Opening move undone - try again");
        chess->status_flash_color[0] = 0.2;
        chess->status_flash_color[1] = 0.8;
        chess->status_flash_color[2] = 1.0;
    }
    
    chess->move_count = chess->move_history_count;
    chess->last_move_glow = 0;
    chess->animation_progress = 0;
    chess->is_animating = false;
    chess->undo_button_glow = 1.0;
    chess->status_flash_timer = 1.5;
}

// ============================================================================
// VISUALIZATION SYSTEM
// ============================================================================

#ifndef MSDOS
void init_beat_chess_system(Visualizer *vis) {
    BeatChessVisualization *chess = &vis->beat_chess;
    
    // Seed random number generator once at system initialization
    // This ensures variety in AI moves (pawn promotion, move selection, etc.)
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = 1;
    }
    
    //init_sprite_cache();
    //set_rendering_mode(true); 
    
    // Initialize game
    chess_init_board(&chess->game);
    chess_init_thinking_state(&chess->thinking_state);
    chess->status = CHESS_PLAYING;
    
    // Start first player thinking
    chess_start_thinking(&chess->thinking_state, &chess->game);
    
    // Initialize animation positions
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            chess->piece_x[r][c] = 0;
            chess->piece_y[r][c] = 0;
            chess->target_x[r][c] = 0;
            chess->target_y[r][c] = 0;
        }
    }
    
    // Initialize state
    chess->last_from_row = -1;
    chess->last_from_col = -1;
    chess->last_to_row = -1;
    chess->last_to_col = -1;
    chess->last_move_glow = 0;
    
    chess->is_animating = false;
    chess->animation_progress = 0;
    
    snprintf(chess->status_text, sizeof(chess->status_text),
            "Game started - White to move");
    chess->status_flash_timer = 0;
    chess->status_flash_color[0] = 1.0;
    chess->status_flash_color[1] = 1.0;
    chess->status_flash_color[2] = 1.0;
    chess->last_eval_change = 0;
    
    // Beat detection
    for (int i = 0; i < BEAT_HISTORY_SIZE; i++) {
        chess->beat_volume_history[i] = 0;
    }
    chess->beat_history_index = 0;
    chess->time_since_last_move = 0;
    chess->beat_threshold = 1.3;
    
    chess->move_count = 0;
    chess->eval_bar_position = 0;
    chess->eval_bar_target = 0;
    
    // Game over handling
    chess->beats_since_game_over = 0;
    chess->waiting_for_restart = false;

    chess->time_thinking = 0;
    chess->min_think_time = 0.5;        // Wait at least 0.5s before auto-playing
    chess->good_move_threshold = 150;   // Auto-play if advantage > 150 centipawns
    chess->auto_play_enabled = true;    // Enable auto-play
    
    // Check/Checkmate/Stalemate display
    chess->is_in_check = false;
    chess->check_display_timer = 0;     // 0 means not displaying
    chess->is_checkmate = false;
    chess->is_stalemate = false;
    
    // Reset button
    chess->reset_button_hovered = false;
    chess->reset_button_glow = 0;
    chess->reset_button_was_pressed = false;
    
    // PvsA toggle button
    chess->pvsa_button_hovered = false;
    chess->pvsa_button_glow = 0;
    chess->pvsa_button_was_pressed = false;
    chess->player_vs_ai = false;  // Start with AI vs AI
    
    // Undo button
    chess->undo_button_hovered = false;
    chess->undo_button_glow = 0;
    chess->undo_button_was_pressed = false;
    chess->undo_button_x = 20;
    chess->undo_button_y = 170;
    chess->undo_button_width = 120;
    chess->undo_button_height = 40;
    
    // Flip board button
    chess->flip_button_hovered = false;
    chess->flip_button_glow = 0;
    chess->flip_button_was_pressed = false;
    chess->board_flipped = false;  // Normal orientation by default
    
    // Render mode button
    chess->render_mode_button_hovered = false;
    chess->render_mode_button_glow = 0;
    chess->render_mode_button_was_pressed = false;
    
    // Move history
    chess->move_history_count = 0;
    
    // Time tracking
    chess->white_total_time = 0.0;
    chess->black_total_time = 0.0;
    chess->current_move_start_time = 0.0;
    chess->last_move_end_time = 0.0;
    
    // Player move tracking
    chess->selected_piece_row = -1;
    chess->selected_piece_col = -1;
    chess->has_selected_piece = false;
    chess->selected_piece_was_pressed = false;
    
}
#endif

bool beat_chess_detect_beat(void *vis_ptr) {
    Visualizer *vis = (Visualizer*)vis_ptr;
    BeatChessVisualization *chess = &vis->beat_chess;
    
    // Update history
    chess->beat_volume_history[chess->beat_history_index] = vis->volume_level;
    chess->beat_history_index = (chess->beat_history_index + 1) % BEAT_HISTORY_SIZE;
    
    // Calculate average
    double avg = 0;
    for (int i = 0; i < BEAT_HISTORY_SIZE; i++) {
        avg += chess->beat_volume_history[i];
    }
    avg /= BEAT_HISTORY_SIZE;
    
    // Detect beat with minimum time between moves
    if (vis->volume_level > avg * chess->beat_threshold && 
        vis->volume_level > 0.05 &&
        chess->time_since_last_move > 0.2) { // Minimum 200ms between moves
        return true;
    }
    
    return false;
}

#ifndef MSDOS
void update_beat_chess(void *vis_ptr, double dt) {
    Visualizer *vis = (Visualizer*)vis_ptr;
    BeatChessVisualization *chess = &vis->beat_chess;
    
    chess->time_since_last_move += dt;
    chess->time_thinking += dt;
    
    // Track current move time (in Player vs AI mode when it's player's or AI's turn)
    if (chess->player_vs_ai && chess->status == CHESS_PLAYING) {
        chess->current_move_start_time += dt;
    }
    
    // Calculate board layout early so player moves can use it
    double available_width = vis->width * 0.8;
    double available_height = vis->height * 0.8;
    chess->cell_size = fmin(available_width / 8, available_height / 8);
    chess->board_offset_x = (vis->width - chess->cell_size * 8) / 2;
    chess->board_offset_y = (vis->height - chess->cell_size * 8) / 2;
    
    // ===== CHECK/CHECKMATE/STALEMATE DISPLAY LOGIC =====
    // Update check display timer
    if (chess->check_display_timer > 0) {
        chess->check_display_timer -= dt;
        if (chess->check_display_timer < 0) {
            chess->check_display_timer = 0;
        }
    }
    
    // Check if current player is in check (only during gameplay)
    if (chess->status == CHESS_PLAYING) {
        bool in_check = chess_is_in_check(&chess->game, chess->game.turn);
        if (in_check && !chess->is_in_check) {
            // Transition from not-in-check to in-check
            chess->is_in_check = true;
            chess->check_display_timer = 1.0;  // Display "CHECK" for 1 second
        } else if (!in_check) {
            chess->is_in_check = false;
            chess->check_display_timer = 0;  // Hide the display
        }
    }
    // =========================================
    
    // ===== CHECK RESET BUTTON INTERACTION =====
    // Detect if mouse is over button (for hover effects)
    bool is_over_reset = (vis->mouse_x >= chess->reset_button_x && 
                          vis->mouse_x <= chess->reset_button_x + chess->reset_button_width &&
                          vis->mouse_y >= chess->reset_button_y && 
                          vis->mouse_y <= chess->reset_button_y + chess->reset_button_height);
    
    chess->reset_button_hovered = is_over_reset;
    
    // Detect click: button was pressed last frame AND released this frame
    bool reset_was_pressed = chess->reset_button_was_pressed;
    bool reset_is_pressed = vis->mouse_left_pressed;
    bool reset_clicked = (reset_was_pressed && !reset_is_pressed && is_over_reset);
    
    // Update for next frame
    chess->reset_button_was_pressed = reset_is_pressed;
    
    // Handle the click if it happened
    if (reset_clicked) {
        // Reset the game
        chess_init_board(&chess->game);
        chess->status = CHESS_PLAYING;
        chess->beats_since_game_over = 0;
        chess->waiting_for_restart = false;
        chess->move_count = 0;
        chess->eval_bar_position = 0;
        chess->eval_bar_target = 0;
        chess->time_thinking = 0;
        chess->last_move_glow = 0;
        chess->animation_progress = 0;
        chess->is_animating = false;
        chess->last_from_row = -1;
        
        // Reset check/checkmate/stalemate flags
        chess->is_in_check = false;
        chess->check_display_timer = 0;
        chess->is_checkmate = false;
        chess->is_stalemate = false;
        
        snprintf(chess->status_text, sizeof(chess->status_text), "Game Reset! White to move");
        chess->status_flash_color[0] = 0.2;
        chess->status_flash_color[1] = 0.8;
        chess->status_flash_color[2] = 1.0;
        chess->status_flash_timer = 1.5;
        
        chess->reset_button_glow = 1.0;
        
        // Start thinking for new game
        chess_start_thinking(&chess->thinking_state, &chess->game);
    }
    // ========================================
    
    // ===== CHECK PVSA TOGGLE BUTTON INTERACTION =====
    // Detect if mouse is over button (for hover effects)
    bool is_over_pvsa = (vis->mouse_x >= chess->pvsa_button_x && 
                         vis->mouse_x <= chess->pvsa_button_x + chess->pvsa_button_width &&
                         vis->mouse_y >= chess->pvsa_button_y && 
                         vis->mouse_y <= chess->pvsa_button_y + chess->pvsa_button_height);
    
    chess->pvsa_button_hovered = is_over_pvsa;
    
    // Detect click: button was pressed last frame AND released this frame
    bool pvsa_was_pressed = chess->pvsa_button_was_pressed;
    bool pvsa_is_pressed = vis->mouse_left_pressed;
    bool pvsa_clicked = (pvsa_was_pressed && !pvsa_is_pressed && is_over_pvsa);
    
    // Update for next frame
    chess->pvsa_button_was_pressed = pvsa_is_pressed;
    
    // Handle the click if it happened
    if (pvsa_clicked) {
        // Toggle between Player vs AI and AI vs AI
        chess->player_vs_ai = !chess->player_vs_ai;
        
        // Reset game when toggling
        chess_init_board(&chess->game);
        chess->status = CHESS_PLAYING;
        chess->beats_since_game_over = 0;
        chess->waiting_for_restart = false;
        chess->move_count = 0;
        chess->eval_bar_position = 0;
        chess->eval_bar_target = 0;
        chess->time_thinking = 0;
        chess->last_move_glow = 0;
        chess->animation_progress = 0;
        chess->is_animating = false;
        chess->last_from_row = -1;
        
        if (chess->player_vs_ai) {
            snprintf(chess->status_text, sizeof(chess->status_text), "Player vs AI - White (player) to move");
            chess->status_flash_color[0] = 0.2;
            chess->status_flash_color[1] = 0.8;
            chess->status_flash_color[2] = 1.0;
        } else {
            snprintf(chess->status_text, sizeof(chess->status_text), "AI vs AI - Game started!");
            chess->status_flash_color[0] = 1.0;
            chess->status_flash_color[1] = 0.65;
            chess->status_flash_color[2] = 0.0;
        }
        chess->status_flash_timer = 2.0;
        chess->pvsa_button_glow = 1.0;
        
        // Start thinking for new game
        chess_start_thinking(&chess->thinking_state, &chess->game);
    }
    // ===============================================
    
    // ===== CHECK UNDO BUTTON INTERACTION =====
    // Only check undo in Player vs AI mode
    if (chess->player_vs_ai) {
        // Detect if mouse is over button (for hover effects)
        bool is_over_undo = (vis->mouse_x >= chess->undo_button_x && 
                             vis->mouse_x <= chess->undo_button_x + chess->undo_button_width &&
                             vis->mouse_y >= chess->undo_button_y && 
                             vis->mouse_y <= chess->undo_button_y + chess->undo_button_height);
        
        chess->undo_button_hovered = is_over_undo && chess_can_undo(chess);
        
        // Detect click: button was pressed last frame AND released this frame
        bool undo_was_pressed = chess->undo_button_was_pressed;
        bool undo_is_pressed = vis->mouse_left_pressed;
        bool undo_clicked = (undo_was_pressed && !undo_is_pressed && is_over_undo && chess_can_undo(chess));
        
        // Update for next frame
        chess->undo_button_was_pressed = undo_is_pressed;
        
        // Handle the click if it happened
        if (undo_clicked) {
            chess_undo_last_move(chess);
        }
    } else {
        chess->undo_button_hovered = false;
        chess->undo_button_was_pressed = false;
    }
    
    // ===== CHECK FLIP BOARD BUTTON INTERACTION =====
    // Only check if in Player vs AI mode
    if (chess->player_vs_ai) {
        bool is_over_flip = (vis->mouse_x >= chess->flip_button_x && 
                             vis->mouse_x <= chess->flip_button_x + chess->flip_button_width &&
                             vis->mouse_y >= chess->flip_button_y && 
                             vis->mouse_y <= chess->flip_button_y + chess->flip_button_height);
        
        chess->flip_button_hovered = is_over_flip;
        
        // Detect click: button was pressed last frame AND released this frame
        bool flip_was_pressed = chess->flip_button_was_pressed;
        bool flip_is_pressed = vis->mouse_left_pressed;
        bool flip_clicked = (flip_was_pressed && !flip_is_pressed && is_over_flip);
        
        // Update for next frame
        chess->flip_button_was_pressed = flip_is_pressed;
        
        // Handle the click if it happened
        if (flip_clicked) {
            chess->board_flipped = !chess->board_flipped;
            chess->flip_button_glow = 1.0;
            
            // Reset the game when flipping
            chess_init_board(&chess->game);
            chess->status = CHESS_PLAYING;
            chess->beats_since_game_over = 0;
            chess->waiting_for_restart = false;
            chess->move_count = 0;
            chess->eval_bar_position = 0;
            chess->eval_bar_target = 0;
            chess->time_thinking = 0;
            chess->last_move_glow = 0;
            chess->animation_progress = 0;
            chess->is_animating = false;
            chess->last_from_row = -1;
            chess->last_from_col = -1;
            chess->last_to_row = -1;
            chess->last_to_col = -1;
            
            // Reset timers
            chess->white_total_time = 0.0;
            chess->black_total_time = 0.0;
            chess->current_move_start_time = 0.0;
            chess->last_move_end_time = 0.0;
            chess->time_since_last_move = 0.0;
            
            // Clear move history
            chess->move_history_count = 0;
            
            // Clear selection
            chess->has_selected_piece = false;
            chess->selected_piece_row = -1;
            chess->selected_piece_col = -1;
            
            // Update status text
            if (chess->board_flipped) {
                snprintf(chess->status_text, sizeof(chess->status_text), "Playing as BLACK - AI plays WHITE");
                chess->status_flash_color[0] = 0.9;
                chess->status_flash_color[1] = 0.9;
                chess->status_flash_color[2] = 0.2;
            } else {
                snprintf(chess->status_text, sizeof(chess->status_text), "Playing as WHITE - AI plays BLACK");
                chess->status_flash_color[0] = 0.2;
                chess->status_flash_color[1] = 0.8;
                chess->status_flash_color[2] = 1.0;
            }
            chess->status_flash_timer = 2.0;
            
            // Start thinking for new game
            chess_start_thinking(&chess->thinking_state, &chess->game);
        }
        
        // Decay glow effect
        chess->flip_button_glow *= 0.95;
    }
    // =============================================
    
    // ===== CHECK RENDER MODE BUTTON INTERACTION =====
    // Detect if mouse is over button (for hover effects)
    bool is_over_render_mode = (vis->mouse_x >= chess->render_mode_button_x && 
                                vis->mouse_x <= chess->render_mode_button_x + chess->render_mode_button_width &&
                                vis->mouse_y >= chess->render_mode_button_y && 
                                vis->mouse_y <= chess->render_mode_button_y + chess->render_mode_button_height);
    
    chess->render_mode_button_hovered = is_over_render_mode;
    
    // Detect click: button was pressed last frame AND released this frame
    bool render_mode_was_pressed = chess->render_mode_button_was_pressed;
    bool render_mode_is_pressed = vis->mouse_left_pressed;
    bool render_mode_clicked = (render_mode_was_pressed && !render_mode_is_pressed && is_over_render_mode);
    
    // Update for next frame
    chess->render_mode_button_was_pressed = render_mode_is_pressed;
    
    // Handle the click if it happened
    /*if (render_mode_clicked) {
        // Toggle between sprites and geometric rendering
        toggle_sprite_mode();
        chess->render_mode_button_glow = 1.0;
    }*/
    
    // Decay glow effect
    chess->render_mode_button_glow *= 0.95;
    // ================================================
    
    // Decay glow effects
    chess->reset_button_glow *= 0.95;
    chess->pvsa_button_glow *= 0.95;
    chess->undo_button_glow *= 0.95;
    // =========================================
    if (chess->player_vs_ai) {
        // Determine which color the player is controlling based on board flip
        ChessColor player_color = chess->board_flipped ? BLACK : WHITE;
        
        if (chess->game.turn == player_color) {
        // Get which square the mouse is over
        double cell = chess->cell_size;
        double ox = chess->board_offset_x;
        double oy = chess->board_offset_y;
        
        // Calculate which square (if any) mouse is over
        int mouse_row = -1, mouse_col = -1;
        if (vis->mouse_x >= ox && vis->mouse_x < ox + cell * 8 &&
            vis->mouse_y >= oy && vis->mouse_y < oy + cell * 8) {
            int visual_row = (int)((vis->mouse_y - oy) / cell);
            int visual_col = (int)((vis->mouse_x - ox) / cell);
            
            // Transform visual coordinates back to logical board coordinates if board is flipped
            if (chess->board_flipped) {
                mouse_row = BOARD_SIZE - 1 - visual_row;
                mouse_col = BOARD_SIZE - 1 - visual_col;
            } else {
                mouse_row = visual_row;
                mouse_col = visual_col;
            }
        }
        
        // Detect single click (press then release)
        bool is_pressed = vis->mouse_left_pressed;
        bool was_pressed = chess->selected_piece_was_pressed;
        bool just_clicked = (was_pressed && !is_pressed);  // Button released this frame
        
        // Update for next frame
        chess->selected_piece_was_pressed = is_pressed;
        
        if (just_clicked && mouse_row >= 0 && mouse_col >= 0) {
            // Handle click based on whether we have a piece selected
            if (!chess->has_selected_piece) {
                // First click: select a piece if it's the player's color
                ChessPiece piece = chess->game.board[mouse_row][mouse_col];
                if (piece.type != EMPTY && piece.color == player_color) {
                    chess->selected_piece_row = mouse_row;
                    chess->selected_piece_col = mouse_col;
                    chess->has_selected_piece = true;
                    snprintf(chess->status_text, sizeof(chess->status_text), "Piece selected - click destination");
                }
            } else {
                // Second click: try to move to destination
                int from_row = chess->selected_piece_row;
                int from_col = chess->selected_piece_col;
                int to_row = mouse_row;
                int to_col = mouse_col;
                
                // Don't allow moving to same square
                if (from_row == to_row && from_col == to_col) {
                    // Same square - deselect piece
                    chess->has_selected_piece = false;
                    snprintf(chess->status_text, sizeof(chess->status_text), "Piece deselected");
                } else {
                    // Try to move
                    if (chess_is_valid_move(&chess->game, from_row, from_col, to_row, to_col)) {
                        // Check if move leaves king in check
                        ChessGameState temp_game = chess->game;
                        ChessMove test_move = {from_row, from_col, to_row, to_col, 0};
                        chess_make_move(&temp_game, test_move);
                        
                        if (!chess_is_in_check(&temp_game, player_color)) {
                            // Valid move! Make it
                            chess_make_move(&chess->game, test_move);
                            
                            // Track time for player's color and save move history
                            double time_on_move = chess->current_move_start_time;
                            if (player_color == WHITE) {
                                chess->white_total_time += time_on_move;
                            } else {
                                chess->black_total_time += time_on_move;
                            }
                            chess->last_move_end_time = 0;  // Reset for next turn
                            
                            chess_save_move_history(chess, test_move, time_on_move);
                            
                            // Update display
                            chess->last_from_row = from_row;
                            chess->last_from_col = from_col;
                            chess->last_to_row = to_row;
                            chess->last_to_col = to_col;
                            chess->last_move_glow = 1.0;
                            
                            chess->animating_from_row = from_row;
                            chess->animating_from_col = from_col;
                            chess->animating_to_row = to_row;
                            chess->animating_to_col = to_col;
                            chess->animation_progress = 0;
                            chess->is_animating = true;
                            
                            // Show whose turn it is now (AI's color)
                            ChessColor ai_color = chess->board_flipped ? WHITE : BLACK;
                            if (ai_color == WHITE) {
                                snprintf(chess->status_text, sizeof(chess->status_text), "White (AI) thinking...");
                            } else {
                                snprintf(chess->status_text, sizeof(chess->status_text), "Black (AI) thinking...");
                            }
                            chess->move_count++;
                            chess->time_since_last_move = 0;
                            chess->current_move_start_time = 0;  // Reset timer for AI's thinking
                            
                            // Check game status
                            chess->status = chess_check_game_status(&chess->game);
                            if (chess->status != CHESS_PLAYING) {
                                chess->waiting_for_restart = true;
                                chess->beats_since_game_over = 0;
                                chess->white_total_time = 0.0;
                                chess->black_total_time = 0.0;
                                chess->current_move_start_time = 0.0;
                                chess->last_move_end_time = 0.0;

                                if (chess->status == CHESS_CHECKMATE_WHITE) {
                                    snprintf(chess->status_text, sizeof(chess->status_text), "Checkmate! Black wins!");
                                    chess->status_flash_color[0] = 0.85;
                                    chess->status_flash_color[1] = 0.65;
                                    chess->status_flash_color[2] = 0.13;
                                    chess->is_checkmate = true;
                                    chess->check_display_timer = 0;  // Hide CHECK
                                } else if (chess->status == CHESS_CHECKMATE_BLACK) {
                                    snprintf(chess->status_text, sizeof(chess->status_text), "Checkmate! White wins!");
                                    chess->status_flash_color[0] = 1.0;
                                    chess->status_flash_color[1] = 1.0;
                                    chess->status_flash_color[2] = 1.0;
                                    chess->is_checkmate = true;
                                    chess->check_display_timer = 0;  // Hide CHECK
                                } else {
                                    snprintf(chess->status_text, sizeof(chess->status_text), "Stalemate!");
                                    chess->status_flash_color[0] = 0.7;
                                    chess->status_flash_color[1] = 0.7;
                                    chess->status_flash_color[2] = 0.7;
                                    chess->is_stalemate = true;
                                    chess->check_display_timer = 0;  // Hide CHECK
                                }
                                chess->status_flash_timer = 2.0;
                            } else {
                                // Start AI thinking for Black's move
                                chess->time_thinking = 0;  // Reset timer for new thinking phase
                                chess_start_thinking(&chess->thinking_state, &chess->game);
                            }
                            
                            // Deselect piece after successful move
                            chess->has_selected_piece = false;
                            chess->selected_piece_row = -1;
                            chess->selected_piece_col = -1;
                        } else {
                            // Move would leave king in check - invalid
                            snprintf(chess->status_text, sizeof(chess->status_text), "Illegal move - king in check");
                            chess->has_selected_piece = false;
                        }
                    } else {
                        // Invalid move
                        snprintf(chess->status_text, sizeof(chess->status_text), "Illegal move");
                        chess->has_selected_piece = false;
                    }
                }
            }
        }
        }  // Close the if (chess->game.turn == player_color) block
    }
    // ===================================================
    
    // Update glow effects
    if (chess->last_move_glow > 0) {
        chess->last_move_glow -= dt * 2.0;
        if (chess->last_move_glow < 0) chess->last_move_glow = 0;
    }
    
    // Update reset button hover glow
    if (chess->reset_button_glow > 0) {
        chess->reset_button_glow -= dt * 2.0;
        if (chess->reset_button_glow < 0) chess->reset_button_glow = 0;
    }
    
    // Update PvsA button hover glow
    if (chess->pvsa_button_glow > 0) {
        chess->pvsa_button_glow -= dt * 2.0;
        if (chess->pvsa_button_glow < 0) chess->pvsa_button_glow = 0;
    }
    
    if (chess->status_flash_timer > 0) {
        chess->status_flash_timer -= dt * 2.0;
        if (chess->status_flash_timer < 0) chess->status_flash_timer = 0;
    }
    
    // Animate piece movement
    if (chess->is_animating) {
        chess->animation_progress += dt * 3.0;
        if (chess->animation_progress >= 1.0) {
            chess->animation_progress = 1.0;
            chess->is_animating = false;
        }
    }
    
    // Smooth eval bar
    double diff = chess->eval_bar_target - chess->eval_bar_position;
    chess->eval_bar_position += diff * dt * 3.0;
    
    // Handle game over
    if (chess->status != CHESS_PLAYING) {
        if (chess->waiting_for_restart) {
            if (beat_chess_detect_beat(vis)) {
                chess->beats_since_game_over++;
                chess->time_since_last_move = 0;
                
                if (chess->beats_since_game_over >= 2) {
                    // Restart game
                    chess_init_board(&chess->game);
                    chess->status = CHESS_PLAYING;
                    chess->beats_since_game_over = 0;
                    chess->waiting_for_restart = false;
                    chess->move_count = 0;
                    chess->eval_bar_position = 0;
                    chess->eval_bar_target = 0;
                    chess->time_thinking = 0;
                    snprintf(chess->status_text, sizeof(chess->status_text), "New game! White to move");
                    chess->status_flash_color[0] = 0.0;
                    chess->status_flash_color[1] = 1.0;
                    chess->status_flash_color[2] = 1.0;
                    chess->status_flash_timer = 1.0;
                    
                    chess_start_thinking(&chess->thinking_state, &chess->game);
                }
            }
        }
        return;
    }
    
    // Track thinking time
#if BEATCHESS_HAS_PTHREAD
    pthread_mutex_lock(&chess->thinking_state.lock);
#endif
    bool is_thinking = chess->thinking_state.thinking;
    bool has_move = chess->thinking_state.has_move;
    int current_depth = chess->thinking_state.current_depth;
    int best_score = chess->thinking_state.best_score;
#if BEATCHESS_HAS_PTHREAD
    pthread_mutex_unlock(&chess->thinking_state.lock);
#endif
    
    // Keep incrementing time as long as we haven't made a move yet
    if (is_thinking || has_move) {
        chess->time_thinking += dt;
    }
    
    // AUTO-PLAY: Check if we should play immediately
    bool should_auto_play = false;
    
    if (chess->auto_play_enabled && has_move && 
        chess->time_thinking >= chess->min_think_time) {
        
        // Determine which color the player is controlling
        ChessColor player_color_local = chess->board_flipped ? BLACK : WHITE;
        
        
        // In Player vs AI mode: don't autoplay if it's the player's turn
        if (chess->player_vs_ai && chess->game.turn == player_color_local) {
            should_auto_play = false;
        }
        // Force move after 4 seconds regardless of depth/evaluation
        else if (chess->time_thinking >= 4.0) {
            should_auto_play = true;
        }
        // Play if we've reached depth 3 or 4
        else if (current_depth >= 3) {
            should_auto_play = true;
        }
        // Or if we found a really good move (even at depth 2)
        else {
            int eval_before = chess_evaluate_position(&chess->game);
            int advantage = (chess->game.turn == WHITE) ? 
                           (best_score - eval_before) : (eval_before - best_score);
            
            
            if (advantage > chess->good_move_threshold && current_depth >= 2) {
                should_auto_play = true;
            } else {
            }
        }
    } else {
    }
    
    // Detect beat OR auto-play trigger
    bool beat_detected = beat_chess_detect_beat(vis);
    
    // Determine which color the player is controlling
    ChessColor player_color = chess->board_flipped ? BLACK : WHITE;
    ChessColor ai_color = chess->board_flipped ? WHITE : BLACK;
    
    // In Player vs AI mode: only AI makes moves when it's AI's turn, player controls their color
    // In AI vs AI mode: both sides make moves
    bool should_make_move = beat_detected || should_auto_play;
    if (chess->player_vs_ai && chess->game.turn == player_color) {
        // Player's turn - don't auto-make move
        should_make_move = false;
    }
    
    if (should_make_move) {
        
        // Get current evaluation
        int eval_before = chess_evaluate_position(&chess->game);
        
        // Force move
        ChessMove forced_move = chess_get_best_move_now(&chess->thinking_state);
        
        // Validate move
        if (!chess_is_valid_move(&chess->game, 
                                 forced_move.from_row, forced_move.from_col,
                                 forced_move.to_row, forced_move.to_col)) {
            chess_start_thinking(&chess->thinking_state, &chess->game);
            chess->time_thinking = 0;
            return;
        }
        
        // Check if move leaves king in check
        ChessGameState temp_game = chess->game;
        chess_make_move(&temp_game, forced_move);
        if (chess_is_in_check(&temp_game, chess->game.turn)) {
            chess_start_thinking(&chess->thinking_state, &chess->game);
            chess->time_thinking = 0;
            return;
        }
        
        
        // Get depth reached
#if BEATCHESS_HAS_PTHREAD
        pthread_mutex_lock(&chess->thinking_state.lock);
#endif
        int depth_reached = chess->thinking_state.current_depth;
#if BEATCHESS_HAS_PTHREAD
        pthread_mutex_unlock(&chess->thinking_state.lock);
#endif
        
        // Make the move
        ChessColor moving_color = chess->game.turn;
        chess_make_move(&chess->game, forced_move);
        
        // Track time and save move history (only in Player vs AI mode)
        if (chess->player_vs_ai) {
            double ai_time = chess->time_thinking;
            // Track time for whichever color the AI is playing
            if (ai_color == WHITE) {
                chess->white_total_time += ai_time;
            } else {
                chess->black_total_time += ai_time;
            }
            chess_save_move_history(chess, forced_move, ai_time);
            chess->time_thinking = 0;  // Reset for next AI turn
        }
        
        // Evaluate
        int eval_after = chess_evaluate_position(&chess->game);
        int eval_change = (moving_color == WHITE) ? 
                         (eval_after - eval_before) : (eval_before - eval_after);
        chess->last_eval_change = eval_change;
        
        // Update eval bar
        chess->eval_bar_target = fmax(-1.0, fmin(1.0, eval_after / 1000.0));
        
        // Update display
        chess->last_from_row = forced_move.from_row;
        chess->last_from_col = forced_move.from_col;
        chess->last_to_row = forced_move.to_row;
        chess->last_to_col = forced_move.to_col;
        chess->last_move_glow = 1.0;
        
        // Animation
        chess->animating_from_row = forced_move.from_row;
        chess->animating_from_col = forced_move.from_col;
        chess->animating_to_row = forced_move.to_row;
        chess->animating_to_col = forced_move.to_col;
        chess->animation_progress = 0;
        chess->is_animating = true;
        
        // Status text
        const char *piece_names[] = {"", "Pawn", "Knight", "Bishop", "Rook", "Queen", "King"};
        ChessPiece moved_piece = chess->game.board[forced_move.to_row][forced_move.to_col];
        
        // Bounds check piece type before using as array index
        #define NUM_PIECE_NAMES (sizeof(piece_names) / sizeof(piece_names[0]))
        int piece_idx = moved_piece.type;
        if (piece_idx < 0 || piece_idx >= int(NUM_PIECE_NAMES)) {
            piece_idx = 0;  // Default to empty string if invalid
        }
        
        const char *trigger = should_auto_play ? "AUTO" : "BEAT";
        
        if (eval_change < -500) {
            snprintf(chess->status_text, sizeof(chess->status_text),
                    "[%s] BLUNDER! %s %c%d->%c%d (depth %d, -%d)",
                    trigger, piece_names[piece_idx],
                    'a' + forced_move.from_col, 8 - forced_move.from_row,
                    'a' + forced_move.to_col, 8 - forced_move.to_row,
                    depth_reached, -eval_change);
            chess->status_flash_color[0] = 1.0;
            chess->status_flash_color[1] = 0.0;
            chess->status_flash_color[2] = 0.0;
            chess->status_flash_timer = 1.0;
        } else if (eval_change > 200) {
            snprintf(chess->status_text, sizeof(chess->status_text),
                    "[%s] Brilliant! %s %c%d->%c%d (depth %d, +%d)",
                    trigger, piece_names[piece_idx],
                    'a' + forced_move.from_col, 8 - forced_move.from_row,
                    'a' + forced_move.to_col, 8 - forced_move.to_row,
                    depth_reached, eval_change);
            chess->status_flash_color[0] = 0.0;
            chess->status_flash_color[1] = 1.0;
            chess->status_flash_color[2] = 0.0;
            chess->status_flash_timer = 1.0;
        } else {
            snprintf(chess->status_text, sizeof(chess->status_text),
                    "[%s] %s: %s %c%d->%c%d (depth %d)",
                    trigger, moving_color == WHITE ? "White" : "Black",
                    piece_names[piece_idx],
                    'a' + forced_move.from_col, 8 - forced_move.from_row,
                    'a' + forced_move.to_col, 8 - forced_move.to_row,
                    depth_reached);
        }
        
        chess->move_count++;
        chess->time_since_last_move = 0;
        chess->time_thinking = 0;
        
        // Check move limit
        if (chess->move_count >= MAX_MOVES_BEFORE_DRAW) {
            snprintf(chess->status_text, sizeof(chess->status_text),
                    "Draw by move limit! New game in 2 beats...");
            chess->status = CHESS_STALEMATE;
            chess->is_stalemate = true;
            chess->check_display_timer = 0;  // Hide CHECK
            chess->waiting_for_restart = true;
            chess->beats_since_game_over = 0;
            return;
        }
        
        // Check game status
        chess->status = chess_check_game_status(&chess->game);
        
        if (chess->status != CHESS_PLAYING) {
            chess->waiting_for_restart = true;
            chess->beats_since_game_over = 0;
            
            if (chess->status == CHESS_CHECKMATE_WHITE) {
                snprintf(chess->status_text, sizeof(chess->status_text),
                        "Checkmate! White wins! New game in 2 beats...");
                chess->status_flash_color[0] = 1.0;
                chess->status_flash_color[1] = 1.0;
                chess->status_flash_color[2] = 1.0;
                chess->is_checkmate = true;
                chess->check_display_timer = 0;  // Hide CHECK
                chess->status_flash_timer = 2.0;
            } else if (chess->status == CHESS_CHECKMATE_BLACK) {
                snprintf(chess->status_text, sizeof(chess->status_text),
                        "Checkmate! Black wins! New game in 2 beats...");
                chess->status_flash_color[0] = 0.85;
                chess->status_flash_color[1] = 0.65;
                chess->status_flash_color[2] = 0.13;
                chess->is_checkmate = true;
                chess->check_display_timer = 0;  // Hide CHECK
                chess->status_flash_timer = 2.0;
            } else {
                snprintf(chess->status_text, sizeof(chess->status_text),
                        "Stalemate! New game in 2 beats...");
                chess->status_flash_color[0] = 0.7;
                chess->status_flash_color[1] = 0.7;
                chess->status_flash_color[2] = 0.7;
                chess->is_stalemate = true;
                chess->check_display_timer = 0;  // Hide CHECK
                chess->status_flash_timer = 2.0;
            }
        } else {
            // Start thinking for next move
            chess_start_thinking(&chess->thinking_state, &chess->game);
        }
    }
}
#endif

void chess_cleanup_thinking_state(ChessThinkingState *ts) {
#if BEATCHESS_HAS_PTHREAD
    // Stop thinking
    pthread_mutex_lock(&ts->lock);
    ts->thinking = false;
    pthread_mutex_unlock(&ts->lock);
    
    // Cancel and wait for thread to finish
    pthread_cancel(ts->thread);
    pthread_join(ts->thread, NULL);
    
    // Destroy mutex
    pthread_mutex_destroy(&ts->lock);
#else
    // DOS version - just stop thinking
    ts->thinking = false;
#endif
}
