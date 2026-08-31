#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cfr/blackjack.h"
#include "cfr/checkpoint.h"
#include "cfr/info_store.h"
#include "cfr/trainer.h"
#include "test_suite.h"

static int failures;

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            failures += 1;                                                      \
        }                                                                      \
    } while (0)

static GameState *as_state(BlackjackState *state) {
    return cfr_blackjack_state_as_game_state(state);
}

static const GameState *as_const_state(const BlackjackState *state) {
    return cfr_blackjack_state_as_game_state_const(state);
}

static bool near(double left, double right) {
    return fabs(left - right) <= 1e-15;
}

static bool same_hand(const BlackjackHand *left, const BlackjackHand *right) {
    return left->total == right->total &&
           left->card_count == right->card_count &&
           left->is_soft == right->is_soft &&
           left->can_split == right->can_split &&
           left->stake_multiplier == right->stake_multiplier &&
           left->from_split == right->from_split;
}

static bool same_state(const BlackjackState *left,
                       const BlackjackState *right) {
    if (left->phase != right->phase ||
        !same_hand(&left->player_hand, &right->player_hand) ||
        left->split_hand_count != right->split_hand_count ||
        !same_hand(&left->dealer_hand, &right->dealer_hand) ||
        left->dealer_up_card != right->dealer_up_card ||
        left->undo_count != right->undo_count) {
        return false;
    }
    for (size_t index = 0; index < CFR_BLACKJACK_UNDO_HISTORY_CAPACITY;
         index += 1) {
        if (left->undo_history[index].previous_phase !=
                right->undo_history[index].previous_phase ||
            left->undo_history[index].applied_action !=
                right->undo_history[index].applied_action ||
            !same_hand(&left->undo_history[index].previous_player_hand,
                       &right->undo_history[index].previous_player_hand) ||
            !same_hand(&left->undo_history[index].previous_dealer_hand,
                       &right->undo_history[index].previous_dealer_hand) ||
            left->undo_history[index].previous_dealer_up_card !=
                right->undo_history[index].previous_dealer_up_card ||
            left->undo_history[index].previous_split_hand_count !=
                right->undo_history[index].previous_split_hand_count) {
            return false;
        }
    }
    return true;
}

static void initialize(BlackjackState *state) {
    *state = (BlackjackState){0};
    CHECK(cfr_blackjack_state_init(state) == CFR_STATUS_SUCCESS);
}

static void apply(const Game *game, BlackjackState *state, Action action) {
    CHECK(cfr_game_apply_action(game, as_state(state), action) ==
          CFR_STATUS_SUCCESS);
}

static void deal_initial_hand(const Game *game, BlackjackState *state,
                              BlackjackAction player_first,
                              BlackjackAction dealer_up,
                              BlackjackAction player_second,
                              BlackjackAction dealer_hole) {
    apply(game, state, player_first);
    apply(game, state, dealer_up);
    apply(game, state, player_second);
    apply(game, state, dealer_hole);
}

static bool action_is_present(Action expected, const Action *actions,
                              size_t action_count) {
    size_t index;

    for (index = 0; index < action_count; index += 1) {
        if (actions[index] == expected)
            return true;
    }
    return false;
}

