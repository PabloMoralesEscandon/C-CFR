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

static bool same_state(const BlackjackState *left,
                       const BlackjackState *right) {
    size_t index;

    if (left->phase != right->phase ||
        left->player_card_count != right->player_card_count ||
        left->dealer_card_count != right->dealer_card_count ||
        left->undo_count != right->undo_count) {
        return false;
    }
    for (index = 0; index < CFR_BLACKJACK_HAND_CAPACITY; index += 1) {
        if (left->player_cards[index] != right->player_cards[index] ||
            left->dealer_cards[index] != right->dealer_cards[index]) {
            return false;
        }
    }
    for (index = 0; index < CFR_BLACKJACK_NUMBER_OF_CARD_RANKS; index += 1) {
        if (left->remaining_cards[index] != right->remaining_cards[index])
            return false;
    }
    for (index = 0; index < CFR_BLACKJACK_UNDO_HISTORY_CAPACITY; index += 1) {
        if (left->undo_history[index].previous_phase !=
                right->undo_history[index].previous_phase ||
            left->undo_history[index].applied_action !=
                right->undo_history[index].applied_action) {
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
    size_t required_count = 81;
    Probability probability_sum = 0.0;
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
    CHECK(strcmp(game->strategy_schema_id, "cfr.blackjack/v1") == 0);
    CHECK(game->operations->is_terminal != NULL);
    CHECK(game->operations->terminal_utility != NULL);
    CHECK(game->operations->current_actor != NULL);
    CHECK(game->operations->legal_actions != NULL);
    CHECK(game->operations->apply_action != NULL);
    CHECK(game->operations->undo_action != NULL);
    CHECK(game->operations->chance_probability != NULL);
    CHECK(game->operations->information_set_key != NULL);

    initialize(&state);
    CHECK(cfr_blackjack_state_as_game_state(&state) == (GameState *)&state);
    CHECK(cfr_blackjack_state_as_game_state_const(&state) ==
          (const GameState *)&state);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST);
    CHECK(state.player_card_count == 0);
    CHECK(state.dealer_card_count == 0);
    CHECK(state.undo_count == 0);
    CHECK(state.remaining_cards[0] == 4);
    CHECK(state.remaining_cards[9] == 16);

    CHECK(cfr_game_is_terminal(game, as_const_state(&state), &terminal) ==
          CFR_STATUS_SUCCESS);
    CHECK(!terminal);
    CHECK(cfr_game_current_actor(game, as_const_state(&state), &actor) ==
          CFR_STATUS_SUCCESS);
    CHECK(actor.kind == CFR_ACTOR_CHANCE);

    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions,
                                 ARRAY_COUNT(actions) - 1, &required_count) ==
          CFR_STATUS_BUFFER_TOO_SMALL);
    CHECK(required_count == CFR_BLACKJACK_NUMBER_OF_CARD_RANKS);
    for (index = 0; index < ARRAY_COUNT(actions); index += 1)
        CHECK(actions[index] == (Action)(71 + index));

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
            CHECK(near(probability, 16.0 / 52.0));
        else
            CHECK(near(probability, 4.0 / 52.0));
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

static void check_depletion_and_player_turn(void) {
    const Game *game = cfr_blackjack_descriptor();
    BlackjackState state;
    Action actions[CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS] = {0};
    size_t action_count = 0;
    Probability probability = -1.0;
    Actor actor = {.kind = CFR_ACTOR_CHANCE, .player = CFR_PLAYER_1};

    initialize(&state);
    apply(game, &state, CFR_BLACKJACK_ACTION_DEAL_TEN);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_DEAL_DEALER_UP_CARD);
    CHECK(state.player_cards[0] == CFR_BLACKJACK_CARD_TEN);
    CHECK(state.remaining_cards[9] == 15);
    CHECK(cfr_game_chance_probability(
              game, as_const_state(&state), CFR_BLACKJACK_ACTION_DEAL_TEN,
              &probability) == CFR_STATUS_SUCCESS);
    CHECK(near(probability, 15.0 / 51.0));

    apply(game, &state, CFR_BLACKJACK_ACTION_DEAL_SIX);
    apply(game, &state, CFR_BLACKJACK_ACTION_DEAL_SEVEN);
    apply(game, &state, CFR_BLACKJACK_ACTION_DEAL_TEN);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_PLAYER_TURN);
    CHECK(state.player_card_count == 2);
    CHECK(state.dealer_card_count == 2);
    CHECK(cfr_game_current_actor(game, as_const_state(&state), &actor) ==
          CFR_STATUS_SUCCESS);
    CHECK(actor.kind == CFR_ACTOR_PLAYER);
    CHECK(actor.player == CFR_PLAYER_0);
    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions,
                                 ARRAY_COUNT(actions), &action_count) ==
          CFR_STATUS_SUCCESS);
    CHECK(action_count == 2);
    CHECK(actions[0] == CFR_BLACKJACK_ACTION_HIT);
    CHECK(actions[1] == CFR_BLACKJACK_ACTION_STAND);
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
    const InfoSetKey different_up_card = information_key_for_hand(
        CFR_BLACKJACK_ACTION_DEAL_TEN, CFR_BLACKJACK_ACTION_DEAL_FIVE,
        CFR_BLACKJACK_ACTION_DEAL_SEVEN, CFR_BLACKJACK_ACTION_DEAL_SIX);

    CHECK(first_hidden == second_hidden);
    CHECK(first_hidden != reversed_player_cards);
    CHECK(first_hidden != different_up_card);
}

