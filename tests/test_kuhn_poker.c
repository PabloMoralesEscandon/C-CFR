#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "cfr/info_node.h"
#include "cfr/info_store.h"
#include "cfr/kuhn_poker.h"
#include "cfr/trainer.h"
#include "test_suite.h"

static int failures;

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: no se cumple: %s\n", __FILE__, __LINE__,   \
                    #condition);                                               \
            failures += 1;                                                     \
        }                                                                      \
    } while (0)

static GameState *as_state(KuhnPokerState *state) {
    return cfr_kuhn_poker_state_as_game_state(state);
}

static const GameState *as_const_state(const KuhnPokerState *state) {
    return cfr_kuhn_poker_state_as_game_state_const(state);
}

static bool near(Utility left, Utility right, Utility tolerance) {
    return fabs(left - right) <= tolerance;
}

static bool same_state(const KuhnPokerState *left,
                       const KuhnPokerState *right) {
    size_t index;
    size_t player;

    if (left->phase != right->phase ||
        left->public_action_count != right->public_action_count ||
        left->undo_count != right->undo_count) {
        return false;
    }
    for (player = 0; player < CFR_KUHN_POKER_NUMBER_OF_PLAYERS; player += 1) {
        if (left->cards[player] != right->cards[player]) {
            return false;
        }
    }
    for (index = 0; index < CFR_KUHN_POKER_PUBLIC_HISTORY_CAPACITY;
         index += 1) {
        if (left->public_actions[index] != right->public_actions[index]) {
            return false;
        }
    }
    for (index = 0; index < CFR_KUHN_POKER_UNDO_HISTORY_CAPACITY; index += 1) {
        const KuhnPokerUndoEntry *left_entry = &left->undo_history[index];
        const KuhnPokerUndoEntry *right_entry = &right->undo_history[index];

        if (left_entry->previous_phase != right_entry->previous_phase ||
            left_entry->previous_public_action_count !=
                right_entry->previous_public_action_count ||
            left_entry->applied_action != right_entry->applied_action) {
            return false;
        }
        for (player = 0; player < CFR_KUHN_POKER_NUMBER_OF_PLAYERS;
             player += 1) {
            if (left_entry->previous_cards[player] !=
                right_entry->previous_cards[player]) {
                return false;
            }
        }
    }
    return true;
}

static void initialize(KuhnPokerState *state) {
    *state = (KuhnPokerState){0};
    CHECK(cfr_kuhn_poker_state_init(state) == CFR_STATUS_SUCCESS);
}

static bool action_is_present(Action action, const Action *actions,
                              size_t action_count) {
    size_t index;

    for (index = 0; index < action_count; index += 1) {
        if (actions[index] == action) {
            return true;
        }
    }
    return false;
}

static KuhnPokerAction deal_for_cards(KuhnPokerCard player0,
                                      KuhnPokerCard player1) {
    if (player0 == CFR_KUHN_POKER_CARD_JACK &&
        player1 == CFR_KUHN_POKER_CARD_QUEEN)
        return CFR_KUHN_POKER_ACTION_JQ;
    if (player0 == CFR_KUHN_POKER_CARD_JACK &&
        player1 == CFR_KUHN_POKER_CARD_KING)
        return CFR_KUHN_POKER_ACTION_JK;
    if (player0 == CFR_KUHN_POKER_CARD_QUEEN &&
        player1 == CFR_KUHN_POKER_CARD_JACK)
        return CFR_KUHN_POKER_ACTION_QJ;
    if (player0 == CFR_KUHN_POKER_CARD_QUEEN &&
        player1 == CFR_KUHN_POKER_CARD_KING)
        return CFR_KUHN_POKER_ACTION_QK;
    if (player0 == CFR_KUHN_POKER_CARD_KING &&
        player1 == CFR_KUHN_POKER_CARD_JACK)
        return CFR_KUHN_POKER_ACTION_KJ;
    return CFR_KUHN_POKER_ACTION_KQ;
}

