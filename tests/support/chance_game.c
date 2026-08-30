#include <stdbool.h>
#include <string.h>

#include "chance_game.h"

static Status chance_is_terminal(const void *context, const GameState *state,
                                 bool *result);
static Status chance_terminal_utility(const void *context,
                                      const GameState *state, Player player,
                                      Utility *result);
static Status chance_current_actor(const void *context, const GameState *state,
                                   Actor *result);
static Status chance_legal_actions(const void *context, const GameState *state,
                                   Action *actions, size_t capacity,
                                   size_t *required_count);
static Status chance_apply_action(const void *context, GameState *state,
                                  Action action);
static Status chance_undo_action(const void *context, GameState *state);
static Status chance_probability(const void *context, const GameState *state,
                                 Action action, Probability *result);
static Status chance_information_set_key(const void *context,
                                         const GameState *state,
                                         InfoSetKey *result);

static const GameOperations CHANCE_GAME_OPERATIONS = {
    .is_terminal = chance_is_terminal,
    .terminal_utility = chance_terminal_utility,
    .current_actor = chance_current_actor,
    .legal_actions = chance_legal_actions,
    .apply_action = chance_apply_action,
    .undo_action = chance_undo_action,
    .chance_probability = chance_probability,
    .information_set_key = chance_information_set_key,
};

static const Game CHANCE_GAME = {
    .operations = &CHANCE_GAME_OPERATIONS,
    .context = NULL,
    .strategic_player_count = 2,
    .max_legal_actions = 2,
    .strategy_schema_id = "cfr.test.chance-game/v1",
};

static const ChanceGameState *as_chance_const(const GameState *state) {
    return (const ChanceGameState *)state;
}

static ChanceGameState *as_chance(GameState *state) {
    return (ChanceGameState *)state;
}

static Status initialize(ChanceGameState *state, ChanceGamePhase phase) {
    if (state == NULL) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    memset(state, 0, sizeof(*state));
    state->phase = phase;
    state->coin_probabilities[0] = 0.5;
    state->coin_probabilities[1] = 0.5;
    return CFR_STATUS_SUCCESS;
}

Status chance_game_state_init_coin(ChanceGameState *state) {
    return initialize(state, CHANCE_GAME_PHASE_COIN_CHANCE);
}

