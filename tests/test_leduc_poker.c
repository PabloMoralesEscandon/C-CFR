#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cfr/checkpoint.h"
#include "cfr/evaluation.h"
#include "cfr/info_store.h"
#include "cfr/leduc_poker.h"
#include "cfr/trainer.h"
#include "test_suite.h"

static int failures;

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__,       \
                    __LINE__, #condition);                                     \
            failures += 1;                                                     \
        }                                                                      \
    } while (0)

static GameState *as_state(LeducPokerState *state) {
    return cfr_leduc_poker_state_as_game_state(state);
}

static const GameState *as_const_state(const LeducPokerState *state) {
    return cfr_leduc_poker_state_as_game_state_const(state);
}

static bool near(double left, double right, double tolerance) {
    return fabs(left - right) <= tolerance;
}

static void initialize(LeducPokerState *state) {
    *state = (LeducPokerState){0};
    CHECK(cfr_leduc_poker_state_init(state) == CFR_STATUS_SUCCESS);
}

static void apply(const Game *game, LeducPokerState *state,
                  LeducPokerAction action) {
    CHECK(cfr_game_apply_action(game, as_state(state), action) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_game_validate_state(game, as_const_state(state)) ==
          CFR_STATUS_SUCCESS);
}

static bool contains_action(const Action *actions, size_t count,
                            LeducPokerAction expected) {
    size_t index;

    for (index = 0; index < count; index++) {
        if (actions[index] == (Action)expected)
            return true;
    }
    return false;
}

static void check_root_and_private_chance(void) {
    const Game *game = cfr_leduc_poker_descriptor();
    LeducPokerState state;
    Action actions[CFR_LEDUC_POKER_MAX_POSSIBLE_ACTIONS] = {0};
    Probability probabilities[CFR_LEDUC_POKER_MAX_POSSIBLE_ACTIONS] = {0};
    size_t count = 0;
    Probability sum = 0.0;
    Actor actor = {.kind = CFR_ACTOR_PLAYER, .player = CFR_PLAYER_1};
    bool terminal = true;
    size_t index;

    initialize(&state);
    CHECK(game != NULL);
    CHECK(game->strategic_player_count == 2);
    CHECK(game->max_legal_actions == 9);
    CHECK(strcmp(game->strategy_schema_id, "cfr.leduc-poker/v1") == 0);
    CHECK(game->operations != NULL);
    CHECK(game->operations->validate_state != NULL);
    CHECK(game->operations->chance_outcomes != NULL);
    CHECK(game->trusted_operations != NULL);
    CHECK(state.phase == CFR_LEDUC_POKER_PHASE_PRIVATE_DEAL);
    CHECK(state.private_cards[0] == CFR_LEDUC_POKER_CARD_NOT_DEALT);
    CHECK(state.private_cards[1] == CFR_LEDUC_POKER_CARD_NOT_DEALT);
    CHECK(state.public_card == CFR_LEDUC_POKER_CARD_NOT_DEALT);
    CHECK(state.contributions[0] == 1);
    CHECK(state.contributions[1] == 1);
    CHECK(cfr_game_is_terminal(game, as_const_state(&state), &terminal) ==
          CFR_STATUS_SUCCESS);
    CHECK(!terminal);
    CHECK(cfr_game_current_actor(game, as_const_state(&state), &actor) ==
          CFR_STATUS_SUCCESS);
    CHECK(actor.kind == CFR_ACTOR_CHANCE);
    CHECK(cfr_game_chance_outcomes(
              game, as_const_state(&state), actions, probabilities,
              ARRAY_COUNT(actions), &count) == CFR_STATUS_SUCCESS);
    CHECK(count == 9);
    for (index = 0; index < count; index++) {
        const bool same_rank = index == 0 || index == 4 || index == 8;
        const Probability expected = same_rank ? 1.0 / 15.0 : 2.0 / 15.0;

        CHECK(actions[index] ==
              (Action)(CFR_LEDUC_POKER_ACTION_DEAL_JJ + (Action)index));
        CHECK(near(probabilities[index], expected, 1e-15));
        sum += probabilities[index];
    }
    CHECK(near(sum, 1.0, 1e-15));

    count = 91;
    actions[0] = 81;
    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions, 1,
                                 &count) == CFR_STATUS_BUFFER_TOO_SMALL);
    CHECK(count == 9);
    CHECK(actions[0] == 81);
}

