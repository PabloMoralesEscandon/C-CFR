#include "fake_game.h"

static const InfoSetKey key_player0_decision = 0;
static const InfoSetKey key_player1_answer = 1;

static Status fake_game_is_terminal(const void *context, const GameState *state,
                                    bool *result);

static Status fake_game_terminal_utility(const void *context,
                                         const GameState *state, Player player,
                                         Utility *result);

static Status fake_game_current_actor(const void *context,
                                      const GameState *state, Actor *result);

static Status fake_game_legal_actions(const void *context,
                                      const GameState *state, Action *actions,
                                      size_t capacity, size_t *required_count);

static Status fake_game_apply_action(const void *context, GameState *state,
                                     Action action);

static Status fake_game_undo_action(const void *context, GameState *state);

static Status fake_game_chance_probability(const void *context,
                                           const GameState *state,
                                           Action action, Probability *result);

static Status fake_game_information_set_key(const void *context,
                                            const GameState *state,
                                            InfoSetKey *result);

static const FakeGameConfig FAKE_GAME_CONFIG = {.heads_probability = 0.5};

static const GameOperations FAKE_GAME_OPERATIONS = {
    .apply_action = fake_game_apply_action,
    .legal_actions = fake_game_legal_actions,
    .undo_action = fake_game_undo_action,
    .chance_probability = fake_game_chance_probability,
    .current_actor = fake_game_current_actor,
    .is_terminal = fake_game_is_terminal,
    .terminal_utility = fake_game_terminal_utility,
    .information_set_key = fake_game_information_set_key};

static const Game FAKE_GAME = {.context = &FAKE_GAME_CONFIG,
                               .strategic_player_count = 2,
                               .max_legal_actions = 2,
                               .operations = &FAKE_GAME_OPERATIONS};