static void check_root_and_distribution(void) {
    const Game *game = cfr_blackjack_descriptor();
    BlackjackState state;
    BlackjackState snapshot;
    Action actions[CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS] = {
        71, 72, 73, 74, 75, 76, 77, 78, 79, 80};
    Probability probabilities[CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS] = {
        61.0, 62.0, 63.0, 64.0, 65.0, 66.0, 67.0, 68.0, 69.0, 70.0};
    size_t required_count = 81;
    Probability probability_sum = 0.0;
    Probability batched_probability_sum = 0.0;
    Actor actor = {.kind = CFR_ACTOR_PLAYER, .player = CFR_PLAYER_1};
    Utility utility = 82.0;
    InfoSetKey key = 83;
    bool terminal = true;
    size_t index;

    CHECK(cfr_blackjack_state_init(NULL) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(game != NULL);
    CHECK(game->operations != NULL);
    CHECK(game->context == NULL);
    CHECK(game->strategic_player_count == 1);
    CHECK(game->max_legal_actions == CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS);
    CHECK(game->strategy_schema_id != NULL);
    CHECK(strcmp(game->strategy_schema_id, "cfr.blackjack/v4") == 0);
    CHECK(game->operations->is_terminal != NULL);
    CHECK(game->operations->terminal_utility != NULL);
    CHECK(game->operations->current_actor != NULL);
    CHECK(game->operations->legal_actions != NULL);
    CHECK(game->operations->apply_action != NULL);
    CHECK(game->operations->undo_action != NULL);
    CHECK(game->operations->chance_probability != NULL);
    CHECK(game->operations->chance_outcomes != NULL);
    CHECK(game->operations->information_set_key != NULL);

    initialize(&state);
    CHECK(cfr_blackjack_state_as_game_state(&state) == (GameState *)&state);
    CHECK(cfr_blackjack_state_as_game_state_const(&state) ==
          (const GameState *)&state);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST);
    CHECK(state.player_hand.total == 0);
    CHECK(state.player_hand.card_count == 0);
    CHECK(!state.player_hand.is_soft);
    CHECK(!state.player_hand.can_split);
    CHECK(state.player_hand.stake_multiplier == 1);
    CHECK(!state.player_hand.from_split);
    CHECK(state.split_hand_count == 1);
    CHECK(state.dealer_hand.total == 0);
    CHECK(state.dealer_hand.card_count == 0);
    CHECK(!state.dealer_hand.is_soft);
    CHECK(state.dealer_up_card == CFR_BLACKJACK_CARD_NOT_DEALT);
    CHECK(state.undo_count == 0);

    CHECK(cfr_game_is_terminal(game, as_const_state(&state), &terminal) ==
          CFR_STATUS_SUCCESS);
    CHECK(!terminal);
    CHECK(cfr_game_current_actor(game, as_const_state(&state), &actor) ==
          CFR_STATUS_SUCCESS);
    CHECK(actor.kind == CFR_ACTOR_CHANCE);

    CHECK(cfr_game_chance_outcomes(game, NULL, actions, probabilities,
                                   ARRAY_COUNT(actions), &required_count) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_chance_outcomes(game, as_const_state(&state), NULL,
                                   probabilities, ARRAY_COUNT(actions),
                                   &required_count) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_chance_outcomes(game, as_const_state(&state), actions, NULL,
                                   ARRAY_COUNT(actions), &required_count) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_chance_outcomes(game, as_const_state(&state), actions,
                                   probabilities, ARRAY_COUNT(actions),
                                   NULL) == CFR_STATUS_INVALID_ARGUMENT);

    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions,
                                 ARRAY_COUNT(actions) - 1, &required_count) ==
          CFR_STATUS_BUFFER_TOO_SMALL);
    CHECK(required_count == CFR_BLACKJACK_NUMBER_OF_CARD_RANKS);
    for (index = 0; index < ARRAY_COUNT(actions); index += 1)
        CHECK(actions[index] == (Action)(71 + index));

    CHECK(cfr_game_chance_outcomes(
              game, as_const_state(&state), actions, probabilities,
              ARRAY_COUNT(actions) - 1, &required_count) ==
          CFR_STATUS_BUFFER_TOO_SMALL);
    CHECK(required_count == CFR_BLACKJACK_NUMBER_OF_CARD_RANKS);
    for (index = 0; index < ARRAY_COUNT(actions); index += 1) {
        CHECK(actions[index] == (Action)(71 + index));
        CHECK(probabilities[index] == (Probability)(61 + index));
    }

    CHECK(cfr_game_chance_outcomes(game, as_const_state(&state), actions,
                                   probabilities, ARRAY_COUNT(actions),
                                   &required_count) == CFR_STATUS_SUCCESS);
    CHECK(required_count == ARRAY_COUNT(actions));
    for (index = 0; index < required_count; index += 1) {
        CHECK(actions[index] ==
              (Action)(CFR_BLACKJACK_ACTION_DEAL_ACE + (int)index));
        if (actions[index] == CFR_BLACKJACK_ACTION_DEAL_TEN)
            CHECK(near(probabilities[index], 4.0 / 13.0));
        else
            CHECK(near(probabilities[index], 1.0 / 13.0));
        batched_probability_sum += probabilities[index];
    }
    CHECK(near(batched_probability_sum, 1.0));

    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions,
                                 ARRAY_COUNT(actions), &required_count) ==
          CFR_STATUS_SUCCESS);
    CHECK(required_count == ARRAY_COUNT(actions));
    for (index = 0; index < required_count; index += 1) {
        Probability probability = -1.0;

        CHECK(actions[index] ==
              (Action)(CFR_BLACKJACK_ACTION_DEAL_ACE + (int)index));
        CHECK(cfr_game_chance_probability(game, as_const_state(&state),
                                          actions[index], &probability) ==
              CFR_STATUS_SUCCESS);
        if (actions[index] == CFR_BLACKJACK_ACTION_DEAL_TEN)
            CHECK(near(probability, 4.0 / 13.0));
        else
            CHECK(near(probability, 1.0 / 13.0));
        probability_sum += probability;
    }
    CHECK(near(probability_sum, 1.0));

    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), CFR_PLAYER_0,
                                    &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(utility == 82.0);
    CHECK(cfr_game_information_set_key(game, as_const_state(&state), &key) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(key == 83);

    snapshot = state;
    CHECK(cfr_game_apply_action(game, as_state(&state),
                                CFR_BLACKJACK_ACTION_HIT) ==
          CFR_STATUS_ILLEGAL_ACTION);
    CHECK(same_state(&state, &snapshot));
    CHECK(cfr_game_undo_action(game, as_state(&state)) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(same_state(&state, &snapshot));
}

static void check_fixed_distribution_and_player_turn(void) {
    const Game *game = cfr_blackjack_descriptor();
    BlackjackState state;
    Action actions[CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS] = {0};
    size_t action_count = 0;
    Probability probability = -1.0;
    Actor actor = {.kind = CFR_ACTOR_CHANCE, .player = CFR_PLAYER_1};

    initialize(&state);
    apply(game, &state, CFR_BLACKJACK_ACTION_DEAL_TEN);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_DEAL_DEALER_UP_CARD);
    CHECK(state.player_hand.total == 10);
    CHECK(state.player_hand.card_count == 1);
    CHECK(!state.player_hand.is_soft);
    CHECK(cfr_game_chance_probability(
              game, as_const_state(&state), CFR_BLACKJACK_ACTION_DEAL_TEN,
              &probability) == CFR_STATUS_SUCCESS);
    CHECK(near(probability, 4.0 / 13.0));

    apply(game, &state, CFR_BLACKJACK_ACTION_DEAL_SIX);
    apply(game, &state, CFR_BLACKJACK_ACTION_DEAL_SEVEN);
    apply(game, &state, CFR_BLACKJACK_ACTION_DEAL_TEN);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_PLAYER_TURN);
    CHECK(state.player_hand.total == 17);
    CHECK(state.player_hand.card_count == 2);
    CHECK(state.dealer_hand.total == 16);
    CHECK(state.dealer_hand.card_count == 2);
    CHECK(state.dealer_up_card == CFR_BLACKJACK_CARD_SIX);
    CHECK(cfr_game_current_actor(game, as_const_state(&state), &actor) ==
          CFR_STATUS_SUCCESS);
    CHECK(actor.kind == CFR_ACTOR_PLAYER);
    CHECK(actor.player == CFR_PLAYER_0);
    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions,
                                 ARRAY_COUNT(actions), &action_count) ==
          CFR_STATUS_SUCCESS);
    CHECK(action_count == 3);
    CHECK(actions[0] == CFR_BLACKJACK_ACTION_HIT);
    CHECK(actions[1] == CFR_BLACKJACK_ACTION_STAND);
    CHECK(actions[2] == CFR_BLACKJACK_ACTION_DOUBLE_DOWN);
    probability = 91.0;
    CHECK(cfr_game_chance_probability(
              game, as_const_state(&state), CFR_BLACKJACK_ACTION_DEAL_ACE,
              &probability) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(probability == 91.0);
}

