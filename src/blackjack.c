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
static Status blackjack_chance_outcomes(const void *context,
                                        const GameState *state,
                                        Action *actions,
                                        Probability *probabilities,
                                        size_t capacity,
                                        size_t *required_count);
static Status blackjack_information_set_key(const void *context,
                                            const GameState *state,
                                            InfoSetKey *result);
static Status blackjack_trusted_is_terminal(const void *context,
                                            const GameState *state,
                                            bool *result);
static Status blackjack_trusted_terminal_utility(const void *context,
                                                 const GameState *state,
                                                 Player player,
                                                 Utility *result);
static Status blackjack_trusted_current_actor(const void *context,
                                              const GameState *state,
                                              Actor *result);
static Status blackjack_trusted_legal_actions(const void *context,
                                              const GameState *state,
                                              Action *actions,
                                              size_t capacity,
                                              size_t *required_count);
static Status blackjack_trusted_apply_action(const void *context,
                                             GameState *state, Action action);
static Status blackjack_trusted_undo_action(const void *context,
                                            GameState *state);
static Status blackjack_trusted_chance_probability(const void *context,
                                                   const GameState *state,
                                                   Action action,
                                                   Probability *result);
static Status blackjack_trusted_chance_outcomes(
    const void *context, const GameState *state, Action *actions,
    Probability *probabilities, size_t capacity, size_t *required_count);
static Status blackjack_trusted_information_set_key(const void *context,
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
    .information_set_key = blackjack_information_set_key,
    .chance_outcomes = blackjack_chance_outcomes};

static const GameOperations BLACKJACK_TRUSTED_GAME_OPERATIONS = {
    .is_terminal = blackjack_trusted_is_terminal,
    .terminal_utility = blackjack_trusted_terminal_utility,
    .current_actor = blackjack_trusted_current_actor,
    .legal_actions = blackjack_trusted_legal_actions,
    .apply_action = blackjack_trusted_apply_action,
    .undo_action = blackjack_trusted_undo_action,
    .chance_probability = blackjack_trusted_chance_probability,
    .information_set_key = blackjack_trusted_information_set_key,
    .chance_outcomes = blackjack_trusted_chance_outcomes};

static const Game BLACKJACK_GAME = {
    .operations = &BLACKJACK_GAME_OPERATIONS,
    .context = NULL,
    .strategic_player_count = 1,
    .max_legal_actions = CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS,
    .strategy_schema_id = "cfr.blackjack/v3",
    .trusted_operations = &BLACKJACK_TRUSTED_GAME_OPERATIONS};

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
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_DOUBLE:
    case CFR_BLACKJACK_PHASE_DEAL_SPLIT_HAND:
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
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_DOUBLE:
    case CFR_BLACKJACK_PHASE_DEAL_SPLIT_HAND:
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
           phase == CFR_BLACKJACK_PHASE_DEAL_PLAYER_HIT ||
           phase == CFR_BLACKJACK_PHASE_DEAL_PLAYER_DOUBLE ||
           phase == CFR_BLACKJACK_PHASE_DEAL_SPLIT_HAND;
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
    case CFR_BLACKJACK_ACTION_DOUBLE_DOWN:
    case CFR_BLACKJACK_ACTION_SPLIT:
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
    if (hand->card_count == 0)
        hand->first_card = card;
    else if (hand->card_count == 1)
        hand->second_card = card;

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
    if (hand->card_count == 0)
        hand->first_card = CFR_BLACKJACK_CARD_NOT_DEALT;
    else if (hand->card_count == 1)
        hand->second_card = CFR_BLACKJACK_CARD_NOT_DEALT;
    if (card == CFR_BLACKJACK_CARD_ACE)
        hand->ace_count -= 1;
    hand->is_soft = hand->ace_count > 0 && low_total <= 11;
    hand->total = low_total + (hand->is_soft ? 10 : 0);
    return true;
}

static bool hand_is_blackjack(const BlackjackHand *hand) {
    return hand->card_count == 2 && hand->total == 21 && !hand->from_split;
}

static bool hand_is_pair(const BlackjackHand *hand) {
    return hand->card_count == 2 &&
           hand->first_card == hand->second_card &&
           card_is_real(hand->first_card);
}

