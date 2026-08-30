#include <stdbool.h>

#include "cfr/blackjack.h"

static Status blackjack_is_terminal(const void *context,
                                    const GameState *state, bool *result);
static Status blackjack_validate_state(const void *context,
                                       const GameState *state);
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
    .validate_state = blackjack_validate_state,
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
    .strategy_schema_id = "cfr.blackjack/v2"};

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

static void hand_add_card(BlackjackHand *hand, BlackjackCard card) {
    if (card == CFR_BLACKJACK_CARD_ACE)
        hand->ace_count += 1;

    if (card == CFR_BLACKJACK_CARD_ACE && hand->total <= 10) {
        hand->total += 11;
        hand->is_soft = true;
    } else {
        hand->total += card_value(card);
    }

    if (hand->total > 21 && hand->is_soft) {
        hand->total -= 10;
        hand->is_soft = false;
    }
    hand->card_count += 1;
}

static bool hand_remove_card(BlackjackHand *hand, BlackjackCard card) {
    const int value = card_value(card);
    int low_total;

    if (hand->card_count == 0 || value == 0 ||
        (card == CFR_BLACKJACK_CARD_ACE && hand->ace_count == 0)) {
        return false;
    }

    low_total = hand->total - (hand->is_soft ? 10 : 0) - value;
    if (low_total < 0)
        return false;

    hand->card_count -= 1;
    if (card == CFR_BLACKJACK_CARD_ACE)
        hand->ace_count -= 1;
    hand->is_soft = hand->ace_count > 0 && low_total <= 11;
    hand->total = low_total + (hand->is_soft ? 10 : 0);
    return true;
}

static bool hand_is_blackjack(const BlackjackHand *hand) {
    return hand->card_count == 2 && hand->total == 21;
}

static bool hands_equal(const BlackjackHand *left,
                        const BlackjackHand *right) {
    return left->total == right->total &&
           left->card_count == right->card_count &&
           left->ace_count == right->ace_count &&
           left->is_soft == right->is_soft;
}

