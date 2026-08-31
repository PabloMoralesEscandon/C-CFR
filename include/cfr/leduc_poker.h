#ifndef CFR_LEDUC_POKER_H
#define CFR_LEDUC_POKER_H

#include <stdbool.h>
#include <stddef.h>

#include "cfr/game.h"

CFR_EXTERN_C_BEGIN

/* Number of strategic players in two-player Leduc Poker. */
#define CFR_LEDUC_POKER_NUMBER_OF_PLAYERS 2
/* Maximum number of visible betting actions across both rounds. */
#define CFR_LEDUC_POKER_PUBLIC_HISTORY_CAPACITY 8
/* Maximum number of reversible transitions, including both chance deals. */
#define CFR_LEDUC_POKER_UNDO_HISTORY_CAPACITY 10
/* The private deal has the largest legal action set: nine rank pairs. */
#define CFR_LEDUC_POKER_MAX_POSSIBLE_ACTIONS 9
/* An opening bet and one subsequent raise are allowed in each round. */
#define CFR_LEDUC_POKER_MAX_AGGRESSIVE_ACTIONS_PER_ROUND 2

/*
 * Identifies a card rank. The deck contains two cards of every real rank.
 * Suits have no strategic meaning and are represented through probabilities.
 */
typedef CFR_ENUM_INT(CfrLeducPokerCard) {
    CFR_LEDUC_POKER_CARD_NOT_DEALT,
    CFR_LEDUC_POKER_CARD_JACK,
    CFR_LEDUC_POKER_CARD_QUEEN,
    CFR_LEDUC_POKER_CARD_KING
} LeducPokerCard;

/* Identifies the current phase and, together with current_player, the actor. */
typedef CFR_ENUM_INT(CfrLeducPokerPhase) {
    CFR_LEDUC_POKER_PHASE_PRIVATE_DEAL,
    CFR_LEDUC_POKER_PHASE_FIRST_BETTING,
    CFR_LEDUC_POKER_PHASE_PUBLIC_DEAL,
    CFR_LEDUC_POKER_PHASE_SECOND_BETTING,
    CFR_LEDUC_POKER_PHASE_TERMINAL
} LeducPokerPhase;

/*
 * Identifies a chance outcome or betting action.
 *
 * Private deals encode the ranks dealt to player zero and player one. Equal
 * ranks are possible because the deck has two copies of each rank. Public-deal
 * actions reveal one rank. At player nodes, an unopened pot uses CHECK/BET and
 * a player facing a wager uses FOLD/CALL and, below the cap, RAISE.
 */
typedef CFR_ENUM_INT(CfrLeducPokerAction) {
    CFR_LEDUC_POKER_ACTION_NONE,
    CFR_LEDUC_POKER_ACTION_DEAL_JJ,
    CFR_LEDUC_POKER_ACTION_DEAL_JQ,
    CFR_LEDUC_POKER_ACTION_DEAL_JK,
    CFR_LEDUC_POKER_ACTION_DEAL_QJ,
    CFR_LEDUC_POKER_ACTION_DEAL_QQ,
    CFR_LEDUC_POKER_ACTION_DEAL_QK,
    CFR_LEDUC_POKER_ACTION_DEAL_KJ,
    CFR_LEDUC_POKER_ACTION_DEAL_KQ,
    CFR_LEDUC_POKER_ACTION_DEAL_KK,
    CFR_LEDUC_POKER_ACTION_REVEAL_J,
    CFR_LEDUC_POKER_ACTION_REVEAL_Q,
    CFR_LEDUC_POKER_ACTION_REVEAL_K,
    CFR_LEDUC_POKER_ACTION_CHECK,
    CFR_LEDUC_POKER_ACTION_BET,
    CFR_LEDUC_POKER_ACTION_FOLD,
    CFR_LEDUC_POKER_ACTION_CALL,
    CFR_LEDUC_POKER_ACTION_RAISE
} LeducPokerAction;

/* Contains the complete non-history state saved before one transition. */
typedef struct {
    LeducPokerPhase previous_phase;
    LeducPokerCard previous_private_cards[CFR_LEDUC_POKER_NUMBER_OF_PLAYERS];
    LeducPokerCard previous_public_card;
    size_t previous_public_action_count;
    size_t previous_round_start_index;
    Player previous_current_player;
    size_t previous_aggressive_action_count;
    int previous_contributions[CFR_LEDUC_POKER_NUMBER_OF_PLAYERS];
    bool previous_folded;
    Player previous_folded_player;
    LeducPokerAction applied_action;
} LeducPokerUndoEntry;

/*
 * Contains the complete state of a Leduc Poker game.
 *
 * Both players have already contributed the one-chip ante in the initialized
 * root. The first-round fixed bet is two chips and the second-round fixed bet
 * is four chips. The caller owns this structure and must mutate it only through
 * the game operations so apply/undo traversal remains valid.
 */
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
    LeducPokerUndoEntry
        undo_history[CFR_LEDUC_POKER_UNDO_HISTORY_CAPACITY];
    size_t undo_count;
} LeducPokerState;

/* Initializes a private-deal root with both one-chip antes committed. */
Status cfr_leduc_poker_state_init(LeducPokerState *state);

/* Returns the immutable Leduc Poker game descriptor. */
const Game *cfr_leduc_poker_descriptor(void);

/* Views a concrete Leduc state through the engine's opaque state type. */
GameState *cfr_leduc_poker_state_as_game_state(LeducPokerState *state);

/* Views a const concrete Leduc state through the engine's opaque state type. */
const GameState *
cfr_leduc_poker_state_as_game_state_const(const LeducPokerState *state);

CFR_EXTERN_C_END

#endif
