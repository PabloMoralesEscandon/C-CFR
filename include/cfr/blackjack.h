#ifndef CFR_BLACKJACK_H
#define CFR_BLACKJACK_H

#include <stdbool.h>
#include <stddef.h>

#include "cfr/game.h"

CFR_EXTERN_C_BEGIN

/* Ten rank classes: ace, 2--9, and any card worth ten. */
#define CFR_BLACKJACK_NUMBER_OF_CARD_RANKS 10
/* The two participants are the player and the dealer. */
#define CFR_BLACKJACK_NUMBER_OF_PLAYERS 2
/* Even with replacement, a live hand reaches 21 within this many cards. */
#define CFR_BLACKJACK_HAND_CAPACITY 21
/* Standard table play limits a split round to four player hands. */
#define CFR_BLACKJACK_MAX_PLAYER_HANDS 4
/* Conservative bound for every deal and decision along one traversal path. */
#define CFR_BLACKJACK_UNDO_HISTORY_CAPACITY                                  \
    (2 * CFR_BLACKJACK_HAND_CAPACITY * CFR_BLACKJACK_MAX_PLAYER_HANDS +      \
     CFR_BLACKJACK_HAND_CAPACITY + 2 * CFR_BLACKJACK_MAX_PLAYER_HANDS)
/* All ten rank classes can be available at chance; player nodes use at most 4. */
#define CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS CFR_BLACKJACK_NUMBER_OF_CARD_RANKS

/*
 * Identifies the relevant value of a card.
 *
 * TEN represents a ten, jack, queen, or king. Chance draws use an independent
 * rank distribution: TEN has probability 4/13 and every other class has
 * probability 1/13.
 */
typedef CFR_ENUM_INT(CfrBlackjackCard) {
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
typedef CFR_ENUM_INT(CfrBlackjackPhase) {
    CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST,
    CFR_BLACKJACK_PHASE_DEAL_DEALER_UP_CARD,
    CFR_BLACKJACK_PHASE_DEAL_PLAYER_SECOND,
    CFR_BLACKJACK_PHASE_DEAL_DEALER_HOLE_CARD,
    CFR_BLACKJACK_PHASE_PLAYER_TURN,
    CFR_BLACKJACK_PHASE_DEAL_PLAYER_HIT,
    CFR_BLACKJACK_PHASE_DEAL_DEALER_HIT,
    CFR_BLACKJACK_PHASE_TERMINAL,
    CFR_BLACKJACK_PHASE_DEAL_PLAYER_DOUBLE,
    CFR_BLACKJACK_PHASE_DEAL_SPLIT_HAND
} BlackjackPhase;

/*
 * Identifies a dealt card or player decision.
 *
 * Card actions always appear in ace-to-ten order. The dealer is not a
 * strategic actor: its rule-driven draws are chance nodes.
 */
typedef CFR_ENUM_INT(CfrBlackjackAction) {
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
    CFR_BLACKJACK_ACTION_STAND,
    CFR_BLACKJACK_ACTION_DOUBLE_DOWN,
    CFR_BLACKJACK_ACTION_SPLIT
} BlackjackAction;

/*
 * Stores the decision-relevant value of one hand.
 *
 * total is the best current blackjack total: an ace counts as eleven exactly
 * when doing so does not bust the hand. is_soft records whether total currently
 * includes such an ace. can_split records whether the first two cards have the
 * same rank class. stake_multiplier is one normally and two after a double
 * down. A two-card hand marked from_split is not a natural blackjack.
 */
typedef struct {
    int total;
    size_t card_count;
    bool is_soft;
    bool can_split;
    size_t stake_multiplier;
    bool from_split;
} BlackjackHand;

/* Stores the information required to undo one transition exactly. */
typedef struct {
    BlackjackPhase previous_phase;
    BlackjackAction applied_action;
    size_t previous_active_hand;
    size_t previous_hand_count;
    BlackjackHand previous_active_hand_state;
    BlackjackHand previous_dealer_hand;
    BlackjackCard previous_dealer_up_card;
} BlackjackUndoEntry;

/*
 * Contains the complete state of a blackjack hand.
 *
 * Adapter rules:
 *
 * - independent rank draws (1/13 for ace through nine and 4/13 for ten-value
 *   cards), matching the basic-strategy infinite-deck abstraction;
 * - the dealer stands on every 17, including soft 17;
 * - a natural blackjack pays 3:2 and a push returns the stake;
 * - the player can hit, stand, double down, or split equal rank classes;
 * - doubling is available on any two-card hand, including after a split;
 * - non-ace pairs can be resplit to at most four hands;
 * - split aces receive one card each and cannot be resplit;
 * - no insurance or surrender.
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
 * player_hand is a compatibility alias for player_hands[0]. player_hand_count
 * gives the number of live entries and active_player_hand identifies the hand
 * currently being played. Retaining every physical hand makes the shared
 * four-hand resplit limit exact, including the intermediate three-hand state.
 * dealer_up_card is kept separately because it is the dealer information
 * visible to the player.
 */
typedef struct {
    BlackjackPhase phase;
    union {
        BlackjackHand player_hand;
        BlackjackHand player_hands[CFR_BLACKJACK_MAX_PLAYER_HANDS];
    };
    size_t player_hand_count;
    size_t active_player_hand;
    BlackjackHand dealer_hand;
    BlackjackCard dealer_up_card;
    BlackjackUndoEntry undo_history[CFR_BLACKJACK_UNDO_HISTORY_CAPACITY];
    size_t undo_count;
} BlackjackState;

/*
 * Initializes state before the first independent draw.
 * A null pointer produces CFR_STATUS_INVALID_ARGUMENT.
 */
Status cfr_blackjack_state_init(BlackjackState *state);

/*
 * Returns the const, static-lifetime blackjack descriptor.
 *
 * The descriptor supplies strategy_schema_id "cfr.blackjack/v5", so trainers
 * bound to it work with cfr_checkpoint_write, cfr_checkpoint_read, and
 * cfr_strategy_write_text. The identifier must change whenever the rules, the
 * information-set keys, or the meaning of the action indices become
 * incompatible with an earlier checkpoint.
 */
const Game *cfr_blackjack_descriptor(void);

/* Views blackjack_state as a mutable opaque game state without copying it. */
GameState *cfr_blackjack_state_as_game_state(BlackjackState *blackjack_state);

/* Views blackjack_state as a const opaque game state without copying it. */
const GameState *
cfr_blackjack_state_as_game_state_const(const BlackjackState *blackjack_state);

CFR_EXTERN_C_END

#endif