static bool hands_equal(const BlackjackHand *left,
                        const BlackjackHand *right) {
    return left->total == right->total &&
           left->card_count == right->card_count &&
           left->ace_count == right->ace_count &&
           left->is_soft == right->is_soft &&
           left->first_card == right->first_card &&
           left->second_card == right->second_card &&
           left->stake_multiplier == right->stake_multiplier &&
           left->from_split == right->from_split;
}

static void initialize_unchecked(BlackjackState *state) {
    size_t index;

    state->phase = CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST;
    for (index = 0; index < CFR_BLACKJACK_MAX_PLAYER_HANDS; index += 1)
        state->player_hands[index] = (BlackjackHand){0};
    state->player_hands[0].stake_multiplier = 1;
    state->player_hand_count = 1;
    state->active_player_hand = 0;
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
        state->undo_history[index] = (BlackjackUndoEntry){
            .previous_phase = CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST,
            .applied_action = CFR_BLACKJACK_ACTION_NONE};
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
        if (state->active_player_hand >= state->player_hand_count)
            return CFR_STATUS_INVALID_ARGUMENT;
        hand = &state->player_hands[state->active_player_hand];
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

static bool all_player_hands_bust(const BlackjackState *state) {
    size_t index;

    for (index = 0; index < state->player_hand_count; index += 1) {
        if (state->player_hands[index].total <= 21)
            return false;
    }
    return true;
}

static void finish_active_hand(BlackjackState *state) {
    if (state->active_player_hand + 1 < state->player_hand_count) {
        state->active_player_hand += 1;
        if (state->player_hands[state->active_player_hand].card_count == 1)
            state->phase = CFR_BLACKJACK_PHASE_DEAL_SPLIT_HAND;
        else
            state->phase = CFR_BLACKJACK_PHASE_PLAYER_TURN;
        return;
    }

    if (all_player_hands_bust(state)) {
        state->phase = CFR_BLACKJACK_PHASE_TERMINAL;
    } else if (state->dealer_hand.total < 17) {
        state->phase = CFR_BLACKJACK_PHASE_DEAL_DEALER_HIT;
    } else {
        state->phase = CFR_BLACKJACK_PHASE_TERMINAL;
    }
}

static bool active_hand_can_double(const BlackjackState *state) {
    const BlackjackHand *hand =
        &state->player_hands[state->active_player_hand];

    return hand->card_count == 2 && hand->stake_multiplier == 1;
}

static bool active_hand_can_split(const BlackjackState *state) {
    const BlackjackHand *hand =
        &state->player_hands[state->active_player_hand];

    return state->player_hand_count < CFR_BLACKJACK_MAX_PLAYER_HANDS &&
           hand->stake_multiplier == 1 && hand_is_pair(hand) &&
           !(hand->from_split &&
             hand->first_card == CFR_BLACKJACK_CARD_ACE);
}

static void split_active_hand(BlackjackState *state) {
    const size_t active = state->active_player_hand;
    const BlackjackCard pair_card = state->player_hands[active].first_card;
    BlackjackHand split_hand = {.stake_multiplier = 1, .from_split = true};
    size_t index;

    hand_add_card(&split_hand, pair_card);
    for (index = state->player_hand_count; index > active + 1; index -= 1)
        state->player_hands[index] = state->player_hands[index - 1];
    state->player_hands[active] = split_hand;
    state->player_hands[active + 1] = split_hand;
    state->player_hand_count += 1;
    state->phase = CFR_BLACKJACK_PHASE_DEAL_SPLIT_HAND;
}

/* Applies a transition without appending it to the undo history. */
static Status advance_state(BlackjackState *state, Action action) {
    const BlackjackPhase previous_phase = state->phase;
    BlackjackHand *active_hand;
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
        if (hand_is_blackjack(&state->player_hands[0]) ||
            hand_is_blackjack(&state->dealer_hand)) {
            state->phase = CFR_BLACKJACK_PHASE_TERMINAL;
        } else {
            state->phase = CFR_BLACKJACK_PHASE_PLAYER_TURN;
        }
        return CFR_STATUS_SUCCESS;

    case CFR_BLACKJACK_PHASE_PLAYER_TURN:
        active_hand = &state->player_hands[state->active_player_hand];
        if (action == CFR_BLACKJACK_ACTION_HIT) {
            state->phase = CFR_BLACKJACK_PHASE_DEAL_PLAYER_HIT;
        } else if (action == CFR_BLACKJACK_ACTION_STAND) {
            finish_active_hand(state);
        } else if (action == CFR_BLACKJACK_ACTION_DOUBLE_DOWN) {
            if (!active_hand_can_double(state))
                return CFR_STATUS_ILLEGAL_ACTION;
            active_hand->stake_multiplier = 2;
            state->phase = CFR_BLACKJACK_PHASE_DEAL_PLAYER_DOUBLE;
        } else if (action == CFR_BLACKJACK_ACTION_SPLIT) {
            if (!active_hand_can_split(state))
                return CFR_STATUS_ILLEGAL_ACTION;
            split_active_hand(state);
        } else {
            return CFR_STATUS_ILLEGAL_ACTION;
        }
        return CFR_STATUS_SUCCESS;

    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_HIT:
        status = deal_card(state, action);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        active_hand = &state->player_hands[state->active_player_hand];
        if (active_hand->total >= 21)
            finish_active_hand(state);
        else
            state->phase = CFR_BLACKJACK_PHASE_PLAYER_TURN;
        return CFR_STATUS_SUCCESS;

    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_DOUBLE:
        status = deal_card(state, action);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        finish_active_hand(state);
        return CFR_STATUS_SUCCESS;

    case CFR_BLACKJACK_PHASE_DEAL_SPLIT_HAND:
        status = deal_card(state, action);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        active_hand = &state->player_hands[state->active_player_hand];
        if (active_hand->first_card == CFR_BLACKJACK_CARD_ACE ||
            active_hand->total == 21) {
            finish_active_hand(state);
        } else {
            state->phase = CFR_BLACKJACK_PHASE_PLAYER_TURN;
        }
        return CFR_STATUS_SUCCESS;

    case CFR_BLACKJACK_PHASE_DEAL_DEALER_HIT:
        status = deal_card(state, action);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (state->dealer_hand.total < 17)
            state->phase = CFR_BLACKJACK_PHASE_DEAL_DEALER_HIT;
        else
            state->phase = CFR_BLACKJACK_PHASE_TERMINAL;
        return CFR_STATUS_SUCCESS;

    case CFR_BLACKJACK_PHASE_TERMINAL:
        return CFR_STATUS_ILLEGAL_ACTION;

    default:
        return CFR_STATUS_INVALID_ARGUMENT;
    }
}

