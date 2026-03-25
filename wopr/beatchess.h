#ifndef BEATCHESS_H
#define BEATCHESS_H

#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef MSDOS
    #define BEATCHESS_DOS 1
    #define BEATCHESS_HAS_PTHREAD 0
#else
    #define BEATCHESS_DOS 0
    #define BEATCHESS_HAS_PTHREAD 1
    #include <pthread.h>
#endif

#define BOARD_SIZE 8
#ifndef MAX_CHESS_DEPTH
#define MAX_CHESS_DEPTH 4
#endif
#define BEAT_HISTORY_SIZE 10
#define MAX_MOVES_BEFORE_DRAW 300
#define MAX_MOVE_HISTORY (2 * MAX_MOVES_BEFORE_DRAW)

#define TRANSPOSITION_TABLE_SIZE 131072
#define MAX_KILLERS_PER_DEPTH 2
#define TT_MASK (TRANSPOSITION_TABLE_SIZE - 1)

#define MOVE_HISTORY_AT(buffer, idx) ((buffer)[(idx)])
#define MOVE_HISTORY_IS_FULL(count) ((count) >= MAX_MOVE_HISTORY)

typedef enum { EMPTY, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING } PieceType;
typedef enum { NONE, WHITE, BLACK } ChessColor;

typedef struct {
    PieceType type;
    ChessColor color;
} ChessPiece;

typedef struct {
    int from_row, from_col;
    int to_row, to_col;
    int score;
} ChessMove;

typedef struct {
    ChessPiece board[BOARD_SIZE][BOARD_SIZE];
    ChessColor turn;
    bool white_king_moved, black_king_moved;
    bool white_rook_a_moved, white_rook_h_moved;
    bool black_rook_a_moved, black_rook_h_moved;
    int en_passant_col;
    int en_passant_row;
} ChessGameState;

typedef enum { TT_EXACT, TT_LOWER, TT_UPPER } TTFlag;

typedef struct {
    uint64_t hash;
    int value;
    int depth;
    TTFlag flag;
} TranspositionEntry;

typedef struct {
    ChessMove killer_moves[MAX_CHESS_DEPTH + 1][MAX_KILLERS_PER_DEPTH];
    int killer_count[MAX_CHESS_DEPTH + 1];
} KillerMoveTable;

typedef struct {
    int history[8][8][8][8];
} HistoryHeuristic;

typedef struct {
    uint64_t zobrist_board[8][8][7];
    uint64_t zobrist_side_to_move;
    uint64_t zobrist_en_passant[8];
    uint64_t zobrist_castling[4];
} ZobristTable;

typedef struct {
    ChessGameState game;
    ChessMove best_move;
    int best_score;
    int current_depth;
    bool has_move;
    bool thinking;
#if BEATCHESS_HAS_PTHREAD
    pthread_mutex_t lock;
    pthread_t thread;
#endif
} ChessThinkingState;

typedef enum {
    CHESS_PLAYING,
    CHESS_CHECKMATE_WHITE,
    CHESS_CHECKMATE_BLACK,
    CHESS_STALEMATE
} ChessGameStatus;

typedef struct {
    ChessGameState game_state;
    ChessMove move;
    double time_elapsed;
} MoveHistory;

typedef struct {
    ChessGameState game;
    ChessThinkingState thinking_state;
    ChessGameStatus status;

    double piece_x[BOARD_SIZE][BOARD_SIZE];
    double piece_y[BOARD_SIZE][BOARD_SIZE];
    double target_x[BOARD_SIZE][BOARD_SIZE];
    double target_y[BOARD_SIZE][BOARD_SIZE];

    double last_move_glow;
    double status_flash_timer;
    double status_flash_color[3];
    int last_eval_change;

    double beat_volume_history[BEAT_HISTORY_SIZE];
    int beat_history_index;
    double time_since_last_move;
    double beat_threshold;

    int animating_from_row, animating_from_col;
    int animating_to_row, animating_to_col;
    double animation_progress;
    bool is_animating;

    double eval_bar_position;
    double eval_bar_target;

    int beats_since_game_over;
    bool waiting_for_restart;

    double time_thinking;
    double min_think_time;
    int good_move_threshold;
    bool auto_play_enabled;

    bool is_in_check;
    double check_display_timer;
    bool is_checkmate;
    bool is_stalemate;

    double reset_button_x, reset_button_y;
    double reset_button_width, reset_button_height;
    bool reset_button_hovered;
    double reset_button_glow;
    bool reset_button_was_pressed;

    double pvsa_button_x, pvsa_button_y;
    double pvsa_button_width, pvsa_button_height;
    bool pvsa_button_hovered;
    double pvsa_button_glow;
    bool pvsa_button_was_pressed;

    double undo_button_x, undo_button_y;
    double undo_button_width, undo_button_height;
    bool undo_button_hovered;
    double undo_button_glow;
    bool undo_button_was_pressed;

    double flip_button_x, flip_button_y;
    double flip_button_width, flip_button_height;
    bool flip_button_hovered;
    double flip_button_glow;
    bool flip_button_was_pressed;

    double render_mode_button_x, render_mode_button_y;
    double render_mode_button_width, render_mode_button_height;
    bool render_mode_button_hovered;
    double render_mode_button_glow;
    bool render_mode_button_was_pressed;

    int last_from_row, last_from_col;
    int last_to_row, last_to_col;

    char status_text[256];

    double board_offset_x, board_offset_y;
    double cell_size;
    int move_count;

    MoveHistory move_history[MAX_MOVE_HISTORY*2];
    int move_history_count;

    double white_total_time;
    double black_total_time;
    double current_move_start_time;
    double last_move_end_time;

    bool player_vs_ai;
    bool board_flipped;

    int selected_piece_row, selected_piece_col;
    bool has_selected_piece;
    int selected_piece_was_pressed;

} BeatChessVisualization;

/* Core chess functions */
bool chess_can_undo(BeatChessVisualization *chess);
void chess_init_board(ChessGameState *game);
bool chess_is_valid_move(ChessGameState *game, int fr, int fc, int tr, int tc);
void chess_execute_move(ChessGameState *game, int fr, int fc, int tr, int tc);
bool chess_is_in_bounds(int r, int c);
bool chess_is_path_clear(ChessGameState *game, int fr, int fc, int tr, int tc);
void chess_make_move(ChessGameState *game, ChessMove move);
void chess_save_move_history(BeatChessVisualization *chess, ChessMove move, double time_elapsed);
int  chess_get_all_moves(ChessGameState *game, ChessColor color, ChessMove *moves);
int  chess_evaluate_position(ChessGameState *game);
bool chess_is_in_check(ChessGameState *game, ChessColor color);
int  chess_minimax(ChessGameState *game, int depth, int alpha, int beta, bool maximizing);
ChessGameStatus chess_check_game_status(ChessGameState *game);

void chess_init_zobrist(void);
uint64_t chess_zobrist_hash(ChessGameState *game);
void chess_clear_transposition_table(void);
void chess_store_tt(uint64_t hash, int depth, int value, TTFlag flag);
bool chess_probe_tt(uint64_t hash, int depth, int alpha, int beta, int *out_value);
void chess_update_killer_move(KillerMoveTable *killers, ChessMove move, int depth);
void chess_clear_killers(KillerMoveTable *killers);
int  chess_minimax_enhanced(ChessGameState *game, int depth, int initial_depth,
                            int alpha, int beta, bool maximizing, KillerMoveTable *killers);

#endif // BEATCHESS_H