static void check_round_transitions_and_public_chance(void) {
    const Game *game = cfr_leduc_poker_descriptor();
    LeducPokerState state;
    Action actions[CFR_LEDUC_POKER_MAX_POSSIBLE_ACTIONS] = {0};
    Probability probabilities[CFR_LEDUC_POKER_MAX_POSSIBLE_ACTIONS] = {0};
    size_t count = 0;
    Actor actor = {0};

    initialize(&state);
    apply(game, &state, CFR_LEDUC_POKER_ACTION_DEAL_JJ);
    CHECK(state.phase == CFR_LEDUC_POKER_PHASE_FIRST_BETTING);
    CHECK(state.private_cards[0] == CFR_LEDUC_POKER_CARD_JACK);
    CHECK(state.private_cards[1] == CFR_LEDUC_POKER_CARD_JACK);
    CHECK(cfr_game_current_actor(game, as_const_state(&state), &actor) ==
          CFR_STATUS_SUCCESS);
    CHECK(actor.kind == CFR_ACTOR_PLAYER && actor.player == CFR_PLAYER_0);
    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions,
                                 ARRAY_COUNT(actions), &count) ==
          CFR_STATUS_SUCCESS);
    CHECK(count == 2);
    CHECK(actions[0] == CFR_LEDUC_POKER_ACTION_CHECK);
    CHECK(actions[1] == CFR_LEDUC_POKER_ACTION_BET);

    apply(game, &state, CFR_LEDUC_POKER_ACTION_CHECK);
    CHECK(state.current_player == CFR_PLAYER_1);
    apply(game, &state, CFR_LEDUC_POKER_ACTION_CHECK);
    CHECK(state.phase == CFR_LEDUC_POKER_PHASE_PUBLIC_DEAL);
    CHECK(state.round_start_index == 2);
    CHECK(cfr_game_chance_outcomes(
              game, as_const_state(&state), actions, probabilities,
              ARRAY_COUNT(actions), &count) == CFR_STATUS_SUCCESS);
    CHECK(count == 2);
    CHECK(actions[0] == CFR_LEDUC_POKER_ACTION_REVEAL_Q);
    CHECK(actions[1] == CFR_LEDUC_POKER_ACTION_REVEAL_K);
    CHECK(near(probabilities[0], 0.5, 1e-15));
    CHECK(near(probabilities[1], 0.5, 1e-15));
    CHECK(cfr_game_apply_action(game, as_state(&state),
                                CFR_LEDUC_POKER_ACTION_REVEAL_J) ==
          CFR_STATUS_ILLEGAL_ACTION);

    apply(game, &state, CFR_LEDUC_POKER_ACTION_REVEAL_Q);
    CHECK(state.phase == CFR_LEDUC_POKER_PHASE_SECOND_BETTING);
    CHECK(state.current_player == CFR_PLAYER_0);
    CHECK(state.public_card == CFR_LEDUC_POKER_CARD_QUEEN);
    CHECK(state.contributions[0] == 1);
    CHECK(state.contributions[1] == 1);
}

static void check_betting_cap_and_fold_utility(void) {
    const Game *game = cfr_leduc_poker_descriptor();
    LeducPokerState state;
    Action actions[CFR_LEDUC_POKER_MAX_POSSIBLE_ACTIONS] = {0};
    size_t count = 0;
    Utility utility0 = 0.0;
    Utility utility1 = 0.0;
    bool terminal = false;

    initialize(&state);
    apply(game, &state, CFR_LEDUC_POKER_ACTION_DEAL_QK);
    apply(game, &state, CFR_LEDUC_POKER_ACTION_BET);
    CHECK(state.contributions[0] == 3);
    CHECK(state.contributions[1] == 1);
    CHECK(state.aggressive_action_count == 1);
    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions,
                                 ARRAY_COUNT(actions), &count) ==
          CFR_STATUS_SUCCESS);
    CHECK(count == 3);
    CHECK(contains_action(actions, count, CFR_LEDUC_POKER_ACTION_FOLD));
    CHECK(contains_action(actions, count, CFR_LEDUC_POKER_ACTION_CALL));
    CHECK(contains_action(actions, count, CFR_LEDUC_POKER_ACTION_RAISE));

    apply(game, &state, CFR_LEDUC_POKER_ACTION_RAISE);
    CHECK(state.contributions[0] == 3);
    CHECK(state.contributions[1] == 5);
    CHECK(state.aggressive_action_count == 2);
    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions,
                                 ARRAY_COUNT(actions), &count) ==
          CFR_STATUS_SUCCESS);
    CHECK(count == 2);
    CHECK(actions[0] == CFR_LEDUC_POKER_ACTION_FOLD);
    CHECK(actions[1] == CFR_LEDUC_POKER_ACTION_CALL);
    CHECK(cfr_game_apply_action(game, as_state(&state),
                                CFR_LEDUC_POKER_ACTION_RAISE) ==
          CFR_STATUS_ILLEGAL_ACTION);
    apply(game, &state, CFR_LEDUC_POKER_ACTION_FOLD);
    CHECK(cfr_game_is_terminal(game, as_const_state(&state), &terminal) ==
          CFR_STATUS_SUCCESS);
    CHECK(terminal);
    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), CFR_PLAYER_0,
                                    &utility0) == CFR_STATUS_SUCCESS);
    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), CFR_PLAYER_1,
                                    &utility1) == CFR_STATUS_SUCCESS);
    CHECK(utility0 == -3.0);
    CHECK(utility1 == 3.0);
}