static bool undo_entry_is_empty(const BlackjackUndoEntry *entry) {
    const BlackjackHand empty_hand = {0};

    return entry->previous_phase == CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST &&
           entry->applied_action == CFR_BLACKJACK_ACTION_NONE &&
           entry->previous_active_hand == 0 &&
           entry->previous_hand_count == 0 &&
           hands_equal(&entry->previous_active_hand_state, &empty_hand);
}

static bool hand_has_valid_shape(const BlackjackHand *hand) {
    if (hand->total < 0 ||
        hand->card_count > CFR_BLACKJACK_HAND_CAPACITY ||
        hand->ace_count > hand->card_count) {
        return false;
    }
    if (hand->card_count == 0) {
        return hand->total == 0 && hand->ace_count == 0 && !hand->is_soft &&
               hand->first_card == CFR_BLACKJACK_CARD_NOT_DEALT &&
               hand->second_card == CFR_BLACKJACK_CARD_NOT_DEALT;
    }
    if (hand->total == 0 || !card_is_real(hand->first_card) ||
        (hand->card_count == 1 &&
         hand->second_card != CFR_BLACKJACK_CARD_NOT_DEALT) ||
        (hand->card_count >= 2 && !card_is_real(hand->second_card))) {
        return false;
    }
    if (hand->is_soft)
        return hand->ace_count > 0 && hand->total >= 11 && hand->total <= 21;
    return hand->ace_count == 0 || hand->total > 11;
}

static bool hand_is_unused(const BlackjackHand *hand) {
    const BlackjackHand empty_hand = {0};

    return hands_equal(hand, &empty_hand);
}

