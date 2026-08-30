#include <float.h>
#include <stdbool.h>
#include <string.h>

#include "traversal_game.h"

static Status traversal_is_terminal(const void *context,
                                    const GameState *state, bool *result);
static Status traversal_terminal_utility(const void *context,
                                         const GameState *state, Player player,
                                         Utility *result);
static Status traversal_current_actor(const void *context,
                                      const GameState *state, Actor *result);
static Status traversal_legal_actions(const void *context,
                                      const GameState *state, Action *actions,
                                      size_t capacity, size_t *required_count);
static Status traversal_apply_action(const void *context, GameState *state,
                                     Action action);
static Status traversal_undo_action(const void *context, GameState *state);
static Status traversal_chance_probability(const void *context,
                                           const GameState *state,
                                           Action action, Probability *result);
static Status traversal_information_set_key(const void *context,
                                            const GameState *state,
                                            InfoSetKey *result);

static const GameOperations TRAVERSAL_OPERATIONS = {
    .is_terminal = traversal_is_terminal,
    .terminal_utility = traversal_terminal_utility,
    .current_actor = traversal_current_actor,
    .legal_actions = traversal_legal_actions,
    .apply_action = traversal_apply_action,
    .undo_action = traversal_undo_action,
    .chance_probability = traversal_chance_probability,
    .information_set_key = traversal_information_set_key,
};

static const Game TRAVERSAL_GAME = {
    .operations = &TRAVERSAL_OPERATIONS,
    .context = NULL,
    .strategic_player_count = 2,
    .max_legal_actions = 2,
    .strategy_schema_id = "cfr.test.traversal-game/v1",
};

static Status initialize_phase(TraversalGameState *state,
                               TraversalPhase phase) {
    if (state == NULL) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    memset(state, 0, sizeof(*state));
    state->phase = phase;
    state->failure_after_apply = CFR_STATUS_SUCCESS;
    return CFR_STATUS_SUCCESS;
}

Status traversal_game_state_init(TraversalGameState *state) {
    return initialize_phase(state, TRAVERSAL_PHASE_ROOT_PLAYER_0);
}

Status traversal_game_state_init_reach(TraversalGameState *state) {
    return initialize_phase(state, TRAVERSAL_PHASE_REACH_ROOT_PLAYER_1);
}

Status traversal_game_state_init_shared(TraversalGameState *state,
                                        bool reverse_root_actions) {
    const Status status =
        initialize_phase(state, TRAVERSAL_PHASE_SHARED_ROOT_PLAYER_0);

    if (status == CFR_STATUS_SUCCESS) {
        state->reverse_shared_root_actions = reverse_root_actions;
    }
    return status;
}

Status traversal_game_state_init_atomic(TraversalGameState *state) {
    return initialize_phase(state, TRAVERSAL_PHASE_ATOMIC_PLAYER_0);
}

Status traversal_game_state_init_terminal(TraversalGameState *state,
                                          Utility utility_player_0) {
    const Status status = initialize_phase(state, TRAVERSAL_PHASE_TERMINAL);

    if (status == CFR_STATUS_SUCCESS) {
        state->terminal_utility_player_0 = utility_player_0;
    }
    return status;
}

void traversal_game_fail_after_apply(TraversalGameState *state,
                                     Status status) {
    if (state != NULL) {
        state->fail_after_any_action = true;
        state->fail_after_selected_action = false;
        state->failure_after_apply = status;
    }
}

void traversal_game_fail_after_action(TraversalGameState *state, Action action,
                                      Status status) {
    if (state != NULL) {
        state->fail_after_any_action = false;
        state->fail_after_selected_action = true;
        state->selected_failure_action = action;
        state->failure_after_apply = status;
    }
}

void traversal_game_fail_undo(TraversalGameState *state, Status status) {
    if (state != NULL) {
        state->undo_failure = status;
    }
}

void traversal_game_force_required_count(TraversalGameState *state,
                                         size_t required_count) {
    if (state != NULL) {
        state->force_required_count = true;
        state->forced_required_count = required_count;
    }
}

const Game *traversal_game_descriptor(void) { return &TRAVERSAL_GAME; }

GameState *traversal_game_state_as_public(TraversalGameState *state) {
    return (GameState *)state;
}

const GameState *
traversal_game_state_as_public_const(const TraversalGameState *state) {
    return (const GameState *)state;
}

static const TraversalGameState *as_traversal_const(const GameState *state) {
    return (const TraversalGameState *)state;
}

static TraversalGameState *as_traversal(GameState *state) {
    return (TraversalGameState *)state;
}