static void check_root_and_descriptor(void) {
    const Game *game = cfr_kuhn_poker_descriptor();
    const KuhnPokerAction expected_deals[] = {
        CFR_KUHN_POKER_ACTION_JQ, CFR_KUHN_POKER_ACTION_JK,
        CFR_KUHN_POKER_ACTION_QJ, CFR_KUHN_POKER_ACTION_QK,
        CFR_KUHN_POKER_ACTION_KJ, CFR_KUHN_POKER_ACTION_KQ,
    };
    KuhnPokerState state;
    KuhnPokerState snapshot;
    Action actions[CFR_KUHN_POKER_MAX_POSSIBLE_ACTIONS] = {91, 92, 93,
                                                           94, 95, 96};
    size_t required_count = 97;
    Probability total_probability = 0.0;
    size_t index;
    bool terminal = true;
    Actor actor = {.kind = CFR_ACTOR_PLAYER, .player = CFR_PLAYER_1};
    Utility utility = 81.0;
    InfoSetKey key = 82;

    CHECK(cfr_kuhn_poker_state_init(NULL) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(game != NULL);
    CHECK(game->operations != NULL);
    CHECK(game->context == NULL);
    CHECK(game->strategic_player_count == 2);
    CHECK(game->max_legal_actions == CFR_KUHN_POKER_MAX_POSSIBLE_ACTIONS);
    CHECK(game->operations->is_terminal != NULL);
    CHECK(game->operations->terminal_utility != NULL);
    CHECK(game->operations->current_actor != NULL);
    CHECK(game->operations->legal_actions != NULL);
    CHECK(game->operations->apply_action != NULL);
    CHECK(game->operations->undo_action != NULL);
    CHECK(game->operations->chance_probability != NULL);
    CHECK(game->operations->information_set_key != NULL);

    initialize(&state);
    CHECK(cfr_kuhn_poker_state_as_game_state(&state) == (GameState *)&state);
    CHECK(cfr_kuhn_poker_state_as_game_state_const(&state) ==
          (const GameState *)&state);
    CHECK(state.phase == CFR_KUHN_POKER_PHASE_CHANCE);
    CHECK(state.cards[0] == CFR_KUHN_POKER_CARD_NOT_DEALT);
    CHECK(state.cards[1] == CFR_KUHN_POKER_CARD_NOT_DEALT);
    CHECK(state.public_action_count == 0);
    CHECK(state.undo_count == 0);

    CHECK(cfr_game_is_terminal(game, as_const_state(&state), &terminal) ==
          CFR_STATUS_SUCCESS);
    CHECK(!terminal);
    CHECK(cfr_game_current_actor(game, as_const_state(&state), &actor) ==
          CFR_STATUS_SUCCESS);
    CHECK(actor.kind == CFR_ACTOR_CHANCE);

    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions, 5,
                                 &required_count) ==
          CFR_STATUS_BUFFER_TOO_SMALL);
    CHECK(required_count == ARRAY_COUNT(expected_deals));
    for (index = 0; index < ARRAY_COUNT(actions); index += 1) {
        CHECK(actions[index] == (Action)(91 + index));
    }

    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions,
                                 ARRAY_COUNT(actions),
                                 &required_count) == CFR_STATUS_SUCCESS);
    CHECK(required_count == ARRAY_COUNT(expected_deals));
    for (index = 0; index < required_count; index += 1) {
        Probability probability = -1.0;

        CHECK(actions[index] == (Action)expected_deals[index]);
        CHECK(cfr_game_chance_probability(game, as_const_state(&state),
                                          actions[index],
                                          &probability) == CFR_STATUS_SUCCESS);
        CHECK(near(probability, 1.0 / 6.0, 1e-15));
        total_probability += probability;
    }
    CHECK(near(total_probability, 1.0, 1e-15));

    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), CFR_PLAYER_0,
                                    &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(utility == 81.0);
    CHECK(cfr_game_information_set_key(game, as_const_state(&state), &key) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(key == 82);

    snapshot = state;
    CHECK(cfr_game_apply_action(game, as_state(&state),
                                CFR_KUHN_POKER_ACTION_CHECK) ==
          CFR_STATUS_ILLEGAL_ACTION);
    CHECK(same_state(&state, &snapshot));
    CHECK(cfr_game_undo_action(game, as_state(&state)) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(same_state(&state, &snapshot));

    {
        Probability probability = 83.0;

        CHECK(cfr_game_chance_probability(
                  game, as_const_state(&state), CFR_KUHN_POKER_ACTION_CHECK,
                  &probability) == CFR_STATUS_ILLEGAL_ACTION);
        CHECK(probability == 83.0);
    }
}

static void expected_phase_actions(KuhnPokerPhase phase,
                                   const Action **expected,
                                   size_t *expected_count) {
    static const Action deals[] = {
        CFR_KUHN_POKER_ACTION_JQ, CFR_KUHN_POKER_ACTION_JK,
        CFR_KUHN_POKER_ACTION_QJ, CFR_KUHN_POKER_ACTION_QK,
        CFR_KUHN_POKER_ACTION_KJ, CFR_KUHN_POKER_ACTION_KQ};
    static const Action open[] = {CFR_KUHN_POKER_ACTION_CHECK,
                                  CFR_KUHN_POKER_ACTION_BET};
    static const Action response[] = {CFR_KUHN_POKER_ACTION_FOLD,
                                      CFR_KUHN_POKER_ACTION_CALL};

    switch (phase) {
    case CFR_KUHN_POKER_PHASE_CHANCE:
        *expected = deals;
        *expected_count = ARRAY_COUNT(deals);
        break;
    case CFR_KUHN_POKER_PHASE_PLAYER_0_OPEN:
    case CFR_KUHN_POKER_PHASE_PLAYER_1_AFTER_CHECK:
        *expected = open;
        *expected_count = ARRAY_COUNT(open);
        break;
    case CFR_KUHN_POKER_PHASE_PLAYER_1_FACING_OPEN_BET:
    case CFR_KUHN_POKER_PHASE_PLAYER_0_FACING_CHECK_BET:
        *expected = response;
        *expected_count = ARRAY_COUNT(response);
        break;
    case CFR_KUHN_POKER_PHASE_TERMINAL:
    default:
        *expected = NULL;
        *expected_count = 0;
        break;
    }
}

static void check_illegal_actions(const Game *game, KuhnPokerState *state,
                                  const Action *legal_actions,
                                  size_t legal_action_count) {
    static const Action candidates[] = {
        -19,
        CFR_KUHN_POKER_ACTION_NONE,
        CFR_KUHN_POKER_ACTION_JQ,
        CFR_KUHN_POKER_ACTION_JK,
        CFR_KUHN_POKER_ACTION_QJ,
        CFR_KUHN_POKER_ACTION_QK,
        CFR_KUHN_POKER_ACTION_KJ,
        CFR_KUHN_POKER_ACTION_KQ,
        CFR_KUHN_POKER_ACTION_BET,
        CFR_KUHN_POKER_ACTION_FOLD,
        CFR_KUHN_POKER_ACTION_CALL,
        CFR_KUHN_POKER_ACTION_CHECK,
        99,
    };
    size_t index;

    for (index = 0; index < ARRAY_COUNT(candidates); index += 1) {
        KuhnPokerState snapshot;

        if (action_is_present(candidates[index], legal_actions,
                              legal_action_count)) {
            continue;
        }
        snapshot = *state;
        CHECK(cfr_game_apply_action(game, as_state(state), candidates[index]) ==
              CFR_STATUS_ILLEGAL_ACTION);
        CHECK(same_state(state, &snapshot));
    }
}

typedef struct {
    size_t states;
    size_t transitions;
    size_t terminals;
} TreeCounts;

static void explore_complete_tree(const Game *game, KuhnPokerState *state,
                                  TreeCounts *counts) {
    bool terminal = false;
    Actor actor = {.kind = CFR_ACTOR_CHANCE, .player = CFR_PLAYER_1};
    const Action *expected = NULL;
    size_t expected_count = 0;
    Action actions[CFR_KUHN_POKER_MAX_POSSIBLE_ACTIONS] = {0};
    size_t action_count = 0;
    size_t index;

    counts->states += 1;
    CHECK(cfr_game_is_terminal(game, as_const_state(state), &terminal) ==
          CFR_STATUS_SUCCESS);
    if (terminal) {
        counts->terminals += 1;
        CHECK(state->phase == CFR_KUHN_POKER_PHASE_TERMINAL);
        check_illegal_actions(game, state, NULL, 0);
        return;
    }

    expected_phase_actions(state->phase, &expected, &expected_count);
    CHECK(expected != NULL);
    CHECK(cfr_game_current_actor(game, as_const_state(state), &actor) ==
          CFR_STATUS_SUCCESS);
    if (state->phase == CFR_KUHN_POKER_PHASE_CHANCE) {
        CHECK(actor.kind == CFR_ACTOR_CHANCE);
    } else {
        const Player expected_player =
            state->phase == CFR_KUHN_POKER_PHASE_PLAYER_0_OPEN ||
                    state->phase ==
                        CFR_KUHN_POKER_PHASE_PLAYER_0_FACING_CHECK_BET
                ? CFR_PLAYER_0
                : CFR_PLAYER_1;
        Probability probability = 54.0;

        CHECK(actor.kind == CFR_ACTOR_PLAYER);
        CHECK(actor.player == expected_player);
        CHECK(cfr_game_chance_probability(
                  game, as_const_state(state), CFR_KUHN_POKER_ACTION_JQ,
                  &probability) == CFR_STATUS_INVALID_ARGUMENT);
        CHECK(probability == 54.0);
    }
    CHECK(cfr_game_legal_actions(game, as_const_state(state), actions,
                                 ARRAY_COUNT(actions),
                                 &action_count) == CFR_STATUS_SUCCESS);
    CHECK(action_count == expected_count);
    for (index = 0; index < action_count && index < expected_count;
         index += 1) {
        CHECK(actions[index] == expected[index]);
    }
    check_illegal_actions(game, state, actions, action_count);

    for (index = 0; index < action_count; index += 1) {
        KuhnPokerState snapshot = *state;

        CHECK(cfr_game_apply_action(game, as_state(state), actions[index]) ==
              CFR_STATUS_SUCCESS);
        counts->transitions += 1;
        explore_complete_tree(game, state, counts);
        CHECK(cfr_game_undo_action(game, as_state(state)) ==
              CFR_STATUS_SUCCESS);
        CHECK(same_state(state, &snapshot));
    }
}

static void test_complete_tree_and_undo(void) {
    const Game *game = cfr_kuhn_poker_descriptor();
    KuhnPokerState state;
    KuhnPokerState root;
    TreeCounts counts = {0};

    initialize(&state);
    root = state;
    explore_complete_tree(game, &state, &counts);
    CHECK(counts.states == 55);
    CHECK(counts.transitions == 54);
    CHECK(counts.terminals == 30);
    CHECK(same_state(&state, &root));
}

typedef struct {
    KuhnPokerAction actions[CFR_KUHN_POKER_PUBLIC_HISTORY_CAPACITY];
    size_t action_count;
    Utility high_card_utility;
    Utility low_card_utility;
} TerminalHistory;

static void test_all_terminal_histories_and_utilities(void) {
    static const KuhnPokerAction deals[] = {
        CFR_KUHN_POKER_ACTION_JQ, CFR_KUHN_POKER_ACTION_JK,
        CFR_KUHN_POKER_ACTION_QJ, CFR_KUHN_POKER_ACTION_QK,
        CFR_KUHN_POKER_ACTION_KJ, CFR_KUHN_POKER_ACTION_KQ,
    };
    static const KuhnPokerCard cards[][CFR_KUHN_POKER_NUMBER_OF_PLAYERS] = {
        {CFR_KUHN_POKER_CARD_JACK, CFR_KUHN_POKER_CARD_QUEEN},
        {CFR_KUHN_POKER_CARD_JACK, CFR_KUHN_POKER_CARD_KING},
        {CFR_KUHN_POKER_CARD_QUEEN, CFR_KUHN_POKER_CARD_JACK},
        {CFR_KUHN_POKER_CARD_QUEEN, CFR_KUHN_POKER_CARD_KING},
        {CFR_KUHN_POKER_CARD_KING, CFR_KUHN_POKER_CARD_JACK},
        {CFR_KUHN_POKER_CARD_KING, CFR_KUHN_POKER_CARD_QUEEN},
    };
    static const TerminalHistory histories[] = {
        {{CFR_KUHN_POKER_ACTION_CHECK, CFR_KUHN_POKER_ACTION_CHECK,
          CFR_KUHN_POKER_ACTION_NONE},
         2,
         1.0,
         -1.0},
        {{CFR_KUHN_POKER_ACTION_BET, CFR_KUHN_POKER_ACTION_FOLD,
          CFR_KUHN_POKER_ACTION_NONE},
         2,
         1.0,
         1.0},
        {{CFR_KUHN_POKER_ACTION_BET, CFR_KUHN_POKER_ACTION_CALL,
          CFR_KUHN_POKER_ACTION_NONE},
         2,
         2.0,
         -2.0},
        {{CFR_KUHN_POKER_ACTION_CHECK, CFR_KUHN_POKER_ACTION_BET,
          CFR_KUHN_POKER_ACTION_FOLD},
         3,
         -1.0,
         -1.0},
        {{CFR_KUHN_POKER_ACTION_CHECK, CFR_KUHN_POKER_ACTION_BET,
          CFR_KUHN_POKER_ACTION_CALL},
         3,
         2.0,
         -2.0},
    };
    const Game *game = cfr_kuhn_poker_descriptor();
    size_t deal_index;
    size_t history_index;

    for (deal_index = 0; deal_index < ARRAY_COUNT(deals); deal_index += 1) {
        for (history_index = 0; history_index < ARRAY_COUNT(histories);
             history_index += 1) {
            const TerminalHistory *history = &histories[history_index];
            const bool player0_has_high_card =
                cards[deal_index][0] > cards[deal_index][1];
            const Utility expected = player0_has_high_card
                                         ? history->high_card_utility
                                         : history->low_card_utility;
            KuhnPokerState state;
            Utility player0 = 71.0;
            Utility player1 = 72.0;
            Utility invalid_player = 73.0;
            size_t action_index;
            bool terminal = false;
            Actor actor = {.kind = CFR_ACTOR_CHANCE, .player = CFR_PLAYER_1};
            Action actions[CFR_KUHN_POKER_MAX_POSSIBLE_ACTIONS] = {81, 82, 83,
                                                                   84, 85, 86};
            size_t required_count = 87;
            InfoSetKey key = 88;
            Probability probability = 89.0;

            initialize(&state);
            CHECK(cfr_game_apply_action(game, as_state(&state),
                                        deals[deal_index]) ==
                  CFR_STATUS_SUCCESS);
            CHECK(state.cards[0] == cards[deal_index][0]);
            CHECK(state.cards[1] == cards[deal_index][1]);
            for (action_index = 0; action_index < history->action_count;
                 action_index += 1) {
                CHECK(cfr_game_apply_action(game, as_state(&state),
                                            history->actions[action_index]) ==
                      CFR_STATUS_SUCCESS);
            }

            CHECK(cfr_game_is_terminal(game, as_const_state(&state),
                                       &terminal) == CFR_STATUS_SUCCESS);
            CHECK(terminal);
            CHECK(state.phase == CFR_KUHN_POKER_PHASE_TERMINAL);
            CHECK(state.public_action_count == history->action_count);
            CHECK(cfr_game_terminal_utility(game, as_const_state(&state),
                                            CFR_PLAYER_0,
                                            &player0) == CFR_STATUS_SUCCESS);
            CHECK(cfr_game_terminal_utility(game, as_const_state(&state),
                                            CFR_PLAYER_1,
                                            &player1) == CFR_STATUS_SUCCESS);
            CHECK(player0 == expected);
            CHECK(player1 == -expected);
            CHECK(player0 + player1 == 0.0);

            CHECK(cfr_game_terminal_utility(game, as_const_state(&state),
                                            (Player)27, &invalid_player) ==
                  CFR_STATUS_INVALID_ARGUMENT);
            CHECK(invalid_player == 73.0);
            CHECK(
                cfr_game_current_actor(game, as_const_state(&state), &actor) ==
                CFR_STATUS_INVALID_ARGUMENT);
            CHECK(actor.kind == CFR_ACTOR_CHANCE &&
                  actor.player == CFR_PLAYER_1);
            CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions,
                                         ARRAY_COUNT(actions),
                                         &required_count) ==
                  CFR_STATUS_INVALID_ARGUMENT);
            CHECK(required_count == 87);
            CHECK(actions[0] == 81 && actions[5] == 86);
            CHECK(cfr_game_information_set_key(game, as_const_state(&state),
                                               &key) ==
                  CFR_STATUS_INVALID_ARGUMENT);
            CHECK(key == 88);
            CHECK(cfr_game_chance_probability(
                      game, as_const_state(&state), CFR_KUHN_POKER_ACTION_JQ,
                      &probability) == CFR_STATUS_INVALID_ARGUMENT);
            CHECK(probability == 89.0);
        }
    }
}

