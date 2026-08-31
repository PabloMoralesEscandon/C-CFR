#include <stdbool.h>
#include <stddef.h>

#include "cfr/leduc_poker.h"

enum {
    LEDUC_ANTE = 1,
    LEDUC_FIRST_ROUND_BET = 2,
    LEDUC_SECOND_ROUND_BET = 4,
    LEDUC_PRIVATE_CARD_COUNT = 2,
    LEDUC_PUBLIC_DEAL_CARD_COUNT = 4
};

typedef struct {
    LeducPokerPhase phase;
    LeducPokerCard private_cards[CFR_LEDUC_POKER_NUMBER_OF_PLAYERS];
    LeducPokerCard public_card;
    LeducPokerAction
        public_actions[CFR_LEDUC_POKER_PUBLIC_HISTORY_CAPACITY];
    size_t public_action_count;
    size_t round_start_index;
    Player current_player;
    size_t aggressive_action_count;
    int contributions[CFR_LEDUC_POKER_NUMBER_OF_PLAYERS];
    bool folded;
    Player folded_player;
} LeducModel;

static Status leduc_is_terminal(const void *context, const GameState *state,
                                bool *result);
static Status leduc_terminal_utility(const void *context,
                                     const GameState *state, Player player,
                                     Utility *result);
static Status leduc_current_actor(const void *context, const GameState *state,
                                  Actor *result);
static Status leduc_legal_actions(const void *context, const GameState *state,
                                  Action *actions, size_t capacity,
                                  size_t *required_count);
static Status leduc_apply_action(const void *context, GameState *state,
                                 Action action);
static Status leduc_undo_action(const void *context, GameState *state);
static Status leduc_chance_probability(const void *context,
                                       const GameState *state, Action action,
                                       Probability *result);
static Status leduc_information_set_key(const void *context,
                                        const GameState *state,
                                        InfoSetKey *result);
static Status leduc_validate_state_operation(const void *context,
                                             const GameState *state);
static Status leduc_chance_outcomes(const void *context,
                                    const GameState *state, Action *actions,
                                    Probability *probabilities,
                                    size_t capacity, size_t *required_count);

static const GameOperations LEDUC_OPERATIONS = {
    .is_terminal = leduc_is_terminal,
    .terminal_utility = leduc_terminal_utility,
    .current_actor = leduc_current_actor,
    .legal_actions = leduc_legal_actions,
    .apply_action = leduc_apply_action,
    .undo_action = leduc_undo_action,
    .chance_probability = leduc_chance_probability,
    .information_set_key = leduc_information_set_key,
    .validate_state = leduc_validate_state_operation,
    .chance_outcomes = leduc_chance_outcomes,
};

static const Game LEDUC_GAME = {
    .operations = &LEDUC_OPERATIONS,
    .context = NULL,
    .strategic_player_count = CFR_LEDUC_POKER_NUMBER_OF_PLAYERS,
    .max_legal_actions = CFR_LEDUC_POKER_MAX_POSSIBLE_ACTIONS,
    .strategy_schema_id = "cfr.leduc-poker/v1",
    .trusted_operations = NULL,
};

static LeducPokerState *as_leduc(GameState *state) {
    return (LeducPokerState *)state;
}

static const LeducPokerState *as_leduc_const(const GameState *state) {
    return (const LeducPokerState *)state;
}

static bool player_is_valid(Player player) {
    return player == CFR_PLAYER_0 || player == CFR_PLAYER_1;
}

static Player other_player(Player player) {
    return player == CFR_PLAYER_0 ? CFR_PLAYER_1 : CFR_PLAYER_0;
}

static void model_init(LeducModel *model) {
    *model = (LeducModel){0};
    model->phase = CFR_LEDUC_POKER_PHASE_PRIVATE_DEAL;
    model->private_cards[0] = CFR_LEDUC_POKER_CARD_NOT_DEALT;
    model->private_cards[1] = CFR_LEDUC_POKER_CARD_NOT_DEALT;
    model->public_card = CFR_LEDUC_POKER_CARD_NOT_DEALT;
    model->current_player = CFR_PLAYER_0;
    model->folded_player = CFR_PLAYER_0;
    model->contributions[0] = LEDUC_ANTE;
    model->contributions[1] = LEDUC_ANTE;
}