static Status traversal_is_terminal(const void *context,
                                    const GameState *state, bool *result) {
    const TraversalGameState *traversal_state = as_traversal_const(state);

    (void)context;
    if (traversal_state->history_count > 0 &&
        traversal_state->failure_after_apply != CFR_STATUS_SUCCESS &&
        (traversal_state->fail_after_any_action ||
         (traversal_state->fail_after_selected_action &&
          traversal_state->last_action ==
              traversal_state->selected_failure_action))) {
        return traversal_state->failure_after_apply;
    }
    switch (traversal_state->phase) {
    case TRAVERSAL_PHASE_ROOT_PLAYER_0:
    case TRAVERSAL_PHASE_PLAYER_1:
    case TRAVERSAL_PHASE_REACH_ROOT_PLAYER_1:
    case TRAVERSAL_PHASE_REACH_SECOND_PLAYER_1:
    case TRAVERSAL_PHASE_SHARED_ROOT_PLAYER_0:
    case TRAVERSAL_PHASE_SHARED_LEFT_PLAYER_1:
    case TRAVERSAL_PHASE_SHARED_RIGHT_PLAYER_1:
    case TRAVERSAL_PHASE_ATOMIC_PLAYER_0:
    case TRAVERSAL_PHASE_CHANCE:
        *result = false;
        return CFR_STATUS_SUCCESS;
    case TRAVERSAL_PHASE_TERMINAL:
        *result = true;
        return CFR_STATUS_SUCCESS;
    default:
        return CFR_STATUS_INVALID_ARGUMENT;
    }
}

static Status traversal_terminal_utility(const void *context,
                                         const GameState *state, Player player,
                                         Utility *result) {
    const TraversalGameState *traversal_state = as_traversal_const(state);

    (void)context;
    if (traversal_state->phase != TRAVERSAL_PHASE_TERMINAL) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    if (player == CFR_PLAYER_0) {
        *result = traversal_state->terminal_utility_player_0;
        return CFR_STATUS_SUCCESS;
    }
    if (player == CFR_PLAYER_1) {
        *result = -traversal_state->terminal_utility_player_0;
        return CFR_STATUS_SUCCESS;
    }
    return CFR_STATUS_INVALID_ARGUMENT;
}

static Status traversal_current_actor(const void *context,
                                      const GameState *state, Actor *result) {
    const TraversalGameState *traversal_state = as_traversal_const(state);

    (void)context;
    switch (traversal_state->phase) {
    case TRAVERSAL_PHASE_ROOT_PLAYER_0:
    case TRAVERSAL_PHASE_SHARED_ROOT_PLAYER_0:
    case TRAVERSAL_PHASE_ATOMIC_PLAYER_0:
        result->kind = CFR_ACTOR_PLAYER;
        result->player = CFR_PLAYER_0;
        return CFR_STATUS_SUCCESS;
    case TRAVERSAL_PHASE_PLAYER_1:
    case TRAVERSAL_PHASE_REACH_ROOT_PLAYER_1:
    case TRAVERSAL_PHASE_REACH_SECOND_PLAYER_1:
    case TRAVERSAL_PHASE_SHARED_LEFT_PLAYER_1:
    case TRAVERSAL_PHASE_SHARED_RIGHT_PLAYER_1:
        result->kind = CFR_ACTOR_PLAYER;
        result->player = CFR_PLAYER_1;
        return CFR_STATUS_SUCCESS;
    case TRAVERSAL_PHASE_CHANCE:
        result->kind = CFR_ACTOR_CHANCE;
        return CFR_STATUS_SUCCESS;
    case TRAVERSAL_PHASE_TERMINAL:
    default:
        return CFR_STATUS_INVALID_ARGUMENT;
    }
}

static Status write_actions(Action first, Action second, Action *actions,
                            size_t capacity, size_t *required_count) {
    *required_count = 2;
    if (capacity < 2) {
        return CFR_STATUS_BUFFER_TOO_SMALL;
    }
    actions[0] = first;
    actions[1] = second;
    return CFR_STATUS_SUCCESS;
}

