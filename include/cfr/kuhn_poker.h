#ifndef CFR_KUHN_POKER_H
#define CFR_KUHN_POKER_H

#include <stddef.h>

#include "cfr/game.h"

CFR_EXTERN_C_BEGIN

/* Maximum number of actions in the public history. */
#define CFR_KUHN_POKER_PUBLIC_HISTORY_CAPACITY 3
/* Maximum number of transitions that can be undone. */
#define CFR_KUHN_POKER_UNDO_HISTORY_CAPACITY 4
/* Number of Kuhn Poker players. */
#define CFR_KUHN_POKER_NUMBER_OF_PLAYERS 2
/*
 * Maximum number of legal actions. The deal node has six actions. Each player
 * node has two actions.
 */
#define CFR_KUHN_POKER_MAX_POSSIBLE_ACTIONS 6

/*
 * Identifies a private card.
 *
 * The order of the real cards represents their rank. The jack is the lowest
 * card, and the king is the highest.
 */
typedef CFR_ENUM_INT(CfrKuhnPokerCard) {
    CFR_KUHN_POKER_CARD_NOT_DEALT,
    CFR_KUHN_POKER_CARD_JACK,
    CFR_KUHN_POKER_CARD_QUEEN,
    CFR_KUHN_POKER_CARD_KING
} KuhnPokerCard;

/* Identifies the current phase and determines the actor and legal actions. */
typedef CFR_ENUM_INT(CfrKuhnPokerPhase) {
    CFR_KUHN_POKER_PHASE_CHANCE,
    CFR_KUHN_POKER_PHASE_PLAYER_0_OPEN,
    CFR_KUHN_POKER_PHASE_PLAYER_1_AFTER_CHECK,
    CFR_KUHN_POKER_PHASE_PLAYER_1_FACING_OPEN_BET,
    CFR_KUHN_POKER_PHASE_PLAYER_0_FACING_CHECK_BET,
    CFR_KUHN_POKER_PHASE_TERMINAL
} KuhnPokerPhase;

/*
 * Identifies a deal or a public action.
 *
 * The chance node enumerates JQ, JK, QJ, QK, KJ, and KQ in that order. A node
 * without a pending bet enumerates CHECK and BET. A node with a pending bet
 * enumerates FOLD and CALL. The order is stable to preserve the meaning of each
 * strategy index.
 */
typedef CFR_ENUM_INT(CfrKuhnPokerAction) {
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
    CFR_KUHN_POKER_ACTION_CHECK
} KuhnPokerAction;

/* Contains a snapshot from before a transition. */
typedef struct {
    /* Phase before the applied action. */
    KuhnPokerPhase previous_phase;
    /* Cards before the applied action. */
    KuhnPokerCard previous_cards[CFR_KUHN_POKER_NUMBER_OF_PLAYERS];
    /* Number of public actions before the applied action. */
    size_t previous_public_action_count;
    /* Action that created this entry. */
    KuhnPokerAction applied_action;
} KuhnPokerUndoEntry;

/*
 * Contains the complete state of a Kuhn Poker game.
 *
 * The caller owns and allocates the structure without dynamic allocation. The
 * adapter does not allocate memory during its operations.
 *
 * The caller must initialize the structure with cfr_kuhn_poker_state_init and
 * change the state through the descriptor operations. The caller must not
 * modify fields directly. Direct modification invalidates the state contract.
 * Operations return CFR_STATUS_INVALID_ARGUMENT when they detect an
 * inconsistent state.
 *
 * public_actions contains only the actions observed by both players.
 * Information-set keys can use this history. undo_history contains snapshots
 * that can include private cards. Information-set keys never use undo_history.
 */
typedef struct {
    /* Current phase of the game. */
    KuhnPokerPhase phase;
    /* Private cards for player zero and player one. */
    KuhnPokerCard cards[CFR_KUHN_POKER_NUMBER_OF_PLAYERS];
    /* Public actions in the order in which they were applied. */
    KuhnPokerAction public_actions[CFR_KUHN_POKER_PUBLIC_HISTORY_CAPACITY];
    /* Number of used positions in public_actions. */
    size_t public_action_count;
    /* Private snapshots of applied transitions. */
    KuhnPokerUndoEntry undo_history[CFR_KUHN_POKER_UNDO_HISTORY_CAPACITY];
    /* Number of used positions in undo_history. */
    size_t undo_count;
} KuhnPokerState;

/*
 * Initializes state as a chance root without cards or actions.
 *
 * The function clears both histories. A null pointer produces
 * CFR_STATUS_INVALID_ARGUMENT.
 */
Status cfr_kuhn_poker_state_init(KuhnPokerState *state);

/*
 * Returns the const Kuhn Poker descriptor.
 *
 * The adapter owns the descriptor. The borrowed pointer has static lifetime.
 * The caller must not modify or free the descriptor.
 */
const Game *cfr_kuhn_poker_descriptor(void);

/*
 * Views kuhn_poker_state as a mutable opaque state.
 *
 * The function returns the same borrowed pointer. It does not copy or allocate
 * memory, and it does not transfer ownership.
 */
GameState *cfr_kuhn_poker_state_as_game_state(KuhnPokerState *kuhn_poker_state);

/*
 * Views kuhn_poker_state as a const opaque state.
 *
 * The function returns the same borrowed pointer. It does not copy or allocate
 * memory, and it does not transfer ownership.
 */
const GameState *cfr_kuhn_poker_state_as_game_state_const(
    const KuhnPokerState *kuhn_poker_state);

CFR_EXTERN_C_END

#endif
