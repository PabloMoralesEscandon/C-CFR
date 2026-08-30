#include <stdbool.h>
#include <stdint.h>

#include "cfr/blackjack.h"

static Status blackjack_is_terminal(const void *context,
                                    const GameState *state, bool *result);
static Status blackjack_terminal_utility(const void *context,
                                         const GameState *state, Player player,
                                         Utility *result);
static Status blackjack_current_actor(const void *context,
                                      const GameState *state, Actor *result);
static Status blackjack_legal_actions(const void *context,
                                      const GameState *state, Action *actions,
                                      size_t capacity, size_t *required_count);
static Status blackjack_apply_action(const void *context, GameState *state,
                                     Action action);
static Status blackjack_undo_action(const void *context, GameState *state);
static Status blackjack_chance_probability(const void *context,
                                           const GameState *state,
                                           Action action,
                                           Probability *result);
static Status blackjack_information_set_key(const void *context,
                                            const GameState *state,
                                            InfoSetKey *result);

static const GameOperations BLACKJACK_GAME_OPERATIONS = {
    .is_terminal = blackjack_is_terminal,
    .terminal_utility = blackjack_terminal_utility,
    .current_actor = blackjack_current_actor,
    .legal_actions = blackjack_legal_actions,
    .apply_action = blackjack_apply_action,
    .undo_action = blackjack_undo_action,
    .chance_probability = blackjack_chance_probability,
    .information_set_key = blackjack_information_set_key};

static const Game BLACKJACK_GAME = {
    .operations = &BLACKJACK_GAME_OPERATIONS,
    .context = NULL,
    .strategic_player_count = 1,
    .max_legal_actions = CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS,
    .strategy_schema_id = "cfr.blackjack/v1"};

static BlackjackState *as_blackjack(GameState *state) {
    return (BlackjackState *)state;
}

static const BlackjackState *as_blackjack_const(const GameState *state) {
    return (const BlackjackState *)state;
}

static bool phase_is_known(BlackjackPhase phase) {
    switch (phase) {
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST:
    case CFR_BLACKJACK_PHASE_DEAL_DEALER_UP_CARD:
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_SECOND:
    case CFR_BLACKJACK_PHASE_DEAL_DEALER_HOLE_CARD:
    case CFR_BLACKJACK_PHASE_PLAYER_TURN:
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_HIT:
    case CFR_BLACKJACK_PHASE_DEAL_DEALER_HIT:
    case CFR_BLACKJACK_PHASE_TERMINAL:
        return true;
    default:
        return false;
    }
}

static bool phase_is_chance(BlackjackPhase phase) {
    switch (phase) {
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST:
    case CFR_BLACKJACK_PHASE_DEAL_DEALER_UP_CARD:
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_SECOND:
    case CFR_BLACKJACK_PHASE_DEAL_DEALER_HOLE_CARD:
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_HIT:
    case CFR_BLACKJACK_PHASE_DEAL_DEALER_HIT:
        return true;
    case CFR_BLACKJACK_PHASE_PLAYER_TURN:
    case CFR_BLACKJACK_PHASE_TERMINAL:
    default:
        return false;
    }
}

static bool phase_deals_to_player(BlackjackPhase phase) {
    return phase == CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST ||
           phase == CFR_BLACKJACK_PHASE_DEAL_PLAYER_SECOND ||
           phase == CFR_BLACKJACK_PHASE_DEAL_PLAYER_HIT;
}

static size_t initial_card_count(BlackjackCard card) {
    if (card == CFR_BLACKJACK_CARD_TEN)
        return 16;
    if (card >= CFR_BLACKJACK_CARD_ACE &&
        card <= CFR_BLACKJACK_CARD_NINE) {
        return 4;
    }
    return 0;
}

static bool card_is_real(BlackjackCard card) {
    return card >= CFR_BLACKJACK_CARD_ACE &&
           card <= CFR_BLACKJACK_CARD_TEN;
}