Status fake_game_state_init(FakeGameState *state) {
    if (state == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    state->coin = FAKE_COIN_NOT_SET;
    state->history_count = 0;
    state->phase = FAKE_PHASE_CHANCE;
    return CFR_STATUS_SUCCESS;
}

const Game *fake_game_descriptor(void) { return &FAKE_GAME; }

GameState *fake_game_state_as_public(FakeGameState *fake_state) {
    return (GameState *)fake_state;
}

const GameState *
fake_game_state_as_public_const(const FakeGameState *fake_state) {
    return (const GameState *)fake_state;
}

static Status fake_game_is_terminal(const void *context, const GameState *state,
                                    bool *result) {
    (void)context;
    const FakeGameState *fake_state = (const FakeGameState *)state;
    switch (fake_state->phase) {
    case FAKE_PHASE_TERMINAL:
        *result = true;
        break;
    case FAKE_PHASE_CHANCE:
    case FAKE_PHASE_PLAYER_0:
    case FAKE_PHASE_PLAYER_1:
        *result = false;
        break;
    default:
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    return CFR_STATUS_SUCCESS;
}

static Status fake_game_terminal_utility(const void *context,
                                         const GameState *state, Player player,
                                         Utility *result) {
    (void)context;
    const FakeGameState *fake_state = (const FakeGameState *)state;
    if (fake_state->phase != FAKE_PHASE_TERMINAL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (fake_state->history_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    Utility player0_utility;
    switch (fake_state->history[fake_state->history_count - 1].applied_action) {
    case FAKE_ACTION_PASS:
        player0_utility = 0;
        break;
    case FAKE_ACTION_FOLD:
        player0_utility = 1.0;
        break;

    case FAKE_ACTION_CALL:
        if (fake_state->coin == FAKE_COIN_TAILS) {
            player0_utility = -2.0;
        } else if (fake_state->coin == FAKE_COIN_HEADS) {
            player0_utility = 2.0;
        } else
            return CFR_STATUS_INVALID_ARGUMENT;
        break;

    default:
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    if (player == CFR_PLAYER_0)
        *result = player0_utility;
    else if (player == CFR_PLAYER_1)
        *result = -player0_utility;
    else
        return CFR_STATUS_INVALID_ARGUMENT;

    return CFR_STATUS_SUCCESS;
}

static Status fake_game_current_actor(const void *context,
                                      const GameState *state, Actor *result) {
    (void)context;
    const FakeGameState *fake_state = (const FakeGameState *)state;
    switch (fake_state->phase) {
    case FAKE_PHASE_CHANCE:
        result->kind = CFR_ACTOR_CHANCE;
        break;
    case FAKE_PHASE_TERMINAL:
        return CFR_STATUS_INVALID_ARGUMENT;
    case FAKE_PHASE_PLAYER_0:
        result->kind = CFR_ACTOR_PLAYER;
        result->player = CFR_PLAYER_0;
        break;
    case FAKE_PHASE_PLAYER_1:
        result->kind = CFR_ACTOR_PLAYER;
        result->player = CFR_PLAYER_1;
        break;
    default:
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    return CFR_STATUS_SUCCESS;
}

static Status fake_game_legal_actions(const void *context,
                                      const GameState *state, Action *actions,
                                      size_t capacity, size_t *required_count) {
    (void)context;
    const FakeGameState *fake_state = (const FakeGameState *)state;
    switch (fake_state->phase) {
    case FAKE_PHASE_CHANCE:
        *required_count = 2;
        if (*required_count > capacity)
            return CFR_STATUS_BUFFER_TOO_SMALL;
        actions[0] = FAKE_ACTION_HEADS;
        actions[1] = FAKE_ACTION_TAILS;
        break;
    case FAKE_PHASE_TERMINAL:
        return CFR_STATUS_INVALID_ARGUMENT;
    case FAKE_PHASE_PLAYER_0:
        *required_count = 2;
        if (*required_count > capacity)
            return CFR_STATUS_BUFFER_TOO_SMALL;
        actions[0] = FAKE_ACTION_PASS;
        actions[1] = FAKE_ACTION_BET;
        break;
    case FAKE_PHASE_PLAYER_1:
        *required_count = 2;
        if (*required_count > capacity)
            return CFR_STATUS_BUFFER_TOO_SMALL;
        actions[0] = FAKE_ACTION_FOLD;
        actions[1] = FAKE_ACTION_CALL;
        break;
    default:
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    return CFR_STATUS_SUCCESS;
}

static Status fake_game_apply_action(const void *context, GameState *state,
                                     Action action) {
    (void)context;
    FakeGameState *fake_state = (FakeGameState *)state;
    FakeCoin next_coin = fake_state->coin;
    FakePhase next_phase = fake_state->phase;
    switch (fake_state->phase) {
    case FAKE_PHASE_CHANCE:
        next_phase = FAKE_PHASE_PLAYER_0;
        switch (action) {
        case FAKE_ACTION_HEADS:
            next_coin = FAKE_COIN_HEADS;
            break;
        case FAKE_ACTION_TAILS:
            next_coin = FAKE_COIN_TAILS;
            break;
        default:
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;
    case FAKE_PHASE_TERMINAL:
        return CFR_STATUS_ILLEGAL_ACTION;
    case FAKE_PHASE_PLAYER_0:
        switch (action) {
        case FAKE_ACTION_PASS:
            next_phase = FAKE_PHASE_TERMINAL;
            break;
        case FAKE_ACTION_BET:
            next_phase = FAKE_PHASE_PLAYER_1;
            break;
        default:
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;
    case FAKE_PHASE_PLAYER_1:
        next_phase = FAKE_PHASE_TERMINAL;
        switch (action) {
        case FAKE_ACTION_FOLD:
            break;
        case FAKE_ACTION_CALL:
            break;
        default:
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;
    default:
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    if (fake_state->history_count >=
        sizeof(fake_state->history) / sizeof(fake_state->history[0]))
        return CFR_STATUS_BUFFER_TOO_SMALL;

    fake_state->history[fake_state->history_count].applied_action = action;
    fake_state->history[fake_state->history_count].previous_phase =
        fake_state->phase;
    fake_state->history[fake_state->history_count].previous_coin =
        fake_state->coin;
    fake_state->history_count += 1;
    fake_state->phase = next_phase;
    fake_state->coin = next_coin;
    return CFR_STATUS_SUCCESS;
}

static Status fake_game_undo_action(const void *context, GameState *state) {
    (void)context;
    FakeGameState *fake_state = (FakeGameState *)state;
    if (fake_state->history_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    fake_state->history_count -= 1;
    fake_state->coin =
        fake_state->history[fake_state->history_count].previous_coin;
    fake_state->phase =
        fake_state->history[fake_state->history_count].previous_phase;
    return CFR_STATUS_SUCCESS;
}

static Status fake_game_chance_probability(const void *context,
                                           const GameState *state,
                                           Action action, Probability *result) {
    if (context == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    const FakeGameState *fake_state = (const FakeGameState *)state;
    const FakeGameConfig *fake_config = (const FakeGameConfig *)context;
    if (fake_state->phase != FAKE_PHASE_CHANCE)
        return CFR_STATUS_INVALID_ARGUMENT;
    switch (action) {
    case FAKE_ACTION_HEADS:
        *result = fake_config->heads_probability;
        break;
    case FAKE_ACTION_TAILS:
        *result = 1 - fake_config->heads_probability;
        break;
    default:
        return CFR_STATUS_ILLEGAL_ACTION;
    }
    return CFR_STATUS_SUCCESS;
}

static Status fake_game_information_set_key(const void *context,
                                            const GameState *state,
                                            InfoSetKey *result) {
    (void)context;
    const FakeGameState *fake_state = (const FakeGameState *)state;
    switch (fake_state->phase) {
    case FAKE_PHASE_PLAYER_0:
        *result = key_player0_decision;
        break;
    case FAKE_PHASE_PLAYER_1:
        *result = key_player1_answer;
        break;
    default:
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    return CFR_STATUS_SUCCESS;
}