static void check_showdowns(void) {
    const Game *game = cfr_leduc_poker_descriptor();
    LeducPokerState state;
    Utility utility = 0.0;

    /* A public-card pair beats a higher unpaired private card. */
    initialize(&state);
    apply(game, &state, CFR_LEDUC_POKER_ACTION_DEAL_QK);
    apply(game, &state, CFR_LEDUC_POKER_ACTION_CHECK);
    apply(game, &state, CFR_LEDUC_POKER_ACTION_CHECK);
    apply(game, &state, CFR_LEDUC_POKER_ACTION_REVEAL_Q);
    apply(game, &state, CFR_LEDUC_POKER_ACTION_CHECK);
    apply(game, &state, CFR_LEDUC_POKER_ACTION_CHECK);
    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), CFR_PLAYER_0,
                                    &utility) == CFR_STATUS_SUCCESS);
    CHECK(utility == 1.0);

    /* Equal private ranks tie when neither player pairs the public card. */
    initialize(&state);
    apply(game, &state, CFR_LEDUC_POKER_ACTION_DEAL_KK);
    apply(game, &state, CFR_LEDUC_POKER_ACTION_CHECK);
    apply(game, &state, CFR_LEDUC_POKER_ACTION_CHECK);
    apply(game, &state, CFR_LEDUC_POKER_ACTION_REVEAL_J);
    apply(game, &state, CFR_LEDUC_POKER_ACTION_BET);
    apply(game, &state, CFR_LEDUC_POKER_ACTION_CALL);
    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), CFR_PLAYER_0,
                                    &utility) == CFR_STATUS_SUCCESS);
    CHECK(utility == 0.0);
    CHECK(state.contributions[0] == 5);
    CHECK(state.contributions[1] == 5);
}

static void check_information_sets_hide_opponent_card(void) {
    const Game *game = cfr_leduc_poker_descriptor();
    LeducPokerState first;
    LeducPokerState second;
    InfoSetKey first_key = -1;
    InfoSetKey second_key = -2;

    initialize(&first);
    initialize(&second);
    apply(game, &first, CFR_LEDUC_POKER_ACTION_DEAL_JQ);
    apply(game, &second, CFR_LEDUC_POKER_ACTION_DEAL_JK);
    CHECK(cfr_game_information_set_key(game, as_const_state(&first),
                                       &first_key) == CFR_STATUS_SUCCESS);
    CHECK(cfr_game_information_set_key(game, as_const_state(&second),
                                       &second_key) == CFR_STATUS_SUCCESS);
    CHECK(first_key == second_key);

    apply(game, &first, CFR_LEDUC_POKER_ACTION_CHECK);
    apply(game, &second, CFR_LEDUC_POKER_ACTION_CHECK);
    CHECK(cfr_game_information_set_key(game, as_const_state(&first),
                                       &first_key) == CFR_STATUS_SUCCESS);
    CHECK(cfr_game_information_set_key(game, as_const_state(&second),
                                       &second_key) == CFR_STATUS_SUCCESS);
    CHECK(first_key != second_key); /* Player one's private cards differ. */
}