static void model_from_state(const LeducPokerState *state, LeducModel *model) {
    size_t index;

    model->phase = state->phase;
    model->private_cards[0] = state->private_cards[0];
    model->private_cards[1] = state->private_cards[1];
    model->public_card = state->public_card;
    for (index = 0; index < CFR_LEDUC_POKER_PUBLIC_HISTORY_CAPACITY; index++)
        model->public_actions[index] = state->public_actions[index];
    model->public_action_count = state->public_action_count;
    model->round_start_index = state->round_start_index;
    model->current_player = state->current_player;
    model->aggressive_action_count = state->aggressive_action_count;
    model->contributions[0] = state->contributions[0];
    model->contributions[1] = state->contributions[1];
    model->folded = state->folded;
    model->folded_player = state->folded_player;
}

static void state_from_model(LeducPokerState *state, const LeducModel *model) {
    size_t index;

    state->phase = model->phase;
    state->private_cards[0] = model->private_cards[0];
    state->private_cards[1] = model->private_cards[1];
    state->public_card = model->public_card;
    for (index = 0; index < CFR_LEDUC_POKER_PUBLIC_HISTORY_CAPACITY; index++)
        state->public_actions[index] = model->public_actions[index];
    state->public_action_count = model->public_action_count;
    state->round_start_index = model->round_start_index;
    state->current_player = model->current_player;
    state->aggressive_action_count = model->aggressive_action_count;
    state->contributions[0] = model->contributions[0];
    state->contributions[1] = model->contributions[1];
    state->folded = model->folded;
    state->folded_player = model->folded_player;
}

static bool models_equal(const LeducModel *left, const LeducModel *right) {
    size_t index;

    if (left->phase != right->phase ||
        left->private_cards[0] != right->private_cards[0] ||
        left->private_cards[1] != right->private_cards[1] ||
        left->public_card != right->public_card ||
        left->public_action_count != right->public_action_count ||
        left->round_start_index != right->round_start_index ||
        left->current_player != right->current_player ||
        left->aggressive_action_count != right->aggressive_action_count ||
        left->contributions[0] != right->contributions[0] ||
        left->contributions[1] != right->contributions[1] ||
        left->folded != right->folded ||
        left->folded_player != right->folded_player) {
        return false;
    }
    for (index = 0; index < CFR_LEDUC_POKER_PUBLIC_HISTORY_CAPACITY; index++) {
        if (left->public_actions[index] != right->public_actions[index])
            return false;
    }
    return true;
}