static InfoSetKey key_for_context(size_t context_index,
                                  KuhnPokerCard private_card,
                                  KuhnPokerCard opponent_card) {
    const Game *game = cfr_kuhn_poker_descriptor();
    KuhnPokerState state;
    KuhnPokerCard player0_card;
    KuhnPokerCard player1_card;
    InfoSetKey key = -1;

    if (context_index == 1 || context_index == 2) {
        player0_card = opponent_card;
        player1_card = private_card;
    } else {
        player0_card = private_card;
        player1_card = opponent_card;
    }

    initialize(&state);
    CHECK(cfr_game_apply_action(game, as_state(&state),
                                deal_for_cards(player0_card, player1_card)) ==
          CFR_STATUS_SUCCESS);
    if (context_index == 1 || context_index == 3) {
        CHECK(cfr_game_apply_action(game, as_state(&state),
                                    CFR_KUHN_POKER_ACTION_CHECK) ==
              CFR_STATUS_SUCCESS);
    } else if (context_index == 2) {
        CHECK(cfr_game_apply_action(game, as_state(&state),
                                    CFR_KUHN_POKER_ACTION_BET) ==
              CFR_STATUS_SUCCESS);
    }
    if (context_index == 3) {
        CHECK(cfr_game_apply_action(game, as_state(&state),
                                    CFR_KUHN_POKER_ACTION_BET) ==
              CFR_STATUS_SUCCESS);
    }
    CHECK(cfr_game_information_set_key(game, as_const_state(&state), &key) ==
          CFR_STATUS_SUCCESS);
    return key;
}

