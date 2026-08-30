#ifndef CFR_GAME_H
#define CFR_GAME_H

#include <stdbool.h>
#include <stddef.h>

#include "cfr/types.h"

/*
 * Representa un estado mediante un tipo opaco.
 *
 * El adaptador define la representación concreta. El llamador posee cada
 * instancia. Las operaciones reciben un préstamo durante cada llamada.
 */
typedef struct CfrGameState GameState;

typedef struct CfrGameOperations GameOperations;
typedef struct CfrGame Game;

/*
 * Define las operaciones que un adaptador ofrece al motor.
 *
 * El adaptador posee esta tabla y el contexto asociado. Ambos objetos deben
 * vivir mientras exista el descriptor Game que los usa.
 *
 * El llamador posee los estados, los arrays y las variables de salida. Cada
 * callback recibe préstamos. Un callback no debe guardar, liberar ni cambiar
 * la propiedad de un puntero recibido.
 *
 * Un callback no debe reservar memoria durante una operación. El llamador
 * proporciona todo el almacenamiento que una operación necesita.
 *
 * Una salida es válida solo cuando el callback devuelve CFR_STATUS_SUCCESS.
 * El callback debe conservar las salidas cuando devuelve un error. La única
 * excepción es required_count cuando el búfer es insuficiente.
 */
struct CfrGameOperations {
    /*
     * Indica si el estado es terminal.
     *
     * Esta consulta acepta cualquier estado válido. result recibe el valor
     * solo cuando la operación termina correctamente. Un estado inválido
     * produce CFR_STATUS_INVALID_ARGUMENT.
     */
    Status (*is_terminal)(const void *context, const GameState *state,
                          bool *result);

    /*
     * Obtiene la utilidad terminal para un jugador.
     *
     * El estado debe ser terminal. player debe identificar un jugador válido.
     * result recibe la utilidad solo cuando la operación termina correctamente.
     * Otro estado o jugador produce CFR_STATUS_INVALID_ARGUMENT.
     */
    Status (*terminal_utility)(const void *context, const GameState *state,
                               Player player, Utility *result);

    /*
     * Obtiene el actor de un estado no terminal.
     *
     * result recibe el actor solo cuando la operación termina correctamente.
     * Un estado terminal o inválido produce CFR_STATUS_INVALID_ARGUMENT.
     */
    Status (*current_actor)(const void *context, const GameState *state,
                            Actor *result);

    /*
     * Enumera las acciones legales de un estado no terminal.
     *
     * capacity indica el número de elementos disponibles en actions. El
     * puntero actions es obligatorio aunque capacity sea cero.
     *
     * required_count recibe el número de elementos necesarios. Si falta
     * capacidad, el callback devuelve CFR_STATUS_BUFFER_TOO_SMALL. En ese caso,
     * required_count recibe la capacidad necesaria y actions no cambia.
     * Un estado terminal o inválido produce CFR_STATUS_INVALID_ARGUMENT.
     */
    Status (*legal_actions)(const void *context, const GameState *state,
                            Action *actions, size_t capacity,
                            size_t *required_count);

    /*
     * Aplica una acción legal a un estado no terminal.
     *
     * El callback devuelve CFR_STATUS_ILLEGAL_ACTION para una acción ilegal.
     * Un estado inválido produce CFR_STATUS_INVALID_ARGUMENT. El callback puede
     * devolver CFR_STATUS_BUFFER_TOO_SMALL si no puede registrar otra acción.
     * El callback no debe modificar el estado cuando devuelve un error.
     */
    Status (*apply_action)(const void *context, GameState *state,
                           Action action);

    /*
     * Deshace la última acción aplicada.
     *
     * El estado debe contener una acción que se pueda deshacer. El callback no
     * debe modificar el estado cuando devuelve un error.
     * Un estado sin historial produce CFR_STATUS_INVALID_ARGUMENT.
     */
    Status (*undo_action)(const void *context, GameState *state);

    /*
     * Obtiene la probabilidad de una acción legal de azar.
     *
     * El actor actual debe ser el azar. result recibe un valor entre cero y uno
     * solo cuando la operación termina correctamente.
     * Otro actor produce CFR_STATUS_INVALID_ARGUMENT. Una acción que no es de
     * azar produce CFR_STATUS_ILLEGAL_ACTION.
     */
    Status (*chance_probability)(const void *context, const GameState *state,
                                 Action action, Probability *result);

    /*
     * Obtiene la clave del conjunto de información del jugador actual.
     *
     * El actor actual debe ser un jugador. Estados que el jugador no puede
     * distinguir deben producir la misma clave estable.
     * Otro actor o un estado inválido produce CFR_STATUS_INVALID_ARGUMENT.
     */
    Status (*information_set_key)(const void *context, const GameState *state,
                                  InfoSetKey *result);
};

/* Describe un juego sin almacenar el estado de una partida. */
struct CfrGame {
    /* Tabla prestada. El adaptador conserva la propiedad. */
    const GameOperations *operations;
    /*
     * Contexto prestado. El adaptador conserva la propiedad.
     *
     * El puntero puede ser nulo si el adaptador no necesita configuración. Un
     * adaptador debe documentar y validar cualquier requisito adicional.
     */
    const void *context;
    /*
     * Número de jugadores que toman decisiones y deben recibir un recorrido
     * de entrenamiento, empezando por CFR_PLAYER_0.
     *
     * Un juego de dos participantes puede tener un solo jugador estratégico:
     * el otro participante puede existir únicamente como perspectiva de
     * utilidad y sus transiciones pueden estar modeladas como azar.
     * El entrenador requiere un valor uno o dos.
     */
    size_t strategic_player_count;
    /* Límite superior de acciones legales en cualquier estado. */
    size_t max_legal_actions;
};

/*
 * Reglas comunes de las envolturas cfr_game_*:
 *
 * Cada envoltura valida game, operations, el callback, state y las salidas
 * obligatorias. Una validación fallida devuelve CFR_STATUS_INVALID_ARGUMENT.
 * En ese caso, la envoltura no llama al callback y no modifica las salidas.
 *
 * Una envoltura no valida las reglas del juego. El adaptador valida la fase,
 * el jugador y la acción. La envoltura devuelve sin cambios el Status del
 * callback.
 */

/* Consulta si state es terminal y escribe el resultado en result. */
Status cfr_game_is_terminal(const Game *game, const GameState *state,
                            bool *result);

/* Consulta la utilidad terminal de player y la escribe en result. */
Status cfr_game_terminal_utility(const Game *game, const GameState *state,
                                 Player player, Utility *result);

/* Consulta el actor actual y lo escribe en result. */
Status cfr_game_current_actor(const Game *game, const GameState *state,
                              Actor *result);

/*
 * Enumera acciones legales en el array actions.
 *
 * capacity cuenta elementos. required_count recibe el número necesario. El
 * puntero actions es obligatorio aunque capacity sea cero.
 */
Status cfr_game_legal_actions(const Game *game, const GameState *state,
                              Action *actions, size_t capacity,
                              size_t *required_count);

/* Aplica action al estado modificable state. */
Status cfr_game_apply_action(const Game *game, GameState *state, Action action);

/* Deshace la última acción aplicada al estado modificable state. */
Status cfr_game_undo_action(const Game *game, GameState *state);

/* Consulta la probabilidad de action en un nodo de azar. */
Status cfr_game_chance_probability(const Game *game, const GameState *state,
                                   Action action, Probability *result);

/* Consulta la clave estable del conjunto de información actual. */
Status cfr_game_information_set_key(const Game *game, const GameState *state,
                                    InfoSetKey *result);

#endif