static void check_exhausted_rank(void) {
    const Game *game = cfr_blackjack_descriptor();
    BlackjackState state;
    BlackjackState snapshot;
    Action actions[CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS] = {0};
    size_t action_count = 0;
    Probability probability = 77.0;

    initialize(&state);
    deal_initial_hand(game, &state, CFR_BLACKJACK_ACTION_DEAL_ACE,
                      CFR_BLACKJACK_ACTION_DEAL_ACE,
                      CFR_BLACKJACK_ACTION_DEAL_ACE,
                      CFR_BLACKJACK_ACTION_DEAL_ACE);
    CHECK(state.phase == CFR_BLACKJACK_PHASE_PLAYER_TURN);
    CHECK(state.remaining_cards[0] == 0);
    apply(game, &state, CFR_BLACKJACK_ACTION_HIT);
    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions,
                                 ARRAY_COUNT(actions), &action_count) ==
          CFR_STATUS_SUCCESS);
    CHECK(action_count == 9);
    CHECK(!action_is_present(CFR_BLACKJACK_ACTION_DEAL_ACE, actions,
                             action_count));
    CHECK(cfr_game_chance_probability(
              game, as_const_state(&state), CFR_BLACKJACK_ACTION_DEAL_ACE,
              &probability) == CFR_STATUS_ILLEGAL_ACTION);
    CHECK(probability == 77.0);
    snapshot = state;
    CHECK(cfr_game_apply_action(game, as_state(&state),
                                CFR_BLACKJACK_ACTION_DEAL_ACE) ==
          CFR_STATUS_ILLEGAL_ACTION);
    CHECK(same_state(&state, &snapshot));
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
    bool terminal = true;

    initialize(&valid);

    corrupt = valid;
    corrupt.remaining_cards[0] = 3;
    CHECK(cfr_game_is_terminal(game, as_const_state(&corrupt), &terminal) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(terminal);

    corrupt = valid;
    corrupt.player_cards[0] = CFR_BLACKJACK_CARD_ACE;
    CHECK(cfr_game_is_terminal(game, as_const_state(&corrupt), &terminal) ==
          CFR_STATUS_INVALID_ARGUMENT);

    corrupt = valid;
    corrupt.phase = CFR_BLACKJACK_PHASE_PLAYER_TURN;
    CHECK(cfr_game_is_terminal(game, as_const_state(&corrupt), &terminal) ==
          CFR_STATUS_INVALID_ARGUMENT);

    corrupt = valid;
    corrupt.undo_history[0].applied_action =
        CFR_BLACKJACK_ACTION_DEAL_ACE;
    CHECK(cfr_game_is_terminal(game, as_const_state(&corrupt), &terminal) ==
          CFR_STATUS_INVALID_ARGUMENT);
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
                      CFR_BLACKJACK_ACTION_DEAL_TEN,
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
    CHECK(store.size == 2);
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
        CHECK(strcmp(game->strategy_schema_id, "cfr.blackjack/v1") == 0);

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
                      CFR_BLACKJACK_ACTION_DEAL_TEN,
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
                      CFR_BLACKJACK_ACTION_DEAL_TEN,
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
    check_depletion_and_player_turn();
    check_terminal_utilities();
    check_soft_seventeen_stands();
    check_information_sets();
    check_exhausted_rank();
    check_undo_and_terminal_contract();
    check_corrupt_states_are_rejected();
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