static Utility utility_for_path(const Action *actions, size_t action_count) {
    const Game *game = cfr_blackjack_descriptor();
    BlackjackState state;
    Utility player0 = 99.0;
    Utility player1 = 98.0;
    bool terminal = false;
    size_t index;

    initialize(&state);
    for (index = 0; index < action_count; index += 1)
        apply(game, &state, actions[index]);

    CHECK(cfr_game_is_terminal(game, as_const_state(&state), &terminal) ==
          CFR_STATUS_SUCCESS);
    CHECK(terminal);
    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), CFR_PLAYER_0,
                                    &player0) == CFR_STATUS_SUCCESS);
    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), CFR_PLAYER_1,
                                    &player1) == CFR_STATUS_SUCCESS);
    CHECK(near(player0 + player1, 0.0));
    return player0;
}

static void check_terminal_utilities(void) {
    static const Action player_blackjack[] = {
        CFR_BLACKJACK_ACTION_DEAL_ACE, CFR_BLACKJACK_ACTION_DEAL_NINE,
        CFR_BLACKJACK_ACTION_DEAL_TEN, CFR_BLACKJACK_ACTION_DEAL_SEVEN};
    static const Action both_blackjack[] = {
        CFR_BLACKJACK_ACTION_DEAL_ACE, CFR_BLACKJACK_ACTION_DEAL_ACE,
        CFR_BLACKJACK_ACTION_DEAL_TEN, CFR_BLACKJACK_ACTION_DEAL_TEN};
    static const Action dealer_blackjack[] = {
        CFR_BLACKJACK_ACTION_DEAL_NINE, CFR_BLACKJACK_ACTION_DEAL_ACE,
        CFR_BLACKJACK_ACTION_DEAL_SEVEN, CFR_BLACKJACK_ACTION_DEAL_TEN};
    static const Action player_bust[] = {
        CFR_BLACKJACK_ACTION_DEAL_TEN, CFR_BLACKJACK_ACTION_DEAL_SIX,
        CFR_BLACKJACK_ACTION_DEAL_NINE, CFR_BLACKJACK_ACTION_DEAL_TEN,
        CFR_BLACKJACK_ACTION_HIT, CFR_BLACKJACK_ACTION_DEAL_FIVE};
    static const Action dealer_bust[] = {
        CFR_BLACKJACK_ACTION_DEAL_TEN, CFR_BLACKJACK_ACTION_DEAL_SIX,
        CFR_BLACKJACK_ACTION_DEAL_EIGHT, CFR_BLACKJACK_ACTION_DEAL_TEN,
        CFR_BLACKJACK_ACTION_STAND, CFR_BLACKJACK_ACTION_DEAL_TEN};
    static const Action player_loses[] = {
        CFR_BLACKJACK_ACTION_DEAL_TEN, CFR_BLACKJACK_ACTION_DEAL_TEN,
        CFR_BLACKJACK_ACTION_DEAL_SEVEN, CFR_BLACKJACK_ACTION_DEAL_EIGHT,
        CFR_BLACKJACK_ACTION_STAND};
    static const Action push[] = {
        CFR_BLACKJACK_ACTION_DEAL_TEN, CFR_BLACKJACK_ACTION_DEAL_NINE,
        CFR_BLACKJACK_ACTION_DEAL_EIGHT, CFR_BLACKJACK_ACTION_DEAL_NINE,
        CFR_BLACKJACK_ACTION_STAND};

    CHECK(near(utility_for_path(player_blackjack,
                                ARRAY_COUNT(player_blackjack)),
               1.5));
    CHECK(near(utility_for_path(both_blackjack, ARRAY_COUNT(both_blackjack)),
               0.0));
    CHECK(near(utility_for_path(dealer_blackjack,
                                ARRAY_COUNT(dealer_blackjack)),
               -1.0));
    CHECK(near(utility_for_path(player_bust, ARRAY_COUNT(player_bust)), -1.0));
    CHECK(near(utility_for_path(dealer_bust, ARRAY_COUNT(dealer_bust)), 1.0));
    CHECK(near(utility_for_path(player_loses, ARRAY_COUNT(player_loses)),
               -1.0));
    CHECK(near(utility_for_path(push, ARRAY_COUNT(push)), 0.0));
}