static Status traversal_legal_actions(const void *context,
                                      const GameState *state, Action *actions,
                                      size_t capacity, size_t *required_count) {
    const TraversalGameState *traversal_state = as_traversal_const(state);
    Status status;

    (void)context;
    switch (traversal_state->phase) {
    case TRAVERSAL_PHASE_ROOT_PLAYER_0:
        status = write_actions(TRAVERSAL_ACTION_EXIT, TRAVERSAL_ACTION_ENTER,
                               actions, capacity, required_count);
        break;
    case TRAVERSAL_PHASE_PLAYER_1:
        status = write_actions(TRAVERSAL_ACTION_YIELD,
                               TRAVERSAL_ACTION_RESIST, actions, capacity,
                               required_count);
        break;
    case TRAVERSAL_PHASE_REACH_ROOT_PLAYER_1:
        status = write_actions(TRAVERSAL_ACTION_STOP,
                               TRAVERSAL_ACTION_CONTINUE, actions, capacity,
                               required_count);
        break;
    case TRAVERSAL_PHASE_REACH_SECOND_PLAYER_1:
        status = write_actions(TRAVERSAL_ACTION_BAD, TRAVERSAL_ACTION_GOOD,
                               actions, capacity, required_count);
        break;
    case TRAVERSAL_PHASE_SHARED_ROOT_PLAYER_0:
        if (traversal_state->reverse_shared_root_actions) {
            status = write_actions(TRAVERSAL_ACTION_SHARED_RIGHT,
                                   TRAVERSAL_ACTION_SHARED_LEFT, actions,
                                   capacity, required_count);
        } else {
            status = write_actions(TRAVERSAL_ACTION_SHARED_LEFT,
                                   TRAVERSAL_ACTION_SHARED_RIGHT, actions,
                                   capacity, required_count);
        }
        break;
    case TRAVERSAL_PHASE_SHARED_LEFT_PLAYER_1:
    case TRAVERSAL_PHASE_SHARED_RIGHT_PLAYER_1:
        status = write_actions(TRAVERSAL_ACTION_FIRST,
                               TRAVERSAL_ACTION_SECOND, actions, capacity,
                               required_count);
        break;
    case TRAVERSAL_PHASE_ATOMIC_PLAYER_0:
        status = write_actions(TRAVERSAL_ACTION_ATOMIC_POSITIVE,
                               TRAVERSAL_ACTION_ATOMIC_NEGATIVE, actions,
                               capacity, required_count);
        break;
    case TRAVERSAL_PHASE_TERMINAL:
    case TRAVERSAL_PHASE_CHANCE:
    default:
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    if (status == CFR_STATUS_SUCCESS &&
        traversal_state->force_required_count) {
        *required_count = traversal_state->forced_required_count;
    }
    return status;
}

static Status prepare_transition(TraversalGameState *state) {
    if (state->history_count >=
        sizeof(state->history) / sizeof(state->history[0])) {
        return CFR_STATUS_BUFFER_TOO_SMALL;
    }
    state->history[state->history_count].phase = state->phase;
    state->history[state->history_count].terminal_utility_player_0 =
        state->terminal_utility_player_0;
    state->history[state->history_count].last_action = state->last_action;
    state->history_count += 1;
    return CFR_STATUS_SUCCESS;
}

static Status traversal_apply_action(const void *context, GameState *state,
                                     Action action) {
    TraversalGameState *traversal_state = as_traversal(state);
    TraversalPhase next_phase;
    Utility next_utility = traversal_state->terminal_utility_player_0;
    Status status;

    (void)context;
    switch (traversal_state->phase) {
    case TRAVERSAL_PHASE_ROOT_PLAYER_0:
        if (action == TRAVERSAL_ACTION_EXIT) {
            next_phase = TRAVERSAL_PHASE_TERMINAL;
            next_utility = 2.0;
        } else if (action == TRAVERSAL_ACTION_ENTER) {
            next_phase = TRAVERSAL_PHASE_PLAYER_1;
        } else {
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;
    case TRAVERSAL_PHASE_PLAYER_1:
        if (action == TRAVERSAL_ACTION_YIELD) {
            next_phase = TRAVERSAL_PHASE_TERMINAL;
            next_utility = 1.0;
        } else if (action == TRAVERSAL_ACTION_RESIST) {
            next_phase = TRAVERSAL_PHASE_TERMINAL;
            next_utility = -1.0;
        } else {
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;
    case TRAVERSAL_PHASE_REACH_ROOT_PLAYER_1:
        if (action == TRAVERSAL_ACTION_STOP) {
            next_phase = TRAVERSAL_PHASE_TERMINAL;
            next_utility = 0.0;
        } else if (action == TRAVERSAL_ACTION_CONTINUE) {
            next_phase = TRAVERSAL_PHASE_REACH_SECOND_PLAYER_1;
        } else {
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;
    case TRAVERSAL_PHASE_REACH_SECOND_PLAYER_1:
        if (action == TRAVERSAL_ACTION_BAD) {
            next_phase = TRAVERSAL_PHASE_TERMINAL;
            next_utility = 1.0;
        } else if (action == TRAVERSAL_ACTION_GOOD) {
            next_phase = TRAVERSAL_PHASE_TERMINAL;
            next_utility = -1.0;
        } else {
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;
    case TRAVERSAL_PHASE_SHARED_ROOT_PLAYER_0:
        if (action == TRAVERSAL_ACTION_SHARED_LEFT) {
            next_phase = TRAVERSAL_PHASE_SHARED_LEFT_PLAYER_1;
        } else if (action == TRAVERSAL_ACTION_SHARED_RIGHT) {
            next_phase = TRAVERSAL_PHASE_SHARED_RIGHT_PLAYER_1;
        } else {
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;
    case TRAVERSAL_PHASE_SHARED_LEFT_PLAYER_1:
        if (action == TRAVERSAL_ACTION_FIRST) {
            next_phase = TRAVERSAL_PHASE_TERMINAL;
            next_utility = -4.0;
        } else if (action == TRAVERSAL_ACTION_SECOND) {
            next_phase = TRAVERSAL_PHASE_TERMINAL;
            next_utility = 0.0;
        } else {
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;
    case TRAVERSAL_PHASE_SHARED_RIGHT_PLAYER_1:
        if (action == TRAVERSAL_ACTION_FIRST) {
            next_phase = TRAVERSAL_PHASE_TERMINAL;
            next_utility = 0.0;
        } else if (action == TRAVERSAL_ACTION_SECOND) {
            next_phase = TRAVERSAL_PHASE_TERMINAL;
            next_utility = -4.0;
        } else {
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;
    case TRAVERSAL_PHASE_ATOMIC_PLAYER_0:
        if (action == TRAVERSAL_ACTION_ATOMIC_POSITIVE) {
            next_phase = TRAVERSAL_PHASE_TERMINAL;
            next_utility = DBL_MAX;
        } else if (action == TRAVERSAL_ACTION_ATOMIC_NEGATIVE) {
            next_phase = TRAVERSAL_PHASE_TERMINAL;
            next_utility = -DBL_MAX;
        } else {
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;
    case TRAVERSAL_PHASE_TERMINAL:
    case TRAVERSAL_PHASE_CHANCE:
    default:
        return CFR_STATUS_ILLEGAL_ACTION;
    }

    status = prepare_transition(traversal_state);
    if (status != CFR_STATUS_SUCCESS) {
        return status;
    }
    traversal_state->phase = next_phase;
    traversal_state->terminal_utility_player_0 = next_utility;
    traversal_state->last_action = action;
    return CFR_STATUS_SUCCESS;
}

static Status traversal_undo_action(const void *context, GameState *state) {
    TraversalGameState *traversal_state = as_traversal(state);

    (void)context;
    if (traversal_state->undo_failure != CFR_STATUS_SUCCESS) {
        return traversal_state->undo_failure;
    }
    if (traversal_state->history_count == 0) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    traversal_state->history_count -= 1;
    traversal_state->phase =
        traversal_state->history[traversal_state->history_count].phase;
    traversal_state->terminal_utility_player_0 =
        traversal_state->history[traversal_state->history_count]
            .terminal_utility_player_0;
    traversal_state->last_action =
        traversal_state->history[traversal_state->history_count].last_action;
    return CFR_STATUS_SUCCESS;
}

static Status traversal_chance_probability(const void *context,
                                           const GameState *state,
                                           Action action,
                                           Probability *result) {
    (void)context;
    (void)state;
    (void)action;
    (void)result;
    return CFR_STATUS_INVALID_ARGUMENT;
}

static Status traversal_information_set_key(const void *context,
                                            const GameState *state,
                                            InfoSetKey *result) {
    const TraversalGameState *traversal_state = as_traversal_const(state);

    (void)context;
    switch (traversal_state->phase) {
    case TRAVERSAL_PHASE_ROOT_PLAYER_0:
        *result = 100;
        return CFR_STATUS_SUCCESS;
    case TRAVERSAL_PHASE_PLAYER_1:
        *result = 200;
        return CFR_STATUS_SUCCESS;
    case TRAVERSAL_PHASE_REACH_ROOT_PLAYER_1:
        *result = 300;
        return CFR_STATUS_SUCCESS;
    case TRAVERSAL_PHASE_REACH_SECOND_PLAYER_1:
        *result = 400;
        return CFR_STATUS_SUCCESS;
    case TRAVERSAL_PHASE_SHARED_ROOT_PLAYER_0:
        *result = 500;
        return CFR_STATUS_SUCCESS;
    case TRAVERSAL_PHASE_SHARED_LEFT_PLAYER_1:
    case TRAVERSAL_PHASE_SHARED_RIGHT_PLAYER_1:
        *result = 501;
        return CFR_STATUS_SUCCESS;
    case TRAVERSAL_PHASE_ATOMIC_PLAYER_0:
        *result = 600;
        return CFR_STATUS_SUCCESS;
    case TRAVERSAL_PHASE_TERMINAL:
    case TRAVERSAL_PHASE_CHANCE:
    default:
        return CFR_STATUS_INVALID_ARGUMENT;
    }
}