static void check_undo_and_invalid_state(void) {
    const Game *game = cfr_leduc_poker_descriptor();
    const LeducPokerAction path[] = {
        CFR_LEDUC_POKER_ACTION_DEAL_JK, CFR_LEDUC_POKER_ACTION_CHECK,
        CFR_LEDUC_POKER_ACTION_BET,     CFR_LEDUC_POKER_ACTION_CALL,
        CFR_LEDUC_POKER_ACTION_REVEAL_Q,
        CFR_LEDUC_POKER_ACTION_BET, CFR_LEDUC_POKER_ACTION_RAISE,
        CFR_LEDUC_POKER_ACTION_CALL,
    };
    LeducPokerState state;
    LeducPokerState invalid;
    size_t index;

    initialize(&state);
    for (index = 0; index < ARRAY_COUNT(path); index++)
        apply(game, &state, path[index]);
    CHECK(state.phase == CFR_LEDUC_POKER_PHASE_TERMINAL);
    for (index = ARRAY_COUNT(path); index > 0; index--) {
        CHECK(cfr_game_undo_action(game, as_state(&state)) ==
              CFR_STATUS_SUCCESS);
        CHECK(cfr_game_validate_state(game, as_const_state(&state)) ==
              CFR_STATUS_SUCCESS);
    }
    CHECK(state.phase == CFR_LEDUC_POKER_PHASE_PRIVATE_DEAL);
    CHECK(state.undo_count == 0);
    CHECK(state.public_action_count == 0);
    CHECK(state.private_cards[0] == CFR_LEDUC_POKER_CARD_NOT_DEALT);
    CHECK(state.private_cards[1] == CFR_LEDUC_POKER_CARD_NOT_DEALT);
    CHECK(state.contributions[0] == 1 && state.contributions[1] == 1);
    CHECK(cfr_game_undo_action(game, as_state(&state)) ==
          CFR_STATUS_INVALID_ARGUMENT);

    invalid = state;
    invalid.contributions[0] = 99;
    CHECK(cfr_game_validate_state(game, as_const_state(&invalid)) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_apply_action(game, as_state(&invalid),
                                CFR_LEDUC_POKER_ACTION_DEAL_JQ) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(invalid.contributions[0] == 99);
}

static void check_training_evaluation_and_checkpoint(void) {
    const Game *game = cfr_leduc_poker_descriptor();
    LeducPokerState state;
    LeducPokerState loaded_state;
    InfoStore store = {0};
    InfoStore loaded_store = {0};
    InfoStoreStats store_stats = {0};
    Trainer trainer = {0};
    Trainer loaded_trainer = {0};
    EvaluationMetrics metrics = {0};
    EvaluationMetrics loaded_metrics = {0};
    FILE *checkpoint = tmpfile();
    FILE *strategy = tmpfile();

    initialize(&state);
    initialize(&loaded_state);
    CHECK(checkpoint != NULL);
    CHECK(strategy != NULL);
    CHECK(cfr_info_store_init(&store) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_init_plus(&trainer, game, as_state(&state), &store) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&trainer, 3) == CFR_STATUS_SUCCESS);
    CHECK(state.phase == CFR_LEDUC_POKER_PHASE_PRIVATE_DEAL);
    CHECK(cfr_info_store_get_stats(&store, &store_stats) == CFR_STATUS_SUCCESS);
    CHECK(store_stats.size > 12);
    CHECK(cfr_evaluation_metrics(game, as_state(&state), &store, &metrics) ==
          CFR_STATUS_SUCCESS);
    CHECK(isfinite(metrics.profile_value_player_0));
    CHECK(isfinite(metrics.exploitability));
    CHECK(metrics.exploitability >= 0.0);

    if (checkpoint != NULL) {
        CHECK(cfr_checkpoint_write(checkpoint, &trainer) == CFR_STATUS_SUCCESS);
        CHECK(fflush(checkpoint) == 0);
        rewind(checkpoint);
        CHECK(cfr_checkpoint_read(checkpoint, game, as_state(&loaded_state),
                                  &loaded_store, &loaded_trainer) ==
              CFR_STATUS_SUCCESS);
        CHECK(loaded_trainer.training_iterations == 3);
        CHECK(loaded_trainer.variant == CFR_TRAINER_VARIANT_CFR_PLUS);
        CHECK(cfr_evaluation_metrics(game, as_state(&loaded_state),
                                     &loaded_store, &loaded_metrics) ==
              CFR_STATUS_SUCCESS);
        CHECK(metrics.profile_value_player_0 ==
              loaded_metrics.profile_value_player_0);
        CHECK(metrics.exploitability == loaded_metrics.exploitability);
        CHECK(cfr_info_store_destroy(&loaded_store) == CFR_STATUS_SUCCESS);
        CHECK(fclose(checkpoint) == 0);
    }
    if (strategy != NULL) {
        char text[128] = {0};

        CHECK(cfr_strategy_write_text(strategy, &trainer) ==
              CFR_STATUS_SUCCESS);
        CHECK(fflush(strategy) == 0);
        rewind(strategy);
        CHECK(fgets(text, sizeof(text), strategy) != NULL);
        CHECK(strstr(text, "schema=cfr.leduc-poker/v1") != NULL);
        CHECK(strstr(text, "variant=cfr-plus") != NULL);
        CHECK(fclose(strategy) == 0);
    }
    CHECK(cfr_info_store_destroy(&store) == CFR_STATUS_SUCCESS);
}

int test_leduc_poker(void) {
    failures = 0;
    check_root_and_private_chance();
    check_round_transitions_and_public_chance();
    check_betting_cap_and_fold_utility();
    check_showdowns();
    check_information_sets_hide_opponent_card();
    check_undo_and_invalid_state();
    check_training_evaluation_and_checkpoint();
    return failures;
}
