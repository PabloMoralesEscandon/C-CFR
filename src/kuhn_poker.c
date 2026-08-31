#include <stdbool.h>

#include "cfr/kuhn_poker.h"

static Status kuhn_poker_is_terminal(const void *context,
                                     const GameState *state, bool *result);

static Status kuhn_poker_terminal_utility(const void *context,
                                          const GameState *state, Player player,
                                          Utility *result);

static Status kuhn_poker_current_actor(const void *context,
                                       const GameState *state, Actor *result);

static Status kuhn_poker_legal_actions(const void *context,
                                       const GameState *state, Action *actions,
                                       size_t capacity, size_t *required_count);

static Status kuhn_poker_apply_action(const void *context, GameState *state,
                                      Action action);

static Status kuhn_poker_undo_action(const void *context, GameState *state);

static Status kuhn_poker_chance_probability(const void *context,
                                            const GameState *state,
                                            Action action, Probability *result);

static Status kuhn_poker_information_set_key(const void *context,
                                             const GameState *state,
                                             InfoSetKey *result);

static Status kuhn_poker_validate_state(const void *context,
                                        const GameState *state);

static Status kuhn_poker_chance_outcomes(const void *context,
                                         const GameState *state,
                                         Action *actions,
                                         Probability *probabilities,
                                         size_t capacity,
                                         size_t *required_count);

static Status kuhn_poker_trusted_is_terminal(const void *context,
                                             const GameState *state,
                                             bool *result);
static Status kuhn_poker_trusted_terminal_utility(
    const void *context, const GameState *state, Player player,
    Utility *result);
static Status kuhn_poker_trusted_current_actor(const void *context,
                                               const GameState *state,
                                               Actor *result);
static Status kuhn_poker_trusted_legal_actions(
    const void *context, const GameState *state, Action *actions,
    size_t capacity, size_t *required_count);
static Status kuhn_poker_trusted_apply_action(const void *context,
                                              GameState *state,
                                              Action action);
static Status kuhn_poker_trusted_undo_action(const void *context,
                                             GameState *state);
static Status kuhn_poker_trusted_chance_probability(
    const void *context, const GameState *state, Action action,
    Probability *result);
static Status kuhn_poker_trusted_chance_outcomes(
    const void *context, const GameState *state, Action *actions,
    Probability *probabilities, size_t capacity, size_t *required_count);
static Status kuhn_poker_trusted_information_set_key(
    const void *context, const GameState *state, InfoSetKey *result);

static const GameOperations KP_GAME_OPERATIONS = {
    .is_terminal = kuhn_poker_is_terminal,
    .terminal_utility = kuhn_poker_terminal_utility,
    .current_actor = kuhn_poker_current_actor,
    .legal_actions = kuhn_poker_legal_actions,
    .apply_action = kuhn_poker_apply_action,
    .undo_action = kuhn_poker_undo_action,
    .chance_probability = kuhn_poker_chance_probability,
    .information_set_key = kuhn_poker_information_set_key,
    .validate_state = kuhn_poker_validate_state,
    .chance_outcomes = kuhn_poker_chance_outcomes};

static const GameOperations KP_TRUSTED_GAME_OPERATIONS = {
    .is_terminal = kuhn_poker_trusted_is_terminal,
    .terminal_utility = kuhn_poker_trusted_terminal_utility,
    .current_actor = kuhn_poker_trusted_current_actor,
    .legal_actions = kuhn_poker_trusted_legal_actions,
    .apply_action = kuhn_poker_trusted_apply_action,
    .undo_action = kuhn_poker_trusted_undo_action,
    .chance_probability = kuhn_poker_trusted_chance_probability,
    .information_set_key = kuhn_poker_trusted_information_set_key,
    .chance_outcomes = kuhn_poker_trusted_chance_outcomes};

static const Game KP_GAME = {.operations = &KP_GAME_OPERATIONS,
                             .context = NULL,
                             .strategic_player_count = 2,
                             .max_legal_actions =
                                 CFR_KUHN_POKER_MAX_POSSIBLE_ACTIONS,
                             .strategy_schema_id = "cfr.kuhn-poker/v1",
                             .trusted_operations =
                                 &KP_TRUSTED_GAME_OPERATIONS};