static bool state_has_valid_shape(const BlackjackState *state) {
    size_t dealt_count = 0;
    size_t index;

    if (state == NULL || !phase_is_known(state->phase) ||
        !hand_has_valid_shape(&state->dealer_hand) ||
        state->dealer_hand.stake_multiplier != 0 ||
        state->dealer_hand.from_split || state->player_hand_count == 0 ||
        state->player_hand_count > CFR_BLACKJACK_MAX_PLAYER_HANDS ||
        state->active_player_hand >= state->player_hand_count ||
        state->undo_count > CFR_BLACKJACK_UNDO_HISTORY_CAPACITY) {
        return false;
    }

    for (index = 0; index < state->player_hand_count; index += 1) {
        const BlackjackHand *hand = &state->player_hands[index];

        if (!hand_has_valid_shape(hand) ||
            (hand->stake_multiplier != 1 && hand->stake_multiplier != 2)) {
            return false;
        }
        dealt_count += hand->card_count;
    }
    for (index = state->player_hand_count;
         index < CFR_BLACKJACK_MAX_PLAYER_HANDS; index += 1) {
        if (!hand_is_unused(&state->player_hands[index]))
            return false;
    }

    dealt_count += state->dealer_hand.card_count;
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
        return state->player_hand_count == 1 &&
               state->active_player_hand == 0 &&
               state->player_hands[0].card_count == 0 &&
               state->player_hands[0].stake_multiplier == 1 &&
               !state->player_hands[0].from_split &&
               state->dealer_hand.card_count == 0 && state->undo_count == 0;
    case CFR_BLACKJACK_PHASE_DEAL_DEALER_UP_CARD:
        return state->player_hand_count == 1 &&
               state->active_player_hand == 0 &&
               state->player_hands[0].card_count == 1 &&
               state->dealer_hand.card_count == 0;
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_SECOND:
        return state->player_hand_count == 1 &&
               state->active_player_hand == 0 &&
               state->player_hands[0].card_count == 1 &&
               state->dealer_hand.card_count == 1;
    case CFR_BLACKJACK_PHASE_DEAL_DEALER_HOLE_CARD:
        return state->player_hand_count == 1 &&
               state->active_player_hand == 0 &&
               state->player_hands[0].card_count == 2 &&
               state->dealer_hand.card_count == 1;
    case CFR_BLACKJACK_PHASE_PLAYER_TURN:
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_HIT:
        return state->player_hands[state->active_player_hand].card_count >= 2 &&
               state->dealer_hand.card_count >= 2 &&
               state->player_hands[state->active_player_hand].total <= 21 &&
               !hand_is_blackjack(
                   &state->player_hands[state->active_player_hand]) &&
               !hand_is_blackjack(&state->dealer_hand);
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_DOUBLE:
        return state->player_hands[state->active_player_hand].card_count == 2 &&
               state->player_hands[state->active_player_hand]
                       .stake_multiplier == 2 &&
               state->player_hands[state->active_player_hand].total <= 21 &&
               state->dealer_hand.card_count >= 2;
    case CFR_BLACKJACK_PHASE_DEAL_SPLIT_HAND:
        return state->player_hand_count >= 2 &&
               state->player_hands[state->active_player_hand].card_count == 1 &&
               state->player_hands[state->active_player_hand].from_split &&
               state->dealer_hand.card_count >= 2;
    case CFR_BLACKJACK_PHASE_DEAL_DEALER_HIT:
        return state->dealer_hand.card_count >= 2 &&
               !all_player_hands_bust(state) &&
               state->dealer_hand.total < 17;
    case CFR_BLACKJACK_PHASE_TERMINAL:
        return state->player_hands[0].card_count >= 2 &&
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
            entry->previous_active_hand != expected.active_player_hand ||
            entry->previous_hand_count != expected.player_hand_count ||
            !hands_equal(&entry->previous_active_hand_state,
                         &expected.player_hands[expected.active_player_hand]) ||
            advance_state(&expected, entry->applied_action) !=
                CFR_STATUS_SUCCESS) {
            return CFR_STATUS_INVALID_ARGUMENT;
        }
    }

    if (state->phase != expected.phase ||
        state->player_hand_count != expected.player_hand_count ||
        state->active_player_hand != expected.active_player_hand ||
        !hands_equal(&state->dealer_hand, &expected.dealer_hand) ||
        state->dealer_up_card != expected.dealer_up_card ||
        state->cards_remaining != expected.cards_remaining) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0; index < CFR_BLACKJACK_MAX_PLAYER_HANDS; index += 1) {
        if (!hands_equal(&state->player_hands[index],
                         &expected.player_hands[index])) {
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

    if (!state_has_valid_shape(blackjack_state))
        return CFR_STATUS_INVALID_ARGUMENT;

    return blackjack_trusted_is_terminal(context, state, result);
}

static Status blackjack_trusted_is_terminal(const void *context,
                                            const GameState *state,
                                            bool *result) {
    const BlackjackState *blackjack_state = as_blackjack_const(state);

    (void)context;
    *result = blackjack_state->phase == CFR_BLACKJACK_PHASE_TERMINAL;
    return CFR_STATUS_SUCCESS;
}

static Utility player_utility(const BlackjackState *state) {
    const bool dealer_blackjack = hand_is_blackjack(&state->dealer_hand);
    const int dealer_total = state->dealer_hand.total;
    Utility utility = 0.0;
    size_t index;

    for (index = 0; index < state->player_hand_count; index += 1) {
        const BlackjackHand *hand = &state->player_hands[index];
        const Utility stake = (Utility)hand->stake_multiplier;

        if (hand_is_blackjack(hand) && dealer_blackjack)
            continue;
        if (hand_is_blackjack(hand)) {
            utility += 1.5 * stake;
        } else if (dealer_blackjack || hand->total > 21) {
            utility -= stake;
        } else if (dealer_total > 21 || hand->total > dealer_total) {
            utility += stake;
        } else if (hand->total < dealer_total) {
            utility -= stake;
        }
    }
    return utility;
}

static Status blackjack_terminal_utility(const void *context,
                                         const GameState *state, Player player,
                                         Utility *result) {
    const BlackjackState *blackjack_state = as_blackjack_const(state);

    if (!state_has_valid_shape(blackjack_state))
        return CFR_STATUS_INVALID_ARGUMENT;

    return blackjack_trusted_terminal_utility(context, state, player, result);
}

static Status blackjack_trusted_terminal_utility(const void *context,
                                                 const GameState *state,
                                                 Player player,
                                                 Utility *result) {
    const BlackjackState *blackjack_state = as_blackjack_const(state);
    Utility utility;

    (void)context;
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

    if (!state_has_valid_shape(blackjack_state))
        return CFR_STATUS_INVALID_ARGUMENT;

    return blackjack_trusted_current_actor(context, state, result);
}

static Status blackjack_trusted_current_actor(const void *context,
                                              const GameState *state,
                                              Actor *result) {
    const BlackjackState *blackjack_state = as_blackjack_const(state);

    (void)context;
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

    if (!state_has_valid_shape(blackjack_state))
        return CFR_STATUS_INVALID_ARGUMENT;

    return blackjack_trusted_legal_actions(context, state, actions, capacity,
                                           required_count);
}

static Status blackjack_trusted_legal_actions(const void *context,
                                              const GameState *state,
                                              Action *actions,
                                              size_t capacity,
                                              size_t *required_count) {
    const BlackjackState *blackjack_state = as_blackjack_const(state);
    size_t count = 0;
    size_t index;

    (void)context;
    if (blackjack_state->phase == CFR_BLACKJACK_PHASE_PLAYER_TURN) {
        count = 2;
        if (active_hand_can_double(blackjack_state))
            count += 1;
        if (active_hand_can_split(blackjack_state))
            count += 1;
        if (count > capacity) {
            *required_count = count;
            return CFR_STATUS_BUFFER_TOO_SMALL;
        }
        actions[0] = CFR_BLACKJACK_ACTION_HIT;
        actions[1] = CFR_BLACKJACK_ACTION_STAND;
        count = 2;
        if (active_hand_can_double(blackjack_state)) {
            actions[count] = CFR_BLACKJACK_ACTION_DOUBLE_DOWN;
            count += 1;
        }
        if (active_hand_can_split(blackjack_state)) {
            actions[count] = CFR_BLACKJACK_ACTION_SPLIT;
            count += 1;
        }
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

    if (!state_has_valid_shape(blackjack_state))
        return CFR_STATUS_INVALID_ARGUMENT;

    return blackjack_trusted_apply_action(context, state, action);
}

static Status blackjack_trusted_apply_action(const void *context,
                                             GameState *state, Action action) {
    BlackjackState *blackjack_state = as_blackjack(state);
    BlackjackUndoEntry entry;
    Status status;

    (void)context;
    if (blackjack_state->undo_count >=
        CFR_BLACKJACK_UNDO_HISTORY_CAPACITY) {
        return CFR_STATUS_BUFFER_TOO_SMALL;
    }

    entry = (BlackjackUndoEntry){
        .previous_phase = blackjack_state->phase,
        .applied_action = (BlackjackAction)action,
        .previous_active_hand = blackjack_state->active_player_hand,
        .previous_hand_count = blackjack_state->player_hand_count,
        .previous_active_hand_state =
            blackjack_state
                ->player_hands[blackjack_state->active_player_hand]};
    status = advance_state(blackjack_state, action);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    blackjack_state->undo_history[blackjack_state->undo_count] = entry;
    blackjack_state->undo_count += 1;
    return CFR_STATUS_SUCCESS;
}

static Status blackjack_undo_action(const void *context, GameState *state) {
    BlackjackState *blackjack_state = as_blackjack(state);

    if (!state_has_valid_shape(blackjack_state))
        return CFR_STATUS_INVALID_ARGUMENT;

    return blackjack_trusted_undo_action(context, state);
}

static Status blackjack_trusted_undo_action(const void *context,
                                            GameState *state) {
    BlackjackState *blackjack_state = as_blackjack(state);
    BlackjackUndoEntry entry;
    BlackjackCard card = CFR_BLACKJACK_CARD_NOT_DEALT;
    size_t rank_index = 0;
    size_t index;
    Status status;

    (void)context;
    if (blackjack_state->undo_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;

    entry = blackjack_state->undo_history[blackjack_state->undo_count - 1];
    if (entry.previous_active_hand >= entry.previous_hand_count ||
        entry.previous_hand_count == 0 ||
        entry.previous_hand_count > CFR_BLACKJACK_MAX_PLAYER_HANDS) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    if (phase_is_chance(entry.previous_phase)) {
        status = action_to_card(entry.applied_action, &card);
        if (status != CFR_STATUS_SUCCESS)
            return CFR_STATUS_INVALID_ARGUMENT;
        rank_index = card_rank_index(card);
        if (blackjack_state->remaining_cards[rank_index] >=
            initial_card_count(card)) {
            return CFR_STATUS_INVALID_ARGUMENT;
        }

        if (phase_deals_to_player(entry.previous_phase)) {
            blackjack_state->player_hands[entry.previous_active_hand] =
                entry.previous_active_hand_state;
        } else if (!hand_remove_card(&blackjack_state->dealer_hand, card)) {
            return CFR_STATUS_INVALID_ARGUMENT;
        }
        blackjack_state->remaining_cards[rank_index] += 1;
        blackjack_state->cards_remaining += 1;
        if (entry.previous_phase ==
            CFR_BLACKJACK_PHASE_DEAL_DEALER_UP_CARD) {
            blackjack_state->dealer_up_card = CFR_BLACKJACK_CARD_NOT_DEALT;
        }
    } else if (entry.previous_phase == CFR_BLACKJACK_PHASE_PLAYER_TURN) {
        if (entry.applied_action == CFR_BLACKJACK_ACTION_SPLIT) {
            if (blackjack_state->player_hand_count !=
                entry.previous_hand_count + 1) {
                return CFR_STATUS_INVALID_ARGUMENT;
            }
            for (index = entry.previous_active_hand + 1;
                 index < entry.previous_hand_count; index += 1) {
                blackjack_state->player_hands[index] =
                    blackjack_state->player_hands[index + 1];
            }
            blackjack_state->player_hands[entry.previous_hand_count] =
                (BlackjackHand){0};
        }
        blackjack_state->player_hands[entry.previous_active_hand] =
            entry.previous_active_hand_state;
    } else {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    blackjack_state->phase = entry.previous_phase;
    blackjack_state->active_player_hand = entry.previous_active_hand;
    blackjack_state->player_hand_count = entry.previous_hand_count;
    blackjack_state->undo_count -= 1;
    blackjack_state->undo_history[blackjack_state->undo_count] =
        (BlackjackUndoEntry){
            .previous_phase = CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST,
            .applied_action = CFR_BLACKJACK_ACTION_NONE};
    return CFR_STATUS_SUCCESS;
}

static Status blackjack_chance_probability(const void *context,
                                           const GameState *state,
                                           Action action,
                                           Probability *result) {
    const BlackjackState *blackjack_state = as_blackjack_const(state);

    if (!state_has_valid_shape(blackjack_state))
        return CFR_STATUS_INVALID_ARGUMENT;

    return blackjack_trusted_chance_probability(context, state, action,
                                                result);
}

static Status blackjack_trusted_chance_probability(const void *context,
                                                   const GameState *state,
                                                   Action action,
                                                   Probability *result) {
    const BlackjackState *blackjack_state = as_blackjack_const(state);
    BlackjackCard card;
    size_t rank_index;
    Status status;

    (void)context;
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

static Status blackjack_chance_outcomes(const void *context,
                                        const GameState *state,
                                        Action *actions,
                                        Probability *probabilities,
                                        size_t capacity,
                                        size_t *required_count) {
    Status status = blackjack_validate_state(context, state);

    if (status != CFR_STATUS_SUCCESS)
        return status;

    return blackjack_trusted_chance_outcomes(
        context, state, actions, probabilities, capacity, required_count);
}

static Status blackjack_trusted_chance_outcomes(
    const void *context, const GameState *state, Action *actions,
    Probability *probabilities, size_t capacity, size_t *required_count) {
    const BlackjackState *blackjack_state = as_blackjack_const(state);
    size_t outcome_count = 0;
    size_t index;

    (void)context;
    if (!phase_is_chance(blackjack_state->phase) ||
        blackjack_state->cards_remaining == 0) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    for (index = 0; index < CFR_BLACKJACK_NUMBER_OF_CARD_RANKS; index += 1) {
        if (blackjack_state->remaining_cards[index] > 0)
            outcome_count += 1;
    }
    if (outcome_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (outcome_count > capacity) {
        *required_count = outcome_count;
        return CFR_STATUS_BUFFER_TOO_SMALL;
    }

    outcome_count = 0;
    for (index = 0; index < CFR_BLACKJACK_NUMBER_OF_CARD_RANKS; index += 1) {
        const size_t card_count = blackjack_state->remaining_cards[index];

        if (card_count > 0) {
            const BlackjackCard card =
                (BlackjackCard)(CFR_BLACKJACK_CARD_ACE + (int)index);

            actions[outcome_count] = card_to_action(card);
            probabilities[outcome_count] =
                (Probability)card_count /
                (Probability)blackjack_state->cards_remaining;
            outcome_count += 1;
        }
    }
    *required_count = outcome_count;
    return CFR_STATUS_SUCCESS;
}

static Status blackjack_information_set_key(const void *context,
                                            const GameState *state,
                                            InfoSetKey *result) {
    const BlackjackState *blackjack_state = as_blackjack_const(state);

    if (!state_has_valid_shape(blackjack_state))
        return CFR_STATUS_INVALID_ARGUMENT;

    return blackjack_trusted_information_set_key(context, state, result);
}

static Status blackjack_trusted_information_set_key(const void *context,
                                                    const GameState *state,
                                                    InfoSetKey *result) {
    const BlackjackState *blackjack_state = as_blackjack_const(state);
    const BlackjackHand *active_hand;
    InfoSetKey decision_class;
    bool player_is_soft;
    int player_total;

    (void)context;
    if (blackjack_state->phase != CFR_BLACKJACK_PHASE_PLAYER_TURN ||
        blackjack_state->dealer_hand.card_count < 1 ||
        blackjack_state->active_player_hand >=
            blackjack_state->player_hand_count ||
        blackjack_state->player_hands[blackjack_state->active_player_hand]
                .card_count < 2 ||
        !card_is_real(blackjack_state->dealer_up_card)) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    active_hand =
        &blackjack_state->player_hands[blackjack_state->active_player_hand];
    player_total = active_hand->total;
    player_is_soft = active_hand->is_soft;
    if (player_total < 0 || player_total > 21)
        return CFR_STATUS_INVALID_ARGUMENT;

    if (active_hand_can_split(blackjack_state))
        decision_class = 2;
    else if (active_hand_can_double(blackjack_state))
        decision_class = 1;
    else
        decision_class = 0;

    /*
     * A decision depends on the dealer's visible value, the active hand's
     * total and softness, and whether double or split is currently available.
     * Card order and the particular ranks that produced a nonsplittable hard
     * total remain merged in this strategy abstraction.
     *
     * There are ten dealer values. Each owns two blocks (hard and soft), with
     * 22 totals and three action-availability classes. The dealer's hidden
     * card never participates in the key.
     */
    *result =
        (((InfoSetKey)blackjack_state->dealer_up_card - 1) * 2 +
         (player_is_soft ? 1 : 0)) *
            66 +
        (InfoSetKey)player_total * 3 + decision_class;
    return CFR_STATUS_SUCCESS;
}
