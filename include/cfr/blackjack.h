#ifndef CFR_BLACKJACK_H
#define CFR_BLACKJACK_H

#include <stddef.h>

#include "cfr/game.h"

/* Blackjack usa una baraja francesa sin comodines. */
#define CFR_BLACKJACK_DECK_SIZE 52
/* Diez clases de valor: as, 2--9 y cualquier carta con valor diez. */
#define CFR_BLACKJACK_NUMBER_OF_CARD_RANKS 10
/* Los dos participantes son el jugador y la banca. */
#define CFR_BLACKJACK_NUMBER_OF_PLAYERS 2
/* Una mano nunca puede contener más cartas que la baraja completa. */
#define CFR_BLACKJACK_HAND_CAPACITY CFR_BLACKJACK_DECK_SIZE
/*
 * Una carta puede requerir una acción PEDIR previa. Se añade una posición
 * adicional para PLANTARSE. Este límite es deliberadamente conservador.
 */
#define CFR_BLACKJACK_UNDO_HISTORY_CAPACITY                                  \
    (2 * CFR_BLACKJACK_DECK_SIZE + 1)
/* En un nodo de azar puede quedar disponible cada una de las diez clases. */
#define CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS CFR_BLACKJACK_NUMBER_OF_CARD_RANKS

/*
 * Identifica el valor relevante de una carta.
 *
 * DIEZ representa indistintamente un diez, una jota, una reina o un rey. La
 * baraja contiene dieciséis cartas de esta clase y cuatro de cada otra clase.
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

/* Identifica el punto exacto de las reglas en el que se encuentra la mano. */
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
 * Identifica una carta extraída o una decisión del jugador.
 *
 * Las acciones de carta aparecen siempre en orden de as a diez. La banca no
 * es un actor estratégico: sus extracciones regladas son nodos de azar.
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

/* Conserva lo necesario para deshacer exactamente una transición. */
typedef struct {
    BlackjackPhase previous_phase;
    BlackjackAction applied_action;
} BlackjackUndoEntry;

/*
 * Contiene el estado completo de una mano de blackjack.
 *
 * Reglas del adaptador:
 *
 * - una sola baraja de 52 cartas, sin reposición;
 * - la banca se planta con cualquier 17, incluido un 17 suave;
 * - un blackjack natural paga 3:2 y un empate devuelve la apuesta;
 * - el jugador puede pedir o plantarse;
 * - no hay doblaje, separación, seguro ni rendición.
 *
 * CFR_PLAYER_0 representa al jugador. CFR_PLAYER_1 representa a la banca y
 * recibe siempre la utilidad opuesta. Las decisiones obligatorias de la banca
 * se modelan como azar para conservar un juego de dos participantes y suma
 * cero compatible con el motor.
 *
 * El llamador posee la estructura y debe inicializarla mediante
 * cfr_blackjack_state_init. Las operaciones no reservan memoria. El llamador
 * no debe modificar los campos directamente; las operaciones rechazan los
 * estados incoherentes que pueden detectar.
 *
 * dealer_cards[0] es la carta visible. Las cartas posteriores a la carta
 * tapada se conservan en orden de extracción.
 */
typedef struct {
    BlackjackPhase phase;
    BlackjackCard player_cards[CFR_BLACKJACK_HAND_CAPACITY];
    size_t player_card_count;
    BlackjackCard dealer_cards[CFR_BLACKJACK_HAND_CAPACITY];
    size_t dealer_card_count;
    /* Cantidad no repartida de cada valor, en orden de as a diez. */
    size_t remaining_cards[CFR_BLACKJACK_NUMBER_OF_CARD_RANKS];
    BlackjackUndoEntry undo_history[CFR_BLACKJACK_UNDO_HISTORY_CAPACITY];
    size_t undo_count;
} BlackjackState;

/*
 * Inicializa state en el primer reparto, con la baraja completa.
 * Un puntero nulo produce CFR_STATUS_INVALID_ARGUMENT.
 */
Status cfr_blackjack_state_init(BlackjackState *state);

/* Devuelve el descriptor constante y de vida estática de blackjack. */
const Game *cfr_blackjack_descriptor(void);

/* Presenta blackjack_state como un estado opaco modificable, sin copiarlo. */
GameState *cfr_blackjack_state_as_game_state(BlackjackState *blackjack_state);

/* Presenta blackjack_state como un estado opaco constante, sin copiarlo. */
const GameState *
cfr_blackjack_state_as_game_state_const(const BlackjackState *blackjack_state);

#endif