Status cfr_kuhn_poker_state_init(KuhnPokerState *state) {
    if (state == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    for (size_t i = 0; i < CFR_KUHN_POKER_NUMBER_OF_PLAYERS; i++)
        state->cards[i] = CFR_KUHN_POKER_CARD_NOT_DEALT;
    state->phase = CFR_KUHN_POKER_PHASE_CHANCE;
    state->public_action_count = 0;
    state->undo_count = 0;
    for (size_t i = 0; i < CFR_KUHN_POKER_PUBLIC_HISTORY_CAPACITY; i++)
        state->public_actions[i] = CFR_KUHN_POKER_ACTION_NONE;
    KuhnPokerUndoEntry empty_entry = {
        .previous_phase = CFR_KUHN_POKER_PHASE_CHANCE,
        .previous_public_action_count = 0,
        .applied_action = CFR_KUHN_POKER_ACTION_NONE};
    for (size_t i = 0; i < CFR_KUHN_POKER_NUMBER_OF_PLAYERS; i++)
        empty_entry.previous_cards[i] = CFR_KUHN_POKER_CARD_NOT_DEALT;

    for (size_t i = 0; i < CFR_KUHN_POKER_UNDO_HISTORY_CAPACITY; i++)
        state->undo_history[i] = empty_entry;
    return CFR_STATUS_SUCCESS;
}

static KuhnPokerState *as_kuhn(GameState *state) {
    return (KuhnPokerState *)state;
}

static const KuhnPokerState *as_kuhn_const(const GameState *state) {
    return (const KuhnPokerState *)state;
}

static Status decode_deal(KuhnPokerAction action, KuhnPokerCard *cards_out);

static bool phase_is_known(KuhnPokerPhase phase) {
    switch (phase) {
    case CFR_KUHN_POKER_PHASE_CHANCE:
    case CFR_KUHN_POKER_PHASE_PLAYER_0_OPEN:
    case CFR_KUHN_POKER_PHASE_PLAYER_1_AFTER_CHECK:
    case CFR_KUHN_POKER_PHASE_PLAYER_1_FACING_OPEN_BET:
    case CFR_KUHN_POKER_PHASE_PLAYER_0_FACING_CHECK_BET:
    case CFR_KUHN_POKER_PHASE_TERMINAL:
        return true;
    default:
        return false;
    }
}

static bool card_is_real(KuhnPokerCard card) {
    switch (card) {
    case CFR_KUHN_POKER_CARD_JACK:
    case CFR_KUHN_POKER_CARD_QUEEN:
    case CFR_KUHN_POKER_CARD_KING:
        return true;
    case CFR_KUHN_POKER_CARD_NOT_DEALT:
    default:
        return false;
    }
}

static bool undo_entry_is_empty(const KuhnPokerUndoEntry *entry) {
    if (entry == NULL)
        return false;

    return entry->previous_phase == CFR_KUHN_POKER_PHASE_CHANCE &&
           entry->previous_cards[0] == CFR_KUHN_POKER_CARD_NOT_DEALT &&
           entry->previous_cards[1] == CFR_KUHN_POKER_CARD_NOT_DEALT &&
           entry->previous_public_action_count == 0 &&
           entry->applied_action == CFR_KUHN_POKER_ACTION_NONE;
}

static Status next_public_phase(KuhnPokerPhase current_phase, Action action,
                                KuhnPokerPhase *next_phase) {
    KuhnPokerPhase result;

    if (next_phase == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    switch (current_phase) {
    case CFR_KUHN_POKER_PHASE_PLAYER_0_OPEN:
        if (action == CFR_KUHN_POKER_ACTION_CHECK) {
            result = CFR_KUHN_POKER_PHASE_PLAYER_1_AFTER_CHECK;
        } else if (action == CFR_KUHN_POKER_ACTION_BET) {
            result = CFR_KUHN_POKER_PHASE_PLAYER_1_FACING_OPEN_BET;
        } else {
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;

    case CFR_KUHN_POKER_PHASE_PLAYER_1_AFTER_CHECK:
        if (action == CFR_KUHN_POKER_ACTION_CHECK) {
            result = CFR_KUHN_POKER_PHASE_TERMINAL;
        } else if (action == CFR_KUHN_POKER_ACTION_BET) {
            result = CFR_KUHN_POKER_PHASE_PLAYER_0_FACING_CHECK_BET;
        } else {
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;

    case CFR_KUHN_POKER_PHASE_PLAYER_1_FACING_OPEN_BET:
    case CFR_KUHN_POKER_PHASE_PLAYER_0_FACING_CHECK_BET:
        if (action == CFR_KUHN_POKER_ACTION_FOLD ||
            action == CFR_KUHN_POKER_ACTION_CALL) {
            result = CFR_KUHN_POKER_PHASE_TERMINAL;
        } else {
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;

    case CFR_KUHN_POKER_PHASE_CHANCE:
    case CFR_KUHN_POKER_PHASE_TERMINAL:
    default:
        return CFR_STATUS_ILLEGAL_ACTION;
    }

    *next_phase = result;
    return CFR_STATUS_SUCCESS;
}

static Status validate_state(const KuhnPokerState *state) {
    KuhnPokerCard dealt_cards[CFR_KUHN_POKER_NUMBER_OF_PLAYERS];
    KuhnPokerPhase expected_phase;
    Status status;
    size_t index;

    if (state == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    /*
     * Check the bounds first so later loops cannot access outside the arrays.
     */
    if (state->public_action_count > CFR_KUHN_POKER_PUBLIC_HISTORY_CAPACITY ||
        state->undo_count > CFR_KUHN_POKER_UNDO_HISTORY_CAPACITY) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    if (!phase_is_known(state->phase))
        return CFR_STATUS_INVALID_ARGUMENT;

    /* Unused positions in the public history must be clear. */
    for (index = state->public_action_count;
         index < CFR_KUHN_POKER_PUBLIC_HISTORY_CAPACITY; index++) {
        if (state->public_actions[index] != CFR_KUHN_POKER_ACTION_NONE)
            return CFR_STATUS_INVALID_ARGUMENT;
    }

    /* Unused positions in the undo history must also be empty. */
    for (index = state->undo_count;
         index < CFR_KUHN_POKER_UNDO_HISTORY_CAPACITY; index++) {
        if (!undo_entry_is_empty(&state->undo_history[index])) {
            return CFR_STATUS_INVALID_ARGUMENT;
        }
    }

    /* The chance phase can represent only the exact root state. */
    if (state->phase == CFR_KUHN_POKER_PHASE_CHANCE) {
        if (state->cards[0] != CFR_KUHN_POKER_CARD_NOT_DEALT ||
            state->cards[1] != CFR_KUHN_POKER_CARD_NOT_DEALT ||
            state->public_action_count != 0 || state->undo_count != 0) {
            return CFR_STATUS_INVALID_ARGUMENT;
        }

        return CFR_STATUS_SUCCESS;
    }

    /* After the deal, there must be two different real cards. */
    if (!card_is_real(state->cards[0]) || !card_is_real(state->cards[1]) ||
        state->cards[0] == state->cards[1]) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    /* There is one undo entry for the deal and one per public action. */
    if (state->undo_count != state->public_action_count + 1)
        return CFR_STATUS_INVALID_ARGUMENT;

    /* The first entry must describe the root before the deal. */
    const KuhnPokerUndoEntry *deal_entry = &state->undo_history[0];

    if (deal_entry->previous_phase != CFR_KUHN_POKER_PHASE_CHANCE ||
        deal_entry->previous_cards[0] != CFR_KUHN_POKER_CARD_NOT_DEALT ||
        deal_entry->previous_cards[1] != CFR_KUHN_POKER_CARD_NOT_DEALT ||
        deal_entry->previous_public_action_count != 0) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    status = decode_deal(deal_entry->applied_action, dealt_cards);

    if (status != CFR_STATUS_SUCCESS)
        return CFR_STATUS_INVALID_ARGUMENT;

    for (size_t i = 0; i < CFR_KUHN_POKER_NUMBER_OF_PLAYERS; i++)
        if (dealt_cards[i] != state->cards[i])
            return CFR_STATUS_INVALID_ARGUMENT;

    /* Player zero always acts first after a valid deal. */
    expected_phase = CFR_KUHN_POKER_PHASE_PLAYER_0_OPEN;

    /*
     * Replay the public history. Each undo entry must exactly describe the
     * state that existed before its action.
     */
    for (index = 0; index < state->public_action_count; index++) {
        const KuhnPokerUndoEntry *entry = &state->undo_history[index + 1];

        if (entry->previous_phase != expected_phase ||
            entry->previous_public_action_count != index ||
            entry->applied_action != state->public_actions[index]) {
            return CFR_STATUS_INVALID_ARGUMENT;
        }
        for (size_t i = 0; i < CFR_KUHN_POKER_NUMBER_OF_PLAYERS; i++)
            if (entry->previous_cards[i] != state->cards[i])
                return CFR_STATUS_INVALID_ARGUMENT;

        status = next_public_phase(expected_phase, state->public_actions[index],
                                   &expected_phase);

        if (status != CFR_STATUS_SUCCESS)
            return CFR_STATUS_INVALID_ARGUMENT;
    }

    /* The phase produced by replaying the history must match the stored phase. */
    if (expected_phase != state->phase)
        return CFR_STATUS_INVALID_ARGUMENT;

    return CFR_STATUS_SUCCESS;
}

static Status decode_deal(KuhnPokerAction action, KuhnPokerCard *cards_out) {
    if (cards_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    switch (action) {

    case CFR_KUHN_POKER_ACTION_JQ:
        cards_out[0] = CFR_KUHN_POKER_CARD_JACK;
        cards_out[1] = CFR_KUHN_POKER_CARD_QUEEN;
        break;

    case CFR_KUHN_POKER_ACTION_JK:
        cards_out[0] = CFR_KUHN_POKER_CARD_JACK;
        cards_out[1] = CFR_KUHN_POKER_CARD_KING;
        break;

    case CFR_KUHN_POKER_ACTION_QJ:
        cards_out[0] = CFR_KUHN_POKER_CARD_QUEEN;
        cards_out[1] = CFR_KUHN_POKER_CARD_JACK;
        break;

    case CFR_KUHN_POKER_ACTION_QK:
        cards_out[0] = CFR_KUHN_POKER_CARD_QUEEN;
        cards_out[1] = CFR_KUHN_POKER_CARD_KING;
        break;

    case CFR_KUHN_POKER_ACTION_KJ:
        cards_out[0] = CFR_KUHN_POKER_CARD_KING;
        cards_out[1] = CFR_KUHN_POKER_CARD_JACK;
        break;

    case CFR_KUHN_POKER_ACTION_KQ:
        cards_out[0] = CFR_KUHN_POKER_CARD_KING;
        cards_out[1] = CFR_KUHN_POKER_CARD_QUEEN;
        break;

    default:
        return CFR_STATUS_ILLEGAL_ACTION;
    }
    return CFR_STATUS_SUCCESS;
}

static Status save_undo(KuhnPokerState *state, KuhnPokerAction applied_action) {
    if (state == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (!(state->undo_count < CFR_KUHN_POKER_UNDO_HISTORY_CAPACITY))
        return CFR_STATUS_BUFFER_TOO_SMALL;

    KuhnPokerUndoEntry *new_entry = &state->undo_history[state->undo_count];
    new_entry->applied_action = applied_action;
    new_entry->previous_phase = state->phase;
    new_entry->previous_public_action_count = state->public_action_count;
    for (size_t i = 0; i < CFR_KUHN_POKER_NUMBER_OF_PLAYERS; i++)
        new_entry->previous_cards[i] = state->cards[i];
    state->undo_count += 1;
    return CFR_STATUS_SUCCESS;
}

static bool player_zero_wins_showdown(const KuhnPokerState *state) {
    return (state->cards[0] > state->cards[1]);
}

const Game *cfr_kuhn_poker_descriptor(void) { return &KP_GAME; }

GameState *
cfr_kuhn_poker_state_as_game_state(KuhnPokerState *kuhn_poker_state) {
    return (GameState *)kuhn_poker_state;
}

const GameState *cfr_kuhn_poker_state_as_game_state_const(
    const KuhnPokerState *kuhn_poker_state) {
    return (const GameState *)kuhn_poker_state;
}

static Status kuhn_poker_validate_state(const void *context,
                                        const GameState *state) {
    (void)context;
    return validate_state(as_kuhn_const(state));
}

static Status kuhn_poker_is_terminal(const void *context,
                                     const GameState *state, bool *result) {
    const KuhnPokerState *kuhn_poker_state = as_kuhn_const(state);
    Status status = validate_state(kuhn_poker_state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    return kuhn_poker_trusted_is_terminal(context, state, result);
}

static Status kuhn_poker_trusted_is_terminal(const void *context,
                                             const GameState *state,
                                             bool *result) {
    (void)context;
    const KuhnPokerState *kuhn_poker_state = as_kuhn_const(state);
    switch (kuhn_poker_state->phase) {
    case CFR_KUHN_POKER_PHASE_TERMINAL:
        *result = true;
        break;
    default:
        *result = false;
    }
    return CFR_STATUS_SUCCESS;
}

static Status kuhn_poker_terminal_utility(const void *context,
                                          const GameState *state, Player player,
                                          Utility *result) {
    const KuhnPokerState *kuhn_poker_state = as_kuhn_const(state);
    Status status = validate_state(kuhn_poker_state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    return kuhn_poker_trusted_terminal_utility(context, state, player, result);
}

static Status kuhn_poker_trusted_terminal_utility(
    const void *context, const GameState *state, Player player,
    Utility *result) {
    (void)context;
    const KuhnPokerState *kuhn_poker_state = as_kuhn_const(state);
    if (kuhn_poker_state->phase != CFR_KUHN_POKER_PHASE_TERMINAL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (!(player == CFR_PLAYER_0 || player == CFR_PLAYER_1))
        return CFR_STATUS_INVALID_ARGUMENT;
    Utility player0_utility;
    if (kuhn_poker_state->public_action_count == 2) {
        if (kuhn_poker_state->public_actions[0] == CFR_KUHN_POKER_ACTION_BET &&
            kuhn_poker_state->public_actions[1] == CFR_KUHN_POKER_ACTION_FOLD)
            player0_utility = 1;
        else if (kuhn_poker_state->public_actions[0] ==
                     CFR_KUHN_POKER_ACTION_BET &&
                 kuhn_poker_state->public_actions[1] ==
                     CFR_KUHN_POKER_ACTION_CALL) {
            if (player_zero_wins_showdown(kuhn_poker_state))
                player0_utility = 2;
            else
                player0_utility = -2;
        } else if (kuhn_poker_state->public_actions[0] ==
                       CFR_KUHN_POKER_ACTION_CHECK &&
                   kuhn_poker_state->public_actions[1] ==
                       CFR_KUHN_POKER_ACTION_CHECK) {
            if (player_zero_wins_showdown(kuhn_poker_state))
                player0_utility = 1;
            else
                player0_utility = -1;
        } else
            return CFR_STATUS_INVALID_ARGUMENT;
    } else if (kuhn_poker_state->public_action_count == 3) {
        if (kuhn_poker_state->public_actions[0] ==
                CFR_KUHN_POKER_ACTION_CHECK &&
            kuhn_poker_state->public_actions[1] == CFR_KUHN_POKER_ACTION_BET &&
            kuhn_poker_state->public_actions[2] == CFR_KUHN_POKER_ACTION_FOLD)
            player0_utility = -1;
        else if (kuhn_poker_state->public_actions[0] ==
                     CFR_KUHN_POKER_ACTION_CHECK &&
                 kuhn_poker_state->public_actions[1] ==
                     CFR_KUHN_POKER_ACTION_BET &&
                 kuhn_poker_state->public_actions[2] ==
                     CFR_KUHN_POKER_ACTION_CALL) {
            if (player_zero_wins_showdown(kuhn_poker_state))
                player0_utility = 2;
            else
                player0_utility = -2;
        } else
            return CFR_STATUS_INVALID_ARGUMENT;
    } else
        return CFR_STATUS_INVALID_ARGUMENT;
    if (player == CFR_PLAYER_0)
        *result = player0_utility;
    else
        *result = -player0_utility;
    return CFR_STATUS_SUCCESS;
}

static Status kuhn_poker_current_actor(const void *context,
                                       const GameState *state, Actor *result) {
    const KuhnPokerState *kuhn_poker_state = as_kuhn_const(state);
    Status status = validate_state(kuhn_poker_state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    return kuhn_poker_trusted_current_actor(context, state, result);
}

static Status kuhn_poker_trusted_current_actor(const void *context,
                                               const GameState *state,
                                               Actor *result) {
    (void)context;
    const KuhnPokerState *kuhn_poker_state = as_kuhn_const(state);
    switch (kuhn_poker_state->phase) {
    case CFR_KUHN_POKER_PHASE_CHANCE:
        result->kind = CFR_ACTOR_CHANCE;
        break;
    case CFR_KUHN_POKER_PHASE_TERMINAL:
        return CFR_STATUS_INVALID_ARGUMENT;
    case CFR_KUHN_POKER_PHASE_PLAYER_0_OPEN:
    case CFR_KUHN_POKER_PHASE_PLAYER_0_FACING_CHECK_BET:
        result->kind = CFR_ACTOR_PLAYER;
        result->player = CFR_PLAYER_0;
        break;
    case CFR_KUHN_POKER_PHASE_PLAYER_1_AFTER_CHECK:
    case CFR_KUHN_POKER_PHASE_PLAYER_1_FACING_OPEN_BET:
        result->kind = CFR_ACTOR_PLAYER;
        result->player = CFR_PLAYER_1;
        break;
    default:
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    return CFR_STATUS_SUCCESS;
}

static Status kuhn_poker_legal_actions(const void *context,
                                       const GameState *state, Action *actions,
                                       size_t capacity,
                                       size_t *required_count) {
    const KuhnPokerState *kuhn_poker_state = as_kuhn_const(state);
    Status status = validate_state(kuhn_poker_state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    return kuhn_poker_trusted_legal_actions(context, state, actions, capacity,
                                            required_count);
}

static Status kuhn_poker_trusted_legal_actions(
    const void *context, const GameState *state, Action *actions,
    size_t capacity, size_t *required_count) {
    (void)context;
    const KuhnPokerState *kuhn_poker_state = as_kuhn_const(state);
    switch (kuhn_poker_state->phase) {
    case CFR_KUHN_POKER_PHASE_CHANCE:
        *required_count = 6;
        if (*required_count > capacity)
            return CFR_STATUS_BUFFER_TOO_SMALL;
        actions[0] = CFR_KUHN_POKER_ACTION_JQ;
        actions[1] = CFR_KUHN_POKER_ACTION_JK;
        actions[2] = CFR_KUHN_POKER_ACTION_QJ;
        actions[3] = CFR_KUHN_POKER_ACTION_QK;
        actions[4] = CFR_KUHN_POKER_ACTION_KJ;
        actions[5] = CFR_KUHN_POKER_ACTION_KQ;
        break;
    case CFR_KUHN_POKER_PHASE_TERMINAL:
        return CFR_STATUS_INVALID_ARGUMENT;
    case CFR_KUHN_POKER_PHASE_PLAYER_0_OPEN:
        *required_count = 2;
        if (*required_count > capacity)
            return CFR_STATUS_BUFFER_TOO_SMALL;
        actions[0] = CFR_KUHN_POKER_ACTION_CHECK;
        actions[1] = CFR_KUHN_POKER_ACTION_BET;
        break;
    case CFR_KUHN_POKER_PHASE_PLAYER_1_AFTER_CHECK:
        *required_count = 2;
        if (*required_count > capacity)
            return CFR_STATUS_BUFFER_TOO_SMALL;
        actions[0] = CFR_KUHN_POKER_ACTION_CHECK;
        actions[1] = CFR_KUHN_POKER_ACTION_BET;
        break;
    case CFR_KUHN_POKER_PHASE_PLAYER_1_FACING_OPEN_BET:
        *required_count = 2;
        if (*required_count > capacity)
            return CFR_STATUS_BUFFER_TOO_SMALL;
        actions[0] = CFR_KUHN_POKER_ACTION_FOLD;
        actions[1] = CFR_KUHN_POKER_ACTION_CALL;
        break;
    case CFR_KUHN_POKER_PHASE_PLAYER_0_FACING_CHECK_BET:
        *required_count = 2;
        if (*required_count > capacity)
            return CFR_STATUS_BUFFER_TOO_SMALL;
        actions[0] = CFR_KUHN_POKER_ACTION_FOLD;
        actions[1] = CFR_KUHN_POKER_ACTION_CALL;
        break;
    default:
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    return CFR_STATUS_SUCCESS;
}

static Status kuhn_poker_apply_action(const void *context, GameState *state,
                                      Action action) {
    KuhnPokerState *kuhn_poker_state = as_kuhn(state);
    Status status = validate_state(kuhn_poker_state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    return kuhn_poker_trusted_apply_action(context, state, action);
}

static Status kuhn_poker_trusted_apply_action(const void *context,
                                              GameState *state,
                                              Action action) {
    (void)context;
    KuhnPokerState *kuhn_poker_state = as_kuhn(state);
    Status status;
    KuhnPokerCard next_cards[2];
    for (size_t i = 0; i < CFR_KUHN_POKER_NUMBER_OF_PLAYERS; i++)
        next_cards[i] = kuhn_poker_state->cards[i];
    KuhnPokerPhase next_phase = kuhn_poker_state->phase;
    bool public_action = true;
    switch (kuhn_poker_state->phase) {
    case CFR_KUHN_POKER_PHASE_CHANCE:
        next_phase = CFR_KUHN_POKER_PHASE_PLAYER_0_OPEN;
        status = decode_deal(static_cast<KuhnPokerAction>(action), next_cards);
        public_action = false;
        if (status != CFR_STATUS_SUCCESS)
            return status;
        break;
    case CFR_KUHN_POKER_PHASE_TERMINAL:
        return CFR_STATUS_ILLEGAL_ACTION;
    case CFR_KUHN_POKER_PHASE_PLAYER_0_OPEN:
        switch (action) {
        case CFR_KUHN_POKER_ACTION_CHECK:
            next_phase = CFR_KUHN_POKER_PHASE_PLAYER_1_AFTER_CHECK;
            break;
        case CFR_KUHN_POKER_ACTION_BET:
            next_phase = CFR_KUHN_POKER_PHASE_PLAYER_1_FACING_OPEN_BET;
            break;
        default:
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;
    case CFR_KUHN_POKER_PHASE_PLAYER_1_AFTER_CHECK:
        switch (action) {
        case CFR_KUHN_POKER_ACTION_BET:
            next_phase = CFR_KUHN_POKER_PHASE_PLAYER_0_FACING_CHECK_BET;
            break;
        case CFR_KUHN_POKER_ACTION_CHECK:
            next_phase = CFR_KUHN_POKER_PHASE_TERMINAL;
            break;
        default:
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;
    case CFR_KUHN_POKER_PHASE_PLAYER_1_FACING_OPEN_BET:
        next_phase = CFR_KUHN_POKER_PHASE_TERMINAL;
        switch (action) {
        case CFR_KUHN_POKER_ACTION_CALL:
        case CFR_KUHN_POKER_ACTION_FOLD:
            break;
        default:
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;
    case CFR_KUHN_POKER_PHASE_PLAYER_0_FACING_CHECK_BET:
        next_phase = CFR_KUHN_POKER_PHASE_TERMINAL;
        switch (action) {
        case CFR_KUHN_POKER_ACTION_CALL:
        case CFR_KUHN_POKER_ACTION_FOLD:
            break;
        default:
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        break;
    default:
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    if (public_action && kuhn_poker_state->public_action_count >=
                             CFR_KUHN_POKER_PUBLIC_HISTORY_CAPACITY)
        return CFR_STATUS_BUFFER_TOO_SMALL;
    status = save_undo(kuhn_poker_state,
                       static_cast<KuhnPokerAction>(action));
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (public_action) {
        kuhn_poker_state
            ->public_actions[kuhn_poker_state->public_action_count] =
            static_cast<KuhnPokerAction>(action);
        kuhn_poker_state->public_action_count += 1;
    }
    kuhn_poker_state->phase = next_phase;
    for (size_t i = 0; i < CFR_KUHN_POKER_NUMBER_OF_PLAYERS; i++)
        kuhn_poker_state->cards[i] = next_cards[i];
    return CFR_STATUS_SUCCESS;
}

static Status kuhn_poker_undo_action(const void *context, GameState *state) {
    KuhnPokerState *kuhn_poker_state = as_kuhn(state);
    Status status = validate_state(kuhn_poker_state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    return kuhn_poker_trusted_undo_action(context, state);
}

static Status kuhn_poker_trusted_undo_action(const void *context,
                                             GameState *state) {
    (void)context;
    KuhnPokerState *kuhn_poker_state = as_kuhn(state);
    if (kuhn_poker_state->undo_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;

    kuhn_poker_state->undo_count -= 1;
    if (kuhn_poker_state->undo_history[kuhn_poker_state->undo_count]
            .previous_public_action_count <
        kuhn_poker_state->public_action_count) {
        kuhn_poker_state->public_action_count -= 1;
        kuhn_poker_state
            ->public_actions[kuhn_poker_state->public_action_count] =
            CFR_KUHN_POKER_ACTION_NONE;
    }
    for (size_t i = 0; i < CFR_KUHN_POKER_NUMBER_OF_PLAYERS; i++) {
        kuhn_poker_state->cards[i] =
            kuhn_poker_state->undo_history[kuhn_poker_state->undo_count]
                .previous_cards[i];
        kuhn_poker_state->undo_history[kuhn_poker_state->undo_count]
            .previous_cards[i] = CFR_KUHN_POKER_CARD_NOT_DEALT;
    }
    kuhn_poker_state->phase =
        kuhn_poker_state->undo_history[kuhn_poker_state->undo_count]
            .previous_phase;
    kuhn_poker_state->undo_history[kuhn_poker_state->undo_count]
        .previous_phase = CFR_KUHN_POKER_PHASE_CHANCE;
    kuhn_poker_state->undo_history[kuhn_poker_state->undo_count]
        .previous_public_action_count = 0;
    kuhn_poker_state->undo_history[kuhn_poker_state->undo_count]
        .applied_action = CFR_KUHN_POKER_ACTION_NONE;
    return CFR_STATUS_SUCCESS;
}

static Status kuhn_poker_chance_probability(const void *context,
                                            const GameState *state,
                                            Action action,
                                            Probability *result) {
    const KuhnPokerState *kuhn_poker_state = as_kuhn_const(state);
    Status status = validate_state(kuhn_poker_state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    return kuhn_poker_trusted_chance_probability(context, state, action,
                                                 result);
}

static Status kuhn_poker_trusted_chance_probability(
    const void *context, const GameState *state, Action action,
    Probability *result) {
    (void)context;
    const KuhnPokerState *kuhn_poker_state = as_kuhn_const(state);
    if (kuhn_poker_state->phase != CFR_KUHN_POKER_PHASE_CHANCE)
        return CFR_STATUS_INVALID_ARGUMENT;
    switch (action) {
    case CFR_KUHN_POKER_ACTION_JQ:
    case CFR_KUHN_POKER_ACTION_JK:
    case CFR_KUHN_POKER_ACTION_QJ:
    case CFR_KUHN_POKER_ACTION_QK:
    case CFR_KUHN_POKER_ACTION_KJ:
    case CFR_KUHN_POKER_ACTION_KQ:
        *result = 1.0 / 6.0;
        break;
    default:
        return CFR_STATUS_ILLEGAL_ACTION;
    }
    return CFR_STATUS_SUCCESS;
}

static Status kuhn_poker_chance_outcomes(const void *context,
                                         const GameState *state,
                                         Action *actions,
                                         Probability *probabilities,
                                         size_t capacity,
                                         size_t *required_count) {
    if (actions == NULL || probabilities == NULL || required_count == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    const Status status = validate_state(as_kuhn_const(state));

    if (status != CFR_STATUS_SUCCESS)
        return status;
    return kuhn_poker_trusted_chance_outcomes(
        context, state, actions, probabilities, capacity, required_count);
}

static Status kuhn_poker_trusted_chance_outcomes(
    const void *context, const GameState *state, Action *actions,
    Probability *probabilities, size_t capacity, size_t *required_count) {
    const KuhnPokerState *kuhn_poker_state = as_kuhn_const(state);

    if (kuhn_poker_state->phase != CFR_KUHN_POKER_PHASE_CHANCE)
        return CFR_STATUS_INVALID_ARGUMENT;
    Status status = kuhn_poker_trusted_legal_actions(
        context, state, actions, capacity, required_count);

    if (status != CFR_STATUS_SUCCESS)
        return status;
    for (size_t index = 0; index < *required_count; index += 1)
        probabilities[index] = 1.0 / 6.0;
    return CFR_STATUS_SUCCESS;
}

static Status kuhn_poker_information_set_key(const void *context,
                                             const GameState *state,
                                             InfoSetKey *result) {
    const KuhnPokerState *kuhn_poker_state = as_kuhn_const(state);
    Status status = validate_state(kuhn_poker_state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    return kuhn_poker_trusted_information_set_key(context, state, result);
}

static Status kuhn_poker_trusted_information_set_key(
    const void *context, const GameState *state, InfoSetKey *result) {
    (void)context;
    const KuhnPokerState *kuhn_poker_state = as_kuhn_const(state);
    KuhnPokerCard private_card;
    InfoSetKey player_code;
    InfoSetKey context_code;
    InfoSetKey card_code;
    switch (kuhn_poker_state->phase) {
    case CFR_KUHN_POKER_PHASE_PLAYER_0_OPEN:
        player_code = 0;
        context_code = 0;
        private_card = kuhn_poker_state->cards[0];
        break;

    case CFR_KUHN_POKER_PHASE_PLAYER_1_AFTER_CHECK:
        player_code = 1;
        context_code = 1;
        private_card = kuhn_poker_state->cards[1];
        break;

    case CFR_KUHN_POKER_PHASE_PLAYER_1_FACING_OPEN_BET:
        player_code = 1;
        context_code = 2;
        private_card = kuhn_poker_state->cards[1];
        break;

    case CFR_KUHN_POKER_PHASE_PLAYER_0_FACING_CHECK_BET:
        player_code = 0;
        context_code = 3;
        private_card = kuhn_poker_state->cards[0];
        break;

    case CFR_KUHN_POKER_PHASE_CHANCE:
    case CFR_KUHN_POKER_PHASE_TERMINAL:
    default:
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    /* Convert the cards to consecutive zero-based codes. */
    switch (private_card) {
    case CFR_KUHN_POKER_CARD_JACK:
        card_code = 0;
        break;

    case CFR_KUHN_POKER_CARD_QUEEN:
        card_code = 1;
        break;

    case CFR_KUHN_POKER_CARD_KING:
        card_code = 2;
        break;

    case CFR_KUHN_POKER_CARD_NOT_DEALT:
    default:
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    /*
     * There are four contexts and three cards. First select the block for the
     * player and context, then select the card within that block.
     */
    *result = ((player_code * 4) + context_code) * 3 + card_code;

    return CFR_STATUS_SUCCESS;
}