static void check_double_down(void) {
    const Game *game = cfr_blackjack_descriptor();
    BlackjackState state;
    BlackjackState initial_turn;
    Action actions[CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS] = {0};
    size_t action_count = 0;
    Utility utility = 0.0;

    initialize(&state);
    deal_initial_hand(game, &state, CFR_BLACKJACK_ACTION_DEAL_FIVE,
                      CFR_BLACKJACK_ACTION_DEAL_SIX,
                      CFR_BLACKJACK_ACTION_DEAL_SIX,
                      CFR_BLACKJACK_ACTION_DEAL_TEN);
    initial_turn = state;
    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions,
                                 ARRAY_COUNT(actions), &action_count) ==
          CFR_STATUS_SUCCESS);
    CHECK(action_count == 3);
    CHECK(actions[2] == CFR_BLACKJACK_ACTION_DOUBLE_DOWN);

    apply(game, &state, CFR_BLACKJACK_ACTION_DOUBLE_DOWN);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_DEAL_PLAYER_DOUBLE);
    CHECK(state.player_hand.stake_multiplier == 2);
    apply(game, &state, CFR_BLACKJACK_ACTION_DEAL_TEN);
    CHECK(state.player_hand.total == 21);
    CHECK(state.player_hand.card_count == 3);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_DEAL_DEALER_HIT);
    apply(game, &state, CFR_BLACKJACK_ACTION_DEAL_TEN);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_TERMINAL);
    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), CFR_PLAYER_0,
                                    &utility) == CFR_STATUS_SUCCESS);
    CHECK(utility == 2.0);

    CHECK(cfr_game_undo_action(game, as_state(&state)) == CFR_STATUS_SUCCESS);
    CHECK(cfr_game_undo_action(game, as_state(&state)) == CFR_STATUS_SUCCESS);
    CHECK(cfr_game_undo_action(game, as_state(&state)) == CFR_STATUS_SUCCESS);
    CHECK(same_state(&state, &initial_turn));

    apply(game, &state, CFR_BLACKJACK_ACTION_HIT);
    apply(game, &state, CFR_BLACKJACK_ACTION_DEAL_TWO);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_PLAYER_TURN);
    initial_turn = state;
    CHECK(cfr_game_apply_action(game, as_state(&state),
                                CFR_BLACKJACK_ACTION_DOUBLE_DOWN) ==
          CFR_STATUS_ILLEGAL_ACTION);
    CHECK(cfr_game_apply_action(game, as_state(&state),
                                CFR_BLACKJACK_ACTION_SPLIT) ==
          CFR_STATUS_ILLEGAL_ACTION);
    CHECK(same_state(&state, &initial_turn));

    initialize(&state);
    deal_initial_hand(game, &state, CFR_BLACKJACK_ACTION_DEAL_TEN,
                      CFR_BLACKJACK_ACTION_DEAL_SIX,
                      CFR_BLACKJACK_ACTION_DEAL_NINE,
                      CFR_BLACKJACK_ACTION_DEAL_TEN);
    apply(game, &state, CFR_BLACKJACK_ACTION_DOUBLE_DOWN);
    apply(game, &state, CFR_BLACKJACK_ACTION_DEAL_FIVE);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_TERMINAL);
    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), CFR_PLAYER_0,
                                    &utility) == CFR_STATUS_SUCCESS);
    CHECK(utility == -2.0);
}

static void check_split_round_and_undo(void) {
    static const Action split_path[] = {
        CFR_BLACKJACK_ACTION_SPLIT,
        CFR_BLACKJACK_ACTION_DEAL_THREE,
        CFR_BLACKJACK_ACTION_DOUBLE_DOWN,
        CFR_BLACKJACK_ACTION_DEAL_TEN,
        CFR_BLACKJACK_ACTION_DEAL_TEN};
    const Game *game = cfr_blackjack_descriptor();
    BlackjackState state;
    BlackjackState snapshots[ARRAY_COUNT(split_path) + 1];
    Action actions[CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS] = {0};
    size_t action_count = 0;
    Utility utility = 0.0;
    size_t index;

    initialize(&state);
    deal_initial_hand(game, &state, CFR_BLACKJACK_ACTION_DEAL_EIGHT,
                      CFR_BLACKJACK_ACTION_DEAL_SIX,
                      CFR_BLACKJACK_ACTION_DEAL_EIGHT,
                      CFR_BLACKJACK_ACTION_DEAL_TEN);
    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions,
                                 ARRAY_COUNT(actions), &action_count) ==
          CFR_STATUS_SUCCESS);
    CHECK(action_count == 4);
    CHECK(actions[0] == CFR_BLACKJACK_ACTION_HIT);
    CHECK(actions[1] == CFR_BLACKJACK_ACTION_STAND);
    CHECK(actions[2] == CFR_BLACKJACK_ACTION_DOUBLE_DOWN);
    CHECK(actions[3] == CFR_BLACKJACK_ACTION_SPLIT);

    snapshots[0] = state;
    for (index = 0; index < ARRAY_COUNT(split_path); index += 1) {
        apply(game, &state, split_path[index]);
        CHECK(cfr_game_validate_state(game, as_const_state(&state)) ==
              CFR_STATUS_SUCCESS);
        snapshots[index + 1] = state;

        if (index == 0) {
            CHECK(state.phase == CFR_BLACKJACK_PHASE_DEAL_SPLIT_HAND);
            CHECK(state.split_hand_count == 2);
            CHECK(state.player_hand.card_count == 1);
            CHECK(state.player_hand.total == 8);
            CHECK(state.player_hand.from_split);
        } else if (index == 3) {
            CHECK(state.phase == CFR_BLACKJACK_PHASE_DEAL_DEALER_HIT);
            CHECK(state.player_hand.total == 21);
            CHECK(state.player_hand.stake_multiplier == 2);
        }
    }

    CHECK(state.phase == CFR_BLACKJACK_PHASE_TERMINAL);
    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), CFR_PLAYER_0,
                                    &utility) == CFR_STATUS_SUCCESS);
    CHECK(utility == 4.0);

    for (index = ARRAY_COUNT(split_path); index > 0; index -= 1) {
        CHECK(cfr_game_undo_action(game, as_state(&state)) ==
              CFR_STATUS_SUCCESS);
        CHECK(same_state(&state, &snapshots[index - 1]));
    }
}

