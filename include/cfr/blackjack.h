#ifndef CFR_BLACKJACK_H
#define CFR_BLACKJACK_H

#include <stddef.h>

#include "cfr/game.h"

/* Blackjack uses a standard deck without jokers. */
#define CFR_BLACKJACK_DECK_SIZE 52
/* Ten rank classes: ace, 2--9, and any card worth ten. */
#define CFR_BLACKJACK_NUMBER_OF_CARD_RANKS 10
/* The two participants are the player and the dealer. */
#define CFR_BLACKJACK_NUMBER_OF_PLAYERS 2
/* A hand can never contain more cards than the complete deck. */
#define CFR_BLACKJACK_HAND_CAPACITY CFR_BLACKJACK_DECK_SIZE
/*
 * A card can require a preceding HIT action. One additional position is kept
 * for STAND. This bound is deliberately conservative.
 */
#define CFR_BLACKJACK_UNDO_HISTORY_CAPACITY                                  \
    (2 * CFR_BLACKJACK_DECK_SIZE + 1)
/* All ten rank classes can be available at a chance node. */
#define CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS CFR_BLACKJACK_NUMBER_OF_CARD_RANKS

/*
 * Identifies the relevant value of a card.
 *
 * TEN represents a ten, jack, queen, or king. The deck contains sixteen cards
 * in this class and four cards in every other class.
 */
typedef enum {
    CFR_BLACKJACK_CARD_NOT_DEALT,
    CFR_BLACKJACK_CARD_ACE,
    CFR_BLACKJACK_CARD_TWO,
    CFR_BLACKJACK_CARD_THREE,
    CFR_BLACKJACK_CARD_FOUR,
    CFR_BLACKJACK_CARD_FIVE,
    CFR_BLACKJACK_CARD_SIX,
    CFR_BLACKJACK_CARD_SEVEN,
    CFR_BLACKJACK_CARD_EIGHT,
    CFR_BLACKJACK_CARD_NINE,
    CFR_BLACKJACK_CARD_TEN
} BlackjackCard;

/* Identifies the exact point in the rules reached by the hand. */
typedef enum {
    CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST,
    CFR_BLACKJACK_PHASE_DEAL_DEALER_UP_CARD,
    CFR_BLACKJACK_PHASE_DEAL_PLAYER_SECOND,
    CFR_BLACKJACK_PHASE_DEAL_DEALER_HOLE_CARD,
    CFR_BLACKJACK_PHASE_PLAYER_TURN,
    CFR_BLACKJACK_PHASE_DEAL_PLAYER_HIT,
    CFR_BLACKJACK_PHASE_DEAL_DEALER_HIT,
    CFR_BLACKJACK_PHASE_TERMINAL
} BlackjackPhase;

/*
 * Identifies a dealt card or player decision.
 *
 * Card actions always appear in ace-to-ten order. The dealer is not a
 * strategic actor: its rule-driven draws are chance nodes.
 */
typedef enum {
    CFR_BLACKJACK_ACTION_NONE,
    CFR_BLACKJACK_ACTION_DEAL_ACE,
    CFR_BLACKJACK_ACTION_DEAL_TWO,
    CFR_BLACKJACK_ACTION_DEAL_THREE,
    CFR_BLACKJACK_ACTION_DEAL_FOUR,
    CFR_BLACKJACK_ACTION_DEAL_FIVE,
    CFR_BLACKJACK_ACTION_DEAL_SIX,
    CFR_BLACKJACK_ACTION_DEAL_SEVEN,
    CFR_BLACKJACK_ACTION_DEAL_EIGHT,
    CFR_BLACKJACK_ACTION_DEAL_NINE,
    CFR_BLACKJACK_ACTION_DEAL_TEN,
    CFR_BLACKJACK_ACTION_HIT,
    CFR_BLACKJACK_ACTION_STAND
} BlackjackAction;

/* Stores the information required to undo one transition exactly. */
typedef struct {
    BlackjackPhase previous_phase;
    BlackjackAction applied_action;
} BlackjackUndoEntry;

/*
 * Contains the complete state of a blackjack hand.
 *
 * Adapter rules:
 *
 * - one 52-card deck without replacement;
 * - the dealer stands on every 17, including soft 17;
 * - a natural blackjack pays 3:2 and a push returns the stake;
 * - the player can hit or stand;
 * - no doubling, splitting, insurance, or surrender.
 *
 * CFR_PLAYER_0 represents the player. CFR_PLAYER_1 represents the dealer and
 * always receives the opposite utility. The dealer's forced decisions are
 * modeled as chance to retain a two-participant, zero-sum game compatible with
 * the engine.
 *
 * The caller owns the structure and must initialize it with
 * cfr_blackjack_state_init. Operations do not allocate memory. The caller must
 * not modify fields directly; operations reject inconsistent states that they
 * can detect.
 *
 * dealer_cards[0] is the up card. Cards after the hole card are stored in draw
 * order.
 */
typedef struct {
    BlackjackPhase phase;
    BlackjackCard player_cards[CFR_BLACKJACK_HAND_CAPACITY];
    size_t player_card_count;
    BlackjackCard dealer_cards[CFR_BLACKJACK_HAND_CAPACITY];
    size_t dealer_card_count;
    /* Undealt count for each rank, in ace-to-ten order. */
    size_t remaining_cards[CFR_BLACKJACK_NUMBER_OF_CARD_RANKS];
    BlackjackUndoEntry undo_history[CFR_BLACKJACK_UNDO_HISTORY_CAPACITY];
    size_t undo_count;
} BlackjackState;

/*
 * Initializes state before the first deal, with a complete deck.
 * A null pointer produces CFR_STATUS_INVALID_ARGUMENT.
 */
Status cfr_blackjack_state_init(BlackjackState *state);

/* Returns the const, static-lifetime blackjack descriptor. */
const Game *cfr_blackjack_descriptor(void);

/* Views blackjack_state as a mutable opaque game state without copying it. */
GameState *cfr_blackjack_state_as_game_state(BlackjackState *blackjack_state);

/* Views blackjack_state as a const opaque game state without copying it. */
const GameState *
cfr_blackjack_state_as_game_state_const(const BlackjackState *blackjack_state);

#endif