static void initialize_unchecked(BlackjackState *state) {
    size_t index;

    state->phase = CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST;
    state->player_hand = (BlackjackHand){0};
    state->dealer_hand = (BlackjackHand){0};
    state->dealer_up_card = CFR_BLACKJACK_CARD_NOT_DEALT;
    state->cards_remaining = CFR_BLACKJACK_DECK_SIZE;
    state->undo_count = 0;

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
    BlackjackHand *hand;
    size_t rank_index;
    Status status;

    status = action_to_card(action, &card);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    rank_index = card_rank_index(card);
    if (state->cards_remaining == 0 ||
        state->remaining_cards[rank_index] == 0) {
        return CFR_STATUS_ILLEGAL_ACTION;
    }

    if (phase_deals_to_player(state->phase)) {
        hand = &state->player_hand;
    } else {
        hand = &state->dealer_hand;
    }

    if (hand->card_count >= CFR_BLACKJACK_HAND_CAPACITY)
        return CFR_STATUS_BUFFER_TOO_SMALL;

    hand_add_card(hand, card);
    if (state->phase == CFR_BLACKJACK_PHASE_DEAL_DEALER_UP_CARD)
        state->dealer_up_card = card;
    state->remaining_cards[rank_index] -= 1;
    state->cards_remaining -= 1;
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
        if (hand_is_blackjack(&state->player_hand) ||
            hand_is_blackjack(&state->dealer_hand)) {
            state->phase = CFR_BLACKJACK_PHASE_TERMINAL;
        } else {
            state->phase = CFR_BLACKJACK_PHASE_PLAYER_TURN;
        }
        return CFR_STATUS_SUCCESS;

    case CFR_BLACKJACK_PHASE_PLAYER_TURN:
        if (action == CFR_BLACKJACK_ACTION_HIT) {
            state->phase = CFR_BLACKJACK_PHASE_DEAL_PLAYER_HIT;
        } else if (action == CFR_BLACKJACK_ACTION_STAND) {
            if (state->dealer_hand.total < 17) {
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
        if (state->player_hand.total > 21) {
            state->phase = CFR_BLACKJACK_PHASE_TERMINAL;
        } else {
            state->phase = CFR_BLACKJACK_PHASE_PLAYER_TURN;
        }
        return CFR_STATUS_SUCCESS;

    case CFR_BLACKJACK_PHASE_DEAL_DEALER_HIT:
        status = deal_card(state, action);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (state->dealer_hand.total < 17) {
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

static bool hand_has_valid_shape(const BlackjackHand *hand) {
    if (hand->total < 0 ||
        hand->card_count > CFR_BLACKJACK_HAND_CAPACITY ||
        hand->ace_count > hand->card_count) {
        return false;
    }
    if (hand->card_count == 0) {
        return hand->total == 0 && hand->ace_count == 0 && !hand->is_soft;
    }
    if (hand->total == 0)
        return false;
    if (hand->is_soft)
        return hand->ace_count > 0 && hand->total >= 11 && hand->total <= 21;
    return hand->ace_count == 0 || hand->total > 11;
}

static bool state_has_valid_shape(const BlackjackState *state) {
    size_t dealt_count;

    if (state == NULL || !phase_is_known(state->phase) ||
        !hand_has_valid_shape(&state->player_hand) ||
        !hand_has_valid_shape(&state->dealer_hand) ||
        state->undo_count > CFR_BLACKJACK_UNDO_HISTORY_CAPACITY) {
        return false;
    }

    dealt_count = state->player_hand.card_count + state->dealer_hand.card_count;
    if (dealt_count > CFR_BLACKJACK_DECK_SIZE ||
        state->cards_remaining != CFR_BLACKJACK_DECK_SIZE - dealt_count ||
        (state->dealer_hand.card_count == 0 &&
         state->dealer_up_card != CFR_BLACKJACK_CARD_NOT_DEALT) ||
        (state->dealer_hand.card_count > 0 &&
         !card_is_real(state->dealer_up_card)) ||
        (state->undo_count < CFR_BLACKJACK_UNDO_HISTORY_CAPACITY &&
         !undo_entry_is_empty(&state->undo_history[state->undo_count]))) {
        return false;
    }

    switch (state->phase) {
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST:
        return state->player_hand.card_count == 0 &&
               state->dealer_hand.card_count == 0 && state->undo_count == 0;
    case CFR_BLACKJACK_PHASE_DEAL_DEALER_UP_CARD:
        return state->player_hand.card_count == 1 &&
               state->dealer_hand.card_count == 0;
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_SECOND:
        return state->player_hand.card_count == 1 &&
               state->dealer_hand.card_count == 1;
    case CFR_BLACKJACK_PHASE_DEAL_DEALER_HOLE_CARD:
        return state->player_hand.card_count == 2 &&
               state->dealer_hand.card_count == 1;
    case CFR_BLACKJACK_PHASE_PLAYER_TURN:
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_HIT:
        return state->player_hand.card_count >= 2 &&
               state->dealer_hand.card_count >= 2 &&
               state->player_hand.total <= 21 &&
               !hand_is_blackjack(&state->player_hand) &&
               !hand_is_blackjack(&state->dealer_hand);
    case CFR_BLACKJACK_PHASE_DEAL_DEALER_HIT:
        return state->player_hand.card_count >= 2 &&
               state->dealer_hand.card_count >= 2 &&
               state->player_hand.total <= 21 &&
               state->dealer_hand.total < 17;
    case CFR_BLACKJACK_PHASE_TERMINAL:
        return state->player_hand.card_count >= 2 &&
               state->dealer_hand.card_count >= 2;
    default:
        return false;
    }
}

static Status blackjack_validate_state(const void *context,
                                       const GameState *public_state) {
    const BlackjackState *state = as_blackjack_const(public_state);
    BlackjackState expected;
    size_t index;

    (void)context;
    if (!state_has_valid_shape(state))
        return CFR_STATUS_INVALID_ARGUMENT;

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
        !hands_equal(&state->player_hand, &expected.player_hand) ||
        !hands_equal(&state->dealer_hand, &expected.dealer_hand) ||
        state->dealer_up_card != expected.dealer_up_card ||
        state->cards_remaining != expected.cards_remaining) {
        return CFR_STATUS_INVALID_ARGUMENT;
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

    (void)context;
    if (!state_has_valid_shape(blackjack_state))
        return CFR_STATUS_INVALID_ARGUMENT;

    *result = blackjack_state->phase == CFR_BLACKJACK_PHASE_TERMINAL;
    return CFR_STATUS_SUCCESS;
}

static Utility player_utility(const BlackjackState *state) {
    const bool player_blackjack = hand_is_blackjack(&state->player_hand);
    const bool dealer_blackjack = hand_is_blackjack(&state->dealer_hand);
    const int player_total = state->player_hand.total;
    const int dealer_total = state->dealer_hand.total;

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

    (void)context;
    if (!state_has_valid_shape(blackjack_state) ||
        blackjack_state->phase != CFR_BLACKJACK_PHASE_TERMINAL ||
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

    (void)context;
    if (!state_has_valid_shape(blackjack_state))
        return CFR_STATUS_INVALID_ARGUMENT;

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

    (void)context;
    if (!state_has_valid_shape(blackjack_state))
        return CFR_STATUS_INVALID_ARGUMENT;

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
    BlackjackPhase previous_phase;
    Status status;

    (void)context;
    if (!state_has_valid_shape(blackjack_state))
        return CFR_STATUS_INVALID_ARGUMENT;
    if (blackjack_state->undo_count >=
        CFR_BLACKJACK_UNDO_HISTORY_CAPACITY) {
        return CFR_STATUS_BUFFER_TOO_SMALL;
    }

    previous_phase = blackjack_state->phase;
    status = advance_state(blackjack_state, action);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    blackjack_state->undo_history[blackjack_state->undo_count].previous_phase =
        previous_phase;
    blackjack_state->undo_history[blackjack_state->undo_count].applied_action =
        (BlackjackAction)action;
    blackjack_state->undo_count += 1;
    return CFR_STATUS_SUCCESS;
}

static Status blackjack_undo_action(const void *context, GameState *state) {
    BlackjackState *blackjack_state = as_blackjack(state);
    const BlackjackUndoEntry *entry;
    BlackjackCard card = CFR_BLACKJACK_CARD_NOT_DEALT;
    BlackjackHand *hand = NULL;
    size_t rank_index = 0;
    Status status;

    (void)context;
    if (!state_has_valid_shape(blackjack_state))
        return CFR_STATUS_INVALID_ARGUMENT;
    if (blackjack_state->undo_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;

    entry = &blackjack_state
                 ->undo_history[blackjack_state->undo_count - 1];
    if (phase_is_chance(entry->previous_phase)) {
        status = action_to_card(entry->applied_action, &card);
        if (status != CFR_STATUS_SUCCESS)
            return CFR_STATUS_INVALID_ARGUMENT;
        if (phase_deals_to_player(entry->previous_phase)) {
            hand = &blackjack_state->player_hand;
        } else {
            hand = &blackjack_state->dealer_hand;
        }
        rank_index = card_rank_index(card);
        if (blackjack_state->remaining_cards[rank_index] >=
                initial_card_count(card) ||
            !hand_remove_card(hand, card)) {
            return CFR_STATUS_INVALID_ARGUMENT;
        }
    } else if (entry->previous_phase != CFR_BLACKJACK_PHASE_PLAYER_TURN) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    if (hand != NULL) {
        blackjack_state->remaining_cards[rank_index] += 1;
        blackjack_state->cards_remaining += 1;
        if (entry->previous_phase ==
            CFR_BLACKJACK_PHASE_DEAL_DEALER_UP_CARD) {
            blackjack_state->dealer_up_card = CFR_BLACKJACK_CARD_NOT_DEALT;
        }
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
    size_t rank_index;
    Status status;

    (void)context;
    if (!state_has_valid_shape(blackjack_state))
        return CFR_STATUS_INVALID_ARGUMENT;
    if (!phase_is_chance(blackjack_state->phase))
        return CFR_STATUS_INVALID_ARGUMENT;

    status = action_to_card(action, &card);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    rank_index = card_rank_index(card);
    if (blackjack_state->remaining_cards[rank_index] == 0)
        return CFR_STATUS_ILLEGAL_ACTION;

    if (blackjack_state->cards_remaining == 0)
        return CFR_STATUS_INVALID_ARGUMENT;

    *result = (Probability)blackjack_state->remaining_cards[rank_index] /
              (Probability)blackjack_state->cards_remaining;
    return CFR_STATUS_SUCCESS;
}

static Status blackjack_information_set_key(const void *context,
                                            const GameState *state,
                                            InfoSetKey *result) {
    const BlackjackState *blackjack_state = as_blackjack_const(state);
    bool player_is_soft;
    int player_total;

    (void)context;
    if (!state_has_valid_shape(blackjack_state) ||
        blackjack_state->phase != CFR_BLACKJACK_PHASE_PLAYER_TURN ||
        blackjack_state->dealer_hand.card_count < 1 ||
        blackjack_state->player_hand.card_count < 2 ||
        !card_is_real(blackjack_state->dealer_up_card)) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    player_total = blackjack_state->player_hand.total;
    player_is_soft = blackjack_state->player_hand.is_soft;
    if (player_total < 0 || player_total > 21)
        return CFR_STATUS_INVALID_ARGUMENT;

    /*
     * A hit-or-stand decision depends on the dealer's visible value and on the
     * player's total. Card order, card count, and the particular ranks that
     * produced a hard total do not change the available actions or their
     * outcomes in this strategy abstraction. Soft hands remain separate
     * because an ace can change from eleven to one after a hit.
     *
     * There are ten dealer values. Each owns two blocks (hard and soft), with
     * 22 slots per block for totals zero through 21. The dealer's hidden card
     * never participates in the key.
     */
    *result =
        (((InfoSetKey)blackjack_state->dealer_up_card - 1) * 2 +
         (player_is_soft ? 1 : 0)) *
            22 +
        (InfoSetKey)player_total;
    return CFR_STATUS_SUCCESS;
}