static void check_split_aces_and_resplit_limit(void) {
    const Game *game = cfr_blackjack_descriptor();
    BlackjackState state;
    Action actions[CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS] = {0};
    size_t action_count = 0;
    Utility utility = 0.0;
    InfoSetKey original_pair_key = -1;
    InfoSetKey resplit_pair_key = -1;

    initialize(&state);
    deal_initial_hand(game, &state, CFR_BLACKJACK_ACTION_DEAL_ACE,
                      CFR_BLACKJACK_ACTION_DEAL_TEN,
                      CFR_BLACKJACK_ACTION_DEAL_ACE,
                      CFR_BLACKJACK_ACTION_DEAL_SEVEN);
    apply(game, &state, CFR_BLACKJACK_ACTION_SPLIT);
    CHECK(state.split_hand_count == 2);
    apply(game, &state, CFR_BLACKJACK_ACTION_DEAL_TEN);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_TERMINAL);
    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), CFR_PLAYER_0,
                                    &utility) == CFR_STATUS_SUCCESS);
    CHECK(utility == 2.0);

    initialize(&state);
    deal_initial_hand(game, &state, CFR_BLACKJACK_ACTION_DEAL_TEN,
                      CFR_BLACKJACK_ACTION_DEAL_SIX,
                      CFR_BLACKJACK_ACTION_DEAL_TEN,
                      CFR_BLACKJACK_ACTION_DEAL_FIVE);
    CHECK(cfr_game_information_set_key(game, as_const_state(&state),
                                       &original_pair_key) ==
          CFR_STATUS_SUCCESS);
    apply(game, &state, CFR_BLACKJACK_ACTION_SPLIT);
    apply(game, &state, CFR_BLACKJACK_ACTION_DEAL_TEN);
    CHECK(state.split_hand_count == 2);
    CHECK(cfr_game_information_set_key(game, as_const_state(&state),
                                       &resplit_pair_key) ==
          CFR_STATUS_SUCCESS);
    CHECK(resplit_pair_key == original_pair_key);
    apply(game, &state, CFR_BLACKJACK_ACTION_SPLIT);
    CHECK(state.split_hand_count == CFR_BLACKJACK_MAX_SPLIT_HANDS);
    apply(game, &state, CFR_BLACKJACK_ACTION_DEAL_TEN);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_PLAYER_TURN);
    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions,
                                 ARRAY_COUNT(actions), &action_count) ==
          CFR_STATUS_SUCCESS);
    CHECK(action_count == 3);
    CHECK(!action_is_present(CFR_BLACKJACK_ACTION_SPLIT, actions,
                             action_count));
    apply(game, &state, CFR_BLACKJACK_ACTION_STAND);
    apply(game, &state, CFR_BLACKJACK_ACTION_DEAL_TEN);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_TERMINAL);
    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), CFR_PLAYER_0,
                                    &utility) == CFR_STATUS_SUCCESS);
    CHECK(utility == -4.0);
}

static void check_soft_seventeen_stands(void) {
    const Game *game = cfr_blackjack_descriptor();
    BlackjackState state;
    Utility utility = 12.0;
    bool terminal = false;

    initialize(&state);
    deal_initial_hand(game, &state, CFR_BLACKJACK_ACTION_DEAL_TEN,
                      CFR_BLACKJACK_ACTION_DEAL_ACE,
                      CFR_BLACKJACK_ACTION_DEAL_SEVEN,
                      CFR_BLACKJACK_ACTION_DEAL_SIX);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_PLAYER_TURN);
    apply(game, &state, CFR_BLACKJACK_ACTION_STAND);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_TERMINAL);
    CHECK(cfr_game_is_terminal(game, as_const_state(&state), &terminal) ==
          CFR_STATUS_SUCCESS);
    CHECK(terminal);
    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), CFR_PLAYER_0,
                                    &utility) == CFR_STATUS_SUCCESS);
    CHECK(utility == 0.0);
}

static void check_soft_player_transition(void) {
    const Game *game = cfr_blackjack_descriptor();
    BlackjackState soft_state;
    BlackjackState soft_before_hit;
    BlackjackState hard_state;

    initialize(&soft_state);
    deal_initial_hand(game, &soft_state, CFR_BLACKJACK_ACTION_DEAL_ACE,
                      CFR_BLACKJACK_ACTION_DEAL_FOUR,
                      CFR_BLACKJACK_ACTION_DEAL_FIVE,
                      CFR_BLACKJACK_ACTION_DEAL_TEN);
    CHECK(soft_state.player_hand.total == 16);
    CHECK(soft_state.player_hand.is_soft);
    soft_before_hit = soft_state;
    apply(game, &soft_state, CFR_BLACKJACK_ACTION_HIT);
    apply(game, &soft_state, CFR_BLACKJACK_ACTION_DEAL_TEN);
    CHECK(soft_state.phase == CFR_BLACKJACK_PHASE_PLAYER_TURN);
    CHECK(soft_state.player_hand.total == 16);
    CHECK(!soft_state.player_hand.is_soft);
    CHECK(soft_state.player_hand.card_count == 3);
    CHECK(cfr_game_undo_action(game, as_state(&soft_state)) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_game_undo_action(game, as_state(&soft_state)) ==
          CFR_STATUS_SUCCESS);
    CHECK(same_state(&soft_state, &soft_before_hit));

    initialize(&hard_state);
    deal_initial_hand(game, &hard_state, CFR_BLACKJACK_ACTION_DEAL_TEN,
                      CFR_BLACKJACK_ACTION_DEAL_FOUR,
                      CFR_BLACKJACK_ACTION_DEAL_SIX,
                      CFR_BLACKJACK_ACTION_DEAL_FIVE);
    CHECK(hard_state.player_hand.total == 16);
    CHECK(!hard_state.player_hand.is_soft);
    apply(game, &hard_state, CFR_BLACKJACK_ACTION_HIT);
    apply(game, &hard_state, CFR_BLACKJACK_ACTION_DEAL_TEN);
    CHECK(hard_state.phase == CFR_BLACKJACK_PHASE_TERMINAL);
    CHECK(hard_state.player_hand.total == 26);
}

static InfoSetKey information_key_for_hand(BlackjackAction player_first,
                                           BlackjackAction dealer_up,
                                           BlackjackAction player_second,
                                           BlackjackAction dealer_hole) {
    const Game *game = cfr_blackjack_descriptor();
    BlackjackState state;
    InfoSetKey key = -1;

    initialize(&state);
    deal_initial_hand(game, &state, player_first, dealer_up, player_second,
                      dealer_hole);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_PLAYER_TURN);
    CHECK(cfr_game_information_set_key(game, as_const_state(&state), &key) ==
          CFR_STATUS_SUCCESS);
    return key;
}

