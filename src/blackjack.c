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
    .strategy_schema_id = "cfr.blackjack/v4",
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

static bool card_is_real(BlackjackCard card) {
    return card >= CFR_BLACKJACK_CARD_ACE &&
           card <= CFR_BLACKJACK_CARD_TEN;
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

static Probability card_probability(BlackjackCard card) {
    return card == CFR_BLACKJACK_CARD_TEN ? 4.0 / 13.0 : 1.0 / 13.0;
}

static void hand_add_card(BlackjackHand *hand, BlackjackCard card) {
    const bool is_second_card = hand->card_count == 1;
    const BlackjackCard first_card =
        hand->is_soft && hand->total == 11
            ? CFR_BLACKJACK_CARD_ACE
            : (BlackjackCard)hand->total;

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
    hand->can_split = is_second_card && first_card == card;
}

static bool hand_is_blackjack(const BlackjackHand *hand) {
    return hand->card_count == 2 && hand->total == 21 && !hand->from_split;
}

static bool hand_is_pair(const BlackjackHand *hand) {
    return hand->card_count == 2 && hand->can_split;
}

static bool hands_equal(const BlackjackHand *left,
                        const BlackjackHand *right) {
    return left->total == right->total &&
           left->card_count == right->card_count &&
           left->is_soft == right->is_soft &&
           left->can_split == right->can_split &&
           left->stake_multiplier == right->stake_multiplier &&
           left->from_split == right->from_split;
}

static void initialize_unchecked(BlackjackState *state) {
    size_t index;

    state->phase = CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST;
    state->player_hand = (BlackjackHand){.stake_multiplier = 1};
    state->split_hand_count = 1;
    state->dealer_hand = (BlackjackHand){0};
    state->dealer_up_card = CFR_BLACKJACK_CARD_NOT_DEALT;
    state->undo_count = 0;

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
    Status status;

    status = action_to_card(action, &card);
    if (status != CFR_STATUS_SUCCESS)
        return status;

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
    return CFR_STATUS_SUCCESS;
}

static void finish_player_hand(BlackjackState *state) {
    if (state->player_hand.total > 21) {
        state->phase = CFR_BLACKJACK_PHASE_TERMINAL;
    } else if (state->dealer_hand.total < 17) {
        state->phase = CFR_BLACKJACK_PHASE_DEAL_DEALER_HIT;
    } else {
        state->phase = CFR_BLACKJACK_PHASE_TERMINAL;
    }
}

static bool active_hand_can_double(const BlackjackState *state) {
    const BlackjackHand *hand = &state->player_hand;

    return hand->card_count == 2 && hand->stake_multiplier == 1;
}

static bool active_hand_can_split(const BlackjackState *state) {
    const BlackjackHand *hand = &state->player_hand;

    return state->split_hand_count < CFR_BLACKJACK_MAX_SPLIT_HANDS &&
           hand->stake_multiplier == 1 && hand_is_pair(hand) &&
           !(hand->from_split && hand->is_soft);
}

static void split_active_hand(BlackjackState *state) {
    const BlackjackCard pair_card = state->player_hand.is_soft
                                        ? CFR_BLACKJACK_CARD_ACE
                                        : (BlackjackCard)(
                                              state->player_hand.total / 2);
    BlackjackHand split_hand = {.stake_multiplier = 1, .from_split = true};

    hand_add_card(&split_hand, pair_card);
    state->player_hand = split_hand;
    state->split_hand_count *= 2;
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
        if (hand_is_blackjack(&state->player_hand) ||
            hand_is_blackjack(&state->dealer_hand)) {
            state->phase = CFR_BLACKJACK_PHASE_TERMINAL;
        } else {
            state->phase = CFR_BLACKJACK_PHASE_PLAYER_TURN;
        }
        return CFR_STATUS_SUCCESS;

    case CFR_BLACKJACK_PHASE_PLAYER_TURN:
        active_hand = &state->player_hand;
        if (action == CFR_BLACKJACK_ACTION_HIT) {
            state->phase = CFR_BLACKJACK_PHASE_DEAL_PLAYER_HIT;
        } else if (action == CFR_BLACKJACK_ACTION_STAND) {
            finish_player_hand(state);
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
        active_hand = &state->player_hand;
        if (active_hand->total >= 21)
            finish_player_hand(state);
        else
            state->phase = CFR_BLACKJACK_PHASE_PLAYER_TURN;
        return CFR_STATUS_SUCCESS;

    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_DOUBLE:
        status = deal_card(state, action);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        finish_player_hand(state);
        return CFR_STATUS_SUCCESS;

    case CFR_BLACKJACK_PHASE_DEAL_SPLIT_HAND:
        active_hand = &state->player_hand;
        const bool split_aces = active_hand->is_soft &&
                                active_hand->total == 11;
        status = deal_card(state, action);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (split_aces || active_hand->total == 21) {
            finish_player_hand(state);
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
           hands_equal(&entry->previous_player_hand, &empty_hand) &&
           hands_equal(&entry->previous_dealer_hand, &empty_hand) &&
           entry->previous_dealer_up_card == CFR_BLACKJACK_CARD_NOT_DEALT &&
           entry->previous_split_hand_count == 0;
}

static bool hand_has_valid_shape(const BlackjackHand *hand) {
    if (hand->total < 0 || hand->card_count > CFR_BLACKJACK_HAND_CAPACITY)
        return false;
    if (hand->card_count == 0)
        return hand->total == 0 && !hand->is_soft && !hand->can_split;
    if (hand->total == 0 ||
        (hand->card_count == 1 && hand->can_split) ||
        (hand->card_count > 2 && hand->can_split)) {
        return false;
    }
    if (hand->card_count == 1)
        return hand->is_soft ? hand->total == 11
                             : hand->total >= 2 && hand->total <= 10;
    if (hand->is_soft && (hand->total < 12 || hand->total > 21))
        return false;
    if (hand->can_split) {
        return hand->is_soft ? hand->total == 12
                             : hand->total >= 4 && hand->total <= 20 &&
                                   hand->total % 2 == 0;
    }
    return true;
}

static bool state_has_valid_shape(const BlackjackState *state) {
    if (state == NULL || !phase_is_known(state->phase) ||
        !hand_has_valid_shape(&state->player_hand) ||
        !hand_has_valid_shape(&state->dealer_hand) ||
        (state->player_hand.stake_multiplier != 1 &&
         state->player_hand.stake_multiplier != 2) ||
        state->dealer_hand.stake_multiplier != 0 ||
        state->dealer_hand.from_split || state->split_hand_count == 0 ||
        state->split_hand_count > CFR_BLACKJACK_MAX_SPLIT_HANDS ||
        (state->split_hand_count & (state->split_hand_count - 1)) != 0 ||
        (state->split_hand_count == 1 && state->player_hand.from_split) ||
        (state->split_hand_count > 1 && !state->player_hand.from_split) ||
        state->undo_count > CFR_BLACKJACK_UNDO_HISTORY_CAPACITY) {
        return false;
    }

    if ((state->dealer_hand.card_count == 0 &&
         state->dealer_up_card != CFR_BLACKJACK_CARD_NOT_DEALT) ||
        (state->dealer_hand.card_count > 0 &&
         !card_is_real(state->dealer_up_card)) ||
        (state->undo_count < CFR_BLACKJACK_UNDO_HISTORY_CAPACITY &&
         !undo_entry_is_empty(&state->undo_history[state->undo_count]))) {
        return false;
    }

    switch (state->phase) {
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST:
        return state->split_hand_count == 1 &&
               state->player_hand.card_count == 0 &&
               state->player_hand.stake_multiplier == 1 &&
               !state->player_hand.from_split &&
               state->dealer_hand.card_count == 0 && state->undo_count == 0;
    case CFR_BLACKJACK_PHASE_DEAL_DEALER_UP_CARD:
        return state->split_hand_count == 1 &&
               state->player_hand.card_count == 1 &&
               state->dealer_hand.card_count == 0;
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_SECOND:
        return state->split_hand_count == 1 &&
               state->player_hand.card_count == 1 &&
               state->dealer_hand.card_count == 1;
    case CFR_BLACKJACK_PHASE_DEAL_DEALER_HOLE_CARD:
        return state->split_hand_count == 1 &&
               state->player_hand.card_count == 2 &&
               state->dealer_hand.card_count == 1;
    case CFR_BLACKJACK_PHASE_PLAYER_TURN:
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_HIT:
        return state->player_hand.card_count >= 2 &&
               state->dealer_hand.card_count >= 2 &&
               state->player_hand.total <= 21 &&
               !hand_is_blackjack(&state->player_hand) &&
               !hand_is_blackjack(&state->dealer_hand);
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_DOUBLE:
        return state->player_hand.card_count == 2 &&
               state->player_hand.stake_multiplier == 2 &&
               state->player_hand.total <= 21 &&
               state->dealer_hand.card_count >= 2;
    case CFR_BLACKJACK_PHASE_DEAL_SPLIT_HAND:
        return state->split_hand_count >= 2 &&
               state->player_hand.card_count == 1 &&
               state->player_hand.from_split &&
               state->dealer_hand.card_count >= 2;
    case CFR_BLACKJACK_PHASE_DEAL_DEALER_HIT:
        return state->dealer_hand.card_count >= 2 &&
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
            !hands_equal(&entry->previous_player_hand,
                         &expected.player_hand) ||
            !hands_equal(&entry->previous_dealer_hand,
                         &expected.dealer_hand) ||
            entry->previous_dealer_up_card != expected.dealer_up_card ||
            entry->previous_split_hand_count != expected.split_hand_count ||
            advance_state(&expected, entry->applied_action) !=
                CFR_STATUS_SUCCESS) {
            return CFR_STATUS_INVALID_ARGUMENT;
        }
    }

    if (state->phase != expected.phase ||
        !hands_equal(&state->player_hand, &expected.player_hand) ||
        !hands_equal(&state->dealer_hand, &expected.dealer_hand) ||
        state->dealer_up_card != expected.dealer_up_card ||
        state->split_hand_count != expected.split_hand_count) {
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
    const BlackjackHand *hand = &state->player_hand;
    const bool dealer_blackjack = hand_is_blackjack(&state->dealer_hand);
    const int dealer_total = state->dealer_hand.total;
    const Utility stake = (Utility)(hand->stake_multiplier *
                                    state->split_hand_count);

    if (hand_is_blackjack(hand) && dealer_blackjack)
        return 0.0;
    if (hand_is_blackjack(hand))
        return 1.5 * stake;
    if (dealer_blackjack || hand->total > 21)
        return -stake;
    if (dealer_total > 21 || hand->total > dealer_total)
        return stake;
    if (hand->total < dealer_total)
        return -stake;
    return 0.0;
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

    count = CFR_BLACKJACK_NUMBER_OF_CARD_RANKS;
    if (count > capacity) {
        *required_count = count;
        return CFR_STATUS_BUFFER_TOO_SMALL;
    }

    for (index = 0; index < CFR_BLACKJACK_NUMBER_OF_CARD_RANKS; index += 1) {
        const BlackjackCard card =
            (BlackjackCard)(CFR_BLACKJACK_CARD_ACE + (int)index);

        actions[index] = card_to_action(card);
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
        .previous_player_hand = blackjack_state->player_hand,
        .previous_dealer_hand = blackjack_state->dealer_hand,
        .previous_dealer_up_card = blackjack_state->dealer_up_card,
        .previous_split_hand_count = blackjack_state->split_hand_count};
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

    (void)context;
    if (blackjack_state->undo_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;

    entry = blackjack_state->undo_history[blackjack_state->undo_count - 1];
    if (entry.applied_action == CFR_BLACKJACK_ACTION_NONE ||
        !phase_is_known(entry.previous_phase) ||
        entry.previous_split_hand_count == 0 ||
        entry.previous_split_hand_count > CFR_BLACKJACK_MAX_SPLIT_HANDS) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    blackjack_state->phase = entry.previous_phase;
    blackjack_state->player_hand = entry.previous_player_hand;
    blackjack_state->dealer_hand = entry.previous_dealer_hand;
    blackjack_state->dealer_up_card = entry.previous_dealer_up_card;
    blackjack_state->split_hand_count = entry.previous_split_hand_count;
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
    Status status;

    (void)context;
    if (!phase_is_chance(blackjack_state->phase))
        return CFR_STATUS_INVALID_ARGUMENT;

    status = action_to_card(action, &card);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    *result = card_probability(card);
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
    size_t index;

    (void)context;
    if (!phase_is_chance(blackjack_state->phase)) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    if (CFR_BLACKJACK_NUMBER_OF_CARD_RANKS > capacity) {
        *required_count = CFR_BLACKJACK_NUMBER_OF_CARD_RANKS;
        return CFR_STATUS_BUFFER_TOO_SMALL;
    }

    for (index = 0; index < CFR_BLACKJACK_NUMBER_OF_CARD_RANKS; index += 1) {
        const BlackjackCard card =
            (BlackjackCard)(CFR_BLACKJACK_CARD_ACE + (int)index);

        actions[index] = card_to_action(card);
        probabilities[index] = card_probability(card);
    }
    *required_count = CFR_BLACKJACK_NUMBER_OF_CARD_RANKS;
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
        blackjack_state->player_hand.card_count < 2 ||
        !card_is_real(blackjack_state->dealer_up_card)) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    active_hand = &blackjack_state->player_hand;
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
     * The split multiplier, split history, dealer hole card, and particular
     * ranks that produced a nonsplittable hard total do not participate in the
     * key. A pair reached after a split therefore reuses the original pair's
     * information set while another split remains legal.
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
