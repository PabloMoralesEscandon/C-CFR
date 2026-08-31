#ifndef CFR_TEST_TRAVERSAL_GAME_H
#define CFR_TEST_TRAVERSAL_GAME_H

#include <stdbool.h>
#include <stddef.h>

#include "cfr/game.h"

typedef enum {
    TRAVERSAL_PHASE_ROOT_PLAYER_0,
    TRAVERSAL_PHASE_PLAYER_1,
    TRAVERSAL_PHASE_REACH_ROOT_PLAYER_1,
    TRAVERSAL_PHASE_REACH_SECOND_PLAYER_1,
    TRAVERSAL_PHASE_SHARED_ROOT_PLAYER_0,
    TRAVERSAL_PHASE_SHARED_LEFT_PLAYER_1,
    TRAVERSAL_PHASE_SHARED_RIGHT_PLAYER_1,
    TRAVERSAL_PHASE_ATOMIC_PLAYER_0,
    TRAVERSAL_PHASE_TERMINAL,
    TRAVERSAL_PHASE_CHANCE
} TraversalPhase;

typedef enum {
    TRAVERSAL_ACTION_EXIT,
    TRAVERSAL_ACTION_ENTER,
    TRAVERSAL_ACTION_YIELD,
    TRAVERSAL_ACTION_RESIST,
    TRAVERSAL_ACTION_STOP,
    TRAVERSAL_ACTION_CONTINUE,
    TRAVERSAL_ACTION_BAD,
    TRAVERSAL_ACTION_GOOD,
    TRAVERSAL_ACTION_SHARED_LEFT,
    TRAVERSAL_ACTION_SHARED_RIGHT,
    TRAVERSAL_ACTION_FIRST,
    TRAVERSAL_ACTION_SECOND,
    TRAVERSAL_ACTION_ATOMIC_POSITIVE,
    TRAVERSAL_ACTION_ATOMIC_NEGATIVE
} TraversalAction;

typedef struct {
    TraversalPhase phase;
    Utility terminal_utility_player_0;
    Action last_action;
} TraversalHistoryEntry;

typedef struct {
    TraversalPhase phase;
    Utility terminal_utility_player_0;
    Action last_action;
    TraversalHistoryEntry history[2];
    size_t history_count;
    bool reverse_shared_root_actions;
    bool reverse_right_shared_actions;
    bool fail_after_any_action;
    bool fail_after_selected_action;
    Action selected_failure_action;
    Status failure_after_apply;
    Status undo_failure;
    bool force_required_count;
    size_t forced_required_count;
} TraversalGameState;

Status traversal_game_state_init(TraversalGameState *state);
Status traversal_game_state_init_reach(TraversalGameState *state);
Status traversal_game_state_init_shared(TraversalGameState *state,
                                        bool reverse_root_actions);
Status traversal_game_state_init_inconsistent_shared(TraversalGameState *state);
Status traversal_game_state_init_atomic(TraversalGameState *state);
Status traversal_game_state_init_terminal(TraversalGameState *state,
                                          Utility utility_player_0);
void traversal_game_fail_after_apply(TraversalGameState *state, Status status);
void traversal_game_fail_after_action(TraversalGameState *state, Action action,
                                      Status status);
void traversal_game_fail_undo(TraversalGameState *state, Status status);
void traversal_game_force_required_count(TraversalGameState *state,
                                         size_t required_count);

const Game *traversal_game_descriptor(void);
GameState *traversal_game_state_as_public(TraversalGameState *state);
const GameState *
traversal_game_state_as_public_const(const TraversalGameState *state);

#endif