static InfoSetKey information_key_after_hit(BlackjackAction player_first,
                                            BlackjackAction dealer_up,
                                            BlackjackAction player_second,
                                            BlackjackAction dealer_hole,
                                            BlackjackAction hit_card) {
    const Game *game = cfr_blackjack_descriptor();
    BlackjackState state;
    InfoSetKey key = -1;

    initialize(&state);
    deal_initial_hand(game, &state, player_first, dealer_up, player_second,
                      dealer_hole);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_PLAYER_TURN);
    apply(game, &state, CFR_BLACKJACK_ACTION_HIT);
    apply(game, &state, hit_card);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_PLAYER_TURN);
    CHECK(cfr_game_information_set_key(game, as_const_state(&state), &key) ==
          CFR_STATUS_SUCCESS);
    return key;
}

static void check_information_sets(void) {
    const InfoSetKey first_hidden = information_key_for_hand(
        CFR_BLACKJACK_ACTION_DEAL_TEN, CFR_BLACKJACK_ACTION_DEAL_SIX,
        CFR_BLACKJACK_ACTION_DEAL_SEVEN, CFR_BLACKJACK_ACTION_DEAL_FIVE);
    const InfoSetKey second_hidden = information_key_for_hand(
        CFR_BLACKJACK_ACTION_DEAL_TEN, CFR_BLACKJACK_ACTION_DEAL_SIX,
        CFR_BLACKJACK_ACTION_DEAL_SEVEN, CFR_BLACKJACK_ACTION_DEAL_EIGHT);
    const InfoSetKey reversed_player_cards = information_key_for_hand(
        CFR_BLACKJACK_ACTION_DEAL_SEVEN, CFR_BLACKJACK_ACTION_DEAL_SIX,
        CFR_BLACKJACK_ACTION_DEAL_TEN, CFR_BLACKJACK_ACTION_DEAL_FIVE);
    const InfoSetKey same_hard_total = information_key_for_hand(
        CFR_BLACKJACK_ACTION_DEAL_FIVE, CFR_BLACKJACK_ACTION_DEAL_SIX,
        CFR_BLACKJACK_ACTION_DEAL_TEN, CFR_BLACKJACK_ACTION_DEAL_EIGHT);
    const InfoSetKey equivalent_hard_total = information_key_for_hand(
        CFR_BLACKJACK_ACTION_DEAL_SEVEN, CFR_BLACKJACK_ACTION_DEAL_SIX,
        CFR_BLACKJACK_ACTION_DEAL_EIGHT, CFR_BLACKJACK_ACTION_DEAL_FIVE);
    const InfoSetKey same_total_after_hit = information_key_after_hit(
        CFR_BLACKJACK_ACTION_DEAL_FIVE, CFR_BLACKJACK_ACTION_DEAL_SIX,
        CFR_BLACKJACK_ACTION_DEAL_TEN, CFR_BLACKJACK_ACTION_DEAL_EIGHT,
        CFR_BLACKJACK_ACTION_DEAL_TWO);
    const InfoSetKey hard_seventeen = information_key_for_hand(
        CFR_BLACKJACK_ACTION_DEAL_TEN, CFR_BLACKJACK_ACTION_DEAL_SIX,
        CFR_BLACKJACK_ACTION_DEAL_SEVEN, CFR_BLACKJACK_ACTION_DEAL_FIVE);
    const InfoSetKey soft_seventeen = information_key_for_hand(
        CFR_BLACKJACK_ACTION_DEAL_ACE, CFR_BLACKJACK_ACTION_DEAL_SIX,
        CFR_BLACKJACK_ACTION_DEAL_SIX, CFR_BLACKJACK_ACTION_DEAL_FIVE);
    const InfoSetKey hard_sixteen = information_key_for_hand(
        CFR_BLACKJACK_ACTION_DEAL_TEN, CFR_BLACKJACK_ACTION_DEAL_FOUR,
        CFR_BLACKJACK_ACTION_DEAL_SIX, CFR_BLACKJACK_ACTION_DEAL_FIVE);
    const InfoSetKey soft_sixteen = information_key_for_hand(
        CFR_BLACKJACK_ACTION_DEAL_ACE, CFR_BLACKJACK_ACTION_DEAL_FOUR,
        CFR_BLACKJACK_ACTION_DEAL_FIVE, CFR_BLACKJACK_ACTION_DEAL_SIX);
    const InfoSetKey different_up_card = information_key_for_hand(
        CFR_BLACKJACK_ACTION_DEAL_TEN, CFR_BLACKJACK_ACTION_DEAL_FIVE,
        CFR_BLACKJACK_ACTION_DEAL_SEVEN, CFR_BLACKJACK_ACTION_DEAL_SIX);

    CHECK(first_hidden == second_hidden);
    CHECK(first_hidden == reversed_player_cards);
    CHECK(first_hidden == hard_seventeen);
    CHECK(first_hidden != same_hard_total);
    CHECK(same_hard_total == equivalent_hard_total);
    CHECK(first_hidden != same_total_after_hit);
    CHECK(hard_seventeen != soft_seventeen);
    CHECK(hard_sixteen != soft_sixteen);
    CHECK(first_hidden != different_up_card);
}

static void check_draws_do_not_deplete(void) {
    const Game *game = cfr_blackjack_descriptor();
    BlackjackState state;
    Action actions[CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS] = {0};
    size_t action_count = 0;
    Probability probability = 77.0;

    initialize(&state);
    deal_initial_hand(game, &state, CFR_BLACKJACK_ACTION_DEAL_ACE,
                      CFR_BLACKJACK_ACTION_DEAL_ACE,
                      CFR_BLACKJACK_ACTION_DEAL_ACE,
                      CFR_BLACKJACK_ACTION_DEAL_ACE);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_PLAYER_TURN);
    apply(game, &state, CFR_BLACKJACK_ACTION_HIT);
    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions,
                                 ARRAY_COUNT(actions), &action_count) ==
          CFR_STATUS_SUCCESS);
    CHECK(action_count == CFR_BLACKJACK_NUMBER_OF_CARD_RANKS);
    CHECK(action_is_present(CFR_BLACKJACK_ACTION_DEAL_ACE, actions,
                            action_count));
    CHECK(cfr_game_chance_probability(
              game, as_const_state(&state), CFR_BLACKJACK_ACTION_DEAL_ACE,
              &probability) == CFR_STATUS_SUCCESS);
    CHECK(near(probability, 1.0 / 13.0));
    apply(game, &state, CFR_BLACKJACK_ACTION_DEAL_ACE);
    CHECK(state.player_hand.total == 13);
    CHECK(state.player_hand.card_count == 3);
}

