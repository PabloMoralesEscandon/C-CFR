#ifndef CFR_TEST_CHANCE_GAME_H
#define CFR_TEST_CHANCE_GAME_H

#include <stdbool.h>
#include <stddef.h>

#include "cfr/game.h"

#define CHANCE_GAME_HISTORY_CAPACITY 96

typedef enum {
    CHANCE_GAME_PHASE_COIN_CHANCE,
    CHANCE_GAME_PHASE_COIN_PLAYER_0,
    CHANCE_GAME_PHASE_DEEP,
    CHANCE_GAME_PHASE_TERMINAL
} ChanceGamePhase;

typedef enum {
    CHANCE_GAME_ACTION_TAILS = 100,
    CHANCE_GAME_ACTION_HEADS,
    CHANCE_GAME_ACTION_STOP,
    CHANCE_GAME_ACTION_PLAY,
    CHANCE_GAME_ACTION_DEEP_NEXT
} ChanceGameAction;

typedef struct {
    ChanceGamePhase phase;
    size_t deep_level;
    Utility terminal_utility_player_0;
    Action last_action;
} ChanceGameHistoryEntry;

typedef struct {
    ChanceGamePhase phase;
    size_t deep_level;
    size_t deep_chance_count;
    Utility terminal_utility_player_0;
    Probability coin_probabilities[2];
    Action last_action;
    ChanceGameHistoryEntry history[CHANCE_GAME_HISTORY_CAPACITY];
    size_t history_count;
    bool probability_failure_enabled;
    Action probability_failure_action;
    Status probability_failure_status;
    bool branch_failure_enabled;
    Action branch_failure_action;
    Status branch_failure_status;
    Status undo_failure_status;
    bool terminal_failure_enabled;
    Player terminal_failure_player;
    Status terminal_failure_status;
} ChanceGameState;

Status chance_game_state_init_coin(ChanceGameState *state);
Status chance_game_state_init_deep(ChanceGameState *state,
                                   size_t chance_depth);
void chance_game_set_probabilities(ChanceGameState *state,
                                   Probability tails_probability,
                                   Probability heads_probability);
void chance_game_fail_probability(ChanceGameState *state, Action action,
                                  Status status);
void chance_game_fail_after_action(ChanceGameState *state, Action action,
                                   Status status);
void chance_game_fail_undo(ChanceGameState *state, Status status);
void chance_game_fail_terminal_for_player(ChanceGameState *state,
                                          Player player, Status status);

bool chance_game_state_equal(const ChanceGameState *left,
                             const ChanceGameState *right);
const Game *chance_game_descriptor(void);
GameState *chance_game_state_as_public(ChanceGameState *state);

#endif