static void test_information_sets(void) {
    static const KuhnPokerCard cards[] = {CFR_KUHN_POKER_CARD_JACK,
                                          CFR_KUHN_POKER_CARD_QUEEN,
                                          CFR_KUHN_POKER_CARD_KING};
    InfoSetKey keys[4][3] = {{0}};
    size_t context;
    size_t card_index;
    size_t left;
    size_t right;

    for (context = 0; context < 4; context += 1) {
        for (card_index = 0; card_index < ARRAY_COUNT(cards); card_index += 1) {
            const KuhnPokerCard private_card = cards[card_index];
            KuhnPokerCard first_opponent = CFR_KUHN_POKER_CARD_JACK;
            KuhnPokerCard second_opponent = CFR_KUHN_POKER_CARD_QUEEN;
            InfoSetKey hidden_first;
            InfoSetKey hidden_second;

            if (private_card == CFR_KUHN_POKER_CARD_JACK) {
                first_opponent = CFR_KUHN_POKER_CARD_QUEEN;
                second_opponent = CFR_KUHN_POKER_CARD_KING;
            } else if (private_card == CFR_KUHN_POKER_CARD_QUEEN) {
                second_opponent = CFR_KUHN_POKER_CARD_KING;
            }
            hidden_first =
                key_for_context(context, private_card, first_opponent);
            hidden_second =
                key_for_context(context, private_card, second_opponent);
            CHECK(hidden_first == hidden_second);
            keys[context][card_index] = hidden_first;
        }
    }

    for (left = 0; left < 12; left += 1) {
        for (right = left + 1; right < 12; right += 1) {
            CHECK(keys[left / 3][left % 3] != keys[right / 3][right % 3]);
        }
    }
}