static void check_undo_and_terminal_contract(void) {
    static const Action path[] = {
        CFR_BLACKJACK_ACTION_DEAL_TEN, CFR_BLACKJACK_ACTION_DEAL_SIX,
        CFR_BLACKJACK_ACTION_DEAL_EIGHT, CFR_BLACKJACK_ACTION_DEAL_TEN,
        CFR_BLACKJACK_ACTION_STAND, CFR_BLACKJACK_ACTION_DEAL_TEN};
    const Game *game = cfr_blackjack_descriptor();
    BlackjackState state;
    BlackjackState snapshots[ARRAY_COUNT(path) + 1];
    Action actions[CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS] = {81};
    size_t required_count = 82;
    Probability probability = 83.0;
    InfoSetKey key = 84;
    Actor actor = {.kind = CFR_ACTOR_CHANCE, .player = CFR_PLAYER_1};
    size_t index;

    initialize(&state);
    snapshots[0] = state;
    for (index = 0; index < ARRAY_COUNT(path); index += 1) {
        apply(game, &state, path[index]);
        snapshots[index + 1] = state;
    }
    CHECK(state.phase == CFR_BLACKJACK_PHASE_TERMINAL);
    CHECK(cfr_game_current_actor(game, as_const_state(&state), &actor) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(actor.kind == CFR_ACTOR_CHANCE && actor.player == CFR_PLAYER_1);
    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions,
                                 ARRAY_COUNT(actions), &required_count) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(actions[0] == 81 && required_count == 82);
    CHECK(cfr_game_chance_probability(
              game, as_const_state(&state), CFR_BLACKJACK_ACTION_DEAL_ACE,
              &probability) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(probability == 83.0);
    CHECK(cfr_game_information_set_key(game, as_const_state(&state), &key) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(key == 84);
    {
        const BlackjackState terminal = state;

        CHECK(cfr_game_apply_action(game, as_state(&state),
                                    CFR_BLACKJACK_ACTION_HIT) ==
              CFR_STATUS_ILLEGAL_ACTION);
        CHECK(same_state(&state, &terminal));
    }

    for (index = ARRAY_COUNT(path); index > 0; index -= 1) {
        CHECK(cfr_game_undo_action(game, as_state(&state)) ==
              CFR_STATUS_SUCCESS);
        CHECK(same_state(&state, &snapshots[index - 1]));
    }
    CHECK(cfr_game_undo_action(game, as_state(&state)) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(same_state(&state, &snapshots[0]));
}

static void check_corrupt_states_are_rejected(void) {
    const Game *game = cfr_blackjack_descriptor();
    BlackjackState valid;
    BlackjackState corrupt;
    Action actions[2] = {91, 92};
    Probability probabilities[2] = {93.0, 94.0};
    size_t required_count = 95;
    bool terminal = true;

    initialize(&valid);
    CHECK(cfr_game_validate_state(game, as_const_state(&valid)) ==
          CFR_STATUS_SUCCESS);

    corrupt = valid;
    corrupt.split_hand_count = 3;
    CHECK(cfr_game_validate_state(game, as_const_state(&corrupt)) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_chance_outcomes(game, as_const_state(&corrupt), actions,
                                   probabilities, ARRAY_COUNT(actions),
                                   &required_count) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(actions[0] == 91 && actions[1] == 92);
    CHECK(probabilities[0] == 93.0 && probabilities[1] == 94.0);
    CHECK(required_count == 95);

    corrupt = valid;
    corrupt.player_hand.total = 11;
    corrupt.player_hand.card_count = 1;
    corrupt.player_hand.is_soft = true;
    corrupt.player_hand.can_split = true;
    CHECK(cfr_game_validate_state(game, as_const_state(&corrupt)) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_is_terminal(game, as_const_state(&corrupt), &terminal) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(terminal);

    corrupt = valid;
    corrupt.phase = CFR_BLACKJACK_PHASE_PLAYER_TURN;
    CHECK(cfr_game_validate_state(game, as_const_state(&corrupt)) ==
          CFR_STATUS_INVALID_ARGUMENT);

    corrupt = valid;
    corrupt.undo_history[0].applied_action =
        CFR_BLACKJACK_ACTION_DEAL_ACE;
    CHECK(cfr_game_validate_state(game, as_const_state(&corrupt)) ==
          CFR_STATUS_INVALID_ARGUMENT);
}

static void check_trusted_operation_invariants(void) {
    static const Action path[] = {
        CFR_BLACKJACK_ACTION_DEAL_TEN, CFR_BLACKJACK_ACTION_DEAL_SIX,
        CFR_BLACKJACK_ACTION_DEAL_EIGHT, CFR_BLACKJACK_ACTION_DEAL_TEN,
        CFR_BLACKJACK_ACTION_STAND, CFR_BLACKJACK_ACTION_DEAL_TEN};
    const Game *game = cfr_blackjack_descriptor();
    Game trusted_game = *game;
    BlackjackState state;
    BlackjackState root;
    size_t index;

    CHECK(game->trusted_operations != NULL);
    CHECK(game->trusted_operations != game->operations);
    if (game->trusted_operations == NULL)
        return;

    trusted_game.operations = game->trusted_operations;
    trusted_game.trusted_operations = NULL;
    initialize(&state);
    root = state;

    for (index = 0; index < ARRAY_COUNT(path); index += 1) {
        CHECK(cfr_game_apply_action(&trusted_game, as_state(&state),
                                    path[index]) == CFR_STATUS_SUCCESS);
        CHECK(cfr_game_validate_state(game, as_const_state(&state)) ==
              CFR_STATUS_SUCCESS);
    }
    CHECK(state.phase == CFR_BLACKJACK_PHASE_TERMINAL);

    for (index = ARRAY_COUNT(path); index > 0; index -= 1) {
        CHECK(cfr_game_undo_action(&trusted_game, as_state(&state)) ==
              CFR_STATUS_SUCCESS);
        CHECK(cfr_game_validate_state(game, as_const_state(&state)) ==
              CFR_STATUS_SUCCESS);
    }
    CHECK(same_state(&state, &root));
}

static void check_trainer_compatibility(void) {
    const Game *game = cfr_blackjack_descriptor();
    BlackjackState state;
    BlackjackState root;
    InfoStore store = {0};
    Trainer trainer = {0};
    TrainerStats stats = {0};

    initialize(&state);
    deal_initial_hand(game, &state, CFR_BLACKJACK_ACTION_DEAL_TEN,
                      CFR_BLACKJACK_ACTION_DEAL_TEN,
                      CFR_BLACKJACK_ACTION_DEAL_NINE,
                      CFR_BLACKJACK_ACTION_DEAL_TEN);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_PLAYER_TURN);
    root = state;

    CHECK(cfr_info_store_init(&store) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_init(&trainer, game, as_state(&state), &store) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&trainer, 1) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_get_stats(&trainer, &stats) == CFR_STATUS_SUCCESS);
    CHECK(stats.iterations == 1);
    CHECK(stats.traversals == 1);
    CHECK(stats.visited_nodes > 0);
    CHECK(stats.errors == 0);
    CHECK(store.size > 0);
    CHECK(same_state(&state, &root));
    CHECK(cfr_info_store_destroy(&store) == CFR_STATUS_SUCCESS);
}

/*
 * The descriptor must expose a schema identifier: without it every checkpoint
 * and text-strategy operation fails only after training has already finished.
 */
static void check_persistence(void) {
    const Game *game = cfr_blackjack_descriptor();
    BlackjackState state;
    BlackjackState restored_state;
    InfoStore store = {0};
    InfoStore restored_store = {0};
    Trainer trainer = {0};
    Trainer restored_trainer = {0};
    FILE *checkpoint = tmpfile();
    FILE *text = tmpfile();
    long text_length = 0;

    CHECK(game->strategy_schema_id != NULL);
    if (game->strategy_schema_id != NULL)
        CHECK(strcmp(game->strategy_schema_id, "cfr.blackjack/v4") == 0);

    CHECK(checkpoint != NULL);
    CHECK(text != NULL);
    if (checkpoint == NULL || text == NULL) {
        if (checkpoint != NULL)
            (void)fclose(checkpoint);
        if (text != NULL)
            (void)fclose(text);
        return;
    }

    initialize(&state);
    deal_initial_hand(game, &state, CFR_BLACKJACK_ACTION_DEAL_TEN,
                      CFR_BLACKJACK_ACTION_DEAL_TEN,
                      CFR_BLACKJACK_ACTION_DEAL_NINE,
                      CFR_BLACKJACK_ACTION_DEAL_TEN);
    CHECK(cfr_info_store_init(&store) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_init(&trainer, game, as_state(&state), &store) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&trainer, 5) == CFR_STATUS_SUCCESS);
    CHECK(store.size > 0);

    CHECK(cfr_checkpoint_write(checkpoint, &trainer) == CFR_STATUS_SUCCESS);
    CHECK(fflush(checkpoint) == 0);
    rewind(checkpoint);

    initialize(&restored_state);
    deal_initial_hand(game, &restored_state, CFR_BLACKJACK_ACTION_DEAL_TEN,
                      CFR_BLACKJACK_ACTION_DEAL_TEN,
                      CFR_BLACKJACK_ACTION_DEAL_NINE,
                      CFR_BLACKJACK_ACTION_DEAL_TEN);
    CHECK(cfr_checkpoint_read(checkpoint, game, as_state(&restored_state),
                              &restored_store,
                              &restored_trainer) == CFR_STATUS_SUCCESS);
    CHECK(restored_store.size == store.size);
    CHECK(restored_trainer.training_iterations == trainer.training_iterations);
    CHECK(restored_trainer.variant == trainer.variant);

    /* The restored trainer must continue training without further setup. */
    CHECK(cfr_trainer_run(&restored_trainer, 1) == CFR_STATUS_SUCCESS);

    CHECK(cfr_strategy_write_text(text, &trainer) == CFR_STATUS_SUCCESS);
    CHECK(fflush(text) == 0);
    CHECK(fseek(text, 0, SEEK_END) == 0);
    text_length = ftell(text);
    CHECK(text_length > 0);

    CHECK(cfr_info_store_destroy(&restored_store) == CFR_STATUS_SUCCESS);
    CHECK(cfr_info_store_destroy(&store) == CFR_STATUS_SUCCESS);
    (void)fclose(checkpoint);
    (void)fclose(text);
}

int test_blackjack(void) {
    failures = 0;

    check_root_and_distribution();
    check_fixed_distribution_and_player_turn();
    check_terminal_utilities();
    check_double_down();
    check_split_round_and_undo();
    check_split_aces_and_resplit_limit();
    check_soft_seventeen_stands();
    check_soft_player_transition();
    check_information_sets();
    check_draws_do_not_deplete();
    check_undo_and_terminal_contract();
    check_corrupt_states_are_rejected();
    check_trusted_operation_invariants();
    check_trainer_compatibility();
    check_persistence();

    return failures;
}

#ifdef CFR_TEST_BLACKJACK_STANDALONE
int main(void) {
    const int result = test_blackjack();

    if (result != 0) {
        fprintf(stderr, "%d blackjack checks failed.\n", result);
        return 1;
    }

    puts("All blackjack tests passed.");
    return 0;
}
#endif
