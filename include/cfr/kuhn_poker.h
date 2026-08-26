#ifndef CFR_KUHN_POKER_H
#define CFR_KUHN_POKER_H

#include <stddef.h>

#include "cfr/game.h"

/* Número máximo de acciones que contiene la historia pública. */
#define CFR_KUHN_POKER_PUBLIC_HISTORY_CAPACITY 3
/* Número máximo de transiciones que se pueden deshacer. */
#define CFR_KUHN_POKER_UNDO_HISTORY_CAPACITY 4
/* Número de jugadores de Kuhn Poker. */
#define CFR_KUHN_POKER_NUMBER_OF_PLAYERS 2
/*
 * Número máximo de acciones legales. El nodo de reparto tiene seis acciones.
 * Cada nodo de jugador tiene dos acciones.
 */
#define CFR_KUHN_POKER_MAX_POSSIBLE_ACTIONS 6

/*
 * Identifica una carta privada.
 *
 * El orden de las cartas reales representa su rango. La jota es la carta más
 * baja. El rey es la carta más alta.
 */
typedef enum {
    CFR_KUHN_POKER_CARD_NOT_DEALT,
    CFR_KUHN_POKER_CARD_JACK,
    CFR_KUHN_POKER_CARD_QUEEN,
    CFR_KUHN_POKER_CARD_KING
} KuhnPokerCard;

/* Identifica la fase actual y determina el actor y las acciones legales. */
typedef enum {
    CFR_KUHN_POKER_PHASE_CHANCE,
    CFR_KUHN_POKER_PHASE_PLAYER_0_OPEN,
    CFR_KUHN_POKER_PHASE_PLAYER_1_AFTER_CHECK,
    CFR_KUHN_POKER_PHASE_PLAYER_1_FACING_OPEN_BET,
    CFR_KUHN_POKER_PHASE_PLAYER_0_FACING_CHECK_BET,
    CFR_KUHN_POKER_PHASE_TERMINAL
} KuhnPokerPhase;

/*
 * Identifica un reparto o una acción pública.
 *
 * El nodo de azar enumera JQ, JK, QJ, QK, KJ y KQ en este orden. Un nodo sin
 * una apuesta pendiente enumera CHECK y BET. Un nodo con una apuesta pendiente
 * enumera FOLD y CALL. El orden es estable para conservar el significado de
 * cada índice de estrategia.
 */
typedef enum {
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

/* Contiene una instantánea anterior a una transición. */
typedef struct {
    /* Fase anterior a la acción aplicada. */
    KuhnPokerPhase previous_phase;
    /* Cartas anteriores a la acción aplicada. */
    KuhnPokerCard previous_cards[CFR_KUHN_POKER_NUMBER_OF_PLAYERS];
    /* Número anterior de acciones públicas. */
    size_t previous_public_action_count;
    /* Acción que creó esta entrada. */
    KuhnPokerAction applied_action;
} KuhnPokerUndoEntry;

/*
 * Contiene el estado completo de una partida de Kuhn Poker.
 *
 * El llamador posee la estructura y la reserva sin asignación dinámica. El
 * adaptador no reserva memoria durante sus operaciones.
 *
 * El llamador debe inicializar la estructura con cfr_kuhn_poker_state_init. El
 * llamador debe cambiar el estado mediante las operaciones del descriptor. El
 * llamador no debe modificar los campos directamente. Una modificación directa
 * invalida el contrato del estado. Las operaciones devuelven
 * CFR_STATUS_INVALID_ARGUMENT cuando detectan un estado incoherente.
 *
 * public_actions contiene solamente las acciones que observan los dos
 * jugadores. Las claves de información pueden usar esta historia.
 * undo_history contiene instantáneas que pueden incluir cartas privadas. Las
 * claves de información nunca usan undo_history.
 */
typedef struct {
    /* Fase actual de la partida. */
    KuhnPokerPhase phase;
    /* Carta privada del jugador cero y del jugador uno. */
    KuhnPokerCard cards[CFR_KUHN_POKER_NUMBER_OF_PLAYERS];
    /* Acciones públicas en el orden en que se aplicaron. */
    KuhnPokerAction public_actions[CFR_KUHN_POKER_PUBLIC_HISTORY_CAPACITY];
    /* Número de posiciones usadas en public_actions. */
    size_t public_action_count;
    /* Instantáneas privadas de las transiciones aplicadas. */
    KuhnPokerUndoEntry undo_history[CFR_KUHN_POKER_UNDO_HISTORY_CAPACITY];
    /* Número de posiciones usadas en undo_history. */
    size_t undo_count;
} KuhnPokerState;

/*
 * Inicializa state como una raíz de azar sin cartas ni acciones.
 *
 * La función limpia los dos historiales. Un puntero nulo produce
 * CFR_STATUS_INVALID_ARGUMENT.
 */
Status cfr_kuhn_poker_state_init(KuhnPokerState *state);

/*
 * Devuelve el descriptor constante de Kuhn Poker.
 *
 * El adaptador posee el descriptor. El préstamo tiene vida estática. El
 * llamador no debe modificar ni liberar el descriptor.
 */
const Game *cfr_kuhn_poker_descriptor(void);

/*
 * Presenta kuhn_poker_state como un estado opaco modificable.
 *
 * La función devuelve el mismo préstamo. La función no copia ni reserva
 * memoria. La función no transfiere la propiedad.
 */
GameState *cfr_kuhn_poker_state_as_game_state(KuhnPokerState *kuhn_poker_state);

/*
 * Presenta kuhn_poker_state como un estado opaco constante.
 *
 * La función devuelve el mismo préstamo. La función no copia ni reserva
 * memoria. La función no transfiere la propiedad.
 */
const GameState *cfr_kuhn_poker_state_as_game_state_const(
    const KuhnPokerState *kuhn_poker_state);

#endif
