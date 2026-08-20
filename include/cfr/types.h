#ifndef CFR_TYPES_H
#define CFR_TYPES_H

#include <stdint.h>

/* Identifica uno de los dos jugadores del juego de suma cero. */
typedef enum { CFR_PLAYER_0, CFR_PLAYER_1 } Player;

/* Identifica la entidad que selecciona la próxima acción. */
typedef enum { CFR_ACTOR_PLAYER, CFR_ACTOR_CHANCE } ActorKind;

/* Describe al actor actual. */
typedef struct {
    ActorKind kind;
    /* Interprete este campo solo cuando kind sea CFR_ACTOR_PLAYER. */
    Player player;
} Actor;

/* Identifica una acción según las reglas del adaptador. */
typedef int Action;

/* Identifica de forma estable un conjunto de información. */
typedef int64_t InfoSetKey;

/* Indica el resultado de una operación del contrato. */
typedef enum {
    /* La operación terminó correctamente. Las salidas son válidas. */
    CFR_STATUS_SUCCESS,
    /* Un argumento es nulo, inválido o no corresponde al estado. */
    CFR_STATUS_INVALID_ARGUMENT,
    /* La acción no es legal en el estado actual. */
    CFR_STATUS_ILLEGAL_ACTION,
    /* El almacenamiento del llamador no tiene capacidad suficiente. */
    CFR_STATUS_BUFFER_TOO_SMALL,
    /* Un acumulado o un resultado aritmético no es finito. */
    CFR_STATUS_NUMERIC_ERROR,
    /* El módulo no pudo completar una reserva interna. */
    CFR_STATUS_OUT_OF_MEMORY,
    /* Una búsqueda válida no encontró la clave solicitada. */
    CFR_STATUS_NOT_FOUND
} Status;

/* Representa la utilidad de un estado terminal para un jugador. */
typedef double Utility;

/* Representa una probabilidad en el intervalo cerrado de cero a uno. */
typedef double Probability;

#endif