static size_t card_rank_index(BlackjackCard card) {
    return (size_t)card - (size_t)CFR_BLACKJACK_CARD_ACE;
}

static Status action_to_card(Action action, BlackjackCard *card_out) {
    BlackjackCard card;

    if (card_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    switch ((BlackjackAction)action) {
    case CFR_BLACKJACK_ACTION_DEAL_ACE:
        card = CFR_BLACKJACK_CARD_ACE;
        break;
    case CFR_BLACKJACK_ACTION_DEAL_TWO:
        card = CFR_BLACKJACK_CARD_TWO;
        break;
    case CFR_BLACKJACK_ACTION_DEAL_THREE:
        card = CFR_BLACKJACK_CARD_THREE;
        break;
    case CFR_BLACKJACK_ACTION_DEAL_FOUR:
        card = CFR_BLACKJACK_CARD_FOUR;
        break;
    case CFR_BLACKJACK_ACTION_DEAL_FIVE:
        card = CFR_BLACKJACK_CARD_FIVE;
        break;
    case CFR_BLACKJACK_ACTION_DEAL_SIX:
        card = CFR_BLACKJACK_CARD_SIX;
        break;
    case CFR_BLACKJACK_ACTION_DEAL_SEVEN:
        card = CFR_BLACKJACK_CARD_SEVEN;
        break;
    case CFR_BLACKJACK_ACTION_DEAL_EIGHT:
        card = CFR_BLACKJACK_CARD_EIGHT;
        break;
    case CFR_BLACKJACK_ACTION_DEAL_NINE:
        card = CFR_BLACKJACK_CARD_NINE;
        break;
    case CFR_BLACKJACK_ACTION_DEAL_TEN:
        card = CFR_BLACKJACK_CARD_TEN;
        break;
    case CFR_BLACKJACK_ACTION_NONE:
    case CFR_BLACKJACK_ACTION_HIT:
    case CFR_BLACKJACK_ACTION_STAND:
    default:
        return CFR_STATUS_ILLEGAL_ACTION;
    }

    *card_out = card;
    return CFR_STATUS_SUCCESS;
}

static BlackjackAction card_to_action(BlackjackCard card) {
    switch (card) {
    case CFR_BLACKJACK_CARD_ACE:
        return CFR_BLACKJACK_ACTION_DEAL_ACE;
    case CFR_BLACKJACK_CARD_TWO:
        return CFR_BLACKJACK_ACTION_DEAL_TWO;
    case CFR_BLACKJACK_CARD_THREE:
        return CFR_BLACKJACK_ACTION_DEAL_THREE;
    case CFR_BLACKJACK_CARD_FOUR:
        return CFR_BLACKJACK_ACTION_DEAL_FOUR;
    case CFR_BLACKJACK_CARD_FIVE:
        return CFR_BLACKJACK_ACTION_DEAL_FIVE;
    case CFR_BLACKJACK_CARD_SIX:
        return CFR_BLACKJACK_ACTION_DEAL_SIX;
    case CFR_BLACKJACK_CARD_SEVEN:
        return CFR_BLACKJACK_ACTION_DEAL_SEVEN;
    case CFR_BLACKJACK_CARD_EIGHT:
        return CFR_BLACKJACK_ACTION_DEAL_EIGHT;
    case CFR_BLACKJACK_CARD_NINE:
        return CFR_BLACKJACK_ACTION_DEAL_NINE;
    case CFR_BLACKJACK_CARD_TEN:
        return CFR_BLACKJACK_ACTION_DEAL_TEN;
    case CFR_BLACKJACK_CARD_NOT_DEALT:
    default:
        return CFR_BLACKJACK_ACTION_NONE;
    }
}

static int card_value(BlackjackCard card) {
    if (card == CFR_BLACKJACK_CARD_ACE)
        return 1;
    if (card >= CFR_BLACKJACK_CARD_TWO &&
        card <= CFR_BLACKJACK_CARD_NINE) {
        return (int)card;
    }
    if (card == CFR_BLACKJACK_CARD_TEN)
        return 10;
    return 0;
}

static int hand_value(const BlackjackCard *cards, size_t card_count,
                      bool *is_soft_out) {
    bool contains_ace = false;
    int value = 0;
    size_t index;

    for (index = 0; index < card_count; index += 1) {
        value += card_value(cards[index]);
        if (cards[index] == CFR_BLACKJACK_CARD_ACE)
            contains_ace = true;
    }

    if (contains_ace && value <= 11) {
        value += 10;
        if (is_soft_out != NULL)
            *is_soft_out = true;
    } else if (is_soft_out != NULL) {
        *is_soft_out = false;
    }

    return value;
}

static bool hand_is_blackjack(const BlackjackCard *cards, size_t card_count) {
    return card_count == 2 && hand_value(cards, card_count, NULL) == 21;
}

static size_t remaining_card_count(const BlackjackState *state) {
    size_t count = 0;
    size_t index;

    for (index = 0; index < CFR_BLACKJACK_NUMBER_OF_CARD_RANKS; index += 1)
        count += state->remaining_cards[index];
    return count;
}

static void initialize_unchecked(BlackjackState *state) {
    size_t index;

    state->phase = CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST;
    state->player_card_count = 0;
    state->dealer_card_count = 0;
    state->undo_count = 0;

    for (index = 0; index < CFR_BLACKJACK_HAND_CAPACITY; index += 1) {
        state->player_cards[index] = CFR_BLACKJACK_CARD_NOT_DEALT;
        state->dealer_cards[index] = CFR_BLACKJACK_CARD_NOT_DEALT;
    }

    for (index = 0; index < CFR_BLACKJACK_NUMBER_OF_CARD_RANKS; index += 1) {
        const BlackjackCard card =
            (BlackjackCard)(CFR_BLACKJACK_CARD_ACE + (int)index);

        state->remaining_cards[index] = initial_card_count(card);
    }

    for (index = 0; index < CFR_BLACKJACK_UNDO_HISTORY_CAPACITY; index += 1) {
        state->undo_history[index].previous_phase =
            CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST;
        state->undo_history[index].applied_action =
            CFR_BLACKJACK_ACTION_NONE;
    }
}

Status cfr_blackjack_state_init(BlackjackState *state) {
    if (state == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    initialize_unchecked(state);
    return CFR_STATUS_SUCCESS;
}

static Status deal_card(BlackjackState *state, Action action) {
    BlackjackCard card;
    BlackjackCard *hand;
    size_t *hand_count;
    size_t rank_index;
    Status status;

    status = action_to_card(action, &card);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    rank_index = card_rank_index(card);
    if (state->remaining_cards[rank_index] == 0)
        return CFR_STATUS_ILLEGAL_ACTION;

    if (phase_deals_to_player(state->phase)) {
        hand = state->player_cards;
        hand_count = &state->player_card_count;
    } else {
        hand = state->dealer_cards;
        hand_count = &state->dealer_card_count;
    }

    if (*hand_count >= CFR_BLACKJACK_HAND_CAPACITY)
        return CFR_STATUS_BUFFER_TOO_SMALL;

    hand[*hand_count] = card;
    *hand_count += 1;
    state->remaining_cards[rank_index] -= 1;
    return CFR_STATUS_SUCCESS;
}

/* Applies a transition without appending it to the undo history. */
static Status advance_state(BlackjackState *state, Action action) {
    const BlackjackPhase previous_phase = state->phase;
    Status status;

    switch (previous_phase) {
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST:
        status = deal_card(state, action);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        state->phase = CFR_BLACKJACK_PHASE_DEAL_DEALER_UP_CARD;
        return CFR_STATUS_SUCCESS;

    case CFR_BLACKJACK_PHASE_DEAL_DEALER_UP_CARD:
        status = deal_card(state, action);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        state->phase = CFR_BLACKJACK_PHASE_DEAL_PLAYER_SECOND;
        return CFR_STATUS_SUCCESS;

    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_SECOND:
        status = deal_card(state, action);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        state->phase = CFR_BLACKJACK_PHASE_DEAL_DEALER_HOLE_CARD;
        return CFR_STATUS_SUCCESS;

    case CFR_BLACKJACK_PHASE_DEAL_DEALER_HOLE_CARD:
        status = deal_card(state, action);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (hand_is_blackjack(state->player_cards,
                              state->player_card_count) ||
            hand_is_blackjack(state->dealer_cards,
                              state->dealer_card_count)) {
            state->phase = CFR_BLACKJACK_PHASE_TERMINAL;
        } else {
            state->phase = CFR_BLACKJACK_PHASE_PLAYER_TURN;
        }
        return CFR_STATUS_SUCCESS;

    case CFR_BLACKJACK_PHASE_PLAYER_TURN:
        if (action == CFR_BLACKJACK_ACTION_HIT) {
            state->phase = CFR_BLACKJACK_PHASE_DEAL_PLAYER_HIT;
        } else if (action == CFR_BLACKJACK_ACTION_STAND) {
            if (hand_value(state->dealer_cards, state->dealer_card_count,
                           NULL) < 17) {
                state->phase = CFR_BLACKJACK_PHASE_DEAL_DEALER_HIT;
            } else {
                state->phase = CFR_BLACKJACK_PHASE_TERMINAL;
            }
        } else {
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        return CFR_STATUS_SUCCESS;

    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_HIT:
        status = deal_card(state, action);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (hand_value(state->player_cards, state->player_card_count, NULL) >
            21) {
            state->phase = CFR_BLACKJACK_PHASE_TERMINAL;
        } else {
            state->phase = CFR_BLACKJACK_PHASE_PLAYER_TURN;
        }
        return CFR_STATUS_SUCCESS;

    case CFR_BLACKJACK_PHASE_DEAL_DEALER_HIT:
        status = deal_card(state, action);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (hand_value(state->dealer_cards, state->dealer_card_count, NULL) <
            17) {
            state->phase = CFR_BLACKJACK_PHASE_DEAL_DEALER_HIT;
        } else {
            state->phase = CFR_BLACKJACK_PHASE_TERMINAL;
        }
        return CFR_STATUS_SUCCESS;

    case CFR_BLACKJACK_PHASE_TERMINAL:
        return CFR_STATUS_ILLEGAL_ACTION;

    default:
        return CFR_STATUS_INVALID_ARGUMENT;
    }
}

static bool undo_entry_is_empty(const BlackjackUndoEntry *entry) {
    return entry->previous_phase == CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST &&
           entry->applied_action == CFR_BLACKJACK_ACTION_NONE;
}

static Status validate_state(const BlackjackState *state) {
    BlackjackState expected;
    size_t index;

    if (state == NULL || !phase_is_known(state->phase) ||
        state->player_card_count > CFR_BLACKJACK_HAND_CAPACITY ||
        state->dealer_card_count > CFR_BLACKJACK_HAND_CAPACITY ||
        state->undo_count > CFR_BLACKJACK_UNDO_HISTORY_CAPACITY) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    if (state->player_card_count + state->dealer_card_count >
        CFR_BLACKJACK_DECK_SIZE) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    for (index = state->player_card_count;
         index < CFR_BLACKJACK_HAND_CAPACITY; index += 1) {
        if (state->player_cards[index] != CFR_BLACKJACK_CARD_NOT_DEALT)
            return CFR_STATUS_INVALID_ARGUMENT;
    }
    for (index = state->dealer_card_count;
         index < CFR_BLACKJACK_HAND_CAPACITY; index += 1) {
        if (state->dealer_cards[index] != CFR_BLACKJACK_CARD_NOT_DEALT)
            return CFR_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0; index < CFR_BLACKJACK_NUMBER_OF_CARD_RANKS; index += 1) {
        const BlackjackCard card =
            (BlackjackCard)(CFR_BLACKJACK_CARD_ACE + (int)index);

        if (state->remaining_cards[index] > initial_card_count(card))
            return CFR_STATUS_INVALID_ARGUMENT;
    }
    for (index = state->undo_count;
         index < CFR_BLACKJACK_UNDO_HISTORY_CAPACITY; index += 1) {
        if (!undo_entry_is_empty(&state->undo_history[index]))
            return CFR_STATUS_INVALID_ARGUMENT;
    }

    initialize_unchecked(&expected);
    for (index = 0; index < state->undo_count; index += 1) {
        const BlackjackUndoEntry *entry = &state->undo_history[index];

        if (entry->previous_phase != expected.phase ||
            entry->applied_action == CFR_BLACKJACK_ACTION_NONE ||
            advance_state(&expected, entry->applied_action) !=
                CFR_STATUS_SUCCESS) {
            return CFR_STATUS_INVALID_ARGUMENT;
        }
    }

    if (state->phase != expected.phase ||
        state->player_card_count != expected.player_card_count ||
        state->dealer_card_count != expected.dealer_card_count)
        return CFR_STATUS_INVALID_ARGUMENT;

    for (index = 0; index < CFR_BLACKJACK_HAND_CAPACITY; index += 1) {
        if (state->player_cards[index] != expected.player_cards[index] ||
            state->dealer_cards[index] != expected.dealer_cards[index]) {
            return CFR_STATUS_INVALID_ARGUMENT;
        }
    }
    for (index = 0; index < CFR_BLACKJACK_NUMBER_OF_CARD_RANKS; index += 1) {
        if (state->remaining_cards[index] != expected.remaining_cards[index])
            return CFR_STATUS_INVALID_ARGUMENT;
    }

    return CFR_STATUS_SUCCESS;
}

const Game *cfr_blackjack_descriptor(void) { return &BLACKJACK_GAME; }

GameState *cfr_blackjack_state_as_game_state(BlackjackState *blackjack_state) {
    return (GameState *)blackjack_state;
}

const GameState *cfr_blackjack_state_as_game_state_const(
    const BlackjackState *blackjack_state) {
    return (const GameState *)blackjack_state;
}

static Status blackjack_is_terminal(const void *context,
                                    const GameState *state, bool *result) {
    const BlackjackState *blackjack_state = as_blackjack_const(state);
    Status status;

    (void)context;
    status = validate_state(blackjack_state);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    *result = blackjack_state->phase == CFR_BLACKJACK_PHASE_TERMINAL;
    return CFR_STATUS_SUCCESS;
}

static Utility player_utility(const BlackjackState *state) {
    const bool player_blackjack =
        hand_is_blackjack(state->player_cards, state->player_card_count);
    const bool dealer_blackjack =
        hand_is_blackjack(state->dealer_cards, state->dealer_card_count);
    const int player_total =
        hand_value(state->player_cards, state->player_card_count, NULL);
    const int dealer_total =
        hand_value(state->dealer_cards, state->dealer_card_count, NULL);

    if (player_blackjack && dealer_blackjack)
        return 0.0;
    if (player_blackjack)
        return 1.5;
    if (dealer_blackjack)
        return -1.0;
    if (player_total > 21)
        return -1.0;
    if (dealer_total > 21)
        return 1.0;
    if (player_total > dealer_total)
        return 1.0;
    if (player_total < dealer_total)
        return -1.0;
    return 0.0;
}

static Status blackjack_terminal_utility(const void *context,
                                         const GameState *state, Player player,
                                         Utility *result) {
    const BlackjackState *blackjack_state = as_blackjack_const(state);
    Utility utility;
    Status status;

    (void)context;
    status = validate_state(blackjack_state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (blackjack_state->phase != CFR_BLACKJACK_PHASE_TERMINAL ||
        (player != CFR_PLAYER_0 && player != CFR_PLAYER_1)) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    utility = player_utility(blackjack_state);
    *result = player == CFR_PLAYER_0 ? utility : -utility;
    return CFR_STATUS_SUCCESS;
}

static Status blackjack_current_actor(const void *context,
                                      const GameState *state, Actor *result) {
    const BlackjackState *blackjack_state = as_blackjack_const(state);
    Status status;

    (void)context;
    status = validate_state(blackjack_state);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    if (phase_is_chance(blackjack_state->phase)) {
        result->kind = CFR_ACTOR_CHANCE;
        return CFR_STATUS_SUCCESS;
    }
    if (blackjack_state->phase == CFR_BLACKJACK_PHASE_PLAYER_TURN) {
        result->kind = CFR_ACTOR_PLAYER;
        result->player = CFR_PLAYER_0;
        return CFR_STATUS_SUCCESS;
    }
    return CFR_STATUS_INVALID_ARGUMENT;
}

static Status blackjack_legal_actions(const void *context,
                                      const GameState *state, Action *actions,
                                      size_t capacity,
                                      size_t *required_count) {
    const BlackjackState *blackjack_state = as_blackjack_const(state);
    size_t count = 0;
    size_t index;
    Status status;

    (void)context;
    status = validate_state(blackjack_state);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    if (blackjack_state->phase == CFR_BLACKJACK_PHASE_PLAYER_TURN) {
        count = 2;
        if (count > capacity) {
            *required_count = count;
            return CFR_STATUS_BUFFER_TOO_SMALL;
        }
        actions[0] = CFR_BLACKJACK_ACTION_HIT;
        actions[1] = CFR_BLACKJACK_ACTION_STAND;
        *required_count = count;
        return CFR_STATUS_SUCCESS;
    }
    if (!phase_is_chance(blackjack_state->phase))
        return CFR_STATUS_INVALID_ARGUMENT;

    for (index = 0; index < CFR_BLACKJACK_NUMBER_OF_CARD_RANKS; index += 1) {
        if (blackjack_state->remaining_cards[index] > 0)
            count += 1;
    }
    if (count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (count > capacity) {
        *required_count = count;
        return CFR_STATUS_BUFFER_TOO_SMALL;
    }

    count = 0;
    for (index = 0; index < CFR_BLACKJACK_NUMBER_OF_CARD_RANKS; index += 1) {
        if (blackjack_state->remaining_cards[index] > 0) {
            const BlackjackCard card =
                (BlackjackCard)(CFR_BLACKJACK_CARD_ACE + (int)index);

            actions[count] = card_to_action(card);
            count += 1;
        }
    }
    *required_count = count;
    return CFR_STATUS_SUCCESS;
}

static Status blackjack_apply_action(const void *context, GameState *state,
                                     Action action) {
    BlackjackState *blackjack_state = as_blackjack(state);
    BlackjackState next;
    Status status;

    (void)context;
    status = validate_state(blackjack_state);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    next = *blackjack_state;
    status = advance_state(&next, action);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (blackjack_state->undo_count >=
        CFR_BLACKJACK_UNDO_HISTORY_CAPACITY) {
        return CFR_STATUS_BUFFER_TOO_SMALL;
    }

    next.undo_history[next.undo_count].previous_phase =
        blackjack_state->phase;
    next.undo_history[next.undo_count].applied_action =
        (BlackjackAction)action;
    next.undo_count += 1;
    *blackjack_state = next;
    return CFR_STATUS_SUCCESS;
}

static Status blackjack_undo_action(const void *context, GameState *state) {
    BlackjackState *blackjack_state = as_blackjack(state);
    const BlackjackUndoEntry *entry;
    BlackjackCard card = CFR_BLACKJACK_CARD_NOT_DEALT;
    BlackjackCard *hand = NULL;
    size_t *hand_count = NULL;
    size_t rank_index = 0;
    Status status;

    (void)context;
    status = validate_state(blackjack_state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (blackjack_state->undo_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;

    entry = &blackjack_state
                 ->undo_history[blackjack_state->undo_count - 1];
    if (phase_is_chance(entry->previous_phase)) {
        status = action_to_card(entry->applied_action, &card);
        if (status != CFR_STATUS_SUCCESS)
            return CFR_STATUS_INVALID_ARGUMENT;
        if (phase_deals_to_player(entry->previous_phase)) {
            hand = blackjack_state->player_cards;
            hand_count = &blackjack_state->player_card_count;
        } else {
            hand = blackjack_state->dealer_cards;
            hand_count = &blackjack_state->dealer_card_count;
        }
        if (*hand_count == 0 || hand[*hand_count - 1] != card)
            return CFR_STATUS_INVALID_ARGUMENT;
        rank_index = card_rank_index(card);
    } else if (entry->previous_phase != CFR_BLACKJACK_PHASE_PLAYER_TURN) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    if (hand != NULL && hand_count != NULL) {
        *hand_count -= 1;
        hand[*hand_count] = CFR_BLACKJACK_CARD_NOT_DEALT;
        blackjack_state->remaining_cards[rank_index] += 1;
    }

    blackjack_state->phase = entry->previous_phase;
    blackjack_state->undo_count -= 1;
    blackjack_state->undo_history[blackjack_state->undo_count].previous_phase =
        CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST;
    blackjack_state->undo_history[blackjack_state->undo_count].applied_action =
        CFR_BLACKJACK_ACTION_NONE;
    return CFR_STATUS_SUCCESS;
}

static Status blackjack_chance_probability(const void *context,
                                           const GameState *state,
                                           Action action,
                                           Probability *result) {
    const BlackjackState *blackjack_state = as_blackjack_const(state);
    BlackjackCard card;
    size_t cards_left;
    size_t rank_index;
    Status status;

    (void)context;
    status = validate_state(blackjack_state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (!phase_is_chance(blackjack_state->phase))
        return CFR_STATUS_INVALID_ARGUMENT;

    status = action_to_card(action, &card);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    rank_index = card_rank_index(card);
    if (blackjack_state->remaining_cards[rank_index] == 0)
        return CFR_STATUS_ILLEGAL_ACTION;

    cards_left = remaining_card_count(blackjack_state);
    if (cards_left == 0)
        return CFR_STATUS_INVALID_ARGUMENT;

    *result = (Probability)blackjack_state->remaining_cards[rank_index] /
              (Probability)cards_left;
    return CFR_STATUS_SUCCESS;
}

static Status blackjack_information_set_key(const void *context,
                                            const GameState *state,
                                            InfoSetKey *result) {
    const BlackjackState *blackjack_state = as_blackjack_const(state);
    InfoSetKey key;
    size_t index;
    Status status;

    (void)context;
    status = validate_state(blackjack_state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (blackjack_state->phase != CFR_BLACKJACK_PHASE_PLAYER_TURN ||
        blackjack_state->dealer_card_count < 1 ||
        blackjack_state->player_card_count < 2 ||
        !card_is_real(blackjack_state->dealer_cards[0])) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Base eleven: zero remains reserved and the ten card ranks occupy digits
     * one through ten. The up card starts the key, followed by the player's
     * complete observable sequence. The dealer's hole card never participates,
     * so all of its possible deals share the same information set.
     */
    key = (InfoSetKey)blackjack_state->dealer_cards[0];
    for (index = 0; index < blackjack_state->player_card_count; index += 1) {
        const InfoSetKey digit =
            (InfoSetKey)blackjack_state->player_cards[index];

        if (!card_is_real(blackjack_state->player_cards[index]) ||
            key > (INT64_MAX - digit) / 11) {
            return CFR_STATUS_INVALID_ARGUMENT;
        }
        key = key * 11 + digit;
    }

    *result = key;
    return CFR_STATUS_SUCCESS;
}