Status chance_game_state_init_deep(ChanceGameState *state,
                                   size_t chance_depth) {
    const Status status = initialize(state, CHANCE_GAME_PHASE_DEEP);

    if (status != CFR_STATUS_SUCCESS) {
        return status;
    }
    if (chance_depth >= CHANCE_GAME_HISTORY_CAPACITY) {
        memset(state, 0, sizeof(*state));
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    state->deep_chance_count = chance_depth;
    return CFR_STATUS_SUCCESS;
}

void chance_game_set_probabilities(ChanceGameState *state,
                                   Probability tails_probability,
                                   Probability heads_probability) {
    if (state != NULL) {
        state->coin_probabilities[0] = tails_probability;
        state->coin_probabilities[1] = heads_probability;
    }
}

void chance_game_fail_probability(ChanceGameState *state, Action action,
                                  Status status) {
    if (state != NULL) {
        state->probability_failure_enabled = status != CFR_STATUS_SUCCESS;
        state->probability_failure_action = action;
        state->probability_failure_status = status;
    }
}

void chance_game_fail_after_action(ChanceGameState *state, Action action,
                                   Status status) {
    if (state != NULL) {
        state->branch_failure_enabled = status != CFR_STATUS_SUCCESS;
        state->branch_failure_action = action;
        state->branch_failure_status = status;
    }
}

void chance_game_fail_undo(ChanceGameState *state, Status status) {
    if (state != NULL) {
        state->undo_failure_status = status;
    }
}

void chance_game_fail_terminal_for_player(ChanceGameState *state,
                                          Player player, Status status) {
    if (state != NULL) {
        state->terminal_failure_enabled = status != CFR_STATUS_SUCCESS;
        state->terminal_failure_player = player;
        state->terminal_failure_status = status;
    }
}

bool chance_game_state_equal(const ChanceGameState *left,
                             const ChanceGameState *right) {
    return left != NULL && right != NULL &&
           memcmp(left, right, sizeof(*left)) == 0;
}

const Game *chance_game_descriptor(void) { return &CHANCE_GAME; }

GameState *chance_game_state_as_public(ChanceGameState *state) {
    return (GameState *)state;
}

static bool deep_actor_is_chance(const ChanceGameState *state) {
    return state->phase == CHANCE_GAME_PHASE_DEEP &&
           state->deep_level < state->deep_chance_count;
}

static Status chance_is_terminal(const void *context, const GameState *state,
                                 bool *result) {
    const ChanceGameState *chance_state = as_chance_const(state);

    (void)context;
    if (chance_state->history_count > 0 &&
        chance_state->branch_failure_enabled &&
        chance_state->last_action == chance_state->branch_failure_action) {
        return chance_state->branch_failure_status;
    }
    *result = chance_state->phase == CHANCE_GAME_PHASE_TERMINAL;
    return CFR_STATUS_SUCCESS;
}

static Status chance_terminal_utility(const void *context,
                                      const GameState *state, Player player,
                                      Utility *result) {
    const ChanceGameState *chance_state = as_chance_const(state);

    (void)context;
    if (chance_state->phase != CHANCE_GAME_PHASE_TERMINAL) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    if (chance_state->terminal_failure_enabled &&
        player == chance_state->terminal_failure_player) {
        return chance_state->terminal_failure_status;
    }
    if (player == CFR_PLAYER_0) {
        *result = chance_state->terminal_utility_player_0;
        return CFR_STATUS_SUCCESS;
    }
    if (player == CFR_PLAYER_1) {
        *result = -chance_state->terminal_utility_player_0;
        return CFR_STATUS_SUCCESS;
    }
    return CFR_STATUS_INVALID_ARGUMENT;
}

static Status chance_current_actor(const void *context, const GameState *state,
                                   Actor *result) {
    const ChanceGameState *chance_state = as_chance_const(state);

    (void)context;
    if (chance_state->phase == CHANCE_GAME_PHASE_COIN_CHANCE ||
        deep_actor_is_chance(chance_state)) {
        result->kind = CFR_ACTOR_CHANCE;
        return CFR_STATUS_SUCCESS;
    }
    if (chance_state->phase == CHANCE_GAME_PHASE_COIN_PLAYER_0 ||
        chance_state->phase == CHANCE_GAME_PHASE_DEEP) {
        result->kind = CFR_ACTOR_PLAYER;
        result->player = CFR_PLAYER_0;
        return CFR_STATUS_SUCCESS;
    }
    return CFR_STATUS_INVALID_ARGUMENT;
}

static Status write_actions(Action first, Action second, size_t count,
                            Action *actions, size_t capacity,
                            size_t *required_count) {
    *required_count = count;
    if (capacity < count) {
        return CFR_STATUS_BUFFER_TOO_SMALL;
    }
    actions[0] = first;
    if (count == 2) {
        actions[1] = second;
    }
    return CFR_STATUS_SUCCESS;
}

static Status chance_legal_actions(const void *context, const GameState *state,
                                   Action *actions, size_t capacity,
                                   size_t *required_count) {
    const ChanceGameState *chance_state = as_chance_const(state);

    (void)context;
    switch (chance_state->phase) {
    case CHANCE_GAME_PHASE_COIN_CHANCE:
        return write_actions(CHANCE_GAME_ACTION_TAILS,
                             CHANCE_GAME_ACTION_HEADS, 2, actions, capacity,
                             required_count);
    case CHANCE_GAME_PHASE_COIN_PLAYER_0:
        return write_actions(CHANCE_GAME_ACTION_STOP,
                             CHANCE_GAME_ACTION_PLAY, 2, actions, capacity,
                             required_count);
    case CHANCE_GAME_PHASE_DEEP:
        if (deep_actor_is_chance(chance_state)) {
            return write_actions(CHANCE_GAME_ACTION_DEEP_NEXT, 0, 1, actions,
                                 capacity, required_count);
        }
        return write_actions(CHANCE_GAME_ACTION_STOP,
                             CHANCE_GAME_ACTION_PLAY, 2, actions, capacity,
                             required_count);
    case CHANCE_GAME_PHASE_TERMINAL:
    default:
        return CFR_STATUS_INVALID_ARGUMENT;
    }
}

static Status save_state(ChanceGameState *state) {
    ChanceGameHistoryEntry *entry;

    if (state->history_count >= CHANCE_GAME_HISTORY_CAPACITY) {
        return CFR_STATUS_BUFFER_TOO_SMALL;
    }
    entry = &state->history[state->history_count];
    entry->phase = state->phase;
    entry->deep_level = state->deep_level;
    entry->terminal_utility_player_0 = state->terminal_utility_player_0;
    entry->last_action = state->last_action;
    state->history_count += 1;
    return CFR_STATUS_SUCCESS;
}

static Status chance_apply_action(const void *context, GameState *state,
                                  Action action) {
    ChanceGameState *chance_state = as_chance(state);
    ChanceGamePhase next_phase = chance_state->phase;
    size_t next_level = chance_state->deep_level;
    Utility next_utility = chance_state->terminal_utility_player_0;
    Status status;

    (void)context;
    switch (chance_state->phase) {
    case CHANCE_GAME_PHASE_COIN_CHANCE:
        if (action == CHANCE_GAME_ACTION_TAILS) {
            next_phase = CHANCE_GAME_PHASE_TERMINAL;
            next_utility = -1.0;
        } else if (action == CHANCE_GAME_ACTION_HEADS) {
            next_phase = CHANCE_GAME_PHASE_COIN_PLAYER_0;
        } else {
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;
    case CHANCE_GAME_PHASE_COIN_PLAYER_0:
        if (action == CHANCE_GAME_ACTION_STOP) {
            next_phase = CHANCE_GAME_PHASE_TERMINAL;
            next_utility = 0.0;
        } else if (action == CHANCE_GAME_ACTION_PLAY) {
            next_phase = CHANCE_GAME_PHASE_TERMINAL;
            next_utility = 2.0;
        } else {
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;
    case CHANCE_GAME_PHASE_DEEP:
        if (deep_actor_is_chance(chance_state)) {
            if (action != CHANCE_GAME_ACTION_DEEP_NEXT) {
                return CFR_STATUS_ILLEGAL_ACTION;
            }
            next_level += 1;
        } else if (action == CHANCE_GAME_ACTION_STOP) {
            next_phase = CHANCE_GAME_PHASE_TERMINAL;
            next_utility = 0.0;
        } else if (action == CHANCE_GAME_ACTION_PLAY) {
            next_phase = CHANCE_GAME_PHASE_TERMINAL;
            next_utility = 2.0;
        } else {
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;
    case CHANCE_GAME_PHASE_TERMINAL:
    default:
        return CFR_STATUS_ILLEGAL_ACTION;
    }

    status = save_state(chance_state);
    if (status != CFR_STATUS_SUCCESS) {
        return status;
    }
    chance_state->phase = next_phase;
    chance_state->deep_level = next_level;
    chance_state->terminal_utility_player_0 = next_utility;
    chance_state->last_action = action;
    return CFR_STATUS_SUCCESS;
}

static Status chance_undo_action(const void *context, GameState *state) {
    ChanceGameState *chance_state = as_chance(state);
    ChanceGameHistoryEntry entry;

    (void)context;
    if (chance_state->undo_failure_status != CFR_STATUS_SUCCESS) {
        return chance_state->undo_failure_status;
    }
    if (chance_state->history_count == 0) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    chance_state->history_count -= 1;
    entry = chance_state->history[chance_state->history_count];
    memset(&chance_state->history[chance_state->history_count], 0,
           sizeof(chance_state->history[chance_state->history_count]));
    chance_state->phase = entry.phase;
    chance_state->deep_level = entry.deep_level;
    chance_state->terminal_utility_player_0 =
        entry.terminal_utility_player_0;
    chance_state->last_action = entry.last_action;
    return CFR_STATUS_SUCCESS;
}

static Status chance_probability(const void *context, const GameState *state,
                                 Action action, Probability *result) {
    const ChanceGameState *chance_state = as_chance_const(state);

    (void)context;
    if (chance_state->probability_failure_enabled &&
        action == chance_state->probability_failure_action) {
        return chance_state->probability_failure_status;
    }
    if (chance_state->phase == CHANCE_GAME_PHASE_COIN_CHANCE) {
        if (action == CHANCE_GAME_ACTION_TAILS) {
            *result = chance_state->coin_probabilities[0];
            return CFR_STATUS_SUCCESS;
        }
        if (action == CHANCE_GAME_ACTION_HEADS) {
            *result = chance_state->coin_probabilities[1];
            return CFR_STATUS_SUCCESS;
        }
        return CFR_STATUS_ILLEGAL_ACTION;
    }
    if (deep_actor_is_chance(chance_state) &&
        action == CHANCE_GAME_ACTION_DEEP_NEXT) {
        *result = 1.0;
        return CFR_STATUS_SUCCESS;
    }
    return CFR_STATUS_INVALID_ARGUMENT;
}

static Status chance_information_set_key(const void *context,
                                         const GameState *state,
                                         InfoSetKey *result) {
    const ChanceGameState *chance_state = as_chance_const(state);

    (void)context;
    if (chance_state->phase == CHANCE_GAME_PHASE_COIN_PLAYER_0) {
        *result = 800;
        return CFR_STATUS_SUCCESS;
    }
    if (chance_state->phase == CHANCE_GAME_PHASE_DEEP &&
        !deep_actor_is_chance(chance_state)) {
        *result = 801;
        return CFR_STATUS_SUCCESS;
    }
    return CFR_STATUS_INVALID_ARGUMENT;
}
