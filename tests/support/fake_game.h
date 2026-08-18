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
    /* La probabilidad debe estar entre cero y uno. */
    Probability heads_probability;
} FakeGameConfig;

/* Inicializa un estado que pertenece al llamador. */
Status fake_game_state_init(FakeGameState *state);

/*
 * Presta un descriptor estático que vive durante todo el programa.
 * El descriptor usa una configuración interna inmutable.
 */
const Game *fake_game_descriptor(void);

/* Presenta un estado modificable mediante el tipo opaco del contrato. */
GameState *fake_game_state_as_public(FakeGameState *fake_state);

/* Presenta un estado de solo lectura mediante el tipo opaco del contrato. */
const GameState *
fake_game_state_as_public_const(const FakeGameState *fake_state);

#endif
