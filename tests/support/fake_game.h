#ifndef SUPPORT_FAKE_GAME_H
#define SUPPORT_FAKE_GAME_H

#include <stddef.h>

#include "cfr/game.h"

typedef enum {
    FAKE_PHASE_CHANCE,
    FAKE_PHASE_PLAYER_0,
    FAKE_PHASE_PLAYER_1,
    FAKE_PHASE_TERMINAL
} FakePhase;

typedef enum { FAKE_COIN_NOT_SET, FAKE_COIN_HEADS, FAKE_COIN_TAILS } FakeCoin;

typedef enum {
    FAKE_ACTION_HEADS,
    FAKE_ACTION_TAILS,
    FAKE_ACTION_BET,
    FAKE_ACTION_FOLD,
    FAKE_ACTION_CALL,
    FAKE_ACTION_PASS
} FakeAction;

typedef struct {
    FakePhase previous_phase;
    FakeCoin previous_coin;
    FakeAction applied_action;
} FakeHistoryEntry;

typedef struct {
    FakePhase phase;
    FakeCoin coin;
    FakeHistoryEntry history[3];
    size_t history_count;
} FakeGameState;

typedef struct {
    /* The probability must be between zero and one. */
    Probability heads_probability;
} FakeGameConfig;

/* Initializes a caller-owned state. */
Status fake_game_state_init(FakeGameState *state);

/*
 * Returns a borrowed static descriptor that lives for the entire program.
 * The descriptor uses immutable internal configuration.
 */
const Game *fake_game_descriptor(void);

/* Views a mutable state through the contract's opaque type. */
GameState *fake_game_state_as_public(FakeGameState *fake_state);

/* Views a read-only state through the contract's opaque type. */
const GameState *
fake_game_state_as_public_const(const FakeGameState *fake_state);

#endif