static Status decode_private_deal(LeducPokerAction action,
                                  LeducPokerCard *cards_out) {
    if (cards_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (action < CFR_LEDUC_POKER_ACTION_DEAL_JJ ||
        action > CFR_LEDUC_POKER_ACTION_DEAL_KK) {
        return CFR_STATUS_ILLEGAL_ACTION;
    }

    const int offset = (int)action - (int)CFR_LEDUC_POKER_ACTION_DEAL_JJ;
    cards_out[0] = (LeducPokerCard)(CFR_LEDUC_POKER_CARD_JACK + offset / 3);
    cards_out[1] = (LeducPokerCard)(CFR_LEDUC_POKER_CARD_JACK + offset % 3);
    return CFR_STATUS_SUCCESS;
}

static Status decode_public_deal(LeducPokerAction action,
                                 LeducPokerCard *card_out) {
    if (card_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    switch (action) {
    case CFR_LEDUC_POKER_ACTION_REVEAL_J:
        *card_out = CFR_LEDUC_POKER_CARD_JACK;
        return CFR_STATUS_SUCCESS;
    case CFR_LEDUC_POKER_ACTION_REVEAL_Q:
        *card_out = CFR_LEDUC_POKER_CARD_QUEEN;
        return CFR_STATUS_SUCCESS;
    case CFR_LEDUC_POKER_ACTION_REVEAL_K:
        *card_out = CFR_LEDUC_POKER_CARD_KING;
        return CFR_STATUS_SUCCESS;
    default:
        return CFR_STATUS_ILLEGAL_ACTION;
    }
}

static size_t remaining_rank_count(const LeducModel *model,
                                   LeducPokerCard rank) {
    size_t count = 2;

    if (model->private_cards[0] == rank)
        count -= 1;
    if (model->private_cards[1] == rank)
        count -= 1;
    return count;
}

static bool phase_is_betting(LeducPokerPhase phase) {
    return phase == CFR_LEDUC_POKER_PHASE_FIRST_BETTING ||
           phase == CFR_LEDUC_POKER_PHASE_SECOND_BETTING;
}

static int current_bet_size(const LeducModel *model) {
    return model->phase == CFR_LEDUC_POKER_PHASE_FIRST_BETTING
               ? LEDUC_FIRST_ROUND_BET
               : LEDUC_SECOND_ROUND_BET;
}

static int amount_to_call(const LeducModel *model) {
    const Player opponent = other_player(model->current_player);

    return model->contributions[opponent] -
           model->contributions[model->current_player];
}

static bool player_action_is_legal(const LeducModel *model,
                                   LeducPokerAction action) {
    const int to_call = amount_to_call(model);

    if (!phase_is_betting(model->phase) || to_call < 0)
        return false;
    if (to_call == 0) {
        return action == CFR_LEDUC_POKER_ACTION_CHECK ||
               (action == CFR_LEDUC_POKER_ACTION_BET &&
                model->aggressive_action_count <
                    CFR_LEDUC_POKER_MAX_AGGRESSIVE_ACTIONS_PER_ROUND);
    }
    return action == CFR_LEDUC_POKER_ACTION_FOLD ||
           action == CFR_LEDUC_POKER_ACTION_CALL ||
           (action == CFR_LEDUC_POKER_ACTION_RAISE &&
            model->aggressive_action_count <
                CFR_LEDUC_POKER_MAX_AGGRESSIVE_ACTIONS_PER_ROUND);
}

static void finish_betting_round(LeducModel *model) {
    if (model->phase == CFR_LEDUC_POKER_PHASE_FIRST_BETTING) {
        model->phase = CFR_LEDUC_POKER_PHASE_PUBLIC_DEAL;
        model->round_start_index = model->public_action_count;
        model->current_player = CFR_PLAYER_0;
        model->aggressive_action_count = 0;
    } else {
        model->phase = CFR_LEDUC_POKER_PHASE_TERMINAL;
    }
}

static Status transition_model(LeducModel *model, LeducPokerAction action) {
    if (model == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    if (model->phase == CFR_LEDUC_POKER_PHASE_PRIVATE_DEAL) {
        LeducPokerCard cards[CFR_LEDUC_POKER_NUMBER_OF_PLAYERS];
        const Status status = decode_private_deal(action, cards);

        if (status != CFR_STATUS_SUCCESS)
            return status;
        model->private_cards[0] = cards[0];
        model->private_cards[1] = cards[1];
        model->phase = CFR_LEDUC_POKER_PHASE_FIRST_BETTING;
        model->current_player = CFR_PLAYER_0;
        return CFR_STATUS_SUCCESS;
    }

    if (model->phase == CFR_LEDUC_POKER_PHASE_PUBLIC_DEAL) {
        LeducPokerCard card;
        const Status status = decode_public_deal(action, &card);

        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (remaining_rank_count(model, card) == 0)
            return CFR_STATUS_ILLEGAL_ACTION;
        model->public_card = card;
        model->phase = CFR_LEDUC_POKER_PHASE_SECOND_BETTING;
        model->current_player = CFR_PLAYER_0;
        model->aggressive_action_count = 0;
        return CFR_STATUS_SUCCESS;
    }

    if (!phase_is_betting(model->phase))
        return CFR_STATUS_ILLEGAL_ACTION;
    if (!player_action_is_legal(model, action))
        return CFR_STATUS_ILLEGAL_ACTION;
    if (model->public_action_count >=
        CFR_LEDUC_POKER_PUBLIC_HISTORY_CAPACITY) {
        return CFR_STATUS_BUFFER_TOO_SMALL;
    }

    const size_t action_index = model->public_action_count;
    const int to_call = amount_to_call(model);

    model->public_actions[action_index] = action;
    model->public_action_count += 1;

    switch (action) {
    case CFR_LEDUC_POKER_ACTION_CHECK:
        if (action_index > model->round_start_index &&
            model->public_actions[action_index - 1] ==
                CFR_LEDUC_POKER_ACTION_CHECK) {
            finish_betting_round(model);
        } else {
            model->current_player = other_player(model->current_player);
        }
        break;
    case CFR_LEDUC_POKER_ACTION_BET:
        model->contributions[model->current_player] +=
            current_bet_size(model);
        model->aggressive_action_count += 1;
        model->current_player = other_player(model->current_player);
        break;
    case CFR_LEDUC_POKER_ACTION_RAISE:
        model->contributions[model->current_player] +=
            to_call + current_bet_size(model);
        model->aggressive_action_count += 1;
        model->current_player = other_player(model->current_player);
        break;
    case CFR_LEDUC_POKER_ACTION_CALL:
        model->contributions[model->current_player] += to_call;
        finish_betting_round(model);
        break;
    case CFR_LEDUC_POKER_ACTION_FOLD:
        model->folded = true;
        model->folded_player = model->current_player;
        model->phase = CFR_LEDUC_POKER_PHASE_TERMINAL;
        break;
    case CFR_LEDUC_POKER_ACTION_NONE:
    case CFR_LEDUC_POKER_ACTION_DEAL_JJ:
    case CFR_LEDUC_POKER_ACTION_DEAL_JQ:
    case CFR_LEDUC_POKER_ACTION_DEAL_JK:
    case CFR_LEDUC_POKER_ACTION_DEAL_QJ:
    case CFR_LEDUC_POKER_ACTION_DEAL_QQ:
    case CFR_LEDUC_POKER_ACTION_DEAL_QK:
    case CFR_LEDUC_POKER_ACTION_DEAL_KJ:
    case CFR_LEDUC_POKER_ACTION_DEAL_KQ:
    case CFR_LEDUC_POKER_ACTION_DEAL_KK:
    case CFR_LEDUC_POKER_ACTION_REVEAL_J:
    case CFR_LEDUC_POKER_ACTION_REVEAL_Q:
    case CFR_LEDUC_POKER_ACTION_REVEAL_K:
    default:
        return CFR_STATUS_ILLEGAL_ACTION;
    }
    return CFR_STATUS_SUCCESS;
}

static bool undo_entry_matches_model(const LeducPokerUndoEntry *entry,
                                     const LeducModel *model) {
    return entry->previous_phase == model->phase &&
           entry->previous_private_cards[0] == model->private_cards[0] &&
           entry->previous_private_cards[1] == model->private_cards[1] &&
           entry->previous_public_card == model->public_card &&
           entry->previous_public_action_count == model->public_action_count &&
           entry->previous_round_start_index == model->round_start_index &&
           entry->previous_current_player == model->current_player &&
           entry->previous_aggressive_action_count ==
               model->aggressive_action_count &&
           entry->previous_contributions[0] == model->contributions[0] &&
           entry->previous_contributions[1] == model->contributions[1] &&
           entry->previous_folded == model->folded &&
           entry->previous_folded_player == model->folded_player;
}

static bool undo_entry_is_empty(const LeducPokerUndoEntry *entry) {
    const LeducPokerUndoEntry empty = {0};

    return entry->previous_phase == empty.previous_phase &&
           entry->previous_private_cards[0] ==
               empty.previous_private_cards[0] &&
           entry->previous_private_cards[1] ==
               empty.previous_private_cards[1] &&
           entry->previous_public_card == empty.previous_public_card &&
           entry->previous_public_action_count ==
               empty.previous_public_action_count &&
           entry->previous_round_start_index ==
               empty.previous_round_start_index &&
           entry->previous_current_player == empty.previous_current_player &&
           entry->previous_aggressive_action_count ==
               empty.previous_aggressive_action_count &&
           entry->previous_contributions[0] ==
               empty.previous_contributions[0] &&
           entry->previous_contributions[1] ==
               empty.previous_contributions[1] &&
           entry->previous_folded == empty.previous_folded &&
           entry->previous_folded_player == empty.previous_folded_player &&
           entry->applied_action == empty.applied_action;
}

static Status validate_state(const LeducPokerState *state) {
    LeducModel replay;
    LeducModel actual;
    size_t index;

    if (state == NULL ||
        state->public_action_count >
            CFR_LEDUC_POKER_PUBLIC_HISTORY_CAPACITY ||
        state->round_start_index > state->public_action_count ||
        state->undo_count > CFR_LEDUC_POKER_UNDO_HISTORY_CAPACITY) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    for (index = state->public_action_count;
         index < CFR_LEDUC_POKER_PUBLIC_HISTORY_CAPACITY; index++) {
        if (state->public_actions[index] != CFR_LEDUC_POKER_ACTION_NONE)
            return CFR_STATUS_INVALID_ARGUMENT;
    }
    for (index = state->undo_count;
         index < CFR_LEDUC_POKER_UNDO_HISTORY_CAPACITY; index++) {
        if (!undo_entry_is_empty(&state->undo_history[index]))
            return CFR_STATUS_INVALID_ARGUMENT;
    }

    model_init(&replay);
    for (index = 0; index < state->undo_count; index++) {
        const LeducPokerUndoEntry *entry = &state->undo_history[index];

        if (entry->applied_action == CFR_LEDUC_POKER_ACTION_NONE ||
            !undo_entry_matches_model(entry, &replay)) {
            return CFR_STATUS_INVALID_ARGUMENT;
        }
        const size_t previous_action_count = replay.public_action_count;
        const Status status = transition_model(&replay, entry->applied_action);

        if (status != CFR_STATUS_SUCCESS)
            return CFR_STATUS_INVALID_ARGUMENT;
        if (replay.public_action_count != previous_action_count) {
            if (previous_action_count >= state->public_action_count ||
                state->public_actions[previous_action_count] !=
                    entry->applied_action) {
                return CFR_STATUS_INVALID_ARGUMENT;
            }
        }
    }

    model_from_state(state, &actual);
    if (!models_equal(&replay, &actual))
        return CFR_STATUS_INVALID_ARGUMENT;
    if (!player_is_valid(state->current_player) ||
        !player_is_valid(state->folded_player)) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    return CFR_STATUS_SUCCESS;
}

static void save_undo(LeducPokerState *state, LeducPokerAction action) {
    LeducPokerUndoEntry *entry = &state->undo_history[state->undo_count];

    entry->previous_phase = state->phase;
    entry->previous_private_cards[0] = state->private_cards[0];
    entry->previous_private_cards[1] = state->private_cards[1];
    entry->previous_public_card = state->public_card;
    entry->previous_public_action_count = state->public_action_count;
    entry->previous_round_start_index = state->round_start_index;
    entry->previous_current_player = state->current_player;
    entry->previous_aggressive_action_count = state->aggressive_action_count;
    entry->previous_contributions[0] = state->contributions[0];
    entry->previous_contributions[1] = state->contributions[1];
    entry->previous_folded = state->folded;
    entry->previous_folded_player = state->folded_player;
    entry->applied_action = action;
    state->undo_count += 1;
}

Status cfr_leduc_poker_state_init(LeducPokerState *state) {
    LeducModel model;

    if (state == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    *state = (LeducPokerState){0};
    model_init(&model);
    state_from_model(state, &model);
    return CFR_STATUS_SUCCESS;
}

const Game *cfr_leduc_poker_descriptor(void) { return &LEDUC_GAME; }

GameState *cfr_leduc_poker_state_as_game_state(LeducPokerState *state) {
    return (GameState *)state;
}

const GameState *
cfr_leduc_poker_state_as_game_state_const(const LeducPokerState *state) {
    return (const GameState *)state;
}

static Status leduc_validate_state_operation(const void *context,
                                             const GameState *state) {
    (void)context;
    return validate_state(as_leduc_const(state));
}

static Status leduc_is_terminal(const void *context, const GameState *state,
                                bool *result) {
    const LeducPokerState *leduc = as_leduc_const(state);
    Status status;

    (void)context;
    if (result == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    status = validate_state(leduc);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    *result = leduc->phase == CFR_LEDUC_POKER_PHASE_TERMINAL;
    return CFR_STATUS_SUCCESS;
}

static int showdown_result_player_0(const LeducPokerState *state) {
    const bool player0_pair = state->private_cards[0] == state->public_card;
    const bool player1_pair = state->private_cards[1] == state->public_card;

    if (player0_pair != player1_pair)
        return player0_pair ? 1 : -1;
    if (state->private_cards[0] > state->private_cards[1])
        return 1;
    if (state->private_cards[0] < state->private_cards[1])
        return -1;
    return 0;
}

static Status leduc_terminal_utility(const void *context,
                                     const GameState *state, Player player,
                                     Utility *result) {
    const LeducPokerState *leduc = as_leduc_const(state);
    Utility player0_utility;
    Status status;

    (void)context;
    if (result == NULL || !player_is_valid(player))
        return CFR_STATUS_INVALID_ARGUMENT;
    status = validate_state(leduc);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (leduc->phase != CFR_LEDUC_POKER_PHASE_TERMINAL)
        return CFR_STATUS_INVALID_ARGUMENT;

    if (leduc->folded) {
        player0_utility = leduc->folded_player == CFR_PLAYER_0
                              ? -(Utility)leduc->contributions[0]
                              : (Utility)leduc->contributions[1];
    } else {
        const int showdown = showdown_result_player_0(leduc);

        if (showdown > 0)
            player0_utility = (Utility)leduc->contributions[1];
        else if (showdown < 0)
            player0_utility = -(Utility)leduc->contributions[0];
        else
            player0_utility =
                ((Utility)leduc->contributions[1] -
                 (Utility)leduc->contributions[0]) /
                2.0;
    }
    *result = player == CFR_PLAYER_0 ? player0_utility : -player0_utility;
    return CFR_STATUS_SUCCESS;
}

static Status leduc_current_actor(const void *context, const GameState *state,
                                  Actor *result) {
    const LeducPokerState *leduc = as_leduc_const(state);
    Status status;

    (void)context;
    if (result == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    status = validate_state(leduc);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (leduc->phase == CFR_LEDUC_POKER_PHASE_PRIVATE_DEAL ||
        leduc->phase == CFR_LEDUC_POKER_PHASE_PUBLIC_DEAL) {
        result->kind = CFR_ACTOR_CHANCE;
        return CFR_STATUS_SUCCESS;
    }
    if (leduc->phase == CFR_LEDUC_POKER_PHASE_TERMINAL)
        return CFR_STATUS_INVALID_ARGUMENT;
    result->kind = CFR_ACTOR_PLAYER;
    result->player = leduc->current_player;
    return CFR_STATUS_SUCCESS;
}

static Status collect_legal_actions(const LeducModel *model, Action *actions,
                                    size_t capacity,
                                    size_t *required_count) {
    Action temporary[CFR_LEDUC_POKER_MAX_POSSIBLE_ACTIONS];
    size_t count = 0;
    size_t index;

    if (model == NULL || actions == NULL || required_count == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    if (model->phase == CFR_LEDUC_POKER_PHASE_PRIVATE_DEAL) {
        for (index = 0; index < 9; index++)
            temporary[count++] = CFR_LEDUC_POKER_ACTION_DEAL_JJ + (Action)index;
    } else if (model->phase == CFR_LEDUC_POKER_PHASE_PUBLIC_DEAL) {
        for (LeducPokerCard rank = CFR_LEDUC_POKER_CARD_JACK;
             rank <= CFR_LEDUC_POKER_CARD_KING; rank++) {
            if (remaining_rank_count(model, rank) > 0) {
                temporary[count++] = CFR_LEDUC_POKER_ACTION_REVEAL_J +
                                     (Action)(rank -
                                              CFR_LEDUC_POKER_CARD_JACK);
            }
        }
    } else if (phase_is_betting(model->phase)) {
        const int to_call = amount_to_call(model);

        if (to_call < 0)
            return CFR_STATUS_INVALID_ARGUMENT;
        if (to_call == 0) {
            temporary[count++] = CFR_LEDUC_POKER_ACTION_CHECK;
            if (model->aggressive_action_count <
                CFR_LEDUC_POKER_MAX_AGGRESSIVE_ACTIONS_PER_ROUND) {
                temporary[count++] = CFR_LEDUC_POKER_ACTION_BET;
            }
        } else {
            temporary[count++] = CFR_LEDUC_POKER_ACTION_FOLD;
            temporary[count++] = CFR_LEDUC_POKER_ACTION_CALL;
            if (model->aggressive_action_count <
                CFR_LEDUC_POKER_MAX_AGGRESSIVE_ACTIONS_PER_ROUND) {
                temporary[count++] = CFR_LEDUC_POKER_ACTION_RAISE;
            }
        }
    } else {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    *required_count = count;
    if (capacity < count)
        return CFR_STATUS_BUFFER_TOO_SMALL;
    for (index = 0; index < count; index++)
        actions[index] = temporary[index];
    return CFR_STATUS_SUCCESS;
}

static Status leduc_legal_actions(const void *context, const GameState *state,
                                  Action *actions, size_t capacity,
                                  size_t *required_count) {
    const LeducPokerState *leduc = as_leduc_const(state);
    LeducModel model;
    Status status;

    (void)context;
    if (actions == NULL || required_count == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    status = validate_state(leduc);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    model_from_state(leduc, &model);
    return collect_legal_actions(&model, actions, capacity, required_count);
}

static Status leduc_apply_action(const void *context, GameState *state,
                                 Action action) {
    LeducPokerState *leduc = as_leduc(state);
    LeducModel next;
    Status status;

    (void)context;
    status = validate_state(leduc);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (leduc->undo_count >= CFR_LEDUC_POKER_UNDO_HISTORY_CAPACITY)
        return CFR_STATUS_BUFFER_TOO_SMALL;
    model_from_state(leduc, &next);
    status = transition_model(&next, (LeducPokerAction)action);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    save_undo(leduc, (LeducPokerAction)action);
    state_from_model(leduc, &next);
    return CFR_STATUS_SUCCESS;
}

static Status leduc_undo_action(const void *context, GameState *state) {
    LeducPokerState *leduc = as_leduc(state);
    LeducPokerUndoEntry entry;
    size_t index;
    Status status;

    (void)context;
    status = validate_state(leduc);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (leduc->undo_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;

    entry = leduc->undo_history[leduc->undo_count - 1];
    for (index = entry.previous_public_action_count;
         index < leduc->public_action_count; index++) {
        leduc->public_actions[index] = CFR_LEDUC_POKER_ACTION_NONE;
    }
    leduc->phase = entry.previous_phase;
    leduc->private_cards[0] = entry.previous_private_cards[0];
    leduc->private_cards[1] = entry.previous_private_cards[1];
    leduc->public_card = entry.previous_public_card;
    leduc->public_action_count = entry.previous_public_action_count;
    leduc->round_start_index = entry.previous_round_start_index;
    leduc->current_player = entry.previous_current_player;
    leduc->aggressive_action_count =
        entry.previous_aggressive_action_count;
    leduc->contributions[0] = entry.previous_contributions[0];
    leduc->contributions[1] = entry.previous_contributions[1];
    leduc->folded = entry.previous_folded;
    leduc->folded_player = entry.previous_folded_player;
    leduc->undo_count -= 1;
    leduc->undo_history[leduc->undo_count] = (LeducPokerUndoEntry){0};
    return CFR_STATUS_SUCCESS;
}

static Status chance_probability_for_model(const LeducModel *model,
                                           LeducPokerAction action,
                                           Probability *result) {
    if (model->phase == CFR_LEDUC_POKER_PHASE_PRIVATE_DEAL) {
        LeducPokerCard cards[CFR_LEDUC_POKER_NUMBER_OF_PLAYERS];
        Status status = decode_private_deal(action, cards);

        if (status != CFR_STATUS_SUCCESS)
            return status;
        *result = cards[0] == cards[1] ? 1.0 / 15.0 : 2.0 / 15.0;
        return CFR_STATUS_SUCCESS;
    }
    if (model->phase == CFR_LEDUC_POKER_PHASE_PUBLIC_DEAL) {
        LeducPokerCard card;
        Status status = decode_public_deal(action, &card);
        size_t remaining;

        if (status != CFR_STATUS_SUCCESS)
            return status;
        remaining = remaining_rank_count(model, card);
        if (remaining == 0)
            return CFR_STATUS_ILLEGAL_ACTION;
        *result = (Probability)remaining / LEDUC_PUBLIC_DEAL_CARD_COUNT;
        return CFR_STATUS_SUCCESS;
    }
    return CFR_STATUS_INVALID_ARGUMENT;
}

static Status leduc_chance_probability(const void *context,
                                       const GameState *state, Action action,
                                       Probability *result) {
    const LeducPokerState *leduc = as_leduc_const(state);
    LeducModel model;
    Status status;

    (void)context;
    if (result == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    status = validate_state(leduc);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    model_from_state(leduc, &model);
    return chance_probability_for_model(&model, (LeducPokerAction)action,
                                        result);
}

static Status leduc_chance_outcomes(const void *context,
                                    const GameState *state, Action *actions,
                                    Probability *probabilities,
                                    size_t capacity, size_t *required_count) {
    const LeducPokerState *leduc = as_leduc_const(state);
    Action temporary_actions[CFR_LEDUC_POKER_MAX_POSSIBLE_ACTIONS];
    Probability
        temporary_probabilities[CFR_LEDUC_POKER_MAX_POSSIBLE_ACTIONS];
    LeducModel model;
    size_t count = 0;
    size_t index;
    Status status;

    (void)context;
    if (actions == NULL || probabilities == NULL || required_count == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    status = validate_state(leduc);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    model_from_state(leduc, &model);
    status = collect_legal_actions(&model, temporary_actions,
                                   CFR_LEDUC_POKER_MAX_POSSIBLE_ACTIONS,
                                   &count);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    for (index = 0; index < count; index++) {
        status = chance_probability_for_model(
            &model, (LeducPokerAction)temporary_actions[index],
            &temporary_probabilities[index]);
        if (status != CFR_STATUS_SUCCESS)
            return status;
    }
    *required_count = count;
    if (capacity < count)
        return CFR_STATUS_BUFFER_TOO_SMALL;
    for (index = 0; index < count; index++) {
        actions[index] = temporary_actions[index];
        probabilities[index] = temporary_probabilities[index];
    }
    return CFR_STATUS_SUCCESS;
}

static int history_action_code(LeducPokerAction action) {
    switch (action) {
    case CFR_LEDUC_POKER_ACTION_CHECK:
        return 1;
    case CFR_LEDUC_POKER_ACTION_BET:
        return 2;
    case CFR_LEDUC_POKER_ACTION_FOLD:
        return 3;
    case CFR_LEDUC_POKER_ACTION_CALL:
        return 4;
    case CFR_LEDUC_POKER_ACTION_RAISE:
        return 5;
    case CFR_LEDUC_POKER_ACTION_NONE:
        return 0;
    default:
        return -1;
    }
}

static Status leduc_information_set_key(const void *context,
                                        const GameState *state,
                                        InfoSetKey *result) {
    const LeducPokerState *leduc = as_leduc_const(state);
    InfoSetKey key;
    int round_code;
    size_t index;
    Status status;

    (void)context;
    if (result == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    status = validate_state(leduc);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (!phase_is_betting(leduc->phase))
        return CFR_STATUS_INVALID_ARGUMENT;

    round_code = leduc->phase == CFR_LEDUC_POKER_PHASE_FIRST_BETTING ? 0 : 1;
    key = (InfoSetKey)leduc->current_player;
    key = key * 3 + (InfoSetKey)(leduc->private_cards[leduc->current_player] -
                                 CFR_LEDUC_POKER_CARD_JACK);
    key = key * 4 + (InfoSetKey)leduc->public_card;
    key = key * 2 + round_code;
    key = key * 9 + (InfoSetKey)leduc->round_start_index;
    key = key * 9 + (InfoSetKey)leduc->public_action_count;
    for (index = 0; index < CFR_LEDUC_POKER_PUBLIC_HISTORY_CAPACITY; index++) {
        const int code = history_action_code(leduc->public_actions[index]);

        if (code < 0)
            return CFR_STATUS_INVALID_ARGUMENT;
        key = key * 6 + code;
    }
    *result = key;
    return CFR_STATUS_SUCCESS;
}