static void check_invalid_state_is_atomic(const KuhnPokerState *invalid) {
    const Game *game = cfr_kuhn_poker_descriptor();
    KuhnPokerState state = *invalid;
    KuhnPokerState snapshot = state;
    bool terminal = true;
    Utility utility = 61.0;
    Actor actor = {.kind = CFR_ACTOR_CHANCE, .player = CFR_PLAYER_1};
    Action actions[CFR_KUHN_POKER_MAX_POSSIBLE_ACTIONS] = {71, 72, 73,
                                                           74, 75, 76};
    size_t required_count = 77;
    Probability probability = 78.0;
    InfoSetKey key = 79;

    CHECK(cfr_game_is_terminal(game, as_const_state(&state), &terminal) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(terminal);
    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), CFR_PLAYER_0,
                                    &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(utility == 61.0);
    CHECK(cfr_game_current_actor(game, as_const_state(&state), &actor) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(actor.kind == CFR_ACTOR_CHANCE && actor.player == CFR_PLAYER_1);
    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions,
                                 ARRAY_COUNT(actions), &required_count) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(required_count == 77);
    CHECK(actions[0] == 71 && actions[5] == 76);
    CHECK(cfr_game_chance_probability(game, as_const_state(&state),
                                      CFR_KUHN_POKER_ACTION_JQ, &probability) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(probability == 78.0);
    CHECK(cfr_game_information_set_key(game, as_const_state(&state), &key) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(key == 79);
    CHECK(cfr_game_apply_action(game, as_state(&state),
                                CFR_KUHN_POKER_ACTION_CHECK) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(same_state(&state, &snapshot));
    CHECK(cfr_game_undo_action(game, as_state(&state)) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(same_state(&state, &snapshot));
}

static void test_invalid_states(void) {
    const Game *game = cfr_kuhn_poker_descriptor();
    KuhnPokerState root;
    KuhnPokerState dealt;
    KuhnPokerState invalid;

    initialize(&root);
    dealt = root;
    CHECK(cfr_game_apply_action(game, as_state(&dealt),
                                CFR_KUHN_POKER_ACTION_JQ) ==
          CFR_STATUS_SUCCESS);

    invalid = root;
    invalid.phase = (KuhnPokerPhase)99;
    check_invalid_state_is_atomic(&invalid);

    invalid = root;
    invalid.cards[0] = CFR_KUHN_POKER_CARD_JACK;
    check_invalid_state_is_atomic(&invalid);

    invalid = dealt;
    invalid.cards[1] = invalid.cards[0];
    check_invalid_state_is_atomic(&invalid);

    invalid = dealt;
    invalid.phase = CFR_KUHN_POKER_PHASE_TERMINAL;
    check_invalid_state_is_atomic(&invalid);

    invalid = dealt;
    invalid.public_action_count = CFR_KUHN_POKER_PUBLIC_HISTORY_CAPACITY + 1;
    check_invalid_state_is_atomic(&invalid);

    invalid = dealt;
    invalid.undo_count = CFR_KUHN_POKER_UNDO_HISTORY_CAPACITY + 1;
    check_invalid_state_is_atomic(&invalid);

    invalid = dealt;
    invalid.public_actions[1] = CFR_KUHN_POKER_ACTION_CHECK;
    check_invalid_state_is_atomic(&invalid);

    invalid = dealt;
    invalid.undo_history[2].applied_action = CFR_KUHN_POKER_ACTION_CHECK;
    check_invalid_state_is_atomic(&invalid);

    invalid = dealt;
    invalid.undo_count = 2;
    check_invalid_state_is_atomic(&invalid);

    invalid = dealt;
    invalid.undo_history[0].applied_action = CFR_KUHN_POKER_ACTION_JK;
    check_invalid_state_is_atomic(&invalid);
}

static Status evaluate_average_profile(const Game *game, GameState *state,
                                       InfoStore *store, Utility *result) {
    bool terminal;
    Actor actor;
    Action actions[CFR_KUHN_POKER_MAX_POSSIBLE_ACTIONS];
    size_t action_count;
    size_t index;
    Utility total = 0.0;
    Status status;

    status = cfr_game_is_terminal(game, state, &terminal);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (terminal)
        return cfr_game_terminal_utility(game, state, CFR_PLAYER_0, result);

    status = cfr_game_current_actor(game, state, &actor);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    status = cfr_game_legal_actions(game, state, actions, ARRAY_COUNT(actions),
                                    &action_count);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    if (actor.kind == CFR_ACTOR_CHANCE) {
        for (index = 0; index < action_count; index += 1) {
            Probability probability;
            Utility child_utility;
            Status child_status;
            Status undo_status;

            status = cfr_game_chance_probability(game, state, actions[index],
                                                 &probability);
            if (status != CFR_STATUS_SUCCESS)
                return status;
            status = cfr_game_apply_action(game, state, actions[index]);
            if (status != CFR_STATUS_SUCCESS)
                return status;
            child_status =
                evaluate_average_profile(game, state, store, &child_utility);
            undo_status = cfr_game_undo_action(game, state);
            if (undo_status != CFR_STATUS_SUCCESS)
                return undo_status;
            if (child_status != CFR_STATUS_SUCCESS)
                return child_status;
            total += probability * child_utility;
        }
    } else {
        InfoSetKey key;
        InfoNode *node = NULL;
        Probability strategy[CFR_KUHN_POKER_MAX_POSSIBLE_ACTIONS] = {0};

        status = cfr_game_information_set_key(game, state, &key);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        status = cfr_info_store_find(store, key, &node);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (node == NULL || node->action_count != action_count)
            return CFR_STATUS_INVALID_ARGUMENT;
        status = cfr_info_node_average_strategy(node, strategy,
                                                ARRAY_COUNT(strategy));
        if (status != CFR_STATUS_SUCCESS)
            return status;

        for (index = 0; index < action_count; index += 1) {
            Utility child_utility;
            Status child_status;
            Status undo_status;

            status = cfr_game_apply_action(game, state, actions[index]);
            if (status != CFR_STATUS_SUCCESS)
                return status;
            child_status =
                evaluate_average_profile(game, state, store, &child_utility);
            undo_status = cfr_game_undo_action(game, state);
            if (undo_status != CFR_STATUS_SUCCESS)
                return undo_status;
            if (child_status != CFR_STATUS_SUCCESS)
                return child_status;
            total += strategy[index] * child_utility;
        }
    }

    *result = total;
    return CFR_STATUS_SUCCESS;
}

static void test_integral_training(void) {
    const size_t iteration_count = 100000;
    const Game *game = cfr_kuhn_poker_descriptor();
    KuhnPokerState state;
    KuhnPokerState root;
    InfoStore store = {0};
    Trainer trainer;
    TrainerStats stats = {0};
    Utility profile_value = 0.0;

    initialize(&state);
    root = state;
    CHECK(cfr_info_store_init(&store) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_init(&trainer, game, as_state(&state), &store) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&trainer, iteration_count) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_get_stats(&trainer, &stats) == CFR_STATUS_SUCCESS);
    CHECK(stats.iterations == iteration_count);
    CHECK(stats.traversals == iteration_count * 2);
    CHECK(stats.visited_nodes == iteration_count * 110);
    CHECK(stats.errors == 0);
    CHECK(same_state(&state, &root));
    CHECK(store.size == 12);

    CHECK(evaluate_average_profile(game, as_state(&state), &store,
                                   &profile_value) == CFR_STATUS_SUCCESS);
    CHECK(same_state(&state, &root));
    CHECK(near(profile_value, -1.0 / 18.0, 0.0001));
    CHECK(cfr_info_store_destroy(&store) == CFR_STATUS_SUCCESS);
}

static InfoNode *find_node(InfoStore *store, InfoSetKey key) {
    InfoNode *node = NULL;

    CHECK(cfr_info_store_find(store, key, &node) == CFR_STATUS_SUCCESS);
    CHECK(node != NULL);
    return node;
}

static void check_same_stats(const TrainerStats *left,
                             const TrainerStats *right) {
    CHECK(left->iterations == right->iterations);
    CHECK(left->traversals == right->traversals);
    CHECK(left->visited_nodes == right->visited_nodes);
    CHECK(left->errors == right->errors);
}

static void check_same_learning(InfoStore *left, InfoStore *right) {
    static const KuhnPokerCard cards[] = {CFR_KUHN_POKER_CARD_JACK,
                                          CFR_KUHN_POKER_CARD_QUEEN,
                                          CFR_KUHN_POKER_CARD_KING};
    size_t context;
    size_t card_index;

    CHECK(left->size == 12);
    CHECK(right->size == 12);
    for (context = 0; context < 4; context += 1) {
        for (card_index = 0; card_index < ARRAY_COUNT(cards); card_index += 1) {
            const KuhnPokerCard private_card = cards[card_index];
            const KuhnPokerCard opponent_card =
                private_card == CFR_KUHN_POKER_CARD_JACK
                    ? CFR_KUHN_POKER_CARD_QUEEN
                    : CFR_KUHN_POKER_CARD_JACK;
            const InfoSetKey key =
                key_for_context(context, private_card, opponent_card);
            InfoNode *left_node = find_node(left, key);
            InfoNode *right_node = find_node(right, key);
            size_t action_index;

            if (left_node == NULL || right_node == NULL)
                continue;
            CHECK(left_node->action_count == 2);
            CHECK(right_node->action_count == left_node->action_count);
            if (right_node->action_count != left_node->action_count)
                continue;
            for (action_index = 0; action_index < left_node->action_count;
                 action_index += 1) {
                CHECK(left_node->regret_sums[action_index] ==
                      right_node->regret_sums[action_index]);
                CHECK(left_node->strategy_sums[action_index] ==
                      right_node->strategy_sums[action_index]);
            }
        }
    }
}

static void test_deterministic_training(void) {
    const size_t first_part = 7;
    const size_t second_part = 13;
    const size_t total = first_part + second_part;
    const Game *game = cfr_kuhn_poker_descriptor();
    KuhnPokerState first_state;
    KuhnPokerState second_state;
    KuhnPokerState split_state;
    KuhnPokerState root;
    InfoStore first_store = {0};
    InfoStore second_store = {0};
    InfoStore split_store = {0};
    Trainer first_trainer;
    Trainer second_trainer;
    Trainer split_trainer;
    TrainerStats first_stats = {0};
    TrainerStats second_stats = {0};
    TrainerStats split_stats = {0};

    initialize(&first_state);
    initialize(&second_state);
    initialize(&split_state);
    initialize(&root);
    CHECK(cfr_info_store_init(&first_store) == CFR_STATUS_SUCCESS);
    CHECK(cfr_info_store_init(&second_store) == CFR_STATUS_SUCCESS);
    CHECK(cfr_info_store_init(&split_store) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_init(&first_trainer, game, as_state(&first_state),
                           &first_store) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_init(&second_trainer, game, as_state(&second_state),
                           &second_store) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_init(&split_trainer, game, as_state(&split_state),
                           &split_store) == CFR_STATUS_SUCCESS);

    CHECK(cfr_trainer_run(&first_trainer, total) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&second_trainer, total) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&split_trainer, first_part) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&split_trainer, second_part) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_get_stats(&first_trainer, &first_stats) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_get_stats(&second_trainer, &second_stats) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_get_stats(&split_trainer, &split_stats) ==
          CFR_STATUS_SUCCESS);

    CHECK(same_state(&first_state, &root));
    CHECK(same_state(&second_state, &root));
    CHECK(same_state(&split_state, &root));
    check_same_stats(&first_stats, &second_stats);
    check_same_learning(&first_store, &second_store);
    check_same_stats(&first_stats, &split_stats);
    check_same_learning(&first_store, &split_store);

    CHECK(cfr_info_store_destroy(&first_store) == CFR_STATUS_SUCCESS);
    CHECK(cfr_info_store_destroy(&second_store) == CFR_STATUS_SUCCESS);
    CHECK(cfr_info_store_destroy(&split_store) == CFR_STATUS_SUCCESS);
}

int test_kuhn_poker(void) {
    failures = 0;

    check_root_and_descriptor();
    test_complete_tree_and_undo();
    test_all_terminal_histories_and_utilities();
    test_information_sets();
    test_invalid_states();
    test_integral_training();
    test_deterministic_training();

    return failures;
}
